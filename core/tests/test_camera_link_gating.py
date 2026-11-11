# SPDX-License-Identifier: GPL-3.0-only
"""Camera on the links and in the AudioService (dev/camera/PHASE2.md §5.1, §7.1, §7.4, §12 CW6).

- `camera_*` rides cap `audio`: the initiator's and the responder's links drop it for a peer without
  `audio`, and hand it over only when both ends announced `audio`; the Mac bridge and the PC's sender
  forward it like `audio_*`.
- The real AudioService carries the CameraPart: `camera1` in `audio_hello` only with the flag and a
  `video1` helper; nothing `camera_*` to a peer without `camera1`; `_new_stream` never reuses a camera
  stream id; audio `send` gets `sr:true` only towards a `video1` peer; video stats stay out of audio's;
  every §7.4 dev key reaches the part (and `helper_dev` the helper) and the dev status file has
  `camera`; each public setter reaches the part on the worker; `subscribe` fires on a camera change;
  `machine_name`; shutdown closes the camera streams.
- Live: a real OutboundLink and LinkResponder on loopback, each feeding an AudioService with a fake
  helper that has the video side, beside a 1.5.x-style peer that must see nothing new."""

import importlib.util
import json
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

from core import feature_flags, protocol, receiver
from core import link as link_module
from core.audio import service as service_module
from core.audio import wire as audio_wire
from core.audio.fake_helper import FakeHelper
from core.audio.model import DeviceRef
from core.audio.service import AudioService
from core.camera import wire
from core.camera.fake_video import MAC_CAMERA_KEY
from core.camera.model import CameraRef
from core.link import OutboundLink
from core.tests.audio_cluster_harness import Net, machine_id
from core.tests.responder_harness import B, C, CAPS, HERE, TOKENS, Initiator, Machine, Settings, entry, wait_for

ROOT = Path(__file__).resolve().parents[2]
AUDIO_CAPS = list(CAPS) + ["audio"]
MAC_CAMERA = CameraRef(MAC_CAMERA_KEY, "MacBook Air Camera")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


audio_win = load("audio_win_under_camera_test", "win_app/audio_win.py")
win_sender = load("win_sender_under_camera_test", "win_app/sender.py")


def camera_message(kind=wire.ROUTE):
    return {"type": kind, "data": {}}


def camera_types(messages):
    return [message["type"] for message in messages if str(message.get("type", "")).startswith("camera_")]


# Links -------------------------------------------------------------------------------------------


class CapabilityTableTests(unittest.TestCase):
    def test_every_camera_type_rides_the_audio_capability(self):
        self.assertEqual(set(link_module._CAMERA_CAP_FOR), set(wire.TYPES))
        self.assertTrue(all(link_module._cap_for(kind) == "audio" for kind in wire.TYPES))
        self.assertTrue(all(link_module._cap_for(kind) == "audio" for kind in audio_wire.TYPES))
        self.assertIsNone(link_module._cap_for("camera_unknown"))

    def test_no_new_capability(self):
        caps = set(link_module._CAP_FOR.values()) | set(link_module._CAMERA_CAP_FOR.values())
        self.assertFalse([cap for cap in caps if "camera" in cap or "video" in cap])
        self.assertEqual(protocol.MAX_CAPS, 16)

    def test_the_feature_flag_is_on_in_the_fork(self):
        self.assertIs(feature_flags.CAMERA, True)


class OutboundGatingTests(unittest.TestCase):
    def link(self, caps):
        settings = Settings([entry(HERE, "Far", token=TOKENS[B], host="127.0.0.1", port=1)])
        book = receiver.PeerBook(settings.load, settings.save)
        link = OutboundLink(TOKENS[B], book, lambda: {}, state=lambda *a: None, up=lambda *a: None, message=lambda *a: None)
        link._live = True
        link._caps = frozenset(caps)
        return link

    def test_camera_is_not_queued_for_a_peer_without_audio(self):
        link = self.link(CAPS)
        for kind in wire.TYPES:
            self.assertTrue(link.post(camera_message(kind)))
        self.assertEqual(len(link._outbox), 0)

    def test_camera_is_queued_for_a_peer_with_audio(self):
        link = self.link(AUDIO_CAPS)
        for kind in wire.TYPES:
            self.assertTrue(link.post(camera_message(kind)))
        self.assertEqual([message["type"] for message in link._outbox], list(wire.TYPES))


class ResponderLinkTests(unittest.TestCase):
    def inbound(self, caps):
        return receiver._Link(None, None, B, b"k" * 16, "192.168.1.138", "Peer", caps, HERE, AUDIO_CAPS, True)

    def test_post_drops_camera_for_a_peer_without_audio(self):
        link = self.inbound(CAPS)
        for kind in wire.TYPES:
            self.assertTrue(link.post(camera_message(kind)))
        self.assertEqual(len(link.outbox), 0)
        self.assertTrue(link.post(protocol.paired_msg([])))
        self.assertEqual(len(link.outbox), 1)

    def test_post_keeps_camera_for_a_peer_with_audio(self):
        link = self.inbound(AUDIO_CAPS)
        self.assertTrue(link.post(camera_message()))
        self.assertEqual(list(link.outbox), [camera_message()])

    def test_dispatch_hands_camera_over_only_when_both_ends_announced_audio(self):
        seen = []
        responder = receiver.LinkResponder(receiver.PeerBook(lambda: {}, lambda data: None), lambda: {},
                                           status_callback=lambda *a: None,
                                           audio_callback=lambda peer, message: seen.append((peer, message["type"])))
        for caps, own in ((AUDIO_CAPS, AUDIO_CAPS), (CAPS, AUDIO_CAPS), (AUDIO_CAPS, CAPS)):
            link = receiver._Link(None, None, B, b"k" * 16, "h", "Peer", caps, HERE, own, True)
            for kind in wire.TYPES:
                responder._dispatch(link, camera_message(kind))
            responder._dispatch(link, {"type": "cameras", "data": {}})     # not a camera_* type
        self.assertEqual(seen, [(B, kind) for kind in wire.TYPES])


class _Links:
    def __init__(self, caps):
        self._caps = frozenset(caps)

    def caps(self, peer):
        return self._caps


class PcSenderTests(unittest.TestCase):
    """win_app/sender.py: what comes back on a link this PC opened goes to the audio callback."""

    def sender(self, caps):
        sender = win_sender.LinkSender(receiver.PeerBook(lambda: {}, lambda data: None), lambda: {},
                                       links=lambda owner: _Links(caps))
        seen = []
        sender.audio_callback = lambda peer, message: seen.append((peer, message["type"]))
        return sender, seen

    def test_camera_is_forwarded_like_audio(self):
        sender, seen = self.sender(AUDIO_CAPS)
        for kind in wire.TYPES:
            sender.on_message(B, camera_message(kind))
        sender.on_message(B, audio_wire.hello("macos", "ok", False))
        self.assertEqual(seen, [(B, kind) for kind in wire.TYPES] + [(B, audio_wire.HELLO)])

    def test_nothing_from_a_peer_without_audio(self):
        sender, seen = self.sender(CAPS)
        for kind in wire.TYPES:
            sender.on_message(B, camera_message(kind))
        self.assertEqual(seen, [])


MAC_BRIDGE_CHECK = r"""
import sys, types
sys.path.insert(0, "mac_app")
import bridge
seen = []
audio = types.SimpleNamespace(message=lambda peer, message: seen.append(message["type"]))
for caps in (["audio"], []):
    fake = types.SimpleNamespace(_audio=audio, logger=bridge.logging.getLogger("t"))
    link = types.SimpleNamespace(peer_id=b"p" * 16, caps=frozenset(caps))
    for kind in ("camera_devices", "camera_route", "camera_open", "camera_opened", "camera_close", "audio_hello"):
        bridge.KVMController._link_message(fake, link, {"type": kind, "data": {}})
print(",".join(seen))
"""


@unittest.skipUnless(sys.platform == "darwin", "the Mac bridge needs AppKit")
class MacBridgeTests(unittest.TestCase):
    def test_camera_is_forwarded_like_audio(self):
        """mac_app/bridge.py, in its own process (it imports AppKit and the app's modules)."""
        result = subprocess.run([sys.executable, "-c", MAC_BRIDGE_CHECK], cwd=ROOT, capture_output=True, text=True,
                                timeout=120)
        if result.returncode != 0 and "ModuleNotFoundError" in result.stderr:
            self.skipTest(result.stderr.strip().splitlines()[-1])
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1].split(","),
                         list(wire.TYPES) + ["audio_hello"])


# The real AudioService with its CameraPart --------------------------------------------------------


class ServiceBase(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)

    def cluster(self, mac_video=None, fork=True, linux=True):
        net = self.net
        self.mac = net.add(1, "mac", "macos", camera=True, video=mac_video)
        self.pc = net.add(2, "lebig", "windows", camera=True)
        nodes = [self.mac, self.pc]
        if fork:
            self.fork = net.add(4, "oldpc", "windows")          # audio, but a helper without video1
            nodes.append(self.fork)
        if linux:
            self.linux = net.add(3, "asusrog", "linux", audio=False)
            nodes.append(self.linux)
        for i, x in enumerate(nodes):
            for y in nodes[i + 1:]:
                net.link(x, y)
        net.settle()
        return nodes

    def stream(self):
        self.mac.service.set_camera(self.mac.id, MAC_CAMERA)
        self.net.settle()
        self.pc.set_in_use(True, "Microsoft Teams")
        self.net.advance(0.5)
        sends = self.mac.video.sends
        self.assertEqual(len(sends), 1)
        return next(iter(sends))


class HelloAndGatingTests(ServiceBase):
    def hello_features(self, node):
        return [message["data"]["features"] for _peer, message in node.host.posted
                if message["type"] == audio_wire.HELLO][-1]

    def test_camera1_only_with_a_video_helper(self):
        self.cluster()
        self.assertIn(wire.FEATURE, self.hello_features(self.mac))
        self.assertIn(wire.FEATURE, self.hello_features(self.pc))
        self.assertNotIn(wire.FEATURE, self.hello_features(self.fork))
        self.assertIsNotNone(self.mac.service.camera_snapshot())

    def test_nothing_camera_to_a_peer_without_camera1_or_audio(self):
        self.cluster()
        self.stream()
        self.pc.set_in_use(False)
        self.net.advance(4)
        self.assertEqual(camera_types([m for _s, m in self.fork.received]), [])
        self.assertEqual(camera_types([m for _s, m in self.linux.received]), [])
        self.assertEqual(self.net.camera_violations, [])
        self.assertEqual(self.net.violations, [])
        # The Mac and the PC exchanged their lists and the route.
        self.assertIn(wire.DEVICES, camera_types([m for _s, m in self.pc.received]))
        self.assertIn(wire.DEVICES, camera_types([m for _s, m in self.mac.received]))
        self.assertIn(self.pc.id, self.mac.camera_snapshot().machines)
        self.assertIn(self.mac.id, self.pc.camera_snapshot().machines)

    def test_flag_off_means_no_part_no_camera1_and_camera_ignored(self):
        with mock.patch.object(feature_flags, "CAMERA", False):
            self.cluster(fork=False, linux=False)
            self.assertIsNone(self.mac.service.camera_snapshot())
            self.assertIsNone(self.mac.service._camera)
            for node in (self.mac, self.pc):
                self.assertNotIn(wire.FEATURE, self.hello_features(node))
                self.assertEqual(camera_types([m for _s, m in node.host.posted]), [])
            self.mac.service.set_camera(self.mac.id, None)           # no-ops, never an error
            self.mac.service.set_quality("1080p")
            self.mac.service.link_message(self.pc.id, camera_message(wire.OPEN))
            self.net.settle()
            self.assertEqual(self.mac.helper.commands("send"), [])


class StreamIdTests(ServiceBase):
    def test_new_stream_never_returns_a_camera_stream_id(self):
        self.cluster(fork=False, linux=False)
        video = self.stream()
        self.assertIn(video, self.mac.service._camera.stream_ids())
        draws = iter([video, video, 12345])
        with mock.patch.object(service_module.secrets, "randbits", lambda bits: next(draws)):
            self.assertEqual(self.mac.service._new_stream(), 12345)
        # And the audio mic stream opened next is not the camera's either.
        self.mac.service.set_mic(self.mac.id, None)
        self.pc.helper.set_in_use(True)
        self.net.settle()
        audio = self.mac.helper.commands("send")[-1]
        self.assertEqual(audio.get("kind"), "mic")
        self.assertNotEqual(audio["stream"], video)

    def test_video_stats_stay_out_of_the_audio_stats(self):
        self.cluster(fork=False, linux=False)
        video = self.stream()
        self.mac.service._queue.put(("helper", {"ev": "stats", "streams": {
            str(video): {"dir": "send", "kind": "video", "path": "udp", "kbps": 2400.0},
            "77": {"dir": "send", "kind": "mic", "path": "udp", "jb_ms": 20}}}))
        self.mac.service.pump()
        self.assertEqual(set(self.mac.service._stats), {77})
        self.assertIn(video, self.mac.service._camera._stats)


class TimeMapTests(ServiceBase):
    def test_audio_send_carries_sr_only_towards_a_video1_peer(self):
        self.cluster(linux=False)
        self.mac.service.set_mic(self.mac.id, None)
        self.pc.helper.set_in_use(True)
        self.fork.helper.set_in_use(True)
        self.net.settle()
        sends = {command["stream"]: command for command in self.mac.helper.commands("send")
                 if command.get("kind") == "mic"}
        by_peer = {}
        for stream, command in sends.items():
            peer = next(p for p, m in self.mac.host.posted if m["type"] == audio_wire.OPEN and m["data"]["stream"] == stream)
            by_peer[peer] = command
        self.assertEqual(set(by_peer), {self.pc.id, self.fork.id})
        self.assertIs(by_peer[self.pc.id].get("sr"), True)
        self.assertNotIn("sr", by_peer[self.fork.id])

    def test_no_sr_when_the_own_helper_lacks_video1(self):
        self.cluster(fork=False, linux=False)
        self.mac.service._helper_info["features"] = [w for w in self.mac.service._helper_info["features"] if w != "video1"]
        self.assertFalse(self.mac.service._time_map_for(self.pc.id))
        self.mac.service._helper_info["features"].append("video1")
        self.assertTrue(self.mac.service._time_map_for(self.pc.id))


class DevCommandTests(ServiceBase):
    def test_every_camera_key_reaches_the_part_and_the_status_has_camera(self):
        self.mac = self.net.add(1, "mac", "macos", camera=True, video={"dev": True, "permission": "undetermined"})
        self.pc = self.net.add(2, "lebig", "windows", camera=True)
        for node in (self.mac, self.pc):
            Path(node.state_dir, "audio-dev.enabled").write_text(json.dumps({}))
            node.restart()
        self.net.link(self.mac, self.pc)
        self.net.settle()
        self.assertIn("--dev", self.mac.helper.cmd)

        def run(node, items):
            Path(node.state_dir, service_module.DEV_CMD).write_text(json.dumps(items))
            self.net.clock.advance(1.1)
            self.net.settle()

        part = self.mac.service._camera
        with mock.patch.object(part, "dev_command", wraps=part.dev_command) as spy:
            run(self.mac, [{"camera_test_source": "synth;start_ms=800"},
                           {"set_camera": {"machine": self.mac.id, "device": None}},
                           {"set_quality": "1080p"},
                           {"camera_sharing": True},
                           {"helper_dev": {"video_loss": {"pct": 5, "burst": 0}}}])
        self.assertEqual(spy.call_count, 5)
        self.assertEqual(self.mac.helper.commands("dev")[-1], {"cmd": "dev", "video_loss": {"pct": 5, "burst": 0}})
        snap = self.mac.camera_snapshot()
        self.assertEqual((snap.route.camera.machine, snap.route.quality.size), (self.mac.id, "1080p"))
        self.assertEqual(snap.machines[self.mac.id].test_source, "synth;start_ms=800")
        run(self.pc, [{"camera_refuse": "silent"}])
        self.assertEqual(self.pc.service._camera._refuse, "silent")
        self.pc.set_in_use(True)
        self.net.advance(0.3)
        send = self.mac.helper.commands("send")[-1]
        self.assertEqual((send["kind"], send["source"]), ("video", {"test": "synth;start_ms=800"}))
        self.assertEqual(self.mac.video.permission_requests, 0)           # a dev command never prompts
        run(self.mac, [{"camera_sharing": False}])
        self.assertFalse(self.mac.video.sends)
        status = json.loads(Path(self.mac.state_dir, service_module.DEV_STATUS).read_text())
        self.assertIn("camera", status)
        self.assertEqual(status["camera"]["test_source"], "synth;start_ms=800")
        self.assertEqual(status["camera"]["snapshot"]["sharing"], False)
        self.assertEqual(status["snapshot"]["own"], self.mac.id)          # the audio status is unchanged
        pc_status = json.loads(Path(self.pc.state_dir, service_module.DEV_STATUS).read_text())
        self.assertEqual(pc_status["camera"]["refuse"], "silent")
        # Dev events: a video link_out is logged with its packet count, not its packets.
        self.mac.service._queue.put(("helper", {"ev": "link_out", "flow": "00000000", "p": ["AAAA"] * 3,
                                                "video": True, "frame": 7, "end": True}))
        self.mac.service.pump()
        events = [json.loads(line) for line in Path(self.mac.state_dir, service_module.DEV_EVENTS).read_text().splitlines()]
        video = [event for event in events if event["ev"] == "link_out" and event.get("video")]
        self.assertEqual(video[-1]["p"], 3)
        self.assertIn("camera", {event["ev"] for event in events})       # camera helper events are logged too


class NamesAndSubscribeTests(ServiceBase):
    def test_machine_name(self):
        self.cluster(fork=False)
        ctx = self.mac.service._camera._ctx
        self.assertEqual(ctx.machine_name(self.mac.id), "mac")                  # own
        self.assertEqual(ctx.machine_name(self.pc.id), "lebig")                 # a held audio list
        self.assertEqual(self.mac.service._machines[self.pc.id].name, "lebig")
        self.assertEqual(ctx.machine_name(self.linux.id), "asusrog")            # no list: the link's name
        unknown = machine_id(9)
        self.assertEqual(ctx.machine_name(unknown), "Machine · " + unknown[-6:])
        self.mac.service._machines.pop(self.pc.id)
        self.mac.host.peer_name = lambda peer: ""
        self.assertEqual(ctx.machine_name(self.pc.id), "Machine · " + self.pc.id[-6:])

    def test_subscribe_fires_on_a_camera_change(self):
        self.cluster(fork=False, linux=False)
        calls = []
        self.mac.service.subscribe(lambda: calls.append(self.mac.service.camera_snapshot()))
        audio_before = self.mac.service.snapshot()
        self.mac.service.set_quality("1080p")
        self.mac.service.pump()
        self.assertEqual(self.mac.service.snapshot(), audio_before)              # audio unchanged
        self.assertTrue(calls)
        self.assertEqual(calls[-1].route.quality.size, "1080p")
        count = len(calls)
        self.mac.service.pump()
        self.assertEqual(len(calls), count)                                       # nothing new, no call

    def test_shutdown_closes_the_camera_streams(self):
        self.cluster(fork=False, linux=False)
        video = self.stream()
        self.mac.service.stop()
        self.mac.service.pump()
        closes = [m["data"] for p, m in self.mac.host.posted if p == self.pc.id and m["type"] == wire.CLOSE]
        self.assertIn({"stream": video, "reason": "shutdown"}, closes)
        self.assertIn({"cmd": "stop", "stream": video}, [{k: c[k] for k in ("cmd", "stream")}
                                                         for c in self.mac.helper.commands("stop")])
        self.assertTrue(Path(self.mac.state_dir, "camera.json").exists())

    def test_part_wake_alone_closes_after_the_hold(self):
        """With nothing else happening during the 3 s hold, the part's wake-ups (schedule) close it."""
        self.cluster(fork=False, linux=False)
        self.stream()
        self.pc.set_in_use(False)
        self.net.settle()
        closed = self.net.clock()
        service = self.mac.service
        for _ in range(20):
            if not self.mac.video.sends:
                break
            deadline = service._next_deadline()
            self.assertIsNotNone(deadline)
            self.net.clock.advance(max(0.0, deadline - self.net.clock()) + 0.001)
            service.pump()                  # this service alone: no message, no fake engine tick
        self.assertEqual(self.mac.video.sends, {})
        self.assertAlmostEqual(self.net.clock() - closed, 3.0, delta=0.1)

    def test_schedule_keeps_the_latest_wake(self):
        self.cluster(fork=False, linux=False)
        ctx = self.mac.service._camera._ctx
        ctx.schedule(5000.0)
        ctx.schedule(5010.0)        # the part moved its single wake later: the earlier one is gone
        self.assertEqual(self.mac.service._camera_wake, 5010.0)


class ThreadedSetterTests(unittest.TestCase):
    """Each public camera setter, called from another thread, runs on the service's worker."""

    def test_each_setter_reaches_the_part_on_the_worker(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        net = Net()
        self.addCleanup(net.close)
        node = net.add(1, "mac", "macos", camera=True)
        helper = FakeHelper(platform="macos", video=True)

        def factory(cmd, deliver):
            helper.bind(deliver)
            return helper
        service = AudioService(node.host, folder, ["beamer-audio"], helper_factory=factory)
        part = service._camera
        seen = queue.Queue()
        for method in ("set_camera", "set_quality", "set_sharing", "ask_permission", "refresh_install"):
            original = getattr(part, method)

            def record(*args, method=method, original=original):
                seen.put((method, threading.current_thread() is service._thread, args))
                return original(*args)
            setattr(part, method, record)
        service.start()
        self.addCleanup(service.stop)
        calls = [lambda: service.set_camera(node.id, MAC_CAMERA), lambda: service.set_quality("1080p"),
                 lambda: service.set_camera_sharing(False), service.ask_camera_permission,
                 service.refresh_camera_install]
        caller = threading.Thread(target=lambda: [call() for call in calls])
        caller.start()
        caller.join(5)
        got = [seen.get(timeout=5) for _ in calls]
        self.assertEqual([name for name, _on_worker, _args in got],
                         ["set_camera", "set_quality", "set_sharing", "ask_permission", "refresh_install"])
        self.assertTrue(all(on_worker for _name, on_worker, _args in got), got)
        self.assertEqual(got[0][2], (node.id, MAC_CAMERA))
        self.assertEqual(got[2][2], (False,))
        self.assertTrue(wait_for(lambda: service.camera_snapshot().route.quality.size == "1080p", 5.0))
        # camera_snapshot() and snapshot() are plain reads, fast from any thread.
        started = time.perf_counter()
        for _ in range(1000):
            service.camera_snapshot()
            service.snapshot()
        self.assertLess(time.perf_counter() - started, 0.05)


# Live links ---------------------------------------------------------------------------------------


class LiveSide:
    """One machine: an AudioService with the fake helper and its video side, fed by LinkPeers."""

    def __init__(self, test, host, platform):
        self.folder = tempfile.mkdtemp()
        test.addCleanup(shutil.rmtree, self.folder, True)
        self.helper = FakeHelper(platform=platform, video=True)

        def factory(cmd, deliver):
            self.helper.bind(deliver)
            return self.helper
        self.service = AudioService(host, self.folder, ["beamer-audio"], helper_factory=factory)
        self.peers = audio_win.LinkPeers(self.service)
        test.addCleanup(self.service.stop)

    def camera_machines(self):
        snap = self.service.camera_snapshot()
        return set(snap.machines) if snap is not None else set()


class ResponderHost:
    def __init__(self, machine):
        self.machine = machine
        self.posted = []

    def own_id(self):
        return protocol.id_text(self.machine.own)

    def own_name(self):
        return "Here"

    def platform(self):
        return "windows"

    def post(self, peer, message):
        self.posted.append((peer, message))
        return self.machine.responder.send(protocol.read_id(peer), message)

    def outbox_depth(self, peer):
        return self.machine.responder.outbox_depth(protocol.read_id(peer))

    def peer_host(self, peer):
        return audio_win.udp_host(self.machine.responder.host_of(protocol.read_id(peer)))

    def peer_name(self, peer):
        return peer


class InitiatorHost:
    def __init__(self):
        self.link = None
        self.posted = []

    def own_id(self):
        return protocol.id_text(B)

    def own_name(self):
        return "Near"

    def platform(self):
        return "macos"

    def post(self, peer, message):
        self.posted.append((peer, message))
        return self.link is not None and self.link.post(message)

    def outbox_depth(self, peer):
        return self.link.pending() if self.link is not None else 0

    def peer_host(self, peer):
        return None

    def peer_name(self, peer):
        return peer


class LiveServicesTests(unittest.TestCase):
    """B (a Mac) dials HERE (a PC) with a real OutboundLink; both run the AudioService with a camera
    part. C, a 1.5.x-style Linux peer without `audio`, is linked to HERE and sees nothing new."""

    def test_camera_devices_cross_real_links_and_the_old_peer_sees_nothing(self):
        machine = Machine([entry(B, "Near"), entry(C, "Old", platform="linux")], caps=AUDIO_CAPS)
        here_host = ResponderHost(machine)
        here = LiveSide(self, here_host, "windows")
        machine.responder.set_audio(lambda peer, message: here.service.link_message(protocol.id_text(peer), message),
                                    lambda peer, up, caps: here.peers.changed(protocol.id_text(peer), "in", up, caps))
        machine.start()
        self.addCleanup(machine.stop)
        here.service.start()

        old = Initiator(machine, C)
        self.addCleanup(old.close)
        old.handshake(old.hello(caps=CAPS, platform="linux"))

        near_host = InitiatorHost()
        near = LiveSide(self, near_host, "macos")
        settings = Settings([entry(HERE, "Far", token=TOKENS[B], host="127.0.0.1", port=machine.port)])
        settings.data["machine_id"] = protocol.id_text(B)
        book = receiver.PeerBook(settings.load, settings.save)
        identity = {"id": B, "name": "Near", "platform": "macos", "app": "1.5.0", "caps": AUDIO_CAPS, "port": 24820}

        def up(link, fields):
            near.peers.changed(protocol.id_text(fields["id"]), "out", True, link.caps)

        def state(link, is_up, text):
            if not is_up and link.peer_id:
                near.peers.changed(link.peer_id, "out", False)

        def message(link, msg):
            # As mac_app/bridge.py: `audio_*` and `camera_*` go to the service when the link has `audio`.
            if isinstance(msg.get("type"), str) and msg["type"].startswith(("audio_", "camera_")) and "audio" in link.caps:
                near.service.link_message(link.peer_id, msg)
        link = OutboundLink(TOKENS[B], book, lambda: dict(identity), state=state, up=up, message=message,
                            reconnect_seconds=0.05)
        near_host.link = link
        near.service.start()
        link.start()
        self.addCleanup(link.stop)

        here_id, near_id = protocol.id_text(HERE), protocol.id_text(B)
        # camera_devices both ways: each part holds the other's list.
        self.assertTrue(wait_for(lambda: near_id in here.camera_machines() and here_id in near.camera_machines(), 5.0),
                        (here.camera_machines(), near.camera_machines()))
        self.assertIn(wire.FEATURE, [m for p, m in near_host.posted if m["type"] == audio_wire.HELLO][-1]["data"]["features"])
        self.assertIn(wire.FEATURE, [m for p, m in here_host.posted if m["type"] == audio_wire.HELLO][-1]["data"]["features"])
        # A route set on the Mac reaches the PC over the real link.
        near.service.set_camera(near_id, MAC_CAMERA)
        self.assertTrue(wait_for(lambda: here.service.camera_snapshot().route.camera.machine == near_id, 5.0))
        time.sleep(0.3)
        old.send(protocol.ping_msg())
        old.expect(protocol.MSG_ACK, timeout=0.5)
        new_types = [m for m in old.seen + old.unclaimed if str(m.get("type", "")).startswith(("audio_", "camera_"))]
        self.assertEqual(new_types, [])
        self.assertFalse([p for p, m in here_host.posted if p == protocol.id_text(C)
                          and str(m.get("type", "")).startswith("camera_")])
        link.stop()
        self.assertTrue(wait_for(lambda: near_id not in here.service.snapshot().direct, 5.0))


if __name__ == "__main__":
    unittest.main()
