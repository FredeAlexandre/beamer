# SPDX-License-Identifier: GPL-3.0-only
"""CameraPart with a fake MediaContext and the fake helper's video side (PHASE2 §6.4-§6.5, §7, §10.1)."""

import base64
import json
import logging
import shutil
import tempfile
import threading
import unittest
from dataclasses import replace

from core.audio.fake_helper import FakeHelper
from core.audio.model import MachineAudio, VirtualMic
from core.camera import model, part as part_module, texts, wire
from core.camera.model import CameraRef, MachineCamera
from core.tests.audio_cluster_harness import Clock, machine_id

MAC = machine_id(1)
PC = machine_id(2)
PC2 = machine_id(4)
KEY = base64.b64encode(bytes(32)).decode()
FLOW = "0a0b0c0d"
FLOW2 = "1a1b1c1d"


class Ctx:
    """MediaContext over a FakeHelper, run on the test's thread (the 'worker'), with test controls."""

    def __init__(self, own, platform, state_dir, clock, video=None):
        self.own = own
        self.platform = platform
        self._clock = clock
        self.clock = clock
        self.wall = clock.wall
        self.logger = logging.getLogger("test.camera")
        self.worker = threading.current_thread()
        self.helper = FakeHelper(platform=platform, video=dict(video or {}, clock=clock))
        self.helper.bind(self._deliver)
        self.events = []
        self.pending = {}
        self.req = 0
        self.posted = []
        self.peers = {}              # peer -> {"features": set, "flow": str|None, "udp": bool}
        self.depth = {}
        self.wakes = []
        self.changes = 0
        self.names = {MAC: "alexandres-macbook-air", PC: "lebig", PC2: "pc2"}
        self.part = None
        self.helper.start()
        self.part = part_module.CameraPart(self, state_dir, platform)
        self.flush()

    @property
    def video(self):
        return self.helper.engine.video

    # MediaContext --------------------------------------------------------------------------------

    def post(self, peer, message):
        assert threading.current_thread() is self.worker, "post off the worker"
        self.posted.append((peer, message))
        return True

    def helper_send(self, command, reply=None):
        assert threading.current_thread() is self.worker
        if reply is not None:
            self.req += 1
            command = dict(command, req=self.req)
            self.pending[self.req] = reply
        self.helper.send(command)

    def helper_state(self):
        return self.helper.state if self.helper.state in ("ok", "starting", "failed") else "starting"

    def helper_features(self):
        return frozenset(self.helper.engine.features + (self.video.features() if self.video else []))

    def flow_for(self, peer):
        entry = self.peers.get(peer)
        return entry["flow"] if entry else None

    def peer_has_udp(self, peer):
        return self.peers.get(peer, {}).get("udp", True)

    def new_stream_id(self):
        self.req += 1
        return 1000 + self.req

    def new_key(self):
        return KEY

    def peers_with(self, feature):
        return frozenset(peer for peer, entry in self.peers.items() if feature in entry["features"])

    def direct(self):
        return frozenset(self.peers)

    def online(self):
        return frozenset(self.peers) | {self.own}

    def audio_machines(self):
        return {ident: MachineAudio(ident, self.names.get(ident, ""), "", 1, "ok", (), (), None, None,
                                    VirtualMic(None, False, False), frozenset(),
                                    frozenset(self.peers) if ident != self.own else frozenset(), "")
                for ident in self.online()}

    def machine_name(self, machine):
        return self.names.get(machine) or f"Machine · {machine[-6:]}"

    def outbox_depth(self, peer):
        return self.depth.get(peer, 0)

    def changed(self):
        self.changes += 1

    def schedule(self, at):
        self.wakes.append(at)

    # Test plumbing --------------------------------------------------------------------------------

    def _deliver(self, event):
        self.events.append(event)

    def flush(self, apply=True):
        """Handle every helper event, as the service would, then apply (one worker iteration)."""
        while self.events:
            event = self.events.pop(0)
            if event.get("ev") in ("ok", "error") and event.get("req") in self.pending:
                self.pending.pop(event["req"])(event)
                continue
            if self.part is not None:
                self.part.helper_event(event)
        if apply and self.part is not None:
            self.part.apply(self._clock())
            if self.events:
                self.flush(apply)

    def add_peer(self, peer, flow=FLOW, features=("camera1", "video1"), udp=True):
        self.peers[peer] = {"features": set(features), "flow": flow, "udp": udp}
        self.helper.engine.flows[flow] = {"peer": peer, "role": "offer", "port": 1, "remote": None, "host": None,
                                          "state": "up"}

    def receive(self, peer, message):
        self.part.link_message(peer, message)
        self.flush()

    def sent(self, kind=None, peer=None):
        return [message for to, message in self.posted
                if (kind is None or message.get("type") == kind) and (peer is None or to == peer)]

    def commands(self, name):
        return self.helper.commands(name)

    def advance(self, seconds):
        self.clock.advance(seconds)
        if self.video is not None:
            self.video.tick()
        self.flush()


def pc_list(rev, in_use=True, install="ok", ident=PC):
    return MachineCamera(id=ident, platform="windows", rev=rev, status="ok", features=frozenset({"virtual_camera"}),
                         cameras=(), preferred=None, permission=None, sharing=None, capture=model.IDLE_CAPTURE,
                         virtual_camera=model.VirtualCamera(install, install == "ok", in_use,
                                                            "Microsoft Teams" if in_use else None,
                                                            (1280, 720, 30) if in_use else None))


def mac_list(rev, **changes):
    machine = MachineCamera(
        id=MAC, platform="macos", rev=rev, status="ok", features=frozenset({"camera_capture"}),
        cameras=(model.Camera("6C707041-05AC-0011-0004-000000000001", "MacBook Air Camera", "builtin", (), True),),
        preferred="6C707041-05AC-0011-0004-000000000001", permission="granted", sharing=True,
        capture=model.IDLE_CAPTURE, virtual_camera=model.NO_VIRTUAL_CAMERA)
    return replace(machine, **changes)


class Base(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="beamer-camera-")
        self.addCleanup(shutil.rmtree, self.dir, True)
        self.clock = Clock()

    def mac(self, video=None, peers=(PC,), state_dir=None):
        ctx = Ctx(MAC, "macos", state_dir or self.dir, self.clock, video)
        for index, peer in enumerate(peers):
            ctx.add_peer(peer, flow=(FLOW, FLOW2)[index])
        ctx.flush()
        return ctx

    def pc(self, video=None):
        ctx = Ctx(PC, "windows", self.dir, self.clock, video)
        ctx.add_peer(MAC)
        ctx.flush()
        return ctx

    def route_to_mac(self, ctx):
        ctx.part.set_camera(MAC, None)
        ctx.flush()

    def demand(self, ctx, rev, in_use=True, peer=PC, **kw):
        ctx.receive(peer, wire.devices(pc_list(rev, in_use, ident=peer, **kw)))


class SenderTests(Base):
    def test_send_and_open_in_the_same_iteration_as_the_demand(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.assertEqual(ctx.commands("send"), [])                  # no demand, no camera
        self.demand(ctx, 1)                                         # one worker iteration
        sends = ctx.commands("send")
        self.assertEqual(len(sends), 1)
        send = sends[0]
        self.assertEqual((send["kind"], send["source"], send["flow"]), ("video", {"camera": None}, FLOW))
        self.assertEqual(send["codec"], wire.codec(1280, 720, 2_500_000))
        self.assertEqual(send["link"], {"bitrate": 1_000_000, "fps": 15})
        opens = ctx.sent(wire.OPEN, PC)
        self.assertEqual(len(opens), 1)
        self.assertEqual(opens[0]["data"]["stream"], send["stream"])
        self.assertEqual(opens[0]["data"]["key"], send["key"])
        self.assertEqual(ctx.part.stream_ids(), frozenset({send["stream"]}))
        ctx.receive(PC, wire.opened(send["stream"], True))
        ctx.video.pli(send["stream"])
        ctx.flush()
        self.assertEqual(ctx.part.snapshot().status.state, "streaming")
        self.assertEqual(ctx.part.snapshot().streams[0].state, "running")

    def test_close_after_the_three_second_hold(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        stream = ctx.commands("send")[0]["stream"]
        ctx.video.pli(stream)
        ctx.flush()
        closed_at = self.clock()
        self.demand(ctx, 2, in_use=False)
        ctx.advance(2.9)
        self.assertTrue(ctx.video.camera_on())
        ctx.advance(0.1)
        self.assertFalse(ctx.video.camera_on())
        self.assertAlmostEqual(self.clock() - closed_at, 3.0, delta=0.1)
        self.assertEqual(ctx.sent(wire.CLOSE, PC)[-1]["data"], {"stream": stream, "reason": "demand"})
        self.assertEqual(ctx.commands("stop")[-1]["stream"], stream)

    def test_quality_change_reconfigures_without_reopening(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        stream = ctx.commands("send")[0]["stream"]
        ctx.part.set_quality("1080p")
        ctx.flush()
        self.assertEqual(len(ctx.commands("send")), 1)
        config = ctx.commands("video_config")
        self.assertEqual(config, [{"cmd": "video_config", "stream": stream, "w": 1920, "h": 1080,
                                   "bitrate": 5_000_000}])
        self.assertEqual(ctx.sent(wire.ROUTE, PC)[-1]["data"]["quality"]["size"], "1080p")

    def test_refusal_stops_at_once_and_waits_for_a_new_token(self):
        for code in wire.OPEN_ERRORS:
            with self.subTest(code=code):
                self.setUp()
                ctx = self.mac()
                self.route_to_mac(ctx)
                self.demand(ctx, 1)
                stream = ctx.commands("send")[0]["stream"]
                self.assertTrue(ctx.video.camera_on())
                ctx.receive(PC, wire.opened(stream, False, code))   # one worker iteration
                self.assertFalse(ctx.video.camera_on())
                self.assertEqual(ctx.commands("stop")[-1]["stream"], stream)
                self.assertEqual(ctx.part.snapshot().sinks[PC].state, "error")
                self.assertEqual(ctx.part.snapshot().sinks[PC].reason, code)
                ctx.advance(120)                                    # no token change: no re-open
                self.assertEqual(len(ctx.commands("send")), 1)
                self.demand(ctx, 2)                                 # T republished (new rev): one re-open
                self.assertEqual(len(ctx.commands("send")), 2)
                ctx.advance(120)
                self.assertLessEqual(len(ctx.commands("send")), 3)  # at most the backoff of no_receiver

    def test_no_receiver_backoff(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        first = ctx.commands("send")[0]["stream"]
        ctx.receive(PC, wire.opened(first, True))
        ctx.advance(3.0)                                            # no PLI: the helper's 3 s guard
        self.assertFalse(ctx.video.camera_on())
        self.assertEqual(ctx.sent(wire.CLOSE, PC)[-1]["data"], {"stream": first, "reason": "no_receiver"})
        times = []
        start = self.clock()
        for _ in range(1300):                                       # 130 s
            before = len(ctx.commands("send"))
            ctx.advance(0.1)
            if len(ctx.commands("send")) > before:
                times.append(round(self.clock() - start, 1))
        # Re-opens at 10 s, then 30 s and 60 s after each failure (each attempt fails 3 s later).
        self.assertEqual(len(times), 3)
        for got, expected in zip(times, (10.0, 43.0, 106.0)):
            self.assertAlmostEqual(got, expected, delta=0.35)
        self.assertIn(start + 10.0, ctx.wakes)                      # the part asks to be woken for it
        # Demand gone: no more attempts; a new demand starts at once with a fresh backoff.
        self.demand(ctx, 2, in_use=False)
        ctx.advance(120)
        count = len(ctx.commands("send"))
        self.demand(ctx, 3)
        self.assertEqual(len(ctx.commands("send")), count + 1)

    def test_running_stream_resets_the_backoff(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        ctx.advance(3.0)
        ctx.advance(10.0)
        stream = ctx.commands("send")[-1]["stream"]
        ctx.video.pli(stream)
        ctx.flush()
        self.assertEqual(ctx.part._failures, {})

    def test_sharing_off_closes_and_is_persisted_before_the_first_plan(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        stream = ctx.commands("send")[0]["stream"]
        ctx.part.set_sharing(False)
        ctx.flush()
        self.assertFalse(ctx.video.camera_on())
        self.assertEqual(ctx.sent(wire.CLOSE, PC)[-1]["data"], {"stream": stream, "reason": "sharing_off"})
        self.assertIs(ctx.sent(wire.DEVICES, PC)[-1]["data"]["sharing"], False)
        self.assertEqual(ctx.commands("video_warm")[-1], {"cmd": "video_warm", "on": False})
        ctx.advance(1.0)                                            # the debounced save
        self.assertIs(json.loads((ctx.part._state_dir / "camera.json").read_text())["sharing"], False)
        # A restarted Beamer: the first plan already has sharing off, so no send ever happens.
        again = Ctx(MAC, "macos", self.dir, self.clock)
        again.add_peer(PC)
        again.part.link_message(PC, wire.devices(pc_list(9)))
        again.flush()
        self.assertEqual(again.commands("send"), [])
        self.assertIs(again.part.snapshot().sharing, False)
        self.assertEqual(again.part.snapshot().status.state, "sharing_off")

    def test_permission_request_only_for_local_mac_actions(self):
        ctx = self.mac(video={"permission": "undetermined"})
        merged = model.CameraRoute(model.CameraEntry(MAC, None, model.Stamp(int(self.clock.wall()) + 5, PC)),
                                   model.QUALITY_DEFAULT, {})
        ctx.receive(PC, wire.route(merged))                         # the PC chose the Mac's camera
        self.demand(ctx, 1)                                         # and an app there opened it
        self.assertEqual(ctx.video.permission_requests, 0)
        self.assertTrue(ctx.part.snapshot().needs_permission)
        self.assertEqual(ctx.commands("send"), [])
        ctx.part.set_camera(MAC, None)                              # the Mac page chose its own camera
        ctx.flush()
        self.assertEqual(ctx.video.permission_requests, 1)
        ctx.part.ask_permission()                                   # "Allow Camera Access…"
        ctx.flush()
        self.assertEqual(ctx.video.permission_requests, 2)
        ctx.part.set_camera(PC, None)                               # not its own camera: no prompt
        ctx.part.dev_command({"set_camera": {"machine": MAC, "device": None}})   # dev: never prompts
        ctx.flush()
        self.assertEqual(ctx.video.permission_requests, 2)
        self.assertEqual(ctx.part.permission_requests, 2)
        ctx.video.set_permission("granted")
        ctx.flush()
        self.assertEqual(len(ctx.commands("send")), 1)

    def test_video_warm_on_changes_and_after_every_ready(self):
        ctx = self.mac()
        self.assertEqual(ctx.commands("video_warm"), [{"cmd": "video_warm", "on": False}])
        self.route_to_mac(ctx)
        on = {"cmd": "video_warm", "on": True, "w": 1280, "h": 720, "bitrate": 2_500_000}
        self.assertEqual(ctx.commands("video_warm")[-1], on)
        self.assertEqual(ctx.commands("send"), [])                  # warm never opens the camera
        ctx.part.set_quality("1080p")
        ctx.flush()
        self.assertEqual(ctx.commands("video_warm")[-1]["w"], 1920)
        count = len(ctx.commands("video_warm"))
        ctx.flush()
        self.assertEqual(len(ctx.commands("video_warm")), count)   # no change, nothing sent
        ctx.helper.crash()
        ctx.flush()
        self.assertEqual(len(ctx.commands("video_warm")), count + 1)
        ctx.video.set_permission("denied")
        ctx.flush()
        self.assertEqual(ctx.commands("video_warm")[-1], {"cmd": "video_warm", "on": False})
        self.assertEqual(ctx.commands("send"), [])

    def test_helper_restart_closes_and_replans(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        stream = ctx.commands("send")[0]["stream"]
        ctx.helper.crash(restart=False)
        ctx.flush()
        self.assertEqual(ctx.sent(wire.CLOSE, PC)[-1]["data"], {"stream": stream, "reason": "helper"})
        self.assertEqual(ctx.part.stream_ids(), frozenset())
        self.assertEqual(ctx.sent(wire.DEVICES, PC)[-1]["data"]["status"], "starting")
        ctx.helper.ready()
        ctx.flush()
        self.assertEqual(len(ctx.commands("send")), 2)
        self.assertNotEqual(ctx.commands("send")[1]["stream"], stream)

    def test_test_source_needs_no_permission(self):
        ctx = self.mac(video={"permission": "undetermined", "dev": True, "cameras": []})
        self.assertTrue(ctx.part.dev_command({"camera_test_source": "synth;start_ms=800"}))
        ctx.part.dev_command({"set_camera": {"machine": MAC, "device": None}})
        ctx.flush()
        self.assertEqual(ctx.sent(wire.DEVICES, PC)[-1]["data"]["test_source"], "synth;start_ms=800")
        self.demand(ctx, 1)
        self.assertEqual(ctx.commands("send")[0]["source"], {"test": "synth;start_ms=800"})
        self.assertEqual(ctx.video.permission_requests, 0)
        self.assertEqual(ctx.part.dev_status()["test_source"], "synth;start_ms=800")

    def test_test_source_ignored_without_a_dev_helper(self):
        ctx = self.mac(video={"permission": "undetermined"})
        ctx.part.dev_command({"camera_test_source": "synth"})
        ctx.part.dev_command({"set_camera": {"machine": MAC, "device": None}})
        ctx.flush()
        self.demand(ctx, 1)
        self.assertEqual(ctx.commands("send"), [])
        self.assertIsNone(ctx.part.dev_status()["test_source"])

    def test_no_camera_one_peer_without_camera1(self):
        ctx = self.mac(peers=())
        ctx.add_peer(PC, features=("video1",))
        ctx.flush()
        self.route_to_mac(ctx)
        ctx.part.link_message(PC, wire.devices(pc_list(1)))
        ctx.flush()
        self.assertEqual([to for to, message in ctx.posted if message["type"].startswith("camera_")], [])
        self.assertEqual(ctx.commands("send"), [])


class LinkOutTests(Base):
    def setUp(self):
        super().setUp()
        self.ctx = self.mac()
        self.route_to_mac(self.ctx)
        self.demand(self.ctx, 1)
        self.stream = self.ctx.commands("send")[0]["stream"]

    def event(self, frame, end, packets=("p1", "p2")):
        return {"ev": "link_out", "flow": FLOW, "p": list(packets), "video": True, "frame": frame, "end": end}

    def frames(self):
        return [message for message in self.ctx.sent("audio_frame", PC)]

    def test_drop_rule(self):
        ctx = self.ctx
        ctx.part.helper_event(self.event(7, False))
        self.assertEqual(len(self.frames()), 1)
        ctx.depth[PC] = 33
        ctx.part.helper_event(self.event(8, False))                 # dropped, and reported
        ctx.depth[PC] = 0
        ctx.part.helper_event(self.event(8, False))                 # later events of frame 8: dropped too
        ctx.part.helper_event(self.event(8, True))
        ctx.part.helper_event(self.event(9, True, ["a", "b", "c", "d", "e"]))   # 5 packets -> 2 messages
        self.assertEqual(len(self.frames()), 3)
        self.assertEqual(ctx.commands("video_link"), [{"cmd": "video_link", "stream": self.stream, "dropped": [8]}])
        self.assertEqual(ctx.part.link_out_dropped, 6)

    def test_pause_after_one_second_and_resume(self):
        ctx = self.ctx
        ctx.depth[PC] = 40
        ctx.part.helper_event(self.event(1, True))
        ctx.advance(0.9)
        self.assertFalse(any("paused" in c for c in ctx.commands("video_link")))
        ctx.advance(0.25)
        self.assertEqual(ctx.commands("video_link")[-1], {"cmd": "video_link", "stream": self.stream, "paused": True})
        ctx.depth[PC] = 5
        low = self.clock()
        while ctx.commands("video_link")[-1].get("paused") is True and self.clock() - low < 5:
            ctx.advance(0.25)                                       # the part's poll period
        self.assertEqual(ctx.commands("video_link")[-1], {"cmd": "video_link", "stream": self.stream, "paused": False})
        self.assertAlmostEqual(self.clock() - low, 2.0, delta=0.3)


class SinkTests(Base):
    def open_from(self, ctx, peer, stream, flow=FLOW):
        msg = wire.open_stream(stream, flow, wire.codec(1280, 720, 2_500_000), KEY, None, model.Stamp(1, peer))
        ctx.receive(peer, msg)

    def route_view(self, ctx, machine, at):
        entry = model.CameraEntry(machine, None, model.Stamp(at, MAC))
        ctx.receive(MAC, wire.route(model.CameraRoute(entry, model.QUALITY_DEFAULT, {})))

    def test_open_from_the_route_source_is_accepted(self):
        ctx = self.pc()
        self.route_view(ctx, MAC, 10)
        self.open_from(ctx, MAC, 77)
        self.assertEqual(ctx.commands("recv")[-1]["sink"], {"virtual_camera": True})
        self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"], {"stream": 77, "ok": True, "error": None})
        ctx.video.recv_running(77)
        ctx.flush()
        self.assertEqual(ctx.part.snapshot().streams[0].state, "running")

    def test_not_source_and_republish_after_the_route_view_changes(self):
        ctx = self.pc()
        ctx.video.set_in_use(True, "Teams")
        ctx.flush()
        rev = ctx.sent(wire.DEVICES, MAC)[-1]["data"]["rev"]
        self.open_from(ctx, MAC, 77)                                # our route view: off
        self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"]["error"], "not_source")
        self.assertEqual(ctx.commands("recv"), [])
        self.route_view(ctx, MAC, 10)
        newest = ctx.sent(wire.DEVICES, MAC)[-1]["data"]
        self.assertGreater(newest["rev"], rev)                      # S sees a new token and retries
        self.open_from(ctx, MAC, 78)
        self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"]["ok"], True)

    def test_refusals_by_install_state_and_helper(self):
        ctx = self.pc(video={"install": "missing"})
        self.route_view(ctx, MAC, 10)
        self.open_from(ctx, MAC, 5)
        self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"]["error"], "not_installed")
        ctx.video.set_vcam(install="outdated")
        ctx.flush()
        self.open_from(ctx, MAC, 6)
        self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"]["error"], "outdated")
        ctx.video.set_vcam(install="ok", present=True)
        ctx.flush()
        self.open_from(ctx, MAC, 7, flow="ffffffff")                 # not our flow with S
        self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"]["error"], "helper")

    def test_a_second_accepted_open_closes_the_first(self):
        ctx = self.pc()
        self.route_view(ctx, MAC, 10)
        self.open_from(ctx, MAC, 77)
        self.open_from(ctx, MAC, 78)
        self.assertEqual(ctx.commands("stop")[-1]["stream"], 77)
        self.assertEqual(ctx.sent(wire.CLOSE, MAC)[-1]["data"], {"stream": 77, "reason": "route"})
        self.assertEqual(ctx.part.stream_ids(), frozenset({78}))
        self.assertEqual(list(ctx.video.recvs), [78])

    def test_camera_refuse_dev_key(self):
        ctx = self.pc()
        self.route_view(ctx, MAC, 10)
        for value, ok, error in (("not_source", False, "not_source"), ("helper", False, "helper"),
                                 ("silent", True, None)):
            ctx.part.dev_command({"camera_refuse": value})
            self.open_from(ctx, MAC, 100)
            self.assertEqual(ctx.sent(wire.OPENED, MAC)[-1]["data"], {"stream": 100, "ok": ok, "error": error})
        self.assertEqual(ctx.commands("recv"), [])                  # silent accepts without a recv
        ctx.part.dev_command({"camera_refuse": None})
        self.open_from(ctx, MAC, 101)
        self.assertEqual(len(ctx.commands("recv")), 1)

    def test_recv_failure_closes_with_error(self):
        ctx = self.pc()
        self.route_view(ctx, MAC, 10)
        self.open_from(ctx, MAC, 77)
        ctx.video.fail_recv(77, "decoder")
        ctx.flush()
        self.assertEqual(ctx.sent(wire.CLOSE, MAC)[-1]["data"], {"stream": 77, "reason": "error"})
        self.assertEqual(ctx.part.stream_ids(), frozenset())

    def test_placeholders_pushed_on_change_and_after_ready_never_twice(self):
        ctx = self.pc()                                             # no page object exists in this test
        first = ctx.commands("vcam")
        self.assertEqual(len(first), 1)
        self.assertEqual(first[0]["idle"], "off")
        self.assertEqual(first[0]["placeholders"],
                         texts.placeholder_texts(ctx.part.snapshot(), ctx.machine_name))
        ctx.flush()
        ctx.advance(5)
        self.assertEqual(len(ctx.commands("vcam")), 1)              # same content: not sent again
        self.route_view(ctx, MAC, 10)
        latest = ctx.commands("vcam")[-1]
        self.assertEqual(latest["idle"], "waiting")
        self.assertEqual(latest["placeholders"]["waiting"]["title"], "Waiting for alexandres-macbook-air")
        self.assertEqual(ctx.video.idle, "waiting")
        count = len(ctx.commands("vcam"))
        ctx.receive(MAC, wire.devices(mac_list(5, sharing=False)))
        self.assertEqual(ctx.commands("vcam")[-1]["idle"], "sharing_off")
        self.assertEqual(len(ctx.commands("vcam")), count + 1)
        ctx.helper.crash()                                          # every ready: sent again
        ctx.flush()
        self.assertEqual(len(ctx.commands("vcam")), count + 2)
        self.assertEqual(ctx.video.idle, "sharing_off")

    def test_refresh_install_sends_an_empty_vcam(self):
        ctx = self.pc(video={"install": "missing"})
        self.assertEqual(ctx.part.snapshot().vcam_install_offer, "install")
        ctx.video.vcam.update(install="ok", present=True, dll_version=1)
        ctx.part.refresh_install()
        ctx.flush()
        self.assertIn({"cmd": "vcam"}, ctx.commands("vcam"))
        self.assertIsNone(ctx.part.snapshot().vcam_install_offer)
        self.assertEqual(ctx.part.snapshot().vcam_local, model.LocalVcam(1, 1, 1))

    def test_mac_never_sends_vcam_and_pc_never_warm(self):
        mac = self.mac()
        self.assertEqual(mac.commands("vcam"), [])
        pc = self.pc()
        self.assertEqual(pc.commands("video_warm"), [])
        pc.part.ask_permission()
        self.assertEqual(pc.commands("permission_request"), [])


class StateTests(Base):
    def test_route_and_lists_flood_and_persist(self):
        ctx = self.mac(peers=(PC, PC2))
        ctx.receive(PC, wire.devices(pc_list(3)))
        self.assertEqual([msg["data"]["machine"] for msg in ctx.sent(wire.DEVICES, PC2)][-1], PC)
        ctx.part.set_camera(MAC, CameraRef("6C707041-05AC-0011-0004-000000000001", "MacBook Air Camera"))
        ctx.flush()
        self.assertEqual(len(ctx.sent(wire.ROUTE, PC)), 2)          # greeting + the change
        ctx.advance(1)
        stored = json.loads((ctx.part._state_dir / "camera.json").read_text())
        self.assertEqual(stored["route"]["camera"]["machine"], MAC)
        self.assertIn(PC, stored["machines"])
        older = model.CameraRoute(model.CameraEntry(None, None, model.Stamp(1, PC)), model.QUALITY_DEFAULT, {})
        before = len(ctx.sent(wire.ROUTE, PC))
        ctx.receive(PC, wire.route(older))                          # older: we answer with ours
        self.assertEqual(len(ctx.sent(wire.ROUTE, PC)), before + 1)
        self.assertEqual(ctx.part.snapshot().route.camera.machine, MAC)

    def test_own_list_from_a_peer_is_never_taken(self):
        ctx = self.mac()
        rev = ctx.sent(wire.DEVICES, PC)[-1]["data"]["rev"]
        forged = mac_list(rev + 10, sharing=False)
        ctx.receive(PC, wire.devices(forged))
        self.assertIs(ctx.part.snapshot().sharing, True)
        self.assertGreater(ctx.sent(wire.DEVICES, PC)[-1]["data"]["rev"], rev + 10)   # republished above it

    def test_sharing_of_a_machine_comes_only_from_its_list(self):
        ctx = self.pc()
        ctx.add_peer(PC2, flow=FLOW2)
        ctx.receive(MAC, wire.devices(mac_list(5, sharing=False)))
        forged = wire.devices(pc_list(4, ident=PC2))
        forged["data"]["sharing"] = True                            # a PC's list cannot carry sharing
        ctx.receive(PC2, forged)
        snap = ctx.part.snapshot()
        self.assertIs(snap.machines[MAC].sharing, False)
        self.assertIsNone(snap.machines[PC2].sharing)

    def test_dev_commands_and_status(self):
        ctx = self.mac()
        self.assertFalse(ctx.part.dev_command({"helper_dev": {"video_loss": {"pct": 1}}}))
        self.assertTrue(ctx.part.dev_command({"set_quality": "1080p"}))
        self.assertTrue(ctx.part.dev_command({"camera_sharing": False}))
        self.assertTrue(ctx.part.dev_command({"set_camera": {"machine": MAC,
                                                             "device": "6C707041-05AC-0011-0004-000000000001"}}))
        ctx.flush()
        snap = ctx.part.snapshot()
        self.assertEqual(snap.route.quality.size, "1080p")
        self.assertEqual(snap.route.camera.device, CameraRef("6C707041-05AC-0011-0004-000000000001",
                                                            "MacBook Air Camera"))
        self.assertIs(snap.sharing, False)
        status = ctx.part.dev_status()
        json.dumps(status)
        self.assertEqual(status["snapshot"]["route"]["quality"]["size"], "1080p")
        self.assertEqual(status["snapshot"]["status"]["state"], "sharing_off")

    def test_link_down_stops_streams_without_messages(self):
        ctx = self.mac()
        self.route_to_mac(ctx)
        self.demand(ctx, 1)
        stream = ctx.commands("send")[0]["stream"]
        posted = len(ctx.posted)
        del ctx.peers[PC]
        ctx.part.link_down(PC)
        ctx.flush()
        self.assertFalse(ctx.video.camera_on())
        self.assertEqual(ctx.commands("stop")[-1]["stream"], stream)
        self.assertEqual(len(ctx.posted), posted)

    def test_link_messages_do_not_touch_the_disk(self):
        ctx = self.mac()
        path = ctx.part._state_dir / "camera.json"
        ctx.advance(1)
        mtime = path.stat().st_mtime_ns
        ctx.part.link_message(PC, wire.devices(pc_list(1)))         # the handler itself: no save
        self.assertEqual(path.stat().st_mtime_ns, mtime)
        self.assertIsNotNone(ctx.part._save_at)


if __name__ == "__main__":
    unittest.main()
