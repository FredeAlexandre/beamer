"""Beamer audio media plane prototype: packets, per-packet AEAD, codecs, jitter buffer, drift.

Packet (all big endian), 24-byte header used as AEAD associated data:

    0  u8   version (1)
    1  u8   ptype   0 media, 1 hello, 2 ping, 3 pong, 4 stats, 5 bye
    2  u8   flags   bit0 fec-capable, bit1 marker (stream (re)start), bit2 silence
    3  u8   codec   0 control(JSON), 1 opus, 2 pcm s16le
    4  u32  stream id (random per sender and per stream; distinct per direction)
    8  u64  seq     per-sender packet counter, every packet type; also the nonce counter
   16  u32  ts      sender timestamp in samples at 48 kHz of the first sample (media)
   20  u16  nsamp   samples per channel in this frame
   22  u8   channels
   23  u8   reserved
   24  ...  ChaCha20-Poly1305 (IETF) ciphertext || 16-byte tag

Nonce = stream id (4) || seq (8): unique as long as one key never sees the same stream id
twice (the stream key is random per audio_open in the real design; here a pre-shared key
is mixed with the stream id, see stream_key()). Receivers keep a 128-packet replay window.
Opus 20 ms mono 32 kb/s = 80 B payload -> 120 B UDP payload, far below the 1200 B budget.
"""
from __future__ import annotations

import collections
import hashlib
import math
import struct
import threading
import time

import numpy as np
from nacl import bindings as nb

RATE = 48000
VERSION = 1
HDR = struct.Struct("!BBBBIQIHBB")
HDR_LEN = HDR.size  # 24
TAG_LEN = 16
OVERHEAD_UDP = HDR_LEN + TAG_LEN + 8 + 20  # + UDP + IPv4 headers

MEDIA, HELLO, PING, PONG, STATS, BYE = range(6)
F_FEC, F_MARK, F_SILENCE = 1, 2, 4
C_CTRL, C_OPUS, C_PCM = 0, 1, 2
CODEC_NAMES = {C_OPUS: "opus", C_PCM: "pcm"}


def stream_key(master: bytes, stream_id: int) -> bytes:
    """Per-stream key. Production: a random 32-byte key carried in audio_open over the link."""
    return hashlib.blake2b(struct.pack("!I", stream_id) + b"beamer-audio-v1",
                           key=master, digest_size=32).digest()


class Sealer:
    def __init__(self, master: bytes, stream_id: int):
        self.stream_id = stream_id
        self.key = stream_key(master, stream_id)
        self.seq = 0
        self.lock = threading.Lock()

    def seal(self, ptype: int, payload: bytes, flags: int = 0, codec: int = C_CTRL,
             ts: int = 0, nsamp: int = 0, channels: int = 0) -> bytes:
        with self.lock:
            self.seq += 1
            seq = self.seq
        hdr = HDR.pack(VERSION, ptype, flags, codec, self.stream_id, seq, ts & 0xFFFFFFFF,
                       nsamp, channels, 0)
        nonce = struct.pack("!IQ", self.stream_id, seq)
        return hdr + nb.crypto_aead_chacha20poly1305_ietf_encrypt(payload, hdr, nonce, self.key)


class Packet:
    __slots__ = ("ptype", "flags", "codec", "stream", "seq", "ts", "nsamp", "channels",
                 "payload", "size", "arrival")


class Opener:
    """Authenticates packets of one or more peer streams, with a sliding replay window."""
    WINDOW = 128

    def __init__(self, master: bytes):
        self.master = master
        self.keys: dict[int, bytes] = {}
        self.win: dict[int, tuple[int, int]] = {}  # stream -> (highest seq, bitmask)
        self.rejected = 0
        self.replayed = 0

    def open(self, data: bytes, arrival: float) -> Packet | None:
        if len(data) < HDR_LEN + TAG_LEN:
            self.rejected += 1
            return None
        hdr = data[:HDR_LEN]
        ver, ptype, flags, codec, stream, seq, ts, nsamp, ch, _ = HDR.unpack(hdr)
        if ver != VERSION:
            self.rejected += 1
            return None
        key = self.keys.get(stream)
        if key is None:
            if len(self.keys) > 64:
                self.keys.clear()
            key = self.keys[stream] = stream_key(self.master, stream)
        hi, mask = self.win.get(stream, (0, 0))
        if seq + self.WINDOW <= hi or (seq <= hi and (mask >> (hi - seq)) & 1):
            self.replayed += 1
            return None
        try:
            pt = nb.crypto_aead_chacha20poly1305_ietf_decrypt(
                data[HDR_LEN:], hdr, struct.pack("!IQ", stream, seq), key)
        except Exception:
            self.rejected += 1
            return None
        if seq > hi:
            mask = ((mask << (seq - hi)) | 1) & ((1 << self.WINDOW) - 1)
            hi = seq
        else:
            mask |= 1 << (hi - seq)
        self.win[stream] = (hi, mask)
        p = Packet()
        p.ptype, p.flags, p.codec, p.stream, p.seq, p.ts = ptype, flags, codec, stream, seq, ts
        p.nsamp, p.channels, p.payload, p.size, p.arrival = nsamp, ch, pt, len(data), arrival
        return p


# ---------------------------------------------------------------- codecs

class OpusCodec:
    id = C_OPUS
    has_fec = True

    def __init__(self, channels: int, frame: int, bitrate: int = 32000, fec: bool = True,
                 loss_perc: int = 10, application: str = "voip", dtx: bool = False,
                 complexity: int = 9):
        import opus
        self.channels, self.frame, self.fec = channels, frame, fec
        self.enc_args = dict(channels=channels, application=application, bitrate=bitrate,
                             fec=fec, loss_perc=loss_perc, dtx=dtx, complexity=complexity)
        self._opus = opus

    def encoder(self):
        return self._opus.Encoder(**self.enc_args)

    @staticmethod
    def decoder(channels: int):
        import opus
        return OpusDecoder(opus.Decoder(channels=channels), channels)


class OpusDecoder:
    def __init__(self, dec, channels):
        self.dec, self.channels = dec, channels

    def decode(self, payload, n):
        return self.dec.decode(payload, n)

    def fec(self, next_payload, n):
        return self.dec.decode(next_payload, n, fec=True)

    def plc(self, n):
        return self.dec.decode(None, n)


class PcmCodec:
    """Raw s16le PCM, for comparison. Loss -> repeat-and-fade concealment."""
    id = C_PCM
    has_fec = False

    def __init__(self, channels: int, frame: int, **_):
        self.channels, self.frame = channels, frame

    def encoder(self):
        return PcmEncoder()

    @staticmethod
    def decoder(channels: int):
        return PcmDecoder(channels)


class PcmEncoder:
    def encode(self, pcm: np.ndarray) -> bytes:
        return (np.clip(pcm, -1, 1) * 32767).astype("<i2").tobytes()


class PcmDecoder:
    def __init__(self, channels):
        self.channels = channels
        self.last = None

    def decode(self, payload, n):
        x = np.frombuffer(payload, "<i2").astype(np.float32).reshape(-1, self.channels) / 32767
        self.last = x
        return x

    def fec(self, next_payload, n):
        return self.plc(n)

    def plc(self, n):
        if self.last is None or len(self.last) != n:
            return np.zeros((n, self.channels), np.float32)
        self.last = self.last * 0.5  # repeat last frame, fading
        return self.last


def decoder_for(codec: int, channels: int):
    return (OpusCodec if codec == C_OPUS else PcmCodec).decoder(channels)


# ---------------------------------------------------------------- resampler

class Resampler:
    """Variable-ratio windowed-sinc interpolator (16 taps, 512 phases, Kaiser beta 8).

    step = input samples consumed per output sample (1 + ppm*1e-6). Used only for gentle
    drift correction (|ppm| <= a few hundred), where it is transparent (see README)."""
    TAPS = 16
    PHASES = 512

    def __init__(self, channels: int):
        self.ch = channels
        half = self.TAPS // 2
        ph = np.arange(self.PHASES + 1)[:, None] / self.PHASES
        k = np.arange(-half + 1, half + 1)[None, :]  # taps relative to floor(pos)
        x = k - ph
        w = np.kaiser(self.TAPS * 64 + 1, 8.0)
        idx = np.clip(((x + half) / (2 * half) * (len(w) - 1)).round().astype(int), 0, len(w) - 1)
        tab = np.sinc(x * 0.97) * 0.97 * w[idx]  # slight lowpass (cutoff 0.97*Nyquist)
        tab /= tab.sum(axis=1, keepdims=True)
        self.tab = tab.astype(np.float32)
        self.buf = np.zeros((self.TAPS, channels), np.float32)  # history; pos indexes into it
        self.pos = float(half - 1)  # fractional read position (input index of next output)

    def available(self) -> float:
        """Input samples buffered beyond the read position (fractional)."""
        return len(self.buf) - self.TAPS // 2 - self.pos

    def feed(self, x: np.ndarray) -> None:
        self.buf = np.concatenate([self.buf, x.astype(np.float32, copy=False)])

    def need(self, n: int, step: float) -> int:
        """Input samples still needed to produce n outputs."""
        last = self.pos + (n - 1) * step
        return max(0, int(math.floor(last)) + self.TAPS // 2 + 1 - len(self.buf))

    def out(self, n: int, step: float) -> np.ndarray:
        half = self.TAPS // 2
        p = self.pos + np.arange(n) * step
        base = np.floor(p).astype(np.int64)
        frac = p - base
        phase = np.minimum((frac * self.PHASES).round().astype(np.int64), self.PHASES)
        idx = base[:, None] + np.arange(-half + 1, half + 1)[None, :]
        w = self.tab[phase]  # (n, taps)
        y = np.einsum("nt,ntc->nc", w, self.buf[idx])
        self.pos += n * step
        drop = int(math.floor(self.pos)) - half + 1
        if drop > 0:
            self.buf = self.buf[drop:]
            self.pos -= drop
        return y.astype(np.float32)


# ---------------------------------------------------------------- jitter buffer

class JitterBuffer:
    """Pull-model adaptive jitter buffer with PLC/FEC and buffer-level drift control.

    push() from the network thread, pull(n) from the playback clock (device callback or a
    paced thread). The controlled variable is the buffer level measured just before each
    pull (ms of audio from the read position to the end of the newest frame received,
    holes included). Target = pull block + frame/2 + jitter (p99-min of transit over a
    sliding window) + margin, clamped to [min_ms, max_ms]. A PI controller turns
    (level - target) into a resampling ratio limited to +-max_ppm (clock drift); larger
    excursions (after a jitter spike) are removed by dropping one frame, preferably a quiet
    one; underruns are concealed by PLC without advancing the timeline (expand)."""

    def __init__(self, codec: int, channels: int, frame: int, min_ms: float = 5.0,
                 max_ms: float = 120.0, fixed_ms: float | None = None, margin_ms: float = 2.0,
                 max_ppm: float = 500.0, window_s: float = 10.0, drift: bool = True,
                 fec: bool = True):
        self.codec, self.ch, self.frame = codec, channels, frame
        self.dec = decoder_for(codec, channels)
        self.has_lbrr = None
        if codec == C_OPUS:
            import opus
            self.has_lbrr = opus.packet_has_lbrr
        self.rs = Resampler(channels)
        self.min_ms, self.max_ms, self.fixed_ms, self.margin_ms = min_ms, max_ms, fixed_ms, margin_ms
        self.max_ppm, self.window_s, self.drift_on, self.use_fec = max_ppm, window_s, drift, fec
        self.lock = threading.Lock()
        self.frames: dict[int, Packet] = {}
        self.next_ts: int | None = None  # ts64 of next frame to decode
        self.last_ts32: int | None = None
        self.ts_hi: int = 0
        self.max_ts: int | None = None
        self.playing = False
        self.transit = collections.deque()  # (arrival, transit seconds)
        self.pulls = collections.deque()  # (time, pull-clock deviation seconds)
        self.pulled = 0
        self.target_ms = fixed_ms if fixed_ms is not None else min(max_ms, max(min_ms, 25.0))
        self.level_avg: float | None = None
        self.ppm = 0.0
        self.integ = 0.0
        self.over_since: float | None = None
        self.expand_run = 0
        self.block = frame
        self.stats = collections.Counter()
        self.levels: list[float] = []
        self.ppm_hist: list[float] = []
        self.dec_time = 0.0
        self.t_first: float | None = None
        self.env: list[tuple[float, float]] = []  # per-second (time, min transit) for drift
        self._env_sec = None
        self._env_min = None

    # -------- network side
    def _unwrap(self, ts32: int) -> int:
        if self.last_ts32 is None:
            self.last_ts32, self.ts_hi = ts32, ts32
            return ts32
        d = (ts32 - self.last_ts32) & 0xFFFFFFFF
        if d >= 0x80000000:
            d -= 0x100000000
        self.ts_hi += d
        self.last_ts32 = ts32
        return self.ts_hi

    def push(self, p: Packet) -> None:
        with self.lock:
            ts = self._unwrap(p.ts)
            self.stats["rx"] += 1
            self.stats["rx_bytes"] += p.size + 28
            if self.t_first is None:
                self.t_first = p.arrival
            tr = p.arrival - ts / RATE
            self.transit.append((p.arrival, tr))
            while self.transit and self.transit[0][0] < p.arrival - self.window_s:
                self.transit.popleft()
            sec = int(p.arrival)
            if sec != self._env_sec:
                if self._env_sec is not None:
                    self.env.append((self._env_sec + 0.5, self._env_min))
                self._env_sec, self._env_min = sec, tr
            else:
                self._env_min = min(self._env_min, tr)
            if p.nsamp != self.frame:
                self.frame = p.nsamp
            if self.next_ts is not None and ts < self.next_ts:
                self.stats["late"] += 1
                return
            if ts in self.frames:
                self.stats["dup"] += 1
                return
            if self.has_lbrr is not None and self.has_lbrr(p.payload):
                self.stats["lbrr"] += 1
            self.frames[ts] = p
            if self.max_ts is None or ts > self.max_ts:
                self.max_ts = ts

    # -------- playback side
    SPIKE_MS = 150.0  # transit excess treated as an outage, not as jitter to provision for

    def _jitter_ms(self) -> float:
        """p97 - min of transit delay over the window, ignoring outage spikes."""
        if len(self.transit) < 10:
            return 10.0
        v = np.fromiter((t for _, t in self.transit), float)
        v = (v - v.min()) * 1000
        v = v[v < self.SPIKE_MS]
        return float(np.quantile(v, 0.97))

    def _pull_jitter_ms(self) -> float:
        """Spread of the playback clock's pull times (device callbacks are not evenly spaced:
        PortAudio/WASAPI may pull two periods back to back). Provisioned like network jitter."""
        if len(self.pulls) < 20:
            return 0.0
        v = np.fromiter((d for _, d in self.pulls), float)
        v = (v - v.min()) * 1000
        return float(np.quantile(v[v < self.SPIKE_MS], 0.97))

    def level_ms(self) -> float:
        """Buffered audio ahead of the playout point, ms."""
        lv = self.rs.available()
        if self.next_ts is not None and self.max_ts is not None and self.max_ts >= self.next_ts:
            lv += self.max_ts + self.frame - self.next_ts
        return lv * 1000 / RATE

    def _decode_next(self) -> np.ndarray:
        n = self.frame
        ts = self.next_ts
        t0 = time.perf_counter()
        p = self.frames.pop(ts, None)
        if p is not None:
            x = self.dec.decode(p.payload, p.nsamp)
            self.stats["ok"] += 1
            self.next_ts += p.nsamp
            self.expand_run = 0
        elif self.max_ts is not None and self.max_ts > ts:
            nxt = self.frames.get(ts + n)
            if nxt is not None and self.use_fec and self.dec.__class__.__name__ == "OpusDecoder":
                x = self.dec.fec(nxt.payload, n)
                self.stats["fec"] += 1
            else:
                x = self.dec.plc(n)
                self.stats["plc"] += 1
            self.stats["lost"] += 1
            self.next_ts += n
            self.expand_run = 0
        else:
            # nothing newer has arrived: the packet is late. Conceal without advancing the
            # timeline (stretch); after 3 frames of this, give up and re-buffer.
            self.stats["expand"] += 1
            if self.expand_run == 0:
                self.stats["underrun"] += 1
            self.expand_run += 1
            x = self.dec.plc(n)
            if self.expand_run > 3:
                self.playing = False
                self.stats["rebuffer"] += 1
        self.dec_time += time.perf_counter() - t0
        return x

    def _start_if_ready(self) -> bool:
        if not self.frames:
            return False
        first = min(self.frames)
        span = (self.max_ts + self.frame - first) * 1000 / RATE
        if self.next_ts is None or first > self.next_ts:
            if span < self.target_ms - self.block * 500 / RATE:
                return False
            self.next_ts = first
        elif self.level_ms() < self.target_ms:
            return False
        self._flush_to_target()
        self.playing = True
        self.expand_run = 0
        return True

    def _flush_to_target(self) -> None:
        """Discard the oldest frames so that about target_ms remains (after an outage the
        burst of late packets would otherwise sit in the buffer as pure latency)."""
        excess = self.level_ms() - self.target_ms
        n = 0
        while excess > self.frame * 1000 / RATE and self.next_ts is not None and self.next_ts < self.max_ts:
            self.frames.pop(self.next_ts, None)
            self.next_ts += self.frame
            excess -= self.frame * 1000 / RATE
            n += 1
        if n:
            self.stats["flushed"] += n
            self.level_avg = None

    def _control(self, now: float, block_s: float) -> None:
        self.pulls.append((now, now - self.pulled / RATE))
        while self.pulls and self.pulls[0][0] < now - self.window_s:
            self.pulls.popleft()
        if self.fixed_ms is None:
            t = (self.block * 1000 / RATE + self.frame * 500 / RATE + self._jitter_ms()
                 + self._pull_jitter_ms() + self.margin_ms)
            t = min(self.max_ms, max(self.min_ms, t))
            # rise fast, decay slowly
            self.target_ms = t if t > self.target_ms else self.target_ms + (t - self.target_ms) * block_s / 2.0
        if self.level_ms() > self.target_ms + max(40.0, self.target_ms):
            self._flush_to_target()  # far too much: catch up at once (one glitch)
        lv = self.level_ms()
        self.levels.append(lv)
        a = min(1.0, block_s / 1.0)
        self.level_avg = lv if self.level_avg is None else self.level_avg + a * (lv - self.level_avg)
        err = self.level_avg - self.target_ms
        if self.drift_on:
            # slow integral = clock drift estimate (crystals: tens of ppm), clamped to +-200 ppm
            # so that expand/drop events cannot wind it up; P term handles level errors
            self.integ = max(-200.0, min(200.0, self.integ + 0.1 * err * block_s))
            self.ppm = max(-self.max_ppm, min(self.max_ppm, 40.0 * err + self.integ))
        # big excursion: drop a frame; a silent one as soon as the excess exceeds a frame,
        # a non-silent one only after 2 s above max(15 ms, frame)
        fms = self.frame * 1000 / RATE
        if err > max(5.0, 0.75 * fms):
            if self.over_since is None:
                self.over_since = now
            elapsed = now - self.over_since
            p = self.frames.get(self.next_ts)
            if p is not None and self.max_ts > self.next_ts:
                quiet = bool(p.flags & F_SILENCE)
                if quiet or (elapsed > 2.0 and err > max(15.0, fms)):
                    x = self.dec.decode(self.frames.pop(self.next_ts).payload, p.nsamp)
                    del x  # decoded to keep decoder state continuous, then discarded
                    self.next_ts += p.nsamp
                    self.stats["drop"] += 1
                    self.level_avg -= p.nsamp * 1000 / RATE
                    self.over_since = None
        else:
            self.over_since = None

    def pull(self, n: int, now: float | None = None) -> np.ndarray:
        now = time.monotonic() if now is None else now
        with self.lock:
            self.block = n
            self.stats["pulls"] += 1
            self.pulled += n
            if not self.playing and not self._start_if_ready():
                self.stats["silent_pulls"] += 1
                return np.zeros((n, self.ch), np.float32)
            self._control(now, n / RATE)
            self.ppm_hist.append(self.ppm)
            step = 1.0 + self.ppm * 1e-6
            while self.rs.need(n, step) > 0:
                if not self.playing:
                    break
                self.rs.feed(self._decode_next())
            if self.rs.need(n, step) > 0:
                self.rs.feed(np.zeros((self.rs.need(n, step), self.ch), np.float32))
            # forget frames that can no longer play
            if self.next_ts is not None:
                for k in [k for k in self.frames if k < self.next_ts]:
                    del self.frames[k]
            return self.rs.out(n, step)

    def snapshot(self, reset: bool = True) -> dict:
        with self.lock:
            lv = np.array(self.levels) if self.levels else np.zeros(1)
            d = {k: int(v) for k, v in self.stats.items()}
            d.update(level_ms=round(float(lv.mean()), 2), level_min_ms=round(float(lv.min()), 2),
                     level_max_ms=round(float(lv.max()), 2), target_ms=round(self.target_ms, 2),
                     jitter_ms=round(self._jitter_ms(), 2), pull_jitter_ms=round(self._pull_jitter_ms(), 2),
                     ppm=round(self.ppm, 1),
                     ppm_i=round(self.integ, 1),
                     dec_us=round(self.dec_time * 1e6 / max(1, self.stats["ok"] + self.stats["fec"] + self.stats["plc"] + self.stats["expand"]), 1))
            if reset:
                self.stats = collections.Counter()
                self.levels = []
                self.dec_time = 0.0
            return d

    def drift_estimate(self) -> float | None:
        """Sender sample clock rate relative to the receiver's monotonic clock, ppm (positive:
        the sender produces samples faster than we count time, so the steady-state resampling
        correction ppm_i should converge to about the same value when the sink is paced by
        the monotonic clock). From the per-second lower envelope of transit delay."""
        with self.lock:
            env = list(self.env)
        if len(env) < 30:
            return None
        t = np.array([e[0] for e in env])
        y = np.array([e[1] for e in env])
        # robust: fit, drop the worst 10 % residuals, refit
        a, b = np.polyfit(t, y, 1)
        r = np.abs(y - (a * t + b))
        keep = r <= np.quantile(r, 0.9)
        a, b = np.polyfit(t[keep], y[keep], 1)
        return float(-a * 1e6)
