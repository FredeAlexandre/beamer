"""Video-over-UDP probe (camera C3): synthetic H.264 frames + audio-like packets, Mac -> PC.

The Mac (`send`) plays the role of the camera sender: 30 fps "access units" with realistic sizes
(model or a trace of real frame sizes), cut into MTU-safe packets exactly as the proposed engine
would (24 B engine header + 12 B video header + fragment + 16 B AEAD tag <= --mtu bytes of UDP
payload), paced or in bursts, plus an audio-like stream (one 80 B packet every 10 ms) on the same
socket. The PC (`recv`) logs every arrival and, like the engine's receiver, sends a 46 B receiver
report every 10 ms (keeps its power-saving Wi-Fi radio awake, audio W3). Pings (UDP, 5/s) give the
clock offset. No crypto: packets have the sealed sizes, payload bytes are padding (the Wi-Fi does
not care what is inside; AEAD CPU is measured by the engine tests, not here).

    PC : python vprobe.py recv --port 24850 --punch MAC:40150 --duration 640 --out pc.npz
    Mac: python vprobe.py send --to PC:24850 --local-port 40150 --duration 600 --out mac.npz \
             --rate 2.5e6 --fps 30 --key-every 60 --key-kb 80 --pace 2.5 [--trace sizes.csv]

TCP mode (--tcp HOST:PORT on send, --tcp-port on recv): the media go length-prefixed over one TCP
connection (through `ssh -L`, a stand-in for the Beamer link), pings stay on UDP for the clock.

The PC never listens for unsolicited traffic: it first sends from its port to the Mac's (--punch),
so Windows' stateful UDP filter admits the Mac's datagrams; the Mac sends first from a connected
socket (no macOS firewall dialog). Same scheme as dev/audio/proto/transport (no firewall rule).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import random
import socket
import struct
import sys
import threading
import time

import numpy as np

clock = time.perf_counter  # QPC on Windows, mach_absolute_time on macOS

MAGIC = b"VP"
# magic, kind, flags, seq, frame, frag, nfrag, t_send, t_frame
HDR = struct.Struct("!2sBBIIHHdd")
K_VIDEO, K_AUDIO, K_RR, K_PING, K_PONG, K_HELLO, K_BYE = range(1, 8)
F_KEY = 1
ENGINE_HDR, VIDEO_HDR, TAG = 24, 12, 16
OVERHEAD = ENGINE_HDR + VIDEO_HDR + TAG  # 52 B per video packet on the wire (UDP payload)
AUDIO_SIZE = 80   # 24 hdr + 40 B Opus (32 kb/s, 10 ms) + 16 tag
RR_SIZE = 46      # as the engine's receiver report


# ----------------------------------------------------------------------------- frame sizes

class FrameModel:
    """Synthetic H.264 CBR-ish sizes: keyframes every `key_every` frames (0 = only the first), size
    ~ N(key_kb, 25 %) clipped to [key_kb/2, 1.5 key_kb]; P-frames lognormal (CV ~0.35) with the mean
    that makes the long-run average equal `rate`; after a keyframe the next few P-frames are smaller
    (VBV drain), as VideoToolbox does in real-time mode."""

    def __init__(self, rate, fps, key_every, key_kb, seed=1):
        self.rng = random.Random(seed)
        self.fps, self.key_every, self.key_b = fps, key_every, key_kb * 1000
        per_frame = rate / 8 / fps
        if key_every > 0:
            self.p_mean = max(1500.0, (per_frame * key_every - self.key_b) / (key_every - 1))
        else:
            self.p_mean = per_frame
        self.sigma = 0.34
        self.force_key = False
        self.i = 0

    def next(self):
        i = self.i
        self.i += 1
        key = i == 0 or self.force_key or (self.key_every > 0 and i % self.key_every == 0)
        self.force_key = False
        if key:
            b = self.rng.gauss(self.key_b, 0.25 * self.key_b)
            return int(min(max(b, self.key_b / 2), self.key_b * 1.5)), True
        mu = math.log(self.p_mean) - self.sigma ** 2 / 2
        return int(max(300, self.rng.lognormvariate(mu, self.sigma))), False


class TraceModel:
    """Frame sizes from a CSV (frame,bytes,key[,...]) written by a real encoder; loops."""

    def __init__(self, path, scale=1.0):
        rows = []
        with open(path) as f:
            for line in f:
                p = line.strip().split(",")
                if not p or not p[0].strip().lstrip("-").isdigit():
                    continue
                rows.append((int(float(p[1]) * scale), p[2].strip() in ("1", "true", "True", "key")))
        if not rows:
            raise SystemExit(f"no rows in {path}")
        self.rows, self.i = rows, 0

    def next(self):
        r = self.rows[self.i % len(self.rows)]
        self.i += 1
        return r


# ----------------------------------------------------------------------------- sender (Mac)

class Sender:
    def __init__(self, a):
        self.a = a
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 1 << 20)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
        self.s.bind(("0.0.0.0", a.local_port))
        host, port = a.to.rsplit(":", 1)
        self.peer = (host, int(port))
        self.s.connect(self.peer)
        self.s.settimeout(0.2)
        self.tcp = None
        self.stop = threading.Event()
        self.vlog = []   # seq, frame, frag, nfrag, flags, size, t_frame, t_send
        self.alog = []   # seq, t_sched, t_send
        self.rrlog = []  # pc_t_send, mac_t_arrival, hi_seq, n_rx
        self.pings = []  # t0, t1, t2, t3
        self.frames = []  # frame, size, key, t_frame, npk
        self.got_hello = threading.Event()
        self.send_err = 0

    def tx(self, b):
        try:
            self.s.send(b)
        except OSError:
            self.send_err += 1

    def tx_media(self, b):
        if self.tcp is None:
            self.tx(b)
        else:
            self.tcp.sendall(struct.pack("!H", len(b)) + b)

    def receiver(self):
        while not self.stop.is_set():
            try:
                b = self.s.recv(65535)
            except (socket.timeout, ConnectionResetError):
                continue
            except OSError:
                return
            ta = clock()
            if len(b) < HDR.size or b[:2] != MAGIC:
                continue
            _, kind, flags, seq, frame, frag, nfrag, x, y = HDR.unpack_from(b)
            if kind == K_RR:
                self.rrlog.append((x, ta, seq, frame))
            elif kind == K_PONG:
                t1, t2 = struct.unpack_from("!dd", b, HDR.size)
                self.pings.append((x, t1, t2, ta))
            elif kind == K_HELLO:
                self.got_hello.set()

    def run(self):
        a = self.a
        rt = threading.Thread(target=self.receiver, daemon=True)
        rt.start()
        # hello until the PC answers (its punch may arrive first; either way it then knows us)
        t_end = clock() + 20
        while not self.got_hello.is_set() and clock() < t_end:
            self.tx(HDR.pack(MAGIC, K_HELLO, 0, 0, 0, 0, 0, clock(), 0) + b"\0" * 16)
            self.got_hello.wait(0.1)
        if not self.got_hello.is_set():
            raise SystemExit("no hello from the PC")
        if a.tcp:
            h, p = a.tcp.rsplit(":", 1)
            self.tcp = socket.create_connection((h, int(p)), timeout=5)
            self.tcp.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.tcp.settimeout(None)
        model = TraceModel(a.trace, a.trace_scale) if a.trace else FrameModel(a.rate, a.fps, a.key_every, a.key_kb, a.seed)
        frag_max = a.mtu - OVERHEAD
        fiv = 1.0 / a.fps
        pace_bps = a.pace_bps or (a.pace * a.rate if a.pace > 0 else 0.0)
        bucket_cap = a.burst_pk * a.mtu * 8.0  # token bucket (bits): bursts of up to burst_pk packets
        tokens, t_tok = bucket_cap, None
        t0 = clock() + 0.2
        t_stop = t0 + a.duration
        vseq = aseq = 0
        frame = 0
        next_frame = t0
        next_audio = t0 if a.audio else float("inf")
        next_ping = t0
        q = []          # pending video packets: (due, bytes, logrow)
        pace_free = t0  # pacer: time the previous packet finished "on the pacer's wire"
        pad = bytes(2048)
        while True:
            now = clock()
            if now >= t_stop and not q:
                break
            if a.video and now >= next_frame and next_frame < t_stop:
                size, key = model.next()
                npk = max(1, math.ceil(size / frag_max))
                tf = next_frame
                self.frames.append((frame, size, key, tf, npk))
                left = size
                for k in range(npk):
                    payload = min(frag_max, left)
                    left -= payload
                    plen = payload + OVERHEAD
                    if pace_bps and bucket_cap:
                        # token bucket in virtual time: refill at pace_bps, capacity bucket_cap
                        due = max(pace_free, tf)
                        if t_tok is None:
                            t_tok = due
                        tokens = min(bucket_cap, tokens + (due - t_tok) * pace_bps)
                        t_tok = due
                        if tokens < plen * 8:
                            due += (plen * 8 - tokens) / pace_bps
                            tokens = plen * 8
                            t_tok = due
                        tokens -= plen * 8
                        pace_free = due
                    elif pace_bps:
                        due = max(pace_free, tf)
                        pace_free = due + plen * 8 / pace_bps
                    else:
                        due = tf
                    vseq += 1
                    hdr = HDR.pack(MAGIC, K_VIDEO, F_KEY if key else 0, vseq, frame, k, npk, 0.0, tf)
                    q.append([due, hdr, plen, [vseq, frame, k, npk, 1 if key else 0, plen, tf, 0.0]])
                frame += 1
                next_frame += fiv
            if now >= next_audio:
                if next_audio < t_stop:
                    aseq += 1
                    ts = clock()
                    self.tx_media(HDR.pack(MAGIC, K_AUDIO, 0, aseq, 0, 0, 0, ts, next_audio) + pad[: AUDIO_SIZE - HDR.size])
                    self.alog.append((aseq, next_audio, ts))
                    next_audio += 0.010
                    if clock() - next_audio > 0.1:  # we fell behind (sleep): resync, keep cadence
                        next_audio = clock()
                else:
                    next_audio = float("inf")
            if now >= next_ping and now < t_stop:
                self.tx(HDR.pack(MAGIC, K_PING, 0, 0, 0, 0, 0, clock(), 0) + b"\0" * 16)
                next_ping += a.ping_ms / 1000
            # due video packets (pacer granularity = this loop's wakeups, ~1 ms)
            while q and q[0][0] <= clock():
                due, hdr, plen, row = q.pop(0)
                ts = clock()
                b = bytearray(hdr)
                struct.pack_into("!d", b, 16, ts)  # t_send (offset of HDR field 8)
                self.tx_media(bytes(b) + pad[: plen - HDR.size])
                row[7] = ts
                self.vlog.append(row)
            nxt = min(next_frame if a.video and next_frame < t_stop else float("inf"), next_audio,
                      next_ping if next_ping < t_stop else float("inf"), q[0][0] if q else float("inf"))
            if nxt == float("inf"):
                if not q:
                    break
                nxt = clock() + 0.001
            d = nxt - clock()
            if d > 0:
                time.sleep(min(d, 0.005))
        for _ in range(5):
            self.tx(HDR.pack(MAGIC, K_BYE, 0, 0, 0, 0, 0, clock(), 0) + b"\0" * 16)
            time.sleep(0.05)
        time.sleep(0.5)
        self.stop.set()
        if self.tcp:
            self.tcp.close()
        np.savez_compressed(
            a.out,
            vlog=np.array(self.vlog, dtype=np.float64).reshape(-1, 8),
            alog=np.array(self.alog, dtype=np.float64).reshape(-1, 3),
            rrlog=np.array(self.rrlog, dtype=np.float64).reshape(-1, 4),
            pings=np.array(self.pings, dtype=np.float64).reshape(-1, 4),
            frames=np.array(self.frames, dtype=np.float64).reshape(-1, 5),
            meta=json.dumps({**vars(a), "send_err": self.send_err, "role": "mac", "platform": sys.platform}),
        )
        print(json.dumps({"frames": len(self.frames), "video_pk": len(self.vlog), "audio_pk": len(self.alog),
                          "rr": len(self.rrlog), "pings": len(self.pings), "send_err": self.send_err}))


# ----------------------------------------------------------------------------- receiver (PC)

class Receiver:
    def __init__(self, a):
        self.a = a
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        if sys.platform == "win32":  # no WSAECONNRESET on ICMP port unreachable
            try:
                self.s.ioctl(socket.SIO_UDP_CONNRESET, False)
            except (AttributeError, OSError):
                pass
        self.s.bind(("0.0.0.0", a.port))
        self.s.settimeout(0.2)
        self.peer = None
        h, p = a.punch.rsplit(":", 1)
        self.punch = (h, int(p))
        self.rx = []  # kind, seq, frame, frag, nfrag, flags, t_send(mac), t_frame(mac), t_arrival(pc), via_tcp
        self.lock = threading.Lock()
        self.hi_seq = 0
        self.n_rx = 0
        self.stop = threading.Event()
        self.bye = threading.Event()
        self.rr_tx = 0

    def tx(self, b, to=None):
        try:
            self.s.sendto(b, to or self.peer)
        except OSError:
            pass

    def handle(self, b, ta, via_tcp):
        if len(b) < HDR.size or b[:2] != MAGIC:
            return
        _, kind, flags, seq, frame, frag, nfrag, x, y = HDR.unpack_from(b)
        if kind in (K_VIDEO, K_AUDIO):
            self.rx.append((kind, seq, frame, frag, nfrag, flags, x, y, ta, via_tcp))
            if kind == K_VIDEO:
                self.n_rx += 1
                self.hi_seq = max(self.hi_seq, seq)

    def rr_loop(self):
        iv = self.a.rr_ms / 1000
        nxt = clock()
        pad = bytes(64)
        while not self.stop.is_set():
            nxt += iv
            d = nxt - clock()
            if d > 0:
                time.sleep(d)
            else:
                nxt = clock()
            if self.peer is not None and self.a.rr_ms > 0:
                self.tx(HDR.pack(MAGIC, K_RR, 0, self.hi_seq, self.n_rx, 0, 0, clock(), 0) + pad[: RR_SIZE - HDR.size])
                self.rr_tx += 1

    def tcp_loop(self):
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", self.a.tcp_port))
        ls.listen(1)
        ls.settimeout(1.0)
        while not self.stop.is_set():
            try:
                c, _ = ls.accept()
            except socket.timeout:
                continue
            c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c.settimeout(1.0)
            buf = b""
            while not self.stop.is_set():
                try:
                    d = c.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    break
                if not d:
                    break
                ta = clock()
                buf += d
                while len(buf) >= 2:
                    n = struct.unpack_from("!H", buf)[0]
                    if len(buf) < 2 + n:
                        break
                    self.handle(buf[2 : 2 + n], ta, 1)
                    buf = buf[2 + n :]
            c.close()
        ls.close()

    def run(self):
        a = self.a
        threading.Thread(target=self.rr_loop, daemon=True).start()
        if a.tcp_port:
            threading.Thread(target=self.tcp_loop, daemon=True).start()
        t_end = clock() + a.duration
        next_punch = 0.0
        while clock() < t_end and not self.bye.is_set():
            if self.peer is None and clock() >= next_punch:  # punch: open WFP's UDP state towards the Mac
                self.tx(HDR.pack(MAGIC, K_HELLO, 0, 0, 0, 0, 0, clock(), 0) + b"\0" * 16, self.punch)
                next_punch = clock() + 0.1
            try:
                b, addr = self.s.recvfrom(65535)
            except (socket.timeout, ConnectionResetError):
                continue
            ta = clock()
            if len(b) < HDR.size or b[:2] != MAGIC:
                continue
            kind = b[2]
            if self.peer is None:
                self.peer = addr
            if kind == K_PING:
                t0 = HDR.unpack_from(b)[7]
                self.tx(HDR.pack(MAGIC, K_PONG, 0, 0, 0, 0, 0, t0, 0) + struct.pack("!dd", ta, clock()))
            elif kind == K_HELLO:
                self.tx(HDR.pack(MAGIC, K_HELLO, 0, 0, 0, 0, 0, clock(), 0) + b"\0" * 16)
            elif kind == K_BYE:
                self.bye.set()
            else:
                self.handle(b, ta, 0)
        self.stop.set()
        time.sleep(1.2)
        np.savez_compressed(a.out, rx=np.array(self.rx, dtype=np.float64).reshape(-1, 10),
                            meta=json.dumps({**vars(a), "rr_tx": self.rr_tx, "role": "pc", "platform": sys.platform}))
        print(json.dumps({"rx": len(self.rx), "rr_tx": self.rr_tx, "peer": str(self.peer)}))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("send")
    s.add_argument("--to", required=True)
    s.add_argument("--local-port", type=int, default=40150)
    s.add_argument("--duration", type=float, default=600)
    s.add_argument("--out", required=True)
    s.add_argument("--rate", type=float, default=2.5e6)
    s.add_argument("--fps", type=float, default=30)
    s.add_argument("--key-every", type=int, default=60)
    s.add_argument("--key-kb", type=float, default=80)
    s.add_argument("--pace", type=float, default=2.5, help="pacing rate as a multiple of --rate; 0 = burst")
    s.add_argument("--pace-bps", type=float, default=0, help="absolute pacing rate (overrides --pace)")
    s.add_argument("--burst-pk", type=int, default=0, help="with --pace-bps: token bucket of this many packets")
    s.add_argument("--mtu", type=int, default=1200, help="max UDP payload incl. engine header and tag")
    s.add_argument("--trace", default=None)
    s.add_argument("--trace-scale", type=float, default=1.0)
    s.add_argument("--seed", type=int, default=1)
    s.add_argument("--ping-ms", type=float, default=200)
    s.add_argument("--no-audio", dest="audio", action="store_false")
    s.add_argument("--no-video", dest="video", action="store_false")
    s.add_argument("--tcp", default=None, help="HOST:PORT: media over this TCP connection")
    r = sub.add_parser("recv")
    r.add_argument("--port", type=int, default=24850)
    r.add_argument("--punch", required=True)
    r.add_argument("--duration", type=float, default=640)
    r.add_argument("--out", required=True)
    r.add_argument("--rr-ms", type=float, default=10)
    r.add_argument("--tcp-port", type=int, default=0)
    a = ap.parse_args()
    (Sender if a.cmd == "send" else Receiver)(a).run()


if __name__ == "__main__":
    main()
