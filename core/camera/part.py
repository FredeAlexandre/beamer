# SPDX-License-Identifier: GPL-3.0-only
"""CameraPart: the camera control plane of one machine (dev/camera/PHASE2.md §5, §6, §7).

It runs inside the AudioService worker and reaches the helper, the links and the audio state only
through a narrow `MediaContext` (the service implements it, CW6). Every method runs on that worker:
the service routes helper events, `camera_*` link messages, link changes and local actions to it,
and calls `apply()` after every audio settle and when the part's wake time comes.

What it does:
- publishes this machine's `camera_devices` (cameras, permission, the Mac-local sharing switch,
  capture state, Beamer Camera install and demand) and floods the cluster-wide `camera_route` (§5.3);
- plans (`planner.plan_camera`) and diffs the video streams this machine sends: `send` + `camera_open`
  at once on demand (Deviation 3), `video_config` on a size change, close on route/demand/sharing/
  permission changes (§6.4 rule 4);
- keeps the camera-on invariant (§6.5): a refusal stops the helper stream in the same iteration; a
  failed stream re-opens only when its retry token changes, or on the 10/30/60 s backoff after
  `no_receiver`;
- on a sink (PC): accepts `camera_open` only from its route's camera machine, one recv at a time, and
  pushes the placeholder texts and idle state to the helper (`vcam`, §7.1);
- applies Beamer's link-outbox drop rule to video `link_out` (§4.10);
- the dev keys of §7.4.
"""

import dataclasses
import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable, Mapping, Optional, Protocol

from core.audio import wire as audio_wire
from core.audio.model import MachineAudio
from core.camera import model, planner, store, texts, wire
from core.camera.model import CameraRef, CameraSnapshot, CameraStatus, MachineCamera, VideoStreamView

SAVE_DELAY = 0.5
VIDEO_OUTBOX_LIMIT = 32        # §4.10: video link_out events are dropped while the link outbox holds more
PAUSE_AFTER = 1.0              # outbox over the limit this long -> video_link paused
RESUME_BELOW = 8               # outbox under this ...
RESUME_AFTER = 2.0             # ... for this long -> video_link paused:false
PAUSE_POLL = 0.25
BACKOFF = (10.0, 30.0, 60.0)   # §6.5: re-open delays after `no_receiver` while the demand persists
LINK_SETTINGS = {"bitrate": 1_000_000, "fps": 15}   # §4.10 link-state settings, sent with every `send`
CAMERA_ERRORS = ("no_camera", "permission", "internal", "unsupported")   # a failed send that means the capture failed
CLOSE_FOR_ERROR = {"permission": "permission", "no_camera": "camera_gone", "no_receiver": "no_receiver"}
INSTALL_OFFERS = {"missing": "install", "outdated": "update", "broken": "repair"}
_UNSENT = object()


class MediaContext(Protocol):          # implemented by AudioService (hunk, CW6); all calls on the worker
    own: str
    def post(self, peer: str, message: dict) -> bool: ...
    def helper_send(self, command: dict, reply: Callable[[dict], None] | None = None) -> None: ...
    def helper_state(self) -> str: ...                 # ok | starting | failed | no_helper
    def helper_features(self) -> frozenset[str]: ...
    def flow_for(self, peer: str) -> str | None: ...   # agreed flow id with a direct peer, else None
    def peer_has_udp(self, peer: str) -> bool: ...     # False for loopback / no host
    def new_stream_id(self) -> int: ...                # unique among audio AND camera streams (service hunk)
    def new_key(self) -> str: ...
    def peers_with(self, feature: str) -> frozenset[str]: ...  # direct peers whose audio_hello has it
    def direct(self) -> frozenset[str]: ...
    def online(self) -> frozenset[str]: ...
    def audio_machines(self) -> Mapping[str, MachineAudio]: ...
    def machine_name(self, machine: str) -> str: ...   # display name of any machine id, own included
    def outbox_depth(self, peer: str) -> int: ...
    def changed(self) -> None: ...                     # publish a new snapshot to subscribers
    def schedule(self, at: float) -> None: ...         # wake the worker at monotonic time `at`
    clock: Callable[[], float]
    wall: Callable[[], float]
    logger: logging.Logger


@dataclass
class _Send:
    spec: planner.VideoSendSpec
    stream: int
    key: str
    flow: str
    source: dict
    opened_at: float
    state: str = "opening"     # opening | starting | running | failed
    error: Optional[str] = None
    retry_token: Any = None
    accepted: bool = False     # camera_opened ok:true
    helper_started: bool = True
    ran: bool = False


@dataclass
class _Recv:
    peer: str
    stream: int
    flow: str
    codec: dict
    state: str = "starting"    # starting | running
    helper: bool = True        # False: `camera_refuse silent` accepted it without opening the recv


@dataclass
class _LinkState:
    dropped: set = field(default_factory=set)
    over_since: Optional[float] = None
    under_since: Optional[float] = None
    paused: bool = False


def snapshot_data(snap: CameraSnapshot) -> dict:
    """A CameraSnapshot as JSON-ready data (the dev status file, §7.4)."""
    def status(value):
        return {"state": value.state, "reason": value.reason, "params": dict(value.params)}
    return {"own": snap.own, "machines": {ident: model.machine_data(machine) for ident, machine in snap.machines.items()},
            "route": model.route_data(snap.route), "status": status(snap.status),
            "sinks": {ident: status(value) for ident, value in snap.sinks.items()},
            "streams": [dataclasses.asdict(stream) for stream in snap.streams], "permission": snap.permission,
            "needs_permission": snap.needs_permission, "sharing": snap.sharing,
            "vcam_install_offer": snap.vcam_install_offer,
            "vcam_local": None if snap.vcam_local is None else dataclasses.asdict(snap.vcam_local)}


def _number(value):
    return float(value) if isinstance(value, (int, float)) and not isinstance(value, bool) else None


class CameraPart:
    def __init__(self, ctx: MediaContext, state_dir: Path, platform: str):
        self._ctx = ctx
        self._own = ctx.own
        self._state_dir = Path(state_dir)
        self._platform = platform
        self._log = getattr(ctx, "logger", None) or logging.getLogger("beamer.camera")
        stored = store.load(self._state_dir, now=self._wall())
        self._route = stored.route
        self._own_rev = stored.own_rev
        self._sharing = stored.sharing                # applied before the first plan (§7.3)
        self._machines = {ident: machine for ident, machine in stored.machines.items() if ident != self._own}
        # Helper-side state of this machine.
        self._cameras = None          # last `cameras` event
        self._permission = None       # camera permission from `permission` / `cameras`
        self._vcam = None             # last `vcam` event
        self._camera_events = {}      # stream -> last `camera` event
        self._stats = {}              # stream -> last video stats
        # Streams.
        self._sends = {}              # peer -> _Send
        self._recv = None             # the one _Recv
        self._failures = {}           # peer -> consecutive no_receiver failures (backoff step)
        self._backoff_at = {}         # peer -> when a no_receiver send may re-open
        self._link = {}               # stream -> _LinkState
        # Cluster bookkeeping.
        self._demand_hold = {}
        self._in_use = {}
        self._greeted = set()
        self._force_republish = False
        self._not_source = _UNSENT    # our route view's camera machine when this sink refused `not_source`
        # What the helper was last told.
        self._sent_warm = _UNSENT
        self._sent_placeholders = _UNSENT
        # Dev (§7.4).
        self._test_source = None
        self._refuse = None
        # Timers and output.
        self._save_at = None
        self._wake = None
        self._plan = planner.CameraPlan()
        self._snapshot = model.empty_snapshot(self._own)
        # Diagnostics (tests, dev status).
        self.permission_requests = 0
        self.link_out_dropped = 0
        self.reopens = []             # (peer, error, old token, new token, at): §6.5 retry accounting
        self.opens = []               # (peer, stream, at)

    # Context helpers ----------------------------------------------------------------------------

    def _now(self):
        return self._ctx.clock()

    def _wall(self):
        return self._ctx.wall()

    def _helper_ok(self):
        return self._ctx.helper_state() == "ok"

    def _features(self):
        return frozenset(self._ctx.helper_features() or ())

    def _camera_peers(self):
        return frozenset(self._ctx.peers_with(wire.FEATURE)) & frozenset(self._ctx.direct())

    def _post(self, peer, msg):
        """Post a camera_* message, only to a direct peer that announced `camera1` (§5.1)."""
        if peer not in self._camera_peers():
            return False
        if not wire.size_ok(msg):
            self._log.warning("camera: %s too large, not sent", msg.get("type"))
            return False
        try:
            return bool(self._ctx.post(peer, msg))
        except Exception:
            self._log.exception("camera: post to a peer failed")
            return False

    def _helper_send(self, command, reply=None):
        if not self._helper_ok():
            return False
        self._ctx.helper_send(command, reply)
        return True

    def _schedule_save(self):
        if self._save_at is None:
            self._save_at = self._now() + SAVE_DELAY

    def _save_now(self):
        self._save_at = None
        try:
            store.save(self._state_dir, store.Stored(self._route, self._own_rev, self._sharing, dict(self._machines)))
        except OSError as error:
            self._log.warning("camera: could not save %s: %s", store.FILE_NAME, error)

    # Helper events ------------------------------------------------------------------------------

    def helper_ready(self, ready: dict) -> None:
        """A (new) helper is ready: tell it the warm and placeholder state again on the next apply."""
        self._sent_warm = _UNSENT
        self._sent_placeholders = _UNSENT

    def helper_event(self, event: dict) -> bool:
        """True if the event is the camera's alone (the service then skips it)."""
        name = event.get("ev")
        if name == "_state":
            self._helper_gone()
            return False
        if name == "ready":
            self.helper_ready(event)
            return False
        if name == "cameras":
            self._cameras = event
            if event.get("permission") in model.PERMISSIONS:
                self._permission = event["permission"]
            return True
        if name == "vcam":
            self._vcam = event
            return True
        if name == "camera":
            stream = event.get("stream")
            if self._is_send_stream(stream):
                self._camera_events[stream] = event
                return True
            return False
        if name == "permission":
            if event.get("camera") in model.PERMISSIONS:
                self._permission = event["camera"]
            return False
        if name == "stream_state":
            return self._on_stream_state(event)
        if name == "stats":
            streams = event.get("streams")
            if isinstance(streams, dict):
                ours = self.stream_ids()
                for key, value in streams.items():
                    if str(key).isdigit() and int(key) in ours and isinstance(value, dict):
                        self._stats[int(key)] = value
            return False
        if name == "link_out" and event.get("video") is True:
            peer = self._peer_for_flow(event.get("flow"))
            if peer is not None:
                self.link_out(peer, event)
            return True
        return False

    def _is_send_stream(self, stream):
        return any(send.stream == stream for send in self._sends.values())

    def _peer_for_flow(self, flow):
        for peer, send in self._sends.items():
            if send.flow == flow:
                return peer
        for peer in self._ctx.direct():
            if self._ctx.flow_for(peer) == flow:
                return peer
        return None

    def _helper_gone(self):
        """The helper is (re)starting or failed: every video stream of this machine is gone (§5.3)."""
        for peer, send in list(self._sends.items()):
            if send.state != "failed":
                self._post(peer, wire.close(send.stream, "helper"))
        self._sends.clear()
        if self._recv is not None:
            self._post(self._recv.peer, wire.close(self._recv.stream, "helper"))
            self._recv = None
        self._camera_events.clear()
        self._stats.clear()
        self._link.clear()
        self._sent_warm = _UNSENT
        self._sent_placeholders = _UNSENT

    def _on_stream_state(self, event):
        stream, state = event.get("stream"), event.get("state")
        for peer, send in self._sends.items():
            if send.stream != stream:
                continue
            if state == "starting" and send.state == "opening":
                send.state = "starting"
            elif state == "running" and send.state != "failed":
                send.state, send.ran = "running", True
                self._failures.pop(peer, None)
                self._backoff_at.pop(peer, None)
            elif state in ("failed", "stopped") and send.state != "failed":
                self._send_failed(peer, send, event.get("error") or state, helper_ended=True)
            return True
        recv = self._recv
        if recv is not None and recv.stream == stream:
            if state == "running":
                recv.state = "running"
            elif state in ("failed", "stopped"):
                self._recv = None
                self._helper_send({"cmd": "stop", "stream": stream})
                self._post(recv.peer, wire.close(stream, "error"))
            return True
        return False

    def _send_failed(self, peer, send, error, helper_ended=False, notify=True):
        send.state, send.error = "failed", error
        send.retry_token = self._retry_token(peer)
        if not helper_ended and send.helper_started:
            self._helper_send({"cmd": "stop", "stream": send.stream})
        send.helper_started = False
        self._stats.pop(send.stream, None)
        self._camera_events.pop(send.stream, None)
        if error == "no_receiver":
            step = self._failures.get(peer, 0)
            self._failures[peer] = step + 1
            self._backoff_at[peer] = self._now() + BACKOFF[min(step, len(BACKOFF) - 1)]
        if notify:
            self._post(peer, wire.close(send.stream, CLOSE_FOR_ERROR.get(error, "error")))

    # Links --------------------------------------------------------------------------------------

    def link_up(self, peer: str) -> None:
        """Nothing to send yet: the peer's `audio_hello` tells whether it speaks `camera1`; `apply`
        greets it as soon as it does."""

    def link_down(self, peer: str) -> None:
        self._greeted.discard(peer)
        send = self._sends.pop(peer, None)
        if send is not None:
            if send.helper_started:
                self._helper_send({"cmd": "stop", "stream": send.stream})
            self._link.pop(send.stream, None)
            self._stats.pop(send.stream, None)
            self._camera_events.pop(send.stream, None)
        if self._recv is not None and self._recv.peer == peer:
            if self._recv.helper:
                self._helper_send({"cmd": "stop", "stream": self._recv.stream})
            self._stats.pop(self._recv.stream, None)
            self._recv = None

    def link_message(self, peer: str, message: dict) -> None:
        read = wire.read(message, now=self._wall())
        if read is None:
            if wire.is_camera(message):
                self._log.debug("camera: malformed %s ignored", message.get("type"))
            return
        kind, data = read
        getattr(self, "_on_" + kind)(peer, data)

    def _on_camera_devices(self, peer, machine):
        if machine.id == self._own:
            if machine.rev > self._own_rev:
                # A list of ours newer than the one we know: camera.json went back.
                self._own_rev = machine.rev
                self._machines.pop(self._own, None)
            return
        held = self._machines.get(machine.id)
        if not model.accept_devices(held, machine, direct_from=machine.id == peer) or machine == held:
            return
        self._machines[machine.id] = machine
        self._schedule_save()
        for other in sorted(self._camera_peers() - {peer, machine.id}):
            self._post(other, wire.devices(machine))

    def _on_camera_route(self, peer, incoming):
        merged, changed, answer = model.merge_route(self._route, incoming)
        if changed:
            self._route = merged
            self._schedule_save()
            for other in sorted(self._camera_peers() - {peer}):
                self._post(other, wire.route(merged))
        if answer:
            self._post(peer, wire.route(merged))

    def _on_camera_open(self, peer, data):
        stream = data["stream"]
        refuse = self._refuse
        if refuse in ("not_source", "helper"):
            self._post(peer, wire.opened(stream, False, refuse))
            return
        if self._route.camera.machine != peer:
            self._not_source = self._route.camera.machine
            self._post(peer, wire.opened(stream, False, "not_source"))
            return
        own = self._machines.get(self._own)
        problem = None
        if own is None or "virtual_camera" not in own.features:
            problem = "unsupported"
        elif own.virtual_camera.install != "ok":
            problem = "outdated" if own.virtual_camera.install == "outdated" else "not_installed"
        elif not self._helper_ok() or self._ctx.flow_for(peer) != data["flow"]:
            problem = "helper"
        if problem is not None:
            self._post(peer, wire.opened(stream, False, problem))
            return
        old = self._recv
        if old is not None:
            # One recv at a time (§6.4 rule 5): the accepted open replaces it.
            self._recv = None
            if old.helper:
                self._helper_send({"cmd": "stop", "stream": old.stream})
            self._stats.pop(old.stream, None)
            if old.stream != stream:
                self._post(old.peer, wire.close(old.stream, "route"))
        if refuse == "silent":
            self._recv = _Recv(peer, stream, data["flow"], data["codec"], helper=False)
            self._post(peer, wire.opened(stream, True))
            return
        recv = _Recv(peer, stream, data["flow"], data["codec"])
        self._recv = recv

        def reply(event, recv=recv):
            if event.get("ev") == "ok":
                if self._recv is recv:
                    self._post(peer, wire.opened(stream, True))
                return
            if self._recv is recv:
                self._recv = None
            code = event.get("code")
            self._post(peer, wire.opened(stream, False, code if code in wire.OPEN_ERRORS else "helper"))
        if not self._helper_send({"cmd": "recv", "kind": "video", "stream": stream, "flow": data["flow"],
                                  "key": data["key"], "codec": data["codec"], "sink": {"virtual_camera": True}},
                                 reply):
            self._recv = None
            self._post(peer, wire.opened(stream, False, "helper"))

    def _on_camera_opened(self, peer, data):
        send = self._sends.get(peer)
        if send is None or send.stream != data["stream"] or send.state == "failed":
            return
        if data["ok"]:
            send.accepted = True
            return
        # §6.5: a refusal turns the camera off at once; no re-open until the retry token changes.
        self._send_failed(peer, send, data["error"], notify=False)

    def _on_camera_close(self, peer, data):
        stream = data["stream"]
        recv = self._recv
        if recv is not None and recv.peer == peer and recv.stream == stream:
            self._recv = None
            if recv.helper:
                self._helper_send({"cmd": "stop", "stream": stream})
            self._stats.pop(stream, None)
            return
        send = self._sends.get(peer)
        if send is not None and send.stream == stream and send.state != "failed":
            self._send_failed(peer, send, data["reason"], notify=False)

    # Video link_out (§4.10) ---------------------------------------------------------------------

    def link_out(self, peer: str, event: dict) -> None:
        """A video `link_out` event of a send stream: post it on the link unless the link outbox is too
        full; a dropped frame drops its later events too and is reported to the engine."""
        send = self._sends.get(peer)
        packets = event.get("p")
        if send is None or send.flow != event.get("flow") or not isinstance(packets, list) or not packets:
            return
        state = self._link.setdefault(send.stream, _LinkState())
        frame, end = event.get("frame"), event.get("end") is True
        try:
            depth = int(self._ctx.outbox_depth(peer))
        except Exception:
            depth = 0
        now = self._now()
        if frame in state.dropped:
            self.link_out_dropped += len(packets)
            if end:
                state.dropped.discard(frame)
        elif depth > VIDEO_OUTBOX_LIMIT:
            self.link_out_dropped += len(packets)
            if not end:
                state.dropped.add(frame)
            self._helper_send({"cmd": "video_link", "stream": send.stream, "dropped": [frame]})
        else:
            for start in range(0, len(packets), audio_wire.MAX_FRAME_PACKETS):
                try:
                    self._ctx.post(peer, audio_wire.frame(send.flow, packets[start:start + audio_wire.MAX_FRAME_PACKETS]))
                except Exception:
                    self._log.exception("camera: video link_out post failed")
        self._track_outbox(peer, send, state, depth, now)

    def _track_outbox(self, peer, send, state, depth, now):
        path = (self._stats.get(send.stream) or {}).get("path")
        if depth > VIDEO_OUTBOX_LIMIT:
            state.under_since = None
            if state.over_since is None:
                state.over_since = now
            if not state.paused and now - state.over_since >= PAUSE_AFTER:
                state.paused = True
                self._helper_send({"cmd": "video_link", "stream": send.stream, "paused": True})
        else:
            state.over_since = None
            if depth < RESUME_BELOW:
                if state.under_since is None:
                    state.under_since = now
            else:
                state.under_since = None
            if state.paused and (path == "udp" or (state.under_since is not None
                                                   and now - state.under_since >= RESUME_AFTER)):
                state.paused = False
                state.under_since = None
                self._helper_send({"cmd": "video_link", "stream": send.stream, "paused": False})

    def _poll_outboxes(self, now):
        wake = None
        for peer, send in self._sends.items():
            state = self._link.get(send.stream)
            if state is None or (not state.paused and state.over_since is None):
                continue
            try:
                depth = int(self._ctx.outbox_depth(peer))
            except Exception:
                depth = 0
            self._track_outbox(peer, send, state, depth, now)
            if state.paused or state.over_since is not None:
                wake = now + PAUSE_POLL
        return wake

    # Local actions (on the worker, through the service's queue) ---------------------------------

    def set_camera(self, machine: Optional[str], device: Optional[CameraRef]) -> None:
        """A local page action. Choosing one of this Mac's own cameras while its permission is
        undetermined is the Mac action that may show the macOS prompt (§9.1)."""
        self._set_camera(machine, device, local_action=True)

    def _set_camera(self, machine, device, local_action):
        if machine is None:
            device = None
        stamp = model.next_stamp(self._route, self._own, self._wall())
        self._route = dataclasses.replace(self._route, camera=model.CameraEntry(machine, device, stamp))
        self._broadcast_route()
        if (local_action and machine == self._own and self._permission == "undetermined"
                and "camera_capture" in self._features() and self._effective_test_source() is None):
            self.ask_permission()

    def set_quality(self, size: str) -> None:
        if size not in model.QUALITIES:
            return
        stamp = model.next_stamp(self._route, self._own, self._wall())
        self._route = dataclasses.replace(self._route, quality=model.QualityEntry(size, stamp))
        self._broadcast_route()

    def set_sharing(self, on: bool) -> None:
        """This machine's kill switch (§6.1): off closes every video stream and never opens the camera."""
        on = bool(on)
        if on != self._sharing:
            self._sharing = on
            self._schedule_save()

    def ask_permission(self) -> None:
        """"Allow Camera Access…" on the Mac: the only way, with `set_camera(own, …)`, to prompt."""
        if "camera_capture" not in self._features():
            return
        if self._helper_send({"cmd": "permission_request", "what": "camera"}):
            self.permission_requests += 1

    def refresh_install(self) -> None:
        """PC: after an install, update or repair, the helper re-reads the install state (§4.6.5)."""
        if "virtual_camera" in self._features():
            self._helper_send({"cmd": "vcam"})

    def _broadcast_route(self):
        self._schedule_save()
        for peer in sorted(self._camera_peers()):
            self._post(peer, wire.route(self._route))

    def shutdown(self) -> None:
        """Beamer quits: close every video stream, save now (the service's stop, CW6)."""
        for peer in list(self._sends):
            self._close_send(peer, "shutdown")
        if self._recv is not None:
            recv, self._recv = self._recv, None
            self._post(recv.peer, wire.close(recv.stream, "shutdown"))
            if recv.helper:
                self._helper_send({"cmd": "stop", "stream": recv.stream})
        self._save_now()

    # Dev (§7.4) ---------------------------------------------------------------------------------

    def dev_command(self, item: dict) -> bool:
        used = False
        if not isinstance(item, dict):
            return False
        if "set_camera" in item and isinstance(item["set_camera"], dict):
            value = item["set_camera"]
            machine, key = value.get("machine"), value.get("device")
            device = None
            if machine is not None and isinstance(key, str):
                held = self._machine(machine)
                found = model.camera_by_key(held.cameras, key) if held is not None else None
                device = CameraRef(key, found.name if found is not None else key)
            # A dev command is not a page action: it never shows the macOS prompt (E2E runs unattended).
            self._set_camera(machine if isinstance(machine, str) else None, device, local_action=False)
            used = True
        if item.get("set_quality") in model.QUALITIES:
            self.set_quality(item["set_quality"])
            used = True
        if isinstance(item.get("camera_sharing"), bool):
            self.set_sharing(item["camera_sharing"])
            used = True
        if "camera_test_source" in item:
            value = item["camera_test_source"]
            self._test_source = value if wire.test_source_ok(value) else None
            used = True
        if "camera_refuse" in item:
            value = item["camera_refuse"]
            self._refuse = value if value in ("not_source", "helper", "silent") else None
            used = True
        return used

    def dev_status(self) -> dict:
        return {"snapshot": snapshot_data(self._snapshot),
                "stats": {str(stream): value for stream, value in self._stats.items()},
                "test_source": self._effective_test_source(), "test_source_requested": self._test_source,
                "refuse": self._refuse, "link_out_dropped": self.link_out_dropped,
                "permission_requests": self.permission_requests, "warm": self._plan.warm}

    def _effective_test_source(self):
        """The synthetic source in force: requested by the dev key and announced by a `--dev` helper."""
        features = self._features()
        if self._test_source and wire.SYNTH_FEATURE in features and "camera_capture" in features:
            return self._test_source
        return None

    # Queries ------------------------------------------------------------------------------------

    def snapshot(self) -> CameraSnapshot:
        return self._snapshot

    def stream_ids(self) -> frozenset:
        ids = {send.stream for send in self._sends.values()}
        if self._recv is not None:
            ids.add(self._recv.stream)
        return frozenset(ids)

    def camera_on(self) -> bool:
        """Whether this machine's helper has a video send stream (tests: the camera-on invariant)."""
        return any(send.helper_started for send in self._sends.values())

    def _machine(self, ident):
        return self._machines.get(ident)

    # Settling -----------------------------------------------------------------------------------

    def apply(self, now: Optional[float] = None) -> None:
        """Plan and diff; called after every audio apply and when the part's wake time comes."""
        now = self._now() if now is None else now
        self._greet()
        self._republish_own()
        self._track_demand(now)
        plan = self._make_plan(now)
        self._plan = plan
        self._diff(plan, now)
        self._warm(plan)
        self._check_recv(plan)
        poll = self._poll_outboxes(now)
        snap = self._compose_snapshot(plan)
        self._push_placeholders(snap, plan)
        if self._save_at is not None and now >= self._save_at:
            self._save_now()
        wakes = [moment for moment in (plan.wake_at, self._save_at, poll) if moment is not None]
        wakes += [at for peer, at in self._backoff_at.items()
                  if peer in self._sends and self._sends[peer].state == "failed" and at > now]
        wake = min(wakes) if wakes else None
        if wake is not None and wake != self._wake:
            self._ctx.schedule(wake)
        self._wake = wake
        if snap != self._snapshot:
            self._snapshot = snap
            self._ctx.changed()

    def _greet(self):
        """A direct peer that now speaks `camera1` gets our list, every held list and the route (§5.3)."""
        peers = self._camera_peers()
        self._greeted &= peers
        for peer in sorted(peers - self._greeted):
            self._greeted.add(peer)
            own = self._machines.get(self._own)
            if own is not None:
                self._post(peer, wire.devices(own))
            for ident, machine in sorted(self._machines.items()):
                if ident not in (self._own, peer):
                    self._post(peer, wire.devices(machine))
            self._post(peer, wire.route(self._route))

    def _compose_own(self) -> MachineCamera:
        features_all = self._features()
        state = self._ctx.helper_state()
        if state == "ok":
            status = "ok" if wire.VIDEO_FEATURE in features_all else "no_video"
        else:
            status = "starting" if state == "starting" else "failed"
        features = frozenset(word for word in features_all if word in model.FEATURES)
        cameras, preferred = (), None
        event = self._cameras or {}
        if "camera_capture" in features and isinstance(event.get("cameras"), list):
            preferred = event.get("preferred") if isinstance(event.get("preferred"), str) else None
            result, seen = [], set()
            for entry in event["cameras"]:
                if not isinstance(entry, dict) or not isinstance(entry.get("key"), str) or not entry["key"]:
                    continue
                key = entry["key"][:wire.MAX_KEY]
                if key in seen:
                    continue
                seen.add(key)
                native = tuple(value for value in (entry.get("native") or [])
                               if isinstance(value, str) and wire.NATIVE_SHAPE.fullmatch(value))[:wire.MAX_NATIVE]
                transport = entry.get("transport") if entry.get("transport") in model.TRANSPORTS else "other"
                result.append(model.Camera(key, str(entry.get("name", ""))[:wire.MAX_NAME], transport, native,
                                           key == preferred))
            cameras = tuple(result[:wire.MAX_CAMERAS])
            if preferred is not None and len(preferred) > wire.MAX_KEY:
                preferred = None
        capture = "camera_capture" in features
        return MachineCamera(
            id=self._own, platform=self._platform, rev=self._own_rev, status=status, features=features,
            cameras=cameras, preferred=preferred,
            permission=(self._permission if self._permission in model.PERMISSIONS else "unknown") if capture else None,
            sharing=self._sharing if capture else None, capture=self._compose_capture(),
            virtual_camera=self._compose_vcam(features), test_source=self._effective_test_source())

    def _compose_capture(self):
        sends = list(self._sends.values())
        events = [self._camera_events[send.stream] for send in sends if send.stream in self._camera_events]
        by_other = any(event.get("by_other") is True for event in events)
        effects = []
        for event in events:
            for effect in event.get("effects") or ():
                if effect in model.EFFECTS and effect not in effects:
                    effects.append(effect)
        effects = tuple(effects)
        if any(event.get("state") == "lost" for event in events):
            return model.Capture("failed", by_other, effects, "no_camera")
        if any(send.state == "running" for send in sends):
            interrupted = any(event.get("state") == "interrupted" for event in events)
            return model.Capture("interrupted" if interrupted else "running", by_other, effects, None)
        if any(send.state in ("opening", "starting") for send in sends):
            return model.Capture("starting", by_other, effects, None)
        failed = [send.error for send in sends if send.state == "failed" and send.error in CAMERA_ERRORS]
        if failed:
            return model.Capture("failed", False, (), failed[0])
        return model.IDLE_CAPTURE

    def _compose_vcam(self, features):
        event = self._vcam
        if "virtual_camera" not in features or not isinstance(event, dict):
            return model.NO_VIRTUAL_CAMERA
        install = event.get("install") if event.get("install") in model.INSTALL_STATES else "broken"
        want = event.get("want")
        if (isinstance(want, dict) and all(type(want.get(name)) is int and 0 < want[name] <= 8192
                                           for name in ("w", "h", "fps"))):
            want = (want["w"], want["h"], want["fps"])
        else:
            want = None
        app = event.get("app") if isinstance(event.get("app"), str) and event.get("app") else None
        return model.VirtualCamera(install, event.get("present") is True, event.get("in_use") is True,
                                   app[:wire.MAX_APP] if app else None, want)

    def _republish_own(self):
        fresh = self._compose_own()
        held = self._machines.get(self._own)
        if self._not_source is not _UNSENT and self._route.camera.machine != self._not_source:
            # §5.3/§6.5: our route view lagged S's when we refused `not_source`; a new rev lets S retry.
            own_demand = held is not None and held.virtual_camera.in_use
            self._not_source = _UNSENT
            if own_demand:
                self._force_republish = True
        if (held is not None and not self._force_republish
                and dataclasses.replace(fresh, rev=held.rev) == held):
            return
        self._force_republish = False
        self._own_rev = max(int(self._wall() * 1000), self._own_rev + 1)
        own = dataclasses.replace(fresh, rev=self._own_rev)
        self._machines[self._own] = own
        self._schedule_save()
        for peer in sorted(self._camera_peers()):
            self._post(peer, wire.devices(own))

    def _track_demand(self, now):
        for ident, machine in self._machines.items():
            in_use = machine.virtual_camera.in_use
            if self._in_use.get(ident) and not in_use:
                self._demand_hold[ident] = now + planner.DEMAND_HOLD
            elif in_use:
                self._demand_hold.pop(ident, None)
            self._in_use[ident] = in_use
        self._demand_hold = {ident: until for ident, until in self._demand_hold.items() if until > now}

    def _make_plan(self, now):
        direct = self._camera_peers()
        no_udp = frozenset(peer for peer in direct if not self._ctx.peer_has_udp(peer))
        return planner.plan_camera(self._own, self._machines, self._ctx.audio_machines(), self._route, direct,
                                   frozenset(self._ctx.online()) | {self._own}, now, self._demand_hold, no_udp)

    # Diff (§6.4 rule 4, §6.5) -------------------------------------------------------------------

    def _retry_token(self, peer):
        machine = self._machines.get(peer)
        return (repr(model.route_data(self._route)), machine.rev if machine is not None else None,
                self._ctx.flow_for(peer))

    def _source(self, spec):
        test = self._effective_test_source()
        return {"test": test} if test is not None else {"camera": spec.device}

    def _close_reason(self, plan, peer):
        if self._route.camera.machine != self._own:
            return "route"
        if not self._sharing:
            return "sharing_off"
        if plan.status.state in ("waiting_permission", "denied"):
            return "permission"
        if plan.status.state in ("no_camera", "waiting_device"):
            return "camera_gone"
        machine = self._machines.get(peer)
        if machine is not None and not planner.demand(machine, self._now(), self._demand_hold):
            return "demand"
        return "route"

    def _close_send(self, peer, reason):
        send = self._sends.pop(peer, None)
        if send is None:
            return
        if send.state != "failed" or reason == "shutdown":
            self._post(peer, wire.close(send.stream, reason))
        if send.helper_started:
            self._helper_send({"cmd": "stop", "stream": send.stream})
        self._stats.pop(send.stream, None)
        self._camera_events.pop(send.stream, None)
        self._link.pop(send.stream, None)

    def _may_retry(self, peer, send, now):
        if send.retry_token != self._retry_token(peer):
            return True
        return send.error == "no_receiver" and now >= self._backoff_at.get(peer, float("inf"))

    def _diff(self, plan, now):
        wanted = {}
        if self._helper_ok():
            direct = self._camera_peers()
            for peer, spec in plan.sends.items():
                if peer in direct and self._ctx.flow_for(peer) is not None:
                    wanted[peer] = spec
        for peer, send in list(self._sends.items()):
            spec = wanted.get(peer)
            if spec is None:
                reason = self._close_reason(plan, peer)
                self._close_send(peer, reason)
                if peer not in plan.sends:      # the demand is gone: the next one starts a fresh backoff
                    self._failures.pop(peer, None)
                    self._backoff_at.pop(peer, None)
            elif send.state == "failed":
                if self._may_retry(peer, send, now):
                    self.reopens.append((peer, send.error, send.retry_token, self._retry_token(peer), now))
                    self._close_send(peer, "route")
            elif (send.flow != self._ctx.flow_for(peer) or spec.device != send.spec.device
                  or self._source(spec) != send.source):
                self._close_send(peer, "route")
            elif (spec.w, spec.h, spec.fps, spec.bitrate) != (send.spec.w, send.spec.h, send.spec.fps, send.spec.bitrate):
                change = {name: getattr(spec, name) for name in ("w", "h", "fps", "bitrate")
                          if getattr(spec, name) != getattr(send.spec, name)}
                self._helper_send(dict({"cmd": "video_config", "stream": send.stream}, **change))
                send.spec = spec
        for peer, spec in sorted(wanted.items()):
            if peer not in self._sends:
                self._open_send(spec, now)

    def _open_send(self, spec, now):
        peer = spec.peer
        flow = self._ctx.flow_for(peer)
        stream = self._ctx.new_stream_id()
        key = self._ctx.new_key()
        codec = wire.codec(spec.w, spec.h, spec.bitrate, spec.fps)
        send = _Send(spec, stream, key, flow, self._source(spec), now)
        self._sends[peer] = send
        self.opens.append((peer, stream, now))

        def reply(event, send=send, peer=peer):
            if event.get("ev") == "error" and self._sends.get(peer) is send and send.state != "failed":
                send.helper_started = False
                self._send_failed(peer, send, event.get("code") or "internal", helper_ended=True)
        # Deviation 3: the camera warms up while the open round trip runs; nothing is encoded before
        # the receiver's start PLI.
        self._helper_send({"cmd": "send", "kind": "video", "stream": stream, "flow": flow, "key": key,
                           "source": send.source, "codec": codec, "link": dict(LINK_SETTINGS)}, reply)
        camera = None
        if self._route.camera.device is not None:
            camera = self._route.camera.device
        elif spec.device is not None:
            own = self._machines.get(self._own)
            found = model.camera_by_key(own.cameras, spec.device) if own is not None else None
            camera = CameraRef(found.key, found.name) if found is not None else None
        self._post(peer, wire.open_stream(stream, flow, codec, key, camera, self._route.camera.stamp))

    def _warm(self, plan):
        if "camera_capture" not in self._features() or not self._helper_ok():
            return
        if plan.warm == self._sent_warm:
            return
        if plan.warm is None:
            command = {"cmd": "video_warm", "on": False}
        else:
            w, h, bitrate = plan.warm
            command = {"cmd": "video_warm", "on": True, "w": w, "h": h, "bitrate": bitrate}
        if self._helper_send(command):
            self._sent_warm = plan.warm

    def _check_recv(self, plan):
        """A sink whose Beamer Camera is no longer installed (or whose helper lost the camera) stops
        taking the stream; route changes are the sender's to close (§6.4 rule 4)."""
        recv = self._recv
        if recv is None:
            return
        own = self._machines.get(self._own)
        if own is None or own.virtual_camera.install != "ok":
            self._recv = None
            if recv.helper:
                self._helper_send({"cmd": "stop", "stream": recv.stream})
            self._post(recv.peer, wire.close(recv.stream, "error"))

    def _push_placeholders(self, snap, plan):
        """On a machine with a virtual camera: the placeholder texts and idle state, whenever either
        changes and after every helper `ready`, with or without a page (§7.1)."""
        if "virtual_camera" not in self._features() or not self._helper_ok():
            return
        content = (texts.placeholder_texts(snap, self._ctx.machine_name), plan.idle_placeholder or "waiting")
        if content == self._sent_placeholders:
            return
        if self._helper_send({"cmd": "vcam", "placeholders": content[0], "idle": content[1]}):
            self._sent_placeholders = content

    # Snapshot -----------------------------------------------------------------------------------

    def _compose_snapshot(self, plan) -> CameraSnapshot:
        views = []
        for peer, send in sorted(self._sends.items()):
            stats = self._stats.get(send.stream, {})
            views.append(VideoStreamView(send.stream, self._own, peer, send.state, stats.get("path") or "",
                                         send.spec.w, send.spec.h, _number(stats.get("fps")),
                                         _number(stats.get("kbps")), _number(stats.get("rtt_ms")), None))
        recv = self._recv
        if recv is not None:
            stats = self._stats.get(recv.stream, {})
            w = stats.get("w") if type(stats.get("w")) is int else recv.codec.get("w", 0)
            h = stats.get("h") if type(stats.get("h")) is int else recv.codec.get("h", 0)
            views.append(VideoStreamView(recv.stream, recv.peer, self._own, recv.state, stats.get("path") or "",
                                         w, h, _number(stats.get("fps_shown")), _number(stats.get("kbps")),
                                         _number(stats.get("target_ms")), _number(stats.get("frozen_ms"))))
        sinks = dict(plan.sinks)
        source = self._route.camera.machine
        for peer, send in self._sends.items():
            params = {"machine": self._own, "to": peer}
            if send.state == "failed":
                sinks[peer] = CameraStatus("error", send.error, params)
            elif send.state == "running":
                sinks[peer] = CameraStatus("reduced" if self._reduced(send.stream) else "streaming", None, params)
            else:
                sinks[peer] = CameraStatus("starting", None, params)
        if recv is not None and source == recv.peer and self._own in sinks:
            state = "starting" if recv.state != "running" else (
                "reduced" if self._reduced(recv.stream) else "streaming")
            sinks[self._own] = CameraStatus(state, None, {"machine": recv.peer, "to": self._own})
        status = plan.status
        if source is not None:
            problem, _key = planner.source_problem(self._own, self._machines.get(source), self._route,
                                                   frozenset(self._ctx.online()) | {self._own})
            if problem is None:
                status = planner.overall(source, None, sinks)
        own = self._machines.get(self._own)
        offer = local = None
        if own is not None and "virtual_camera" in own.features:
            offer = INSTALL_OFFERS.get(own.virtual_camera.install)
            event = self._vcam or {}
            versions = [event.get(name) if type(event.get(name)) is int else 0
                        for name in ("loaded_version", "dll_version", "bundled_version")]
            local = model.LocalVcam(*versions)
        return CameraSnapshot(own=self._own, machines=dict(self._machines), route=self._route, status=status,
                              sinks=sinks, streams=tuple(views),
                              permission=own.permission if own is not None else None,
                              needs_permission=plan.needs_permission,
                              sharing=own.sharing if own is not None else None,
                              vcam_install_offer=offer, vcam_local=local)

    def _reduced(self, stream):
        stats = self._stats.get(stream) or {}
        step = stats.get("step")
        return stats.get("path") == "link" or (type(step) is int and step > 0)
