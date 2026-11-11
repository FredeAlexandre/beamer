# SPDX-License-Identifier: GPL-3.0-only
"""N AudioServices on in-memory links, in lockstep rounds (dev/audio/PHASE2.md §11.1).

Each node is a machine: an AudioService with a FakeHelper, or (`audio=False`) a Beamer without the
`audio` cap that only counts what reaches it. A link is up or down; messages posted on it wait in
flight until the next `round()`, which delivers every message then in flight and lets every service
handle everything queued. Taking a link down drops what is in flight on it, as a closed TCP link does.
Every post to a peer whose caps lack `audio` is recorded in `violations`.

Camera (dev/camera/PHASE2.md §10.1): `add(..., camera=True)` gives the node a CameraPart (inside its
AudioService, see `camera_service`) and a fake helper with a video side on the harness's `VideoNet`.
Every `camera_*` post to a peer without `audio` or without `camera1` is in `camera_violations`;
`check_camera()` is the camera-on invariant checker (§6.5), run after every `advance` step.
"""

import shutil
import tempfile
import threading

from core import protocol
from core.audio import service as service_module
from core.audio.fake_helper import FakeHelper
from core.camera.fake_video import VideoNet

BASE_CAPS = ("clipboard", "clipboard_image", "gestures", "media_keys", "text", "settings", "design_sync",
             "layout", "profiles", "displays", "ping_rtt", "identify_screens", "motion_path")


def machine_id(seed):
    return protocol.id_text(bytes([seed]) * 16)


class Clock:
    def __init__(self, start=1000.0, wall=1793000000.0):
        self.now = start
        self.wall_now = wall

    def __call__(self):
        return self.now

    def wall(self):
        return self.wall_now

    def advance(self, seconds):
        self.now += seconds
        self.wall_now += seconds


class Host:
    """AudioHost of one node: posts go in flight on the harness's links."""

    def __init__(self, net, node):
        self.net = net
        self.node = node
        self.depth = 0
        self.require_thread = None
        self.posted = []

    def own_id(self):
        return self.node.id

    def own_name(self):
        return self.node.name

    def platform(self):
        return self.node.platform

    def post(self, peer, message):
        if self.require_thread is not None:
            assert threading.current_thread() is self.require_thread(), "post off the worker"
        self.posted.append((peer, message))
        return self.net.post(self.node.id, peer, message)

    def outbox_depth(self, peer):
        return self.depth

    def peer_host(self, peer):
        return self.net.nodes[peer].address if peer in self.net.nodes else None

    def peer_name(self, peer):
        return self.net.nodes[peer].name if peer in self.net.nodes else ""


class Node:
    def __init__(self, net, seed, name, platform, audio, state_dir, devices=None, features=None, camera=False,
                 video=None):
        self.net = net
        self.camera = bool(camera and audio)
        self.video_options = dict(video or {}, clock=net.clock)
        self.demand_until = 0.0     # the harness's truth of this sink's demand (in use, or its 3 s hold)
        self.id = machine_id(seed)
        self.name = name
        self.platform = platform
        self.audio = audio
        self.address = f"192.168.1.{seed}"
        self.caps = frozenset(BASE_CAPS + (("audio",) if audio else ()))
        self.received = []
        self.state_dir = state_dir
        self.host = Host(net, self)
        self.helper = None
        self.service = None
        if audio:
            self.helper = self._new_helper(devices, features)
            self.service = self._new_service()

    def _new_helper(self, devices, features):
        helper = FakeHelper(platform=self.platform, devices=devices, features=features,
                            video=self.video_options if self.camera else False)
        if self.camera:
            self.net.video.register(self.id, helper.engine.video)
        return helper

    def _new_service(self):
        def factory(cmd, deliver):
            self.helper.cmd = cmd
            self.helper.bind(deliver)
            return self.helper
        kind = camera_service() if self.camera else service_module.AudioService
        return kind(self.host, self.state_dir, ["beamer-audio"], clock=self.net.clock,
                    wall=self.net.clock.wall, helper_factory=factory)

    def restart(self):
        """Quit Beamer on this machine and start it again (same state dir, fresh helper)."""
        self.service.stop()
        self.service.pump()
        old = self.helper.engine
        self.helper = self._new_helper(old.devices, old.features)
        if self.camera:
            video = self.helper.engine.video
            video.cameras, video.preferred, video.permission = old.video.cameras, old.video.preferred, old.video.permission
            video.vcam.update(old.video.vcam, in_use=False, app=None, want=None)
            self.demand_until = min(self.demand_until, self.net.clock())
        self.service = self._new_service()
        self.service.start(run_thread=False)

    @property
    def part(self):
        """The node's CameraPart (`camera` on the stand-in; CW6's service may name it `_camera`)."""
        if not self.camera:
            return None
        return getattr(self.service, "camera", None) or getattr(self.service, "_camera", None)

    def camera_dev(self, item):
        """A §7.4 dev command item, through the worker as the dev command file would deliver it."""
        if hasattr(self.service, "camera_dev"):
            self.service.camera_dev(item)
        else:
            self.service._queue.put(("call", "_dev_command", (item,)))

    @property
    def video(self):
        return self.helper.engine.video if self.camera else None

    def camera_snapshot(self):
        return self.part.snapshot()

    def set_in_use(self, in_use, app=None):
        """An app on this PC opens (True) or closes Beamer Camera."""
        self.video.set_in_use(in_use, app)
        self.demand_until = float("inf") if in_use else min(self.demand_until, self.net.clock() + 3.0)

    def snapshot(self):
        return self.service.snapshot()


class Net:
    def __init__(self):
        self.clock = Clock()
        self.nodes = {}
        self.links = set()
        self.in_flight = []
        self.violations = []
        self.camera_violations = []
        self.invariant_failures = []
        self.video = VideoNet()
        self._unwanted = {}         # (S id, stream) -> since when no receiver wants that send
        self.delivered = 0
        self._dirs = []

    def add(self, seed, name, platform="macos", audio=True, devices=None, features=None, camera=False, video=None):
        state_dir = tempfile.mkdtemp(prefix="beamer-audio-")
        self._dirs.append(state_dir)
        node = Node(self, seed, name, platform, audio, state_dir, devices, features, camera, video)
        self.nodes[node.id] = node
        if node.service is not None:
            node.service.start(run_thread=False)
        return node

    def close(self):
        for node in self.nodes.values():
            if node.service is not None:
                node.service.stop()
        for path in self._dirs:
            shutil.rmtree(path, ignore_errors=True)

    def audio_nodes(self):
        return [node for node in self.nodes.values() if node.audio]

    def up(self, a, b):
        return frozenset((a, b)) in self.links

    def post(self, source, target, message):
        if target not in self.nodes or not self.up(source, target):
            return False
        if "audio" not in self.nodes[target].caps:
            self.violations.append((source, target, message.get("type")))
        if str(message.get("type", "")).startswith("camera_") and not self.nodes[target].camera:
            self.camera_violations.append((source, target, message.get("type")))
        self.in_flight.append((source, target, message))
        return True

    def link(self, a, b):
        pair = frozenset((a.id, b.id))
        if pair in self.links:
            return
        self.links.add(pair)
        for one, other in ((a, b), (b, a)):
            if one.service is not None:
                one.service.link_up(other.id, other.caps)

    def unlink(self, a, b):
        pair = frozenset((a.id, b.id))
        if pair not in self.links:
            return
        self.links.discard(pair)
        self.in_flight = [item for item in self.in_flight if frozenset(item[:2]) != pair]
        for one, other in ((a, b), (b, a)):
            if one.service is not None:
                one.service.link_down(other.id)

    def pump(self):
        for node in self.audio_nodes():
            if node.camera and node.helper.state == "ok":
                node.video.tick()
            node.service.pump()

    def cut_udp(self, a, b, cut=True):
        """UDP between two machines stops (or works again); the Beamer link is untouched."""
        self.video.cut_udp(a.id, b.id, cut)

    def advance(self, seconds, step=0.1):
        """Let `seconds` pass in steps: messages are delivered, fake engines tick, services pump, and the
        camera-on invariant is checked after every step."""
        remaining = seconds
        while remaining > 1e-9:
            this = min(step, remaining)
            self.round(this)
            self.check_camera()
            remaining -= this

    def check_camera(self, start_bound=3.5, running_bound=5.5):
        """The camera-on invariant (§6.5): no Mac's fake camera stays on longer than `start_bound` (a
        stream that never ran) or `running_bound` (one that ran) without a receiver that wants it, i.e.
        a sink with demand (or in its 3 s hold) whose helper holds a recv for that stream; a Mac with
        sharing off never has a send; refusals never re-open without a new retry token. Failures go
        to `invariant_failures`; returns how many were added."""
        now = self.clock()
        before = len(self.invariant_failures)
        live = set()
        for node in self.nodes.values():
            if not node.camera or node.video is None:
                continue
            if node.part is not None and node.part._sharing is False and node.video.sends:
                self.invariant_failures.append((now, node.name, "send while sharing is off"))
            for stream, send in node.video.sends.items():
                key = (node.id, stream)
                live.add(key)
                target = self.nodes.get(send["peer"])
                wanted = (target is not None and target.camera and target.demand_until > now
                          and target.video is not None and stream in target.video.recvs)
                record = self._unwanted.setdefault(key, {"since": None, "ran": False})
                record["ran"] = record["ran"] or send["state"] == "running"
                if wanted:
                    record["since"] = None
                    continue
                if record["since"] is None:
                    record["since"] = now if send["state"] == "running" else min(now, send["since"])
                bound = running_bound if record["ran"] else start_bound
                if now - record["since"] > bound + 1e-6:
                    self.invariant_failures.append((now, node.name, f"camera on {now - record['since']:.2f} s "
                                                                    f"without a receiver (stream {stream})"))
                    record["since"] = now      # report once per bound
            for peer, error, old, new, _at in getattr(node.part, "reopens", ()):
                if error != "no_receiver" and old == new:
                    self.invariant_failures.append((now, node.name, f"re-open after {error} without a new token"))
            if node.part is not None:
                node.part.reopens = [entry for entry in node.part.reopens if entry[1] == "no_receiver"]
        self._unwanted = {key: value for key, value in self._unwanted.items() if key in live}
        return len(self.invariant_failures) - before

    def round(self, step=0.01):
        """Deliver what is in flight, then let every service handle it. The number delivered."""
        batch, self.in_flight = self.in_flight, []
        for source, target, message in batch:
            node = self.nodes[target]
            node.received.append((source, message))
            if node.service is not None:
                node.service.link_message(source, message)
        self.delivered += len(batch)
        self.clock.advance(step)
        self.pump()
        return len(batch)

    def settle(self, limit=200, step=0.01):
        """Rounds until nothing is in flight; the number of rounds that carried messages."""
        self.pump()
        rounds = 0
        while self.in_flight and rounds < limit:
            self.round(step)
            rounds += 1
        return rounds


_CAMERA_SERVICE = None


def camera_service():
    """The AudioService class with a CameraPart in its worker.

    CW6 adds the part to `core.audio.service.AudioService` itself (PHASE2 §2.2); until then this
    subclass is a test-only stand-in for that hunk: it implements `MediaContext` and routes helper
    events, `camera_*` messages, link changes and settles to the part, as §2.2 and §7.1 describe.
    Once the real service has the part (`camera_snapshot`), the real class is used unchanged."""
    global _CAMERA_SERVICE
    if hasattr(service_module.AudioService, "camera_snapshot"):
        return service_module.AudioService
    if _CAMERA_SERVICE is None:
        _CAMERA_SERVICE = _make_camera_service()
    return _CAMERA_SERVICE


def _make_camera_service():
    from core.audio import model as audio_model
    from core.camera import part as part_module
    from core.camera import wire as camera_wire

    class CameraAudioService(service_module.AudioService):
        def __init__(self, host, state_dir, helper_cmd, **kwargs):
            super().__init__(host, state_dir, helper_cmd, **kwargs)
            self._camera_wake = None
            self.camera = part_module.CameraPart(self, state_dir, host.platform())

        # MediaContext ---------------------------------------------------------------------------

        @property
        def own(self):
            return self._own

        @property
        def clock(self):
            return self._clock

        @property
        def wall(self):
            return self._wall

        @property
        def logger(self):
            return self._log

        def post(self, peer, message):
            return self._post(peer, message)

        def helper_send(self, command, reply=None):
            self._helper_send(command, reply)

        def helper_state(self):
            return self._helper_state

        def helper_features(self):
            return frozenset(self._helper_info.get("features", ()))

        def flow_for(self, peer):
            state = self._peers.get(peer)
            if state is None or state.flow is None or not state.flow.agreed:
                return None
            return state.flow.id

        def peer_has_udp(self, peer):
            return service_module._usable_host(self._host.peer_host(peer))

        def new_stream_id(self):
            return self._new_stream()

        def new_key(self):
            return service_module._key()

        def peers_with(self, feature):
            return frozenset(ident for ident, peer in self._peers.items()
                             if peer.direct and feature in peer.hello.get("features", ()))

        def direct(self):
            return self._direct()

        def online(self):
            return audio_model.online(self._own, self._direct(), self._machines)

        def audio_machines(self):
            return dict(self._machines)

        def machine_name(self, machine):
            if machine == self._own:
                return self._host.own_name()
            held = self._machines.get(machine)
            if held is not None and held.name:
                return held.name
            name = self._host.peer_name(machine)
            return name or f"Machine · {(machine or '')[-6:]}"

        def outbox_depth(self, peer):
            return self._host.outbox_depth(peer)

        def changed(self):
            for fn in self._listeners(self._subscribers):
                self._call(fn)

        def schedule(self, at):
            self._camera_wake = at if self._camera_wake is None else min(self._camera_wake, at)

        def camera_snapshot(self):
            return self.camera.snapshot()

        # Public camera API (§7.1): any thread, through the worker's queue.

        def set_camera(self, machine, device):
            self._queue.put(("call", "_camera_call", ("set_camera", (machine, device))))

        def set_quality(self, size):
            self._queue.put(("call", "_camera_call", ("set_quality", (size,))))

        def set_camera_sharing(self, on):
            self._queue.put(("call", "_camera_call", ("set_sharing", (bool(on),))))

        def ask_camera_permission(self):
            self._queue.put(("call", "_camera_call", ("ask_permission", ())))

        def refresh_camera_install(self):
            self._queue.put(("call", "_camera_call", ("refresh_install", ())))

        def camera_dev(self, item):
            self._queue.put(("call", "_camera_call", ("dev_command", (item,))))

        def _camera_call(self, method, args):
            getattr(self.camera, method)(*args)

        # Routing (the CW6 hunk's job) ----------------------------------------------------------

        def _hello(self):
            message = super()._hello()
            if camera_wire.announce(True, self._helper_info.get("features", ())):
                message["data"]["features"] = sorted(set(message["data"]["features"]) | {camera_wire.FEATURE})
            return message

        def _new_stream(self):
            used = set(self.camera.stream_ids()) if hasattr(self, "camera") else set()
            while True:
                stream = super()._new_stream()
                if stream not in used:
                    return stream

        def _on_link_up(self, peer, caps):
            super()._on_link_up(peer, caps)
            self.camera.link_up(peer)

        def _on_link_down(self, peer):
            super()._on_link_down(peer)
            self.camera.link_down(peer)

        def _on_link_message(self, peer, message):
            if camera_wire.is_camera(message):
                state = self._peers.get(peer)
                if state is not None and state.audio:
                    self.camera.link_message(peer, message)
                return
            super()._on_link_message(peer, message)

        def _on_helper(self, event):
            if self.camera.helper_event(event):
                return
            super()._on_helper(event)

        def _next_deadline(self):
            deadlines = [moment for moment in (super()._next_deadline(), self._camera_wake) if moment is not None]
            return min(deadlines) if deadlines else None

        def _tick(self):
            if self._camera_wake is not None and self._clock() >= self._camera_wake:
                self._camera_wake = None
                self._dirty = True
            super()._tick()

        def _settle(self):
            super()._settle()
            self.camera.apply(self._clock())

        def _shutdown(self):
            self.camera.shutdown()
            super()._shutdown()

    return CameraAudioService
