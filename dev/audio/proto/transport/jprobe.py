"""Raw UDP one-way delay probe (no audio, no crypto): paced packets in both directions at once,
sender timestamps, receiver arrival times, ping clock offset. Used to find where Mac -> PC
jitter comes from (see README "Mac->PC jitter").

    PC : python jprobe.py server --port 24823 --punch MAC:40123 --duration 70 --out pc.npz
    Mac: python jprobe.py client --to PC:24823 --local-port 40123 --duration 60 --out mac.npz \
             [--interval-ms 10] [--size 80] [--tos 0xb8] [--svc vo] [--qos ui] [--burst N]

The client (Mac) always sends first; the server answers its source address (no listening on
the Mac, no firewall rule on the PC thanks to --punch). Each side logs, per data packet sent:
(seq, t_sched, t_before_send, t_after_send) and per data packet received: (seq, t_remote_sent,
t_arrival); pings give (t0, t1, t2, t3). All times are time.monotonic() of the logging machine.
At the end the server sends its logs to the client in chunks, so one .npz holds both sides.

Options that change how packets are sent (each side applies its own):
  --tos N       IP_TOS (DSCP << 2), e.g. 0xb8 = EF (46). Linux/macOS honour it; Windows ignores it.
  --svc vo|vi|bk|be|oam|rv|av|sig   macOS SO_NET_SERVICE_TYPE (maps to the Wi-Fi WMM access category)
  --qos ui|ia|def|ut  macOS pthread QoS class of the sender thread (user-interactive etc.)
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import socket
import struct
import sys
import threading
import time
import zlib

import numpy as np

mono = time.monotonic
HDR = struct.Struct("!2sBIdd")  # magic, kind, seq, t_sched, t_before
MAGIC = b"JP"
K_DATA, K_HELLO, K_PING, K_PONG, K_BYE, K_LOG, K_LOGEND = range(7)

SVC = {"be": 0, "bk_sys": 1, "bk": 2, "rd": 3, "oam": 4, "av": 5, "rv": 6, "vi": 7, "vo": 8, "sig": 9}
SO_NET_SERVICE_TYPE = 0x1116
QOS = {"ui": 0x21, "ia": 0x19, "def": 0x15, "ut": 0x11, "bg": 0x09}


def set_thread_qos(name):
    if not name or sys.platform != "darwin":
        return None
    libc = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
    return libc.pthread_set_qos_class_self_np(ctypes.c_uint(QOS[name]), ctypes.c_int(0))


def apply_sock_opts(s, a):
    info = {}
    if a.tos is not None:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_TOS, a.tos)
        info["tos"] = s.getsockopt(socket.IPPROTO_IP, socket.IP_TOS)
    if a.svc and sys.platform == "darwin":
        s.setsockopt(socket.SOL_SOCKET, SO_NET_SERVICE_TYPE, SVC[a.svc])
        info["svc"] = s.getsockopt(socket.SOL_SOCKET, SO_NET_SERVICE_TYPE)
    return info


class Side:
    def __init__(self, sock, a, connected):
        self.s, self.a, self.connected = sock, a, connected
        self.peer = None
        self.tx, self.rx, self.pings = [], [], []
        self.stop = threading.Event()      # receiver
        self.stop_tx = threading.Event()   # sender
        self.done = threading.Event()      # server: client said bye
        self.started = threading.Event()
        self.remote_logs = {}
        self.remote_done = threading.Event()
        self.remote_buf = {}

    def send(self, b):
        try:
            if self.connected:
                self.s.send(b)
            elif self.peer is not None:
                self.s.sendto(b, self.peer)
        except OSError:
            pass

    def sender(self):
        info = set_thread_qos(self.a.qos)
        a = self.a
        if a.interval_ms <= 0:
            return info  # receive only
        iv = a.interval_ms / 1000
        pad = b"\0" * max(0, a.size - HDR.size)
        self.started.wait()
        t0 = mono() + 0.05
        seq = 0
        while not self.stop_tx.is_set():
            due = t0 + seq * iv
            d = due - mono()
            if d > 0:
                time.sleep(d)
            tb = mono()
            self.send(HDR.pack(MAGIC, K_DATA, seq, due, tb) + pad)
            self.tx.append((seq, due, tb, mono()))
            seq += 1
        return info

    def receiver(self):
        while not self.stop.is_set():
            try:
                b, addr = self.s.recvfrom(65535)
            except socket.timeout:
                continue
            except ConnectionResetError:
                continue
            except OSError:
                return
            ta = mono()
            if len(b) < HDR.size or b[:2] != MAGIC:
                continue
            _, kind, seq, x, y = HDR.unpack_from(b)
            if kind == K_DATA:
                self.rx.append((seq, y, ta))
            elif kind == K_HELLO:
                if not self.connected:
                    self.peer = addr
                    self.send(HDR.pack(MAGIC, K_HELLO, 0, 0, 0))
                self.started.set()
            elif kind == K_PING:
                self.send(HDR.pack(MAGIC, K_PONG, seq, x, ta) + struct.pack("!d", mono()))
            elif kind == K_PONG:
                (t2,) = struct.unpack_from("!d", b, HDR.size)
                self.pings.append((x, y, t2, ta))
            elif kind == K_BYE:
                self.started.set()
                self.stop_tx.set()
                self.done.set()
            elif kind == K_LOG:
                self.remote_buf[seq] = b[HDR.size:]
            elif kind == K_LOGEND:
                self.remote_total = seq
                self.remote_done.set()


def pack_logs(side, extra):
    d = {"tx": np.array(side.tx, float), "rx": np.array(side.rx, float), "pings": np.array(side.pings, float)}
    import io
    bio = io.BytesIO()
    np.savez(bio, meta=np.frombuffer(json.dumps(extra).encode(), np.uint8), **d)
    return zlib.compress(bio.getvalue())


def cmd_server(a):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    s.bind(("0.0.0.0", a.port))
    s.settimeout(0.5)
    info = apply_sock_opts(s, a)
    side = Side(s, a, connected=False)
    if a.punch:
        h, pt = a.punch.rsplit(":", 1)

        def punch():
            t0 = mono()
            while side.peer is None and mono() - t0 < 30:
                s.sendto(os.urandom(8), (h, int(pt)))
                time.sleep(0.2)
        threading.Thread(target=punch, daemon=True).start()
    rt = threading.Thread(target=side.receiver, daemon=True)
    rt.start()
    st = threading.Thread(target=side.sender, daemon=True)
    st.start()
    if a.ka:
        # keep this machine's radio busy with outgoing packets to a third host (not the peer)
        h, pt = a.ka.rsplit(":", 1)
        ks = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)

        def ka():
            n = 0
            t0 = mono()
            while not side.stop_tx.is_set():
                n += 1
                d = t0 + n * a.ka_ms / 1000 - mono()
                if d > 0:
                    time.sleep(d)
                try:
                    ks.sendto(b"\0" * 16, (h, int(pt)))
                except OSError:
                    pass
        threading.Thread(target=ka, daemon=True).start()
    end = mono() + a.duration
    while not side.done.is_set() and mono() < end:
        time.sleep(0.1)
    side.stop_tx.set()
    time.sleep(0.3)
    # ship logs to the client (it keeps listening a few seconds after its bye)
    blob = pack_logs(side, {"side": "server", "sock": info, "platform": sys.platform, "args": vars(a)})
    if a.out:
        open(a.out, "wb").write(blob)
    n = 0
    for i in range(0, len(blob), 1200):
        side.send(HDR.pack(MAGIC, K_LOG, n, 0, 0) + blob[i:i + 1200])
        n += 1
        time.sleep(0.0005)
    for _ in range(5):
        side.send(HDR.pack(MAGIC, K_LOGEND, n, 0, 0))
        time.sleep(0.05)
    print(json.dumps({"server": "done", "tx": len(side.tx), "rx": len(side.rx), "log_chunks": n}))


def cmd_client(a):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 20)
    if a.local_port:
        s.bind(("0.0.0.0", a.local_port))
    host, port = a.to.rsplit(":", 1)
    s.connect((host, int(port)))
    s.settimeout(0.5)
    info = apply_sock_opts(s, a)
    side = Side(s, a, connected=True)
    rt = threading.Thread(target=side.receiver, daemon=True)
    rt.start()
    t0 = mono()
    while not side.started.wait(0.2):
        side.send(HDR.pack(MAGIC, K_HELLO, 0, 0, 0))
        if mono() - t0 > 15:
            raise SystemExit("no answer")
    st = threading.Thread(target=side.sender, daemon=True)
    st.start()
    end = mono() + a.duration
    k = 0
    while mono() < end:
        side.send(HDR.pack(MAGIC, K_PING, k, mono(), 0))
        k += 1
        time.sleep(a.ping_s)
    side.stop_tx.set()
    st.join()
    for _ in range(3):
        side.send(HDR.pack(MAGIC, K_BYE, 0, 0, 0))
        time.sleep(0.02)
    ok = side.remote_done.wait(a.wait_logs)
    side.stop.set()
    remote = None
    if ok and len(side.remote_buf) == side.remote_total:
        import io
        blob = zlib.decompress(b"".join(side.remote_buf[i] for i in range(side.remote_total)))
        remote = dict(np.load(io.BytesIO(blob)))
    out = {"tx": np.array(side.tx, float), "rx": np.array(side.rx, float), "pings": np.array(side.pings, float),
           "meta": np.frombuffer(json.dumps({"side": "client", "sock": info, "args": vars(a)}).encode(), np.uint8)}
    if remote is not None:
        for k2, v in remote.items():
            out["srv_" + k2] = v
    np.savez(a.out, **out)
    print(json.dumps({"client": "done", "tx": len(side.tx), "rx": len(side.rx), "pings": len(side.pings),
                      "server_logs": remote is not None, "sock": info, "out": a.out}))


def clock_offset(pings):
    """PC clock minus Mac clock as a linear function of Mac time, from the lowest-RTT 30 %
    of pings (NTP formula). Returns (f(t_mac) -> offset, rtt array)."""
    P = np.asarray(pings)
    t0, t1, t2, t3 = P.T
    rtt = (t3 - t0) - (t2 - t1)
    off = ((t1 - t0) + (t2 - t3)) / 2
    keep = rtt <= np.quantile(rtt, 0.3)
    c = np.polyfit(t0[keep], off[keep], 1)
    return (lambda t: np.polyval(c, t)), rtt


def one_way(tx, rx, to_rx_clock):
    """Returns None if the sender sent nothing.
    Join sender log (seq, sched, before, after) with receiver log (seq, sent, arrival):
    returns dict of arrays: seq, t (sender time), owd (s), send_lag (before - sched)."""
    tx = np.asarray(tx)
    rx = np.asarray(rx)
    if tx.ndim != 2 or len(tx) < 100:
        return None
    seqs = tx[:, 0].astype(np.int64)
    pos = {int(s): i for i, s in enumerate(seqs)}
    got = np.zeros(len(tx), bool)
    arr = np.full(len(tx), np.nan)
    for s, _, ta in rx:
        i = pos.get(int(s))
        if i is not None and not got[i]:
            got[i] = True
            arr[i] = ta
    owd = arr - to_rx_clock(tx[:, 2])
    return {"seq": seqs, "t": tx[:, 2], "sched": tx[:, 1], "owd": owd, "send_lag": tx[:, 2] - tx[:, 1],
            "send_dur": tx[:, 3] - tx[:, 2], "arr": arr}


def describe(d, spike_ms, skip_s=1.0):
    if d is None:
        return None
    t = d["t"]
    k = t > t[0] + skip_s
    owd = d["owd"][k] * 1000
    lost = int(np.isnan(owd).sum())
    v = owd[~np.isnan(owd)]
    base = float(np.quantile(v, 0.01))
    rel = v - base
    q = lambda x, p: round(float(np.quantile(x, p)), 2)  # noqa: E731
    tt = t[k][~np.isnan(owd)]
    sp = rel > spike_ms
    # spike events = runs of consecutive packets above the threshold
    starts = np.nonzero(sp & ~np.r_[False, sp[:-1]])[0]
    ev_t = tt[starts]
    ends = np.nonzero(sp & ~np.r_[sp[1:], False])[0]
    peak = [float(rel[s:e + 1].max()) for s, e in zip(starts, ends)]
    dur = [float((tt[e] - tt[s]) * 1000 + 10) for s, e in zip(starts, ends)]
    gaps = np.diff(ev_t) if len(ev_t) > 1 else np.zeros(0)
    lag = d["send_lag"][k] * 1000
    out = {
        "n": int(k.sum()), "lost": lost, "owd_min_ms": round(float(v.min()), 2), "owd_p01_ms": round(base, 2),
        "rel_p50": q(rel, .5), "rel_p90": q(rel, .9), "rel_p97": q(rel, .97), "rel_p99": q(rel, .99),
        "rel_p999": q(rel, .999), "rel_max": round(float(rel.max()), 1),
        "spike_events": int(len(starts)), "spikes_per_min": round(len(starts) / max(1e-9, (tt[-1] - tt[0]) / 60), 1),
        "spike_peak_p50": round(float(np.median(peak)), 1) if peak else None,
        "spike_peak_max": round(float(np.max(peak)), 1) if peak else None,
        "spike_dur_p50_ms": round(float(np.median(dur)), 1) if dur else None,
        "spike_gap_s_p10_p50_p90": [q(gaps, .1), q(gaps, .5), q(gaps, .9)] if len(gaps) > 2 else None,
        "send_lag_p50_p99_max_ms": [q(lag, .5), q(lag, .99), round(float(lag.max()), 2)],
        "send_call_p99_ms": q(d["send_dur"][k] * 1000, .99),
    }
    # periodicity of the delay series (resampled on the send grid): strongest periods 50 ms..5 s
    x = np.nan_to_num(d["owd"][k] * 1000 - base, nan=0.0)
    x = x - x.mean()
    if len(x) > 1000:
        F = np.abs(np.fft.rfft(x * np.hanning(len(x)))) ** 2
        f = np.fft.rfftfreq(len(x), d=float(np.median(np.diff(d["sched"]))))
        m = (f > 0.2) & (f < 20)
        top = np.argsort(F[m])[-5:][::-1]
        tot = F[m].sum()
        out["top_periods_s"] = [(round(1 / f[m][i], 3), round(float(F[m][i] / tot), 3)) for i in top]
    return out


def cmd_analyze(a):
    rows = []
    for fn in a.files:
        z = np.load(fn)
        meta = json.loads(bytes(z["meta"]).decode())
        if "srv_rx" not in z:
            print(fn, "no server logs")
            continue
        off, rtt = clock_offset(z["pings"])
        m2p = one_way(z["tx"], z["srv_rx"], lambda t: t + off(t))          # Mac clock -> PC clock
        # PC->Mac: PC send time to Mac clock: t_mac = t_pc - off(t_mac) ~ t_pc - off(t_pc - off0)
        o0 = float(off(np.median(z["pings"][:, 0])))
        p2m = one_way(z["srv_tx"], z["rx"], lambda t: t - off(t - o0))
        r = {"file": os.path.basename(fn), "args": {k: meta["args"].get(k) for k in ("interval_ms", "size", "tos", "svc", "qos")},
             "srv_args": None, "rtt_ms_p50_p99": [round(float(np.median(rtt)) * 1000, 2), round(float(np.quantile(rtt, .99)) * 1000, 2)],
             "mac_to_pc": describe(m2p, a.spike_ms), "pc_to_mac": describe(p2m, a.spike_ms)}
        if "srv_meta" in z:
            sm = json.loads(bytes(z["srv_meta"]).decode())
            r["srv_args"] = {k: sm["args"].get(k) for k in ("tos", "svc", "qos")}
            r["srv_sock"] = sm.get("sock")
        r["sock"] = meta.get("sock")
        rows.append(r)
        if a.json:
            print(json.dumps(r))
        else:
            print(f"== {r['file']} {r['args']} srv {r['srv_args']} sock {r.get('sock')} rtt {r['rtt_ms_p50_p99']}")
            for dname in ("mac_to_pc", "pc_to_mac"):
                print(f"  {dname}: {json.dumps(r[dname])}")
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("server", "client"):
        p = sub.add_parser(name)
        p.add_argument("--duration", type=float, default=60)
        p.add_argument("--interval-ms", type=float, default=10)
        p.add_argument("--size", type=int, default=80)
        p.add_argument("--tos", type=lambda x: int(x, 0), default=None)
        p.add_argument("--svc", choices=list(SVC), default=None)
        p.add_argument("--qos", choices=list(QOS), default=None)
        p.add_argument("--out", default=None)
        if name == "server":
            p.add_argument("--port", type=int, default=24823)
            p.add_argument("--punch", default=None)
            p.add_argument("--ka", default=None, help="HOST:PORT: also send 16 B every --ka-ms to a third host")
            p.add_argument("--ka-ms", type=float, default=10)
        else:
            p.add_argument("--to", required=True)
            p.add_argument("--local-port", type=int, default=40123)
            p.add_argument("--ping-s", type=float, default=0.25)
            p.add_argument("--wait-logs", type=float, default=8.0)
    z = sub.add_parser("analyze")
    z.add_argument("files", nargs="+")
    z.add_argument("--spike-ms", type=float, default=10.0)
    z.add_argument("--json", action="store_true")
    a = ap.parse_args()
    {"server": cmd_server, "client": cmd_client, "analyze": cmd_analyze}[a.cmd](a)


if __name__ == "__main__":
    main()
