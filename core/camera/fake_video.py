# SPDX-License-Identifier: GPL-3.0-only
"""The video side of the fake helper, for tests (dev/camera/PHASE2.md §3, §6.5, §10.1).

`FakeVideo` answers the video commands of §3.2 with the events of §3.3 a real helper would send:
cameras and permission (Mac), the Beamer Camera state (Windows), video send and recv streams. It
moves no picture. `core.audio.fake_helper.FakeEngine` hands it every command it does not know, and
the video `send`/`recv`/`stop` ones.

The start PLI of a send stream (§4.7) and the receiver's feedback come through a `VideoNet` shared
by the fake helpers of a test cluster: a send stream on S runs once T's fake helper has a `recv` for
that stream id and the test has not cut UDP between them. Without one, it starts only when the test
calls `pli()`. The engine's camera-on guards (§6.5) are modelled on the injected clock: no PLI
within 3 s of `send`, or no feedback for 5 s on a running stream, ends it `failed no_receiver`.
"""

import copy
import time

START_GUARD = 3.0       # s: no start PLI within this of `send` -> no_receiver (§6.5)
FEEDBACK_GUARD = 5.0    # s: a running stream with no feedback this long -> no_receiver
MAC_CAMERA_KEY = "6C707041-05AC-0011-0004-000000000001"


def default_cameras():
    return [{"key": MAC_CAMERA_KEY, "name": "MacBook Air Camera", "transport": "builtin",
             "native": ["1920x1080@30", "1280x720@30", "640x480@30"], "preferred": True}]


class VideoNet:
    """The fake video network of a test cluster: which fake helpers can reach each other by UDP."""

    def __init__(self):
        self.helpers = {}           # machine id -> FakeVideo
        self.cut = set()            # frozenset pairs with UDP cut

    def register(self, machine, video):
        self.helpers[machine] = video
        video.network = self
        video.machine = machine

    def cut_udp(self, a, b, cut=True):
        pair = frozenset((a, b))
        if cut:
            self.cut.add(pair)
        else:
            self.cut.discard(pair)

    def udp_ok(self, a, b):
        return frozenset((a, b)) not in self.cut

    def recv_of(self, machine, stream):
        video = self.helpers.get(machine)
        return None if video is None else video.recvs.get(stream)

    def send_of(self, machine, stream):
        video = self.helpers.get(machine)
        return None if video is None else video.sends.get(stream)


class FakeVideo:
    """Video commands without video: `handle(command)` -> (events, error) or None (not video)."""

    def __init__(self, engine, platform="macos", clock=None, dev=False, permission="granted", cameras=None,
                 install="ok"):
        self.engine = engine
        self.platform = platform
        self.clock = clock or time.monotonic
        self.dev = dev
        self.network = None
        self.machine = None
        self.emit = lambda events: None     # set by FakeHelper: deliver events outside a command
        self.cameras = copy.deepcopy(default_cameras() if cameras is None else cameras)
        self.preferred = next((camera["key"] for camera in self.cameras if camera.get("preferred")), None)
        self.permission = permission
        self.prompt_answer = None           # tests: what the macOS prompt answers (None: nobody answers)
        self.vcam = {"install": install, "dll_version": 1 if install == "ok" else 0, "loaded_version": 1,
                     "bundled_version": 1, "present": install == "ok", "creating": False, "in_use": False,
                     "app": None, "want": None, "error": None}
        self.placeholders = None
        self.idle = None
        self.warm = None
        self.sends = {}       # stream -> {"flow", "peer", "source", "codec", "state", "since", "fed", "device"}
        self.recvs = {}       # stream -> {"flow", "peer", "state", "codec"}
        self.recv_error = None              # tests: an error code every video `recv` gets
        self.send_error = None              # tests: an error code every video `send` gets
        self.permission_requests = 0
        self.video_links = []
        self.dev_commands = []
        self.camera_starts = 0

    # The helper's words -------------------------------------------------------------------------

    def features(self):
        words = []
        if self.platform == "macos":
            words = ["video1", "camera_capture"] + (["camera_synth"] if self.dev else [])
        elif self.platform == "windows":
            words = ["video1", "virtual_camera"]
        return words

    def ready_events(self):
        if self.platform == "macos":
            return [self.cameras_event()]
        if self.platform == "windows":
            return [self.vcam_event()]
        return []

    def cameras_event(self):
        cameras = [dict(camera, preferred=camera["key"] == self.preferred) for camera in self.cameras]
        return {"ev": "cameras", "cameras": copy.deepcopy(cameras), "preferred": self.preferred,
                "permission": self.permission}

    def vcam_event(self):
        return dict({"ev": "vcam"}, **copy.deepcopy(self.vcam))

    # Commands -----------------------------------------------------------------------------------

    def handle(self, command):
        name = command.get("cmd")
        if name in ("send", "recv"):
            if command.get("kind") != "video":
                return None
        elif name == "stop":
            if command.get("stream") not in self.sends and command.get("stream") not in self.recvs:
                return None
        method = getattr(self, f"_cmd_{name}", None) if isinstance(name, str) else None
        if method is None:
            return None
        return method(command)

    def _cmd_send(self, command):
        if "video1" not in self.features() or self.platform != "macos":
            return [], "unsupported"
        flow = self.engine.flows.get(command["flow"])
        if flow is None:
            return [], "unknown_flow"
        source = command["source"]
        if not isinstance(source, dict) or not isinstance(command.get("codec"), dict):
            return [], "bad_args"
        if "test" in source:
            if not self.dev:
                return [], "bad_args"
            device = "synth"
        else:
            if self.send_error:
                return [], self.send_error
            if self.permission != "granted":
                return [], "permission"
            key = source.get("camera") or self.preferred
            if key is None or key not in {camera["key"] for camera in self.cameras}:
                return [], "no_camera"
            device = key
        stream = command["stream"]
        self.sends[stream] = {"flow": command["flow"], "peer": flow.get("peer"), "source": source,
                              "codec": dict(command["codec"]), "state": "starting", "since": self.clock(),
                              "fed": None, "device": device}
        self.camera_starts += 1
        return [{"ev": "stream_state", "stream": stream, "state": "starting", "device": device},
                {"ev": "camera", "stream": stream, "state": "starting", "device": device,
                 "native": "1920x1080@30", "format": f"{command['codec'].get('w')}x{command['codec'].get('h')}@30",
                 "fps": 0.0, "by_other": False, "effects": [], "encoder": "standby", "encoder_ms": 1,
                 "camera_ms": 5}], None

    def _cmd_recv(self, command):
        if "virtual_camera" not in self.features():
            return [], "unsupported"
        if command["flow"] not in self.engine.flows:
            return [], "unknown_flow"
        if self.recv_error:
            return [], self.recv_error
        if self.vcam["install"] != "ok":
            return [], "not_installed"
        if self.recvs:
            return [], "internal"        # one sink at a time (§2.4 open_sink)
        flow = self.engine.flows[command["flow"]]
        self.recvs[command["stream"]] = {"flow": command["flow"], "peer": flow.get("peer"), "state": "starting",
                                         "codec": dict(command.get("codec") or {})}
        return [{"ev": "stream_state", "stream": command["stream"], "state": "starting", "device": "vcam"}], None

    def _cmd_stop(self, command):
        stream = command["stream"]
        if self.sends.pop(stream, None) is not None:
            return [{"ev": "camera", "stream": stream, "state": "stopped"},
                    {"ev": "stream_state", "stream": stream, "state": "stopped", "device": None}], None
        self.recvs.pop(stream, None)
        return [{"ev": "stream_state", "stream": stream, "state": "stopped", "device": None}], None

    def _cmd_video_config(self, command):
        send = self.sends.get(command.get("stream"))
        if send is None:
            return [], "unknown_stream"
        for name in ("w", "h", "fps", "bitrate"):
            if name in command:
                send["codec"][name] = command[name]
        return [], None

    def _cmd_video_warm(self, command):
        self.warm = dict(command)
        return [], None

    def _cmd_video_link(self, command):
        self.video_links.append(dict(command))
        return [], None

    def _cmd_cameras(self, command):
        if self.platform != "macos":
            return [], "unsupported"
        return [self.cameras_event()], None

    def _cmd_permission_request(self, command):
        if self.platform != "macos" or command.get("what") != "camera":
            return [], "unsupported"
        self.permission_requests += 1
        if self.prompt_answer is not None and self.permission == "undetermined":
            self.permission = self.prompt_answer
            return [self.cameras_event(), {"ev": "permission", "camera": self.permission}], None
        return [], None

    def _cmd_vcam(self, command):
        if self.platform != "windows":
            return [], "unsupported"
        if isinstance(command.get("placeholders"), dict):
            self.placeholders = copy.deepcopy(command["placeholders"])
        if isinstance(command.get("idle"), str):
            self.idle = command["idle"]
        return [self.vcam_event()], None

    def _cmd_dev(self, command):
        if not self.dev:
            return [], "bad_args"
        self.dev_commands.append(dict(command))
        return [], None

    # Engine-side behaviour ----------------------------------------------------------------------

    def flow_closed(self, flow):
        events = []
        for table in (self.sends, self.recvs):
            for stream in [stream for stream, value in table.items() if value["flow"] == flow]:
                del table[stream]
                events.append({"ev": "stream_state", "stream": stream, "state": "stopped", "device": None})
        return events

    def reset(self):
        """The helper died: every stream is gone."""
        self.sends.clear()
        self.recvs.clear()
        self.placeholders = None
        self.idle = None
        self.warm = None

    def camera_on(self):
        return bool(self.sends)

    def _receiver_feeds(self, stream, send):
        """Whether the receiver of `stream` answers (PLI / RR) through the fake network."""
        if self.network is None or send["peer"] is None:
            return False
        if not self.network.udp_ok(self.machine, send["peer"]):
            return False
        recv = self.network.recv_of(send["peer"], stream)
        return recv is not None and recv["flow"] == send["flow"]

    def tick(self):
        """Advance the fake engine to the clock's time; emits what happened."""
        now = self.clock()
        events = []
        for stream, send in list(self.sends.items()):
            fed = self._receiver_feeds(stream, send)
            if send["state"] == "starting":
                if fed:
                    events += self._run(stream, send, now)
                elif now - send["since"] >= START_GUARD:
                    events += self._no_receiver(stream)
            elif send["state"] == "running":
                if fed:
                    send["fed"] = now
                elif now - send["fed"] >= FEEDBACK_GUARD:
                    events += self._no_receiver(stream)
        if self.network is not None:
            for stream, recv in self.recvs.items():
                sender = self.network.send_of(recv["peer"], stream) if recv["peer"] else None
                if (recv["state"] == "starting" and sender is not None and sender["state"] == "running"
                        and self.network.udp_ok(self.machine, recv["peer"])):
                    recv["state"] = "running"
                    events.append({"ev": "stream_state", "stream": stream, "state": "running", "device": "vcam"})
        if events:
            self.emit(events)
        return events

    def _run(self, stream, send, now):
        send["state"], send["fed"] = "running", now
        return [{"ev": "camera", "stream": stream, "state": "running", "device": send["device"],
                 "native": "1920x1080@30", "format": f"{send['codec'].get('w')}x{send['codec'].get('h')}@30",
                 "fps": 30.0, "by_other": False, "effects": []},
                {"ev": "stream_state", "stream": stream, "state": "running", "device": send["device"]}]

    def _no_receiver(self, stream):
        del self.sends[stream]
        return [{"ev": "camera", "stream": stream, "state": "stopped"},
                {"ev": "stream_state", "stream": stream, "state": "failed", "error": "no_receiver", "device": None}]

    # Test controls ------------------------------------------------------------------------------

    def pli(self, stream):
        """The receiver's start PLI reached S (tests without a VideoNet)."""
        send = self.sends.get(stream)
        if send is not None and send["state"] == "starting":
            self.emit(self._run(stream, send, self.clock()))
        elif send is not None:
            send["fed"] = self.clock()

    def recv_running(self, stream):
        recv = self.recvs.get(stream)
        if recv is not None and recv["state"] != "running":
            recv["state"] = "running"
            self.emit([{"ev": "stream_state", "stream": stream, "state": "running", "device": "vcam"}])

    def fail_recv(self, stream, error="decoder"):
        if self.recvs.pop(stream, None) is not None:
            self.emit([{"ev": "stream_state", "stream": stream, "state": "failed", "error": error, "device": None}])

    def set_permission(self, permission):
        self.permission = permission
        self.emit([self.cameras_event(), {"ev": "permission", "camera": permission}])

    def set_cameras(self, cameras, preferred=None):
        self.cameras = copy.deepcopy(cameras)
        self.preferred = preferred
        self.emit([self.cameras_event()])

    def set_vcam(self, **changes):
        self.vcam.update(copy.deepcopy(changes))
        self.emit([self.vcam_event()])

    def set_in_use(self, in_use, app=None, want=None):
        self.set_vcam(in_use=bool(in_use), app=app if in_use else None,
                      want=(want or {"w": 1280, "h": 720, "fps": 30}) if in_use else None)
