"""Beamer audio transport prototype: one bidirectional, encrypted UDP media flow.

    peer.py keygen KEYFILE
    peer.py serve --port 24823 --key-file K [--source S] [--sink S] ...      (PC)
    peer.py send --to HOST:PORT --key-file K [--source S] [--sink S] ...     (Mac, initiates)
    peer.py local --key-file K ...                                           (both ends in one
                                                                              process, no network)

Sources: tone[:HZ[:AMP]] | clicks[:PERIOD_MS] | silence | stdin | cmd:COMMAND | sd:DEVICE | echo
Sinks:   null | stdout | cmd:COMMAND | analyze | wav:PATH | sd:DEVICE | echo
(f32le interleaved at 48 kHz on stdin/stdout/cmd pipes, per the beamer-audio contract.)
'echo' on a server turns received audio straight back to the client (network-only loop).

The client (Mac) always sends first from a connect()-ed socket; the server only ever answers
the source address of an authenticated hello, so the Mac never listens.
"""
from __future__ import annotations

import argparse
import heapq
import json
import math
import os
import random
import shlex
import socket
import struct
import subprocess
import sys
import threading
import time
import wave

import numpy as np

import media as M
from media import RATE

mono = time.monotonic


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# ================================================================ transports

class UdpClient:
    kind = "udp"
    last_tx = 0.0

    def __init__(self, host, port, local_port=0):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        if local_port:
            self.sock.bind(("0.0.0.0", local_port))
        self.sock.connect((host, port))
        self.peer = (host, port)

    def send(self, data):
        self.last_tx = mono()
        try:
            self.sock.send(data)
        except OSError:
            pass  # ICMP unreachable before the server is up etc.

    def recv(self):
        while True:
            try:
                return self.sock.recv(65535), self.peer
            except ConnectionResetError:  # Windows: ICMP port unreachable
                continue

    def close(self):
        self.sock.close()


class UdpServer:
    kind = "udp"
    last_tx = 0.0

    def __init__(self, bind, port):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.sock.bind((bind, port))
        self.peer = None

    def send(self, data):
        if self.peer is not None:
            self.last_tx = mono()
            try:
                self.sock.sendto(data, self.peer)
            except OSError:
                pass

    def recv(self):
        while True:
            try:
                return self.sock.recvfrom(65535)
            except ConnectionResetError:  # Windows reports ICMP port unreachable this way
                continue

    def close(self):
        self.sock.close()


class PairEnd:
    """In-process datagram pipe for `local` (no network, no firewall)."""
    kind = "pair"
    last_tx = 0.0

    def __init__(self, sock):
        self.sock, self.peer = sock, "pair"
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)

    def send(self, data):
        try:
            self.sock.send(data)
        except OSError:
            pass

    def recv(self):
        return self.sock.recv(65535), self.peer

    def close(self):
        self.sock.close()


class TcpStream:
    """Fallback transport: the same sealed packets, u16 length-prefixed, over one TCP stream
    (stands in for 'audio frames inside the Beamer link')."""
    kind = "tcp"
    last_tx = 0.0

    def __init__(self, sock, peer):
        self.sock, self.peer = sock, peer
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.wlock = threading.Lock()
        self.buf = b""

    def send(self, data):
        self.last_tx = mono()
        with self.wlock:
            try:
                self.sock.sendall(struct.pack("!H", len(data)) + data)
            except OSError:
                pass

    def _read(self, n):
        while len(self.buf) < n:
            b = self.sock.recv(65536)
            if not b:
                raise OSError("closed")
            self.buf += b
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        (n,) = struct.unpack("!H", self._read(2))
        return self._read(n), self.peer

    def close(self):
        try:
            self.sock.close()
        except OSError:
            pass


# ================================================================ sender + impairment

class Sender:
    def __init__(self, transport, sealer, args, channels):
        self.t, self.sealer, self.ch = transport, sealer, channels
        self.frame = int(round(args.frame_ms * RATE / 1000))
        if args.codec == "opus":
            app = args.app or ("voip" if channels == 1 else "audio")
            self.codec = M.OpusCodec(channels, self.frame, bitrate=args.bitrate or (32000 if channels == 1 else 96000),
                                     fec=args.fec, loss_perc=args.loss_hint, application=app,
                                     dtx=args.dtx, complexity=args.complexity)
        else:
            self.codec = M.PcmCodec(channels, self.frame)
        self.enc = self.codec.encoder()
        self.pend = np.zeros((0, channels), np.float32)
        self.ts = random.getrandbits(32)  # random initial timestamp, as RTP
        self.loss, self.burst, self.jitter = args.loss / 100.0, args.burst, args.jitter_ms / 1000.0
        self.rng = random.Random(args.seed)
        self.in_loss = False
        self.enabled = False
        self.first = True
        self.lock = threading.Lock()
        self.st = {"tx": 0, "tx_bytes": 0, "inj_drop": 0, "enc_s": 0.0, "frames": 0}
        self.heap = []
        self.hcv = threading.Condition()
        self.trace = [] if getattr(args, "pkt_log", None) else None  # (ts, t_feed, t_sent)
        self.payloads = []  # encoded frames, same order as trace (for offline decoding)
        self._t_feed = 0.0
        if self.jitter > 0:
            threading.Thread(target=self._delayer, daemon=True, name="delayer").start()

    def feed(self, x: np.ndarray):
        """x: (n, channels) float32 at 48 kHz, any n. Called from the source thread."""
        if x.ndim == 1:
            x = x.reshape(-1, self.ch)
        if x.shape[1] != self.ch:  # adapt channel count
            x = np.repeat(x.mean(axis=1, keepdims=True), self.ch, axis=1) if self.ch > 1 else x.mean(axis=1, keepdims=True)
        with self.lock:
            self._t_feed = mono()
            self.pend = np.concatenate([self.pend, x.astype(np.float32, copy=False)])
            while len(self.pend) >= self.frame:
                fr, self.pend = self.pend[:self.frame], self.pend[self.frame:]
                self._send_frame(fr)

    def _send_frame(self, fr):
        ts = self.ts
        self.ts = (self.ts + self.frame) & 0xFFFFFFFF
        if not self.enabled:
            return
        t0 = time.perf_counter()
        payload = self.enc.encode(fr)
        self.st["enc_s"] += time.perf_counter() - t0
        self.st["frames"] += 1
        flags = (M.F_FEC if self.codec.id == M.C_OPUS and self.codec.fec else 0)
        if self.first:
            flags |= M.F_MARK
            self.first = False
        if float(np.sqrt(np.mean(fr * fr))) < 0.003:  # < -50 dBFS
            flags |= M.F_SILENCE
        pkt = self.sealer.seal(M.MEDIA, payload, flags, self.codec.id, ts, self.frame, self.ch)
        self.st["tx"] += 1
        self.st["tx_bytes"] += len(pkt) + 28
        if self._lose():
            self.st["inj_drop"] += 1
            return
        if self.jitter > 0:
            with self.hcv:
                heapq.heappush(self.heap, (mono() + self.rng.uniform(0, self.jitter), self.st["tx"], pkt))
                self.hcv.notify()
        else:
            self.t.send(pkt)
        if self.trace is not None:
            self.trace.append((ts, self._t_feed, mono()))
            self.payloads.append(payload)

    def _lose(self):
        if self.loss <= 0:
            return False
        if self.burst <= 1:
            return self.rng.random() < self.loss
        # Gilbert model: mean burst length `burst`, stationary loss rate `loss`
        p_exit = 1.0 / self.burst
        p_enter = self.loss * p_exit / (1 - self.loss)
        self.in_loss = (self.rng.random() >= p_exit) if self.in_loss else (self.rng.random() < p_enter)
        return self.in_loss

    def _delayer(self):
        while True:
            with self.hcv:
                while not self.heap:
                    self.hcv.wait()
                due = self.heap[0][0]
                now = mono()
                if due > now:
                    self.hcv.wait(due - now)
                    continue
                _, _, pkt = heapq.heappop(self.heap)
            self.t.send(pkt)

    def snapshot(self):
        with self.lock:
            s = dict(self.st)
            for k in self.st:
                self.st[k] = 0 if k != "enc_s" else 0.0
        s["enc_us"] = round(s.pop("enc_s") * 1e6 / max(1, s["frames"]), 1)
        return s


# ================================================================ sources

class SynthSource:
    """Paced by the monotonic clock: sample s 'happens' at t0 + s/RATE and is emitted
    once its 10 ms block is complete (an ideal zero-latency capture device)."""

    def __init__(self, spec, channels, block=480):
        parts = spec.split(":")
        self.kind = parts[0]
        self.ch, self.block = channels, block
        self.freq = float(parts[1]) if self.kind == "tone" and len(parts) > 1 else 440.0
        self.amp = float(parts[2]) if self.kind == "tone" and len(parts) > 2 else 0.25
        self.period = (float(parts[1]) if self.kind == "clicks" and len(parts) > 1 else 1000.0) / 1000
        n = 96  # 2 ms Hann-windowed 2 kHz burst, peak at sample 48
        self.click = (0.5 * np.hanning(n) * np.sin(2 * np.pi * 2000 * (np.arange(n) - 48) / RATE + np.pi / 2)).astype(np.float32)
        self.t0 = None
        self.stop_ev = threading.Event()

    def start(self, emit):
        self.emit = emit
        threading.Thread(target=self._run, daemon=True, name="synth").start()

    def _run(self):
        s = 0
        self.t0 = mono() + 0.05
        P = int(round(self.period * RATE))
        while not self.stop_ev.is_set():
            due = self.t0 + (s + self.block) / RATE
            d = due - mono()
            if d > 0:
                time.sleep(d)
            idx = np.arange(s, s + self.block)
            if self.kind == "tone":
                x = self.amp * np.sin(2 * np.pi * self.freq * idx / RATE)
            elif self.kind == "voice":
                x = voice_signal(idx)
            elif self.kind == "clicks":
                x = np.zeros(self.block)
                ph = idx % P
                m = ph < len(self.click)
                x[m] = self.click[ph[m]]
            else:
                x = np.zeros(self.block)
            s += self.block
            self.emit(np.repeat(x.astype(np.float32)[:, None], self.ch, axis=1))

    def click_peak_time(self, k):
        return self.t0 + (k * self.period * RATE + 48) / RATE

    def stop(self):
        self.stop_ev.set()


VOICE_F0 = 180.0


def voice_signal(idx):
    """Voiced-speech stand-in that the analyzer can model exactly: 12 harmonics of 180 Hz
    (amplitude 1/k), 3 Hz amplitude modulation (depth 0.45), peak about 0.2. Opus' SILK
    layer treats it as active speech (LBRR/FEC on ~70 % of frames, like real voiced speech)."""
    t = idx / RATE
    env = 0.55 + 0.45 * np.sin(2 * np.pi * 3 * t)
    return 0.12 * env * sum(np.sin(2 * np.pi * k * VOICE_F0 * t) / k for k in range(1, 13))


class PipeSource:
    def __init__(self, spec, channels):
        self.ch = channels
        self.proc = None
        if spec == "stdin":
            self.f = sys.stdin.buffer
        else:
            cmd = spec[4:]
            self.proc = subprocess.Popen(cmd if sys.platform == "win32" else shlex.split(cmd),
                                         stdout=subprocess.PIPE, bufsize=0)
            self.f = self.proc.stdout

    def start(self, emit):
        self.emit = emit
        threading.Thread(target=self._run, daemon=True, name="pipe-src").start()

    def _run(self):
        fsz = 4 * self.ch
        rest = b""
        while True:
            b = self.f.read(fsz * 240) if self.proc is None else os.read(self.f.fileno(), 65536)
            if not b:
                break
            b = rest + b
            n = len(b) // fsz * fsz
            rest = b[n:]
            if n:
                self.emit(np.frombuffer(b[:n], "<f4").reshape(-1, self.ch))

    def stop(self):
        if self.proc:
            self.proc.terminate()


class SdSource:
    def __init__(self, spec, channels, block=480):
        import sounddevice as sd
        self.sd = sd
        self.dev = _sd_device(spec[3:], "input")
        self.ch, self.block = channels, block

    def start(self, emit):
        def cb(indata, frames, t, status):
            if status:
                self.status = str(status)
            emit(indata.copy())
        self.stream = self.sd.InputStream(device=self.dev, samplerate=RATE, channels=self.ch,
                                          dtype="float32", blocksize=self.block, latency="low",
                                          callback=cb)
        self.stream.start()
        log(json.dumps({"event": "sd_input", "device": self.dev, "latency_ms": round(self.stream.latency * 1000, 2)}))

    def stop(self):
        self.stream.stop()


class EchoSource:
    def start(self, emit):
        self.emit = emit

    def stop(self):
        pass


def _sd_device(name, kind):
    import sounddevice as sd
    if name.isdigit():
        return int(name)
    hostapis = sd.query_hostapis()
    best = None
    for i, d in enumerate(sd.query_devices()):
        if name.lower() in d["name"].lower() and d[f"max_{kind}_channels"] > 0:
            api = hostapis[d["hostapi"]]["name"]
            score = {"Windows WASAPI": 3, "Core Audio": 3, "Windows WDM-KS": 1}.get(api, 0)
            if best is None or score > best[0]:
                best = (score, i)
    if best is None:
        raise SystemExit(f"no {kind} device matching {name!r}")
    return best[1]


# ================================================================ sinks

class RxSlot:
    """Where the current remote stream's jitter buffer lives; sinks pull from it."""

    def __init__(self, channels):
        self.jb = None
        self.ch = channels

    def pull(self, n, now=None):
        jb = self.jb
        if jb is None:
            return np.zeros((n, self.ch), np.float32)
        x = jb.pull(n, now)
        if x.shape[1] != self.ch:
            x = np.repeat(x.mean(axis=1, keepdims=True), self.ch, axis=1) if self.ch > 1 else x.mean(axis=1, keepdims=True)
        return x


class Analyzer:
    """Click latency (vs. the local click source) and tone continuity (glitch windows)."""

    def __init__(self, src: SynthSource | None, tone_hz: float | None):
        self.src, self.tone = src, tone_hz
        self.warmup = 3.0
        self.t_start = None
        self.frozen = False
        self.lat = []
        self.hist = np.zeros(0, np.float32)
        self.hist_t0 = None
        self.cool_until = 0.0
        self.win = []  # tone residual dB per 10 ms window
        self.glitch_windows = 0
        self.glitch_events = 0
        self._in_glitch = False
        self.tone_buf = np.zeros(0, np.float32)
        self.tone_n = 0
        self.harm = 1
        self.thr_db = -20.0
        self._basis = None
        self.pw = []
        self.glitch_t = []

    def feed(self, x, t_first):
        if self.frozen:
            return
        x = x[:, 0]
        if self.t_start is None:
            self.t_start = t_first
        if self.src is not None and self.src.kind == "clicks" and self.src.t0 is not None:
            a = np.abs(x)
            idx = np.nonzero(a > 0.1)[0]
            for i in idx:
                t = t_first + i / RATE
                if t < self.cool_until or t < self.src.t0 + self.warmup:
                    continue
                seg = a[i:i + 192]
                j = i + int(np.argmax(seg))
                tp = t_first + j / RATE
                k = math.floor((tp - self.src.t0) / self.src.period)
                lat = tp - self.src.click_peak_time(k)
                if lat < 0:
                    lat += self.src.period
                self.lat.append(lat * 1000)
                self.cool_until = t + self.src.period * 0.5
                break
        if self.tone:
            self.tone_buf = np.concatenate([self.tone_buf, x])
            W = 480
            if self._basis is None:
                n = np.arange(W) / RATE
                cols = []
                for k in range(1, self.harm + 1):
                    for f in (np.sin, np.cos):
                        b = f(2 * np.pi * k * self.tone * n)
                        cols.append(b)
                        if self.harm > 1:
                            cols.append(b * (n - n.mean()) * 100)
                self._basis = np.stack(cols, 1)
                self._pinv = np.linalg.pinv(self._basis)
            while len(self.tone_buf) >= W:
                w, self.tone_buf = self.tone_buf[:W], self.tone_buf[W:]
                res = w - self._basis @ (self._pinv @ w)
                p = float(np.mean(w * w)) + 1e-12
                rdb = 10 * math.log10(float(np.mean(res * res)) / p + 1e-12)  # relative to window power
                self.pw.append(p)
                med = float(np.median(self.pw[-100:]))
                ldb = 10 * math.log10(p / med)
                self.tone_n += 1
                bad = rdb > self.thr_db or ldb < (-20 if self.harm > 1 else -6)
                if self.tone_n > 50:  # skip start-up
                    self.win.append(rdb)
                    if bad:
                        self.glitch_windows += 1
                        if not self._in_glitch:
                            self.glitch_events += 1
                            if len(self.glitch_t) < 30:
                                self.glitch_t.append(round(t_first - self.t_start, 2))
                    self._in_glitch = bad

    def summary(self, reset=False):
        d = {}
        if self.lat:
            L = np.array(self.lat)
            d["click_latency_ms"] = {"n": len(L), "mean": round(float(L.mean()), 2),
                                     "p50": round(float(np.median(L)), 2), "min": round(float(L.min()), 2),
                                     "p95": round(float(np.quantile(L, 0.95)), 2), "max": round(float(L.max()), 2),
                                     "sd": round(float(L.std()), 2)}
        if self.tone:
            W = np.array(self.win) if self.win else np.zeros(1)
            d["tone"] = {"windows": len(self.win), "glitch_windows": self.glitch_windows,
                         "glitch_events": self.glitch_events,
                         "residual_db_median": round(float(np.median(W)), 1),
                         "residual_db_p99": round(float(np.quantile(W, 0.99)), 1),
                         "glitch_at_s": self.glitch_t}
        return d


class PacedSink:
    """Pulls `block` samples every block period from the jitter buffer (monotonic clock):
    an ideal zero-latency playback device. Handles null/stdout/cmd/analyze/wav/echo."""

    def __init__(self, spec, slot, block, analyzer=None, echo=None):
        self.spec, self.slot, self.block = spec, slot, block
        self.analyzer, self.echo = analyzer, echo
        self.out = None
        self.wav = None
        if spec == "stdout":
            self.out = sys.stdout.buffer
        elif spec.startswith("cmd:"):
            cmd = spec[4:]
            self.proc = subprocess.Popen(cmd if sys.platform == "win32" else shlex.split(cmd),
                                         stdin=subprocess.PIPE, bufsize=0)
            self.out = self.proc.stdin
        elif spec.startswith("wav:"):
            self.wav = wave.open(spec[4:], "wb")
            self.wav.setnchannels(slot.ch)
            self.wav.setsampwidth(2)
            self.wav.setframerate(RATE)
        self.late = 0
        self.stop_ev = threading.Event()

    def start(self):
        threading.Thread(target=self._run, daemon=True, name="sink").start()

    def _run(self):
        t = mono()
        while not self.stop_ev.is_set():
            t += self.block / RATE
            d = t - mono()
            if d > 0:
                time.sleep(d)
            elif d < -0.05:
                self.late += 1
                t = mono()
            x = self.slot.pull(self.block, t)
            if self.analyzer:
                self.analyzer.feed(x, t)
            if self.echo:
                self.echo.emit(x)
            if self.out:
                try:
                    self.out.write(x.astype("<f4").tobytes())
                    self.out.flush()
                except (BrokenPipeError, OSError):
                    self.out = None
            if self.wav:
                self.wav.writeframes((np.clip(x, -1, 1) * 32767).astype("<i2").tobytes())

    def stop(self):
        self.stop_ev.set()
        if self.wav:
            time.sleep(0.05)
            self.wav.close()


class SdSink:
    def __init__(self, spec, slot, block, analyzer=None):
        import sounddevice as sd
        self.sd, self.slot, self.block = sd, slot, block
        self.dev = _sd_device(spec[3:], "output")
        self.analyzer = analyzer
        self.dac = []

    def start(self):
        def cb(outdata, frames, t, status):
            now = mono()
            x = self.slot.pull(frames, now)
            outdata[:] = x
            if len(self.dac) < 2000:
                self.dac.append(t.outputBufferDacTime - t.currentTime)
        self.stream = self.sd.OutputStream(device=self.dev, samplerate=RATE, channels=self.slot.ch,
                                           dtype="float32", blocksize=self.block, latency="low",
                                           callback=cb)
        self.stream.start()
        log(json.dumps({"event": "sd_output", "device": self.dev, "latency_ms": round(self.stream.latency * 1000, 2)}))

    def stop(self):
        self.stream.stop()


# ================================================================ session

class Session:
    def __init__(self, transport, master, args, role, src_channels, sink_channels, name):
        self.t, self.master, self.args, self.role, self.name = transport, master, args, role, name
        self.my_stream = random.getrandbits(32)
        self.sealer = M.Sealer(master, self.my_stream)
        self.opener = M.Opener(master)
        self.peer_stream = None
        self.peer_hello = None
        self.connected = threading.Event()
        self.last_rx = 0.0
        self.slot = RxSlot(sink_channels)
        self.sender = Sender(transport, self.sealer, args, src_channels) if src_channels else None
        self.pings = []  # (t_local, rtt, offset)
        self.remote_stats = []
        self.stats_log = []
        self.remote_summary = None
        self.analyzer = None
        self.cpu0 = (time.process_time(), mono())
        self.stop_ev = threading.Event()
        self.out = None
        self.rx_trace = [] if getattr(args, "pkt_log", None) else None  # (ts32, arrival)
        self.last_jb = None
        self.last_media = 0.0
        self.rr_sent = 0
        self.rr_rx = 0

    def rr_loop(self):
        """Receiver reports: while media arrives and this side has sent nothing for one
        interval, send a tiny authenticated RR packet (keeps this machine's Wi-Fi out of
        power save; see media.RR)."""
        iv = self.args.rr_ms / 1000
        if iv <= 0 or self.t.kind == "tcp":
            return
        nxt = mono()
        while not self.stop_ev.is_set():
            nxt += iv
            d = nxt - mono()
            if d > 0:
                time.sleep(d)
            else:
                nxt = mono()
            now = mono()
            jb = self.slot.jb
            if jb is None or now - self.last_media > 1.0 or now - self.t.last_tx < iv * 0.9:
                continue
            lv = min(65535, int(jb.level_ms() * 10)) if jb.playing else 0
            self.t.send(self.sealer.seal(M.RR, struct.pack("!IH", (jb.last_ts32 or 0) & 0xFFFFFFFF, lv)))
            self.rr_sent += 1

    def dump_trace(self):
        """--pkt-log PREFIX: PREFIX.tx.npy (ts, t_feed, t_sent), PREFIX.rx.npy (ts, arrival),
        PREFIX.pull.npy (t_sched, t_now, level_ms, event) -- time.monotonic() of this machine."""
        pfx = self.args.pkt_log
        if not pfx:
            return
        if self.sender is not None and self.sender.trace:
            np.save(pfx + ".tx.npy", np.array(self.sender.trace, float))
            with open(pfx + ".tx.opus", "wb") as f:  # u16 length + payload per frame
                for b in self.sender.payloads:
                    f.write(struct.pack("<H", len(b)) + b)
        if self.rx_trace:
            np.save(pfx + ".rx.npy", np.array(self.rx_trace, float))
        jb = self.last_jb
        if jb is not None and jb.trace:
            np.save(pfx + ".pull.npy", np.array(jb.trace, float))

    # ---- control
    def hello_payload(self):
        a = self.args
        return json.dumps({"name": self.name, "role": self.role, "stream": self.my_stream,
                           "send": None if not self.sender else {
                               "codec": a.codec, "frame_ms": a.frame_ms, "ch": self.sender.ch}}).encode()

    def send_ctrl(self, ptype, obj):
        self.t.send(self.sealer.seal(ptype, json.dumps(obj).encode() if not isinstance(obj, bytes) else obj))

    def rx_loop(self):
        while not self.stop_ev.is_set():
            try:
                data, addr = self.t.recv()
            except OSError:
                if self.stop_ev.is_set():
                    return
                if self.t.kind == "tcp" or self.t.sock.fileno() < 0:
                    log("transport closed")
                    self.stop_ev.set()
                    return
                continue
            now = mono()
            p = self.opener.open(data, now)
            if p is None:
                continue
            if p.stream == self.my_stream:  # would mean a reflected packet or a collision
                continue
            self.last_rx = now
            if p.ptype == M.MEDIA:
                if p.stream != self.peer_stream:
                    continue
                self.last_media = now
                if self.rx_trace is not None:
                    self.rx_trace.append((p.ts, now))
                jb = self.slot.jb
                if jb is None:
                    a = self.args
                    jb = M.JitterBuffer(p.codec, p.channels, p.nsamp, min_ms=a.jb_min, max_ms=a.jb_max,
                                        fixed_ms=a.jb_fixed, margin_ms=a.jb_margin, max_ppm=a.max_ppm,
                                        drift=not a.no_drift, fec=a.fec, catchup_ppm=a.catchup_ppm,
                                        drop_ms=a.drop_ms, peak_hold_s=a.jb_peak_hold, peak_count=a.jb_peak_count)
                    if a.pkt_log:
                        jb.trace = []
                    self.slot.jb = self.last_jb = jb
                jb.push(p)
            elif p.ptype == M.HELLO:
                h = json.loads(p.payload)
                if self.role == "server":
                    if addr != self.t.peer or p.stream != self.peer_stream:
                        log(json.dumps({"event": "peer", "addr": str(addr), "name": h.get("name"), "stream": p.stream}))
                        self.t.peer = addr
                        self.slot.jb = None
                    self.send_ctrl(M.HELLO, self.hello_payload())
                if p.stream != self.peer_stream:
                    self.slot.jb = None
                self.peer_stream, self.peer_hello = p.stream, h
                if self.sender:
                    self.sender.enabled = True
                self.connected.set()
            elif p.ptype == M.PING:
                t1 = now
                q = json.loads(p.payload)
                self.send_ctrl(M.PONG, {"t0": q["t0"], "t1": t1, "t2": mono()})
            elif p.ptype == M.PONG:
                q = json.loads(p.payload)
                rtt = (now - q["t0"]) - (q["t2"] - q["t1"])
                off = ((q["t1"] - q["t0"]) + (q["t2"] - now)) / 2
                self.pings.append((now, rtt, off))
            elif p.ptype == M.STATS:
                q = json.loads(p.payload)
                if q.get("final"):
                    self.remote_summary = q
                else:
                    self.remote_stats.append(q)
                    if self.out:
                        self.out.write(json.dumps(q) + "\n")
            elif p.ptype == M.RR:
                self.rr_rx += 1
            elif p.ptype == M.BYE:
                if self.role == "server":
                    log(json.dumps({"event": "bye"}))
                    self.t.peer = None
                    self.peer_stream = None
                    self.slot.jb = None
                    if self.sender:
                        self.sender.enabled = False
                        self.sender.first = True
                    self.connected.clear()
                else:
                    self.stop_ev.set()

    def local_stats(self):
        c1 = (time.process_time(), mono())
        cpu = 100 * (c1[0] - self.cpu0[0]) / max(1e-6, c1[1] - self.cpu0[1])
        self.cpu0 = c1
        d = {"side": self.role, "t": round(mono(), 3), "cpu_pct": round(cpu, 1)}
        if self.slot.jb:
            d["rx"] = self.slot.jb.snapshot()
        if self.sender:
            d["tx"] = self.sender.snapshot()
        if self.pings:
            r = [x[1] * 1000 for x in self.pings[-5:]]
            d["rtt_ms"] = round(float(np.median(r)), 3)
        if self.rr_sent or self.rr_rx:
            d["rr_tx"], d["rr_rx"] = self.rr_sent, self.rr_rx
        d["auth_rejected"] = self.opener.rejected
        d["replayed"] = self.opener.replayed
        return d

    def summary(self):
        d = {"side": self.role}
        jb = self.slot.jb
        if jb is not None:
            d["drift_env_ppm"] = None if jb.drift_estimate() is None else round(jb.drift_estimate(), 2)
            ph = np.array(jb.ppm_hist[-int(60 * RATE / max(1, jb.block)):] or [0])
            d["ppm_last60s_mean"] = round(float(ph.mean()), 1)
            d["ppm_i"] = round(jb.integ, 1)
        if self.analyzer:
            d.update(self.analyzer.summary())
        if isinstance(self._sink, SdSink) and self._sink.dac:
            d["sd_out_dac_ms"] = round(float(np.median(self._sink.dac)) * 1000, 2)
        return d


def build_io(sess, args, src_spec, sink_spec, ch_src, ch_sink):
    block = int(round(args.block_ms * RATE / 1000))
    src = None
    if src_spec and src_spec != "none":
        if src_spec.split(":")[0] in ("tone", "clicks", "silence", "voice"):
            src = SynthSource(src_spec, ch_src)
        elif src_spec == "stdin" or src_spec.startswith("cmd:"):
            src = PipeSource(src_spec, ch_src)
        elif src_spec.startswith("sd:"):
            src = SdSource(src_spec, ch_src, block)
        elif src_spec == "echo":
            src = EchoSource()
        else:
            raise SystemExit(f"bad source {src_spec}")
    sink = None
    analyzer = None
    if sink_spec and sink_spec != "none":
        if sink_spec.startswith("analyze"):
            arg = sink_spec.split(":")[1] if ":" in sink_spec else None
            tone = VOICE_F0 if arg == "voice" else (float(arg) if arg else None)
            clk = src if isinstance(src, SynthSource) and src.kind == "clicks" else None
            analyzer = Analyzer(clk, tone)
            if arg == "voice":
                analyzer.harm = 12
            sink = PacedSink("null", sess.slot, block, analyzer=analyzer)
        elif sink_spec.startswith("sd:"):
            sink = SdSink(sink_spec, sess.slot, block)
        elif sink_spec == "echo":
            sink = PacedSink("null", sess.slot, block, echo=src)
        else:
            sink = PacedSink(sink_spec, sess.slot, block)
    sess.analyzer = analyzer
    sess._sink = sink
    return src, sink


def run_stats(sess, args, out, remote=False, stop=None, final_hook=None):
    """Every second: local stats -> file/stderr; server also sends them to the client."""
    while not (stop or sess.stop_ev).wait(1.0):
        s = sess.local_stats()
        if remote and sess.connected.is_set():
            sess.send_ctrl(M.STATS, s)
        sess.stats_log.append(s)
        if out:
            out.write(json.dumps(s) + "\n")
            out.flush()
        if not args.quiet:
            rx = s.get("rx", {})
            log(f"[{s['side']}] cpu {s['cpu_pct']}% rtt {s.get('rtt_ms')} "
                f"rx {rx.get('rx', 0)} lvl {rx.get('level_ms')} tgt {rx.get('target_ms')} jit {rx.get('jitter_ms')} "
                f"ppm {rx.get('ppm')} lost {rx.get('lost', 0)} fec {rx.get('fec', 0)} plc {rx.get('plc', 0)} "
                f"und {rx.get('underrun', 0)} drop {rx.get('drop', 0)} late {rx.get('late', 0)}")


def aggregate(stats, skip=3):
    """Totals and means over a list of per-second stats dicts (first `skip` s ignored)."""
    rx = [s["rx"] for s in stats[skip:] if "rx" in s]
    tx = [s["tx"] for s in stats[skip:] if "tx" in s]
    d = {"seconds": len(rx)}
    if rx:
        for k in ("rx", "lbrr", "lost", "fec", "plc", "expand", "underrun", "rebuffer", "drop", "flushed", "late", "dup", "rx_bytes"):
            d[k] = sum(r.get(k, 0) for r in rx)
        for k in ("level_ms", "target_ms", "jitter_ms", "dec_us"):
            d[k + "_mean"] = round(float(np.mean([r[k] for r in rx])), 2)
        d["level_min_ms"] = round(min(r["level_min_ms"] for r in rx), 2)
        d["level_max_ms"] = round(max(r["level_max_ms"] for r in rx), 2)
        d["rx_kbps"] = round(d["rx_bytes"] * 8 / 1000 / max(1, len(rx)), 1)
    if tx:
        d["tx_kbps"] = round(sum(t["tx_bytes"] for t in tx) * 8 / 1000 / len(tx), 1)
        d["enc_us_mean"] = round(float(np.mean([t["enc_us"] for t in tx])), 1)
        d["inj_drop"] = sum(t["inj_drop"] for t in tx)
    cpu = [s["cpu_pct"] for s in stats[skip:]]
    if cpu:
        d["cpu_pct_mean"] = round(float(np.mean(cpu)), 1)
    return d


def load_key(path):
    raw = open(path, "rb").read().strip()
    key = bytes.fromhex(raw.decode())
    if len(key) != 32:
        raise SystemExit("key file must hold 32 bytes as hex")
    return key


def cmd_keygen(a):
    with open(a.path, "w") as f:
        f.write(os.urandom(32).hex() + "\n")
    log(f"wrote {a.path}")


def channels_for(spec, default):
    return default


def cmd_serve(a):
    key = load_key(a.key_file)
    if a.transport == "tcp":
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind((a.bind, a.port))
        ls.listen(1)
        log(json.dumps({"event": "listening", "tcp": a.port}))
        conn, addr = ls.accept()
        tr = TcpStream(conn, addr)
    else:
        tr = UdpServer(a.bind, a.port)
        log(json.dumps({"event": "listening", "udp": a.port}))
        if a.punch:
            # Without an inbound rule: send first towards the client's address and port (which
            # the link control would carry), so the PC's stateful firewall admits the replies.
            h, pt = a.punch.rsplit(":", 1)

            def punch():
                t0 = mono()
                while tr.peer is None and mono() - t0 < 30:
                    tr.sock.sendto(os.urandom(8), (h, int(pt)))
                    time.sleep(0.2)
            threading.Thread(target=punch, daemon=True).start()
    serve_on(tr, key, a)


def serve_on(tr, key, a, stop=None):
    sess = Session(tr, key, a, "server", a.channels if a.source != "none" else 0, a.sink_channels or a.channels, socket.gethostname())
    src, sink = build_io(sess, a, a.source, a.sink, a.channels, a.sink_channels or a.channels)
    if src is not None and sess.sender:
        src.start(sess.sender.feed)
    if sink:
        sink.start()
    threading.Thread(target=sess.rx_loop, daemon=True, name="rx").start()
    threading.Thread(target=sess.rr_loop, daemon=True, name="rr").start()
    out = open(a.stats, "a") if a.stats else None
    st = threading.Thread(target=run_stats, args=(sess, a, out, True, stop), daemon=True)
    st.start()
    end = mono() + a.duration if a.duration else None
    try:
        while not (stop and stop.is_set()) and (end is None or mono() < end):
            time.sleep(0.2)
            if sess.connected.is_set() and mono() - sess.last_rx > 3.0:
                log(json.dumps({"event": "peer_timeout"}))
                sess.connected.clear()
                sess.t.peer = None
                sess.slot.jb = None
                if sess.sender:
                    sess.sender.enabled = False
                    sess.sender.first = True
            # send the summary periodically so the client always has a recent one
            if sess.connected.is_set() and int(mono()) % 5 == 0:
                s = sess.summary()
                s["final"] = True
                sess.send_ctrl(M.STATS, s)
                time.sleep(0.8)
    except KeyboardInterrupt:
        pass
    finally:
        sess.stop_ev.set()
        if src:
            src.stop()
        if sink:
            sink.stop()
        sess.dump_trace()


def client_run(tr, key, a, stop_after=None):
    sess = Session(tr, key, a, "client", a.channels if a.source != "none" else 0, a.sink_channels or a.channels, socket.gethostname())
    src, sink = build_io(sess, a, a.source, a.sink, a.channels, a.sink_channels or a.channels)
    threading.Thread(target=sess.rx_loop, daemon=True, name="rx").start()
    t0 = mono()
    while not sess.connected.wait(0.2):
        sess.send_ctrl(M.HELLO, sess.hello_payload())
        if mono() - t0 > a.connect_timeout:
            return None, sess
    threading.Thread(target=sess.rr_loop, daemon=True, name="rr").start()
    log(json.dumps({"event": "connected", "transport": tr.kind, "after_ms": round((mono() - t0) * 1000, 1),
                    "peer": sess.peer_hello}))
    if src is not None:
        src.start(sess.sender.feed)
    if sink:
        sink.start()
    out = open(a.stats, "a") if a.stats else None
    sess.out = out
    if out:
        out.write(json.dumps({"event": "start", "args": vars(a), "transport": tr.kind}) + "\n")
    threading.Thread(target=run_stats, args=(sess, a, out), daemon=True).start()
    end = mono() + a.duration if a.duration else None
    nping = 0
    try:
        while not sess.stop_ev.is_set() and (end is None or mono() < end):
            sess.send_ctrl(M.PING, {"t0": mono()})
            nping += 1
            if nping % 3 == 0:
                sess.send_ctrl(M.HELLO, sess.hello_payload())  # keepalive / NAT refresh
            sess.stop_ev.wait(a.ping_s)
    except KeyboardInterrupt:
        pass
    if sess.analyzer:
        sess.analyzer.frozen = True  # ignore the teardown
    time.sleep(0.5)
    for _ in range(3):
        sess.send_ctrl(M.BYE, b"{}")
    sess.stop_ev.set()
    if src:
        src.stop()
    if sink:
        sink.stop()
    sess.dump_trace()
    res = {"event": "summary", "local": sess.summary(), "remote": sess.remote_summary,
           "local_agg": aggregate(sess.stats_log), "remote_agg": aggregate(sess.remote_stats)}
    if sess.pings:
        P = np.array(sess.pings)
        r = P[:, 1] * 1000
        res["rtt_ms"] = {"n": len(r), "min": round(float(r.min()), 3), "p50": round(float(np.median(r)), 3),
                         "p95": round(float(np.quantile(r, 0.95)), 3), "max": round(float(r.max()), 3)}
        if len(P) > 60:
            # clock-rate difference from the offset of the best (lowest-rtt) pings per 10 s bucket
            b = {}
            for t, rtt, off in sess.pings:
                k = int(t // 10)
                if k not in b or rtt < b[k][1]:
                    b[k] = (t, rtt, off)
            T = np.array([v[0] for v in b.values()])
            O = np.array([v[2] for v in b.values()])
            if len(T) > 5:
                res["clock_offset_slope_ppm"] = round(float(np.polyfit(T, O, 1)[0] * 1e6), 2)
    if out:
        out.write(json.dumps(res) + "\n")
        out.close()
    print(json.dumps(res), flush=True)
    return res, sess


def cmd_send(a):
    key = load_key(a.key_file)
    host, port = a.to.rsplit(":", 1)
    port = int(port)
    if a.transport in ("udp", "auto"):
        tr = UdpClient(host, port, a.local_port)
        a2 = argparse.Namespace(**vars(a))
        if a.transport == "auto":
            a2.connect_timeout = 2.0
        res, sess = client_run(tr, key, a2)
        if res is not None or a.transport == "udp":
            if res is None:
                log("no answer over UDP")
                sys.exit(2)
            return
        log(json.dumps({"event": "udp_failed_fallback_tcp"}))
        sess.stop_ev.set()  # end the UDP session's receive thread before closing its socket
        tr.close()
    s = socket.create_connection((host, port), timeout=5)
    s.settimeout(None)
    res, _ = client_run(TcpStream(s, (host, port)), key, a)
    if res is None:
        sys.exit(2)


def cmd_devices(a):
    import sounddevice as sd
    apis = sd.query_hostapis()
    for i, d in enumerate(sd.query_devices()):
        if a.filter.lower() in d["name"].lower():
            print(i, repr(d["name"]), apis[d["hostapi"]]["name"], "in", d["max_input_channels"],
                  "out", d["max_output_channels"], int(d["default_samplerate"]),
                  "lat_lo_ms", round(d["default_low_output_latency"] * 1000, 1), round(d["default_low_input_latency"] * 1000, 1))


def cmd_local(a):
    """Both ends in one process over a datagram socketpair: codec/jitter/loss experiments
    without network or firewall. Client options as usual; server side uses --peer-source /
    --peer-sink."""
    key = load_key(a.key_file)
    if hasattr(socket, "AF_UNIX") and sys.platform != "win32":
        s1, s2 = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    else:  # Windows has no AF_UNIX datagrams: two loopback UDP sockets wired to each other
        s1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s1.bind(("127.0.0.1", 0))
        s2.bind(("127.0.0.1", 0))
        s1.connect(s2.getsockname())
        s2.connect(s1.getsockname())
    b = argparse.Namespace(**vars(a))
    b.source, b.sink = a.peer_source, a.peer_sink
    b.loss, b.jitter_ms = a.peer_loss, a.peer_jitter_ms
    b.stats = None
    b.quiet = True
    stop = threading.Event()
    srv = PairEnd(s2)
    th = threading.Thread(target=serve_on, args=(srv, key, b, stop), daemon=True)
    th.start()
    client_run(PairEnd(s1), key, a)
    stop.set()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("path")
    dv = sub.add_parser("devices")
    dv.add_argument("filter", nargs="?", default="")

    def common(p):
        p.add_argument("--key-file", required=True)
        p.add_argument("--source", default="none")
        p.add_argument("--sink", default="none")
        p.add_argument("--channels", type=int, default=1, help="source channels")
        p.add_argument("--sink-channels", type=int, default=0, help="sink channels (default = --channels)")
        p.add_argument("--codec", choices=["opus", "pcm"], default="opus")
        p.add_argument("--frame-ms", type=float, default=10.0)
        p.add_argument("--bitrate", type=int, default=0)
        p.add_argument("--app", choices=["voip", "audio", "lowdelay"], default=None)
        p.add_argument("--fec", action=argparse.BooleanOptionalAction, default=True)
        p.add_argument("--loss-hint", type=int, default=10, help="OPUS_SET_PACKET_LOSS_PERC")
        p.add_argument("--dtx", action="store_true")
        p.add_argument("--complexity", type=int, default=9)
        p.add_argument("--block-ms", type=float, default=10.0, help="sink pull block")
        p.add_argument("--jb-min", type=float, default=5.0)
        p.add_argument("--jb-max", type=float, default=120.0)
        p.add_argument("--jb-fixed", type=float, default=None)
        p.add_argument("--jb-margin", type=float, default=2.0)
        p.add_argument("--max-ppm", type=float, default=500.0)
        p.add_argument("--no-drift", action="store_true")
        p.add_argument("--catchup-ppm", type=float, default=2000.0,
                       help="max resampling speed-up to absorb a late burst (500 = old behaviour)")
        p.add_argument("--drop-ms", type=float, default=30.0,
                       help="drop a non-silent frame only after 2 s this far above target (old: 15)")
        p.add_argument("--jb-peak-hold", type=float, default=10.0,
                       help="spike-aware target: after 2 delay spikes > 15 ms within N s, provision "
                            "for the highest until N s after the last (0: off, old behaviour)")
        p.add_argument("--jb-peak-count", type=int, default=2)
        p.add_argument("--loss", type=float, default=0.0, help="inject loss, percent")
        p.add_argument("--burst", type=float, default=1.0, help="mean loss burst length (Gilbert)")
        p.add_argument("--jitter-ms", type=float, default=0.0, help="inject uniform 0..J ms delay")
        p.add_argument("--seed", type=int, default=1)
        p.add_argument("--duration", type=float, default=0)
        p.add_argument("--stats", default=None, help="JSON lines file")
        p.add_argument("--quiet", action="store_true")
        p.add_argument("--ping-s", type=float, default=1.0)
        p.add_argument("--connect-timeout", type=float, default=10.0)
        p.add_argument("--transport", choices=["udp", "tcp", "auto"], default="udp")
        p.add_argument("--rr-ms", type=float, default=10.0,
                       help="receiver reports every N ms while receiving and not sending (0: off)")
        p.add_argument("--pkt-log", default=None, help="PREFIX: per-packet send/receive/pull traces (.npy)")

    s = sub.add_parser("serve")
    common(s)
    s.add_argument("--port", type=int, default=24823)
    s.add_argument("--bind", default="0.0.0.0")
    s.add_argument("--punch", default=None, help="HOST:PORT to send to first (no inbound rule)")
    c = sub.add_parser("send")
    common(c)
    c.add_argument("--to", required=True)
    c.add_argument("--local-port", type=int, default=0)
    l = sub.add_parser("local")
    common(l)
    l.add_argument("--peer-source", default="none")
    l.add_argument("--peer-sink", default="none")
    l.add_argument("--peer-loss", type=float, default=0.0)
    l.add_argument("--peer-jitter-ms", type=float, default=0.0)
    a = ap.parse_args()
    {"keygen": cmd_keygen, "devices": cmd_devices, "serve": cmd_serve, "send": cmd_send, "local": cmd_local}[a.cmd](a)


if __name__ == "__main__":
    main()
