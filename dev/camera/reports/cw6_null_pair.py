# SPDX-License-Identifier: GPL-3.0-only
"""CW6 substitute for the two-machine deploy check: two AudioServices (a "Mac" and a "PC") in this process,
each with a real beamer-audio-null helper (null audio + null video backend), linked in memory, UDP on
127.0.0.1 only (no LAN bind, so no firewall prompt), plus a 1.5.x-style peer without `audio`.
usage: python null_pair.py <export root>"""
import json, logging, os, sys, tempfile, threading, time
ROOT = sys.argv[1]
sys.path.insert(0, ROOT)
from core import protocol
from core.audio import service as sm
from core.camera import wire as cw

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(threadName)s %(name)s %(message)s",
                    stream=sys.stdout)
sm._usable_host = lambda host: isinstance(host, str) and bool(host)     # loopback allowed for this check only
NULL = os.path.join(ROOT, "native/audio/build/tests/beamer-audio-null")
ids = {name: protocol.id_text(bytes([seed]) * 16) for name, seed in (("mac", 1), ("pc", 2), ("old", 3))}
names = {v: k for k, v in ids.items()}
services, posted = {}, []


class Host:
    def __init__(self, name, platform):
        self.name, self._platform = name, platform

    def own_id(self): return ids[self.name]
    def own_name(self): return self.name
    def platform(self): return self._platform
    def outbox_depth(self, peer): return 0
    def peer_host(self, peer): return "127.0.0.1"
    def peer_name(self, peer): return names.get(peer, "")

    def post(self, peer, message):
        posted.append((self.name, names.get(peer), message.get("type")))
        target = services.get(names.get(peer))
        if target is None:
            return True
        target.link_message(ids[self.name], message)
        return True


def make(name, platform):
    state = tempfile.mkdtemp(prefix=f"cw6-{name}-")
    with open(os.path.join(state, "audio-dev.enabled"), "w") as f:
        f.write("{}")
    service = sm.AudioService(Host(name, platform), state, [NULL, "--backend", "null:", "--video", "null:"])
    services[name] = service
    return service


def wait(what, fn, timeout=10.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if fn():
            print(f"OK   {what}")
            return True
        time.sleep(0.05)
    print(f"FAIL {what}")
    return False


mac, pc = make("mac", "macos"), make("pc", "windows")
sent_commands = []
_orig = sm.AudioService._helper_send
def _spy(self, command, on_reply=None):
    if self is services.get("mac"):
        sent_commands.append(dict(command))
    return _orig(self, command, on_reply)
sm.AudioService._helper_send = _spy
mac.start(); pc.start()
wait("both helpers ready", lambda: mac.snapshot().helper == "ok" and pc.snapshot().helper == "ok")
print("mac helper features", mac._helper_info.get("features"))
audio = frozenset({"audio", "clipboard"})
old_caps = frozenset({"clipboard"})
mac.link_up(ids["pc"], audio); pc.link_up(ids["mac"], audio)
mac.link_up(ids["old"], old_caps); pc.link_up(ids["old"], old_caps)
ok = True
ok &= wait("camera_devices both ways", lambda: ids["pc"] in mac.camera_snapshot().machines
           and ids["mac"] in pc.camera_snapshot().machines)
ok &= wait("flow agreed", lambda: mac._camera._ctx.flow_for(ids["pc"]) is not None)
mac.set_camera(ids["mac"], None)
ok &= wait("route reached the PC", lambda: pc.camera_snapshot().route.camera.machine == ids["mac"])
pc._queue.put(("call", "_helper_send", ({"cmd": "dev", "null_video": {"vcam_in_use": True,
                                                                       "want": {"w": 1280, "h": 720, "fps": 30}}},)))
t0 = time.monotonic()
ok &= wait("Mac streaming", lambda: mac.camera_snapshot().status.state == "streaming", 15)
ok &= wait("PC recv running", lambda: any(s.target == ids["pc"] and s.state == "running"
                                          for s in pc.camera_snapshot().streams), 15)
print("start ->", round(time.monotonic() - t0, 3), "s")
time.sleep(3)
recv = pc._camera._stats
send = mac._camera._stats
print("PC video recv stats", json.dumps({k: {n: v.get(n) for n in ("fps_shown", "kbps", "lost", "target_ms", "path")} for k, v in recv.items()}))
print("Mac video send stats", json.dumps({k: {n: v.get(n) for n in ("fps", "kbps", "idr", "path")} for k, v in send.items()}))
print("audio _stats ids", sorted(mac._stats), sorted(pc._stats), "camera ids", sorted(mac._camera.stream_ids()))
# A mic stream PC <- Mac: sr only because the PC announced video1.
mac.set_mic(ids["mac"], None)
pc._queue.put(("call", "_helper_send", ({"cmd": "dev", "null": {"vmic_in_use": True}},)))
ok &= wait("mic stream running", lambda: any(s.kind == "mic" and s.state == "running" for s in mac.snapshot().streams), 10)
mic = [c for c in sent_commands if c.get("cmd") == "send" and c.get("kind") == "mic"]
print("mic send to the video1 PC:", {k: mic[-1].get(k) for k in ("kind", "sr")} if mic else None)
ok &= bool(mic) and mic[-1].get("sr") is True
pc._queue.put(("call", "_helper_send", ({"cmd": "dev", "null_video": {"vcam_in_use": False}},)))
t1 = time.monotonic()
ok &= wait("Mac send closed after the demand hold", lambda: not mac._camera._sends, 10)
print("close after", round(time.monotonic() - t1, 2), "s (the 3 s demand hold)")
mac.stop(); pc.stop()
old_msgs = [p for p in posted if p[1] == "old"]
cam_to = {(a, b) for a, b, t in posted if str(t).startswith("camera_")}
print("posts to the old peer:", old_msgs)
print("camera_* directions:", sorted(cam_to))
print("hello features mac", [w for w in mac._hello()["data"]["features"]])
ok &= not old_msgs
print("RESULT", "PASS" if ok else "FAIL")
