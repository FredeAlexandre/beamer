# SPDX-License-Identifier: GPL-3.0-only
"""WP14: the engine path of a system send stream (process loopback -> ring -> encoder), on lebig in session 0.

    py -3 -I system_loss.py --bin ...\\beamer-audio.exe --seconds 600 [--load N]

Starts one helper (Windows backend) with a flow offered to a dead local port and a system send stream (no local
mute: nothing of ours plays), reads its stats, and prints encoded frames vs QPC (ppm), ring overflow, dev drops.
With --load N, a backend_check `busy` child spins N threads meanwhile. Standard library only.
"""
import argparse, base64, json, os, subprocess, sys, threading, time

ap = argparse.ArgumentParser()
ap.add_argument("--bin", required=True)
ap.add_argument("--check", default=None, help="backend_check.exe (for --load)")
ap.add_argument("--seconds", type=float, default=600)
ap.add_argument("--load", type=int, default=0)
ap.add_argument("--state-dir", default=os.path.join(os.environ["TEMP"], "beamer-wp14-engine"))
a = ap.parse_args()
os.makedirs(a.state_dir, exist_ok=True)
p = subprocess.Popen([a.bin, "--control", "--dev", "--parent-pid", str(os.getpid()), "--state-dir", a.state_dir,
                      "--exclude-pid", str(os.getpid()), "--log-level", "info"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
events, cv = [], threading.Condition()


def rd():
    for line in p.stdout:
        try:
            e = json.loads(line)
        except ValueError:
            continue
        with cv:
            events.append(e)
            cv.notify_all()


def er():
    for line in p.stderr:
        s = line.decode("utf-8", "replace").rstrip()
        if "system stream" in s or "warn" in s.lower() or "error" in s.lower():
            print("helper:", s, flush=True)


threading.Thread(target=rd, daemon=True).start()
threading.Thread(target=er, daemon=True).start()
req = [0]


def call(cmd, timeout=5):
    req[0] += 1
    cmd["req"] = req[0]
    p.stdin.write((json.dumps(cmd) + "\n").encode())
    end = time.time() + timeout
    with cv:
        while time.time() < end:
            for e in events:
                if e.get("req") == req[0] and e.get("ev") in ("ok", "error"):
                    return e
            cv.wait(0.1)
    return None


def wait_ev(pred, timeout):
    end = time.time() + timeout
    with cv:
        while time.time() < end:
            for e in events:
                if pred(e):
                    return e
            cv.wait(0.1)
    return None


if not wait_ev(lambda e: e.get("ev") == "ready", 10):
    sys.exit("no ready")
key = base64.b64encode(os.urandom(32)).decode()
print("flow_open", call({"cmd": "flow_open", "flow": "e14e14e1", "peer": "nobody", "role": "offer", "port": 0,
                         "host": "127.0.0.1", "remote": {"host": "127.0.0.1", "port": 9}, "key": key, "rr_ms": 10}))
codec = {"name": "opus", "app": "audio", "channels": 2, "bitrate": 96000, "frame_ms": 10, "fec": False}
print("send", call({"cmd": "send", "stream": 102, "flow": "e14e14e1", "kind": "system", "source": {"system": True},
                    "codec": codec, "key": base64.b64encode(os.urandom(32)).decode(), "mute_local": False, "aec": None}))
time.sleep(3)
busy = None
if a.load:
    busy = subprocess.Popen([a.check, "busy", "--threads", str(a.load), "--seconds", str(int(a.seconds) + 5)])


def last_stats():
    with cv:
        for e in reversed(events):
            if e.get("ev") == "stats":
                s = e.get("streams", {}).get("102")
                if s:
                    return e["now_ns"], s
    return None, None


n0 = len(events)
wait_ev(lambda e: e.get("ev") == "stats" and events.index(e) >= n0, 5)
t0, s0 = last_stats()
if s0 is None:
    with cv:
        print("no stream stats; last events:", events[-3:])
    sys.exit(1)
end = time.time() + a.seconds
while time.time() < end:
    time.sleep(min(60, max(0.1, end - time.time())))
    t, s = last_stats()
    el = (t - t0) / 1e9
    fr = (s["frames"] - s0["frames"]) * 480
    print(f"t={el:6.0f} s frames {fr} ({(fr / (el * 48000) - 1) * 1e6:+.0f} ppm vs QPC) overflow {s['overflow'] - s0['overflow']} "
          f"dev_dropped {s['dev_dropped']} enc_err {s['enc_err']}", flush=True)
t, s = last_stats()
el = (t - t0) / 1e9
fr = (s["frames"] - s0["frames"]) * 480
print(f"RESULT engine seconds {el:.1f} load {a.load}: encoded {fr} frames ({(fr / (el * 48000) - 1) * 1e6:+.1f} ppm vs QPC, "
      f"+-{480 / (el * 48000) * 1e6:.0f}), ring overflow {s['overflow'] - s0['overflow']}, dev_dropped {s['dev_dropped']}, enc_err {s['enc_err']}")
if busy:
    busy.kill()
p.stdin.write(b'{"cmd":"quit"}\n')
try:
    p.wait(10)
except subprocess.TimeoutExpired:
    p.kill()
