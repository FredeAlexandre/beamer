# SPDX-License-Identifier: GPL-3.0-only
"""AudioService: the audio control plane of one machine (dev/audio/PHASE2.md §4.2, §4.5, §5, §6, §8.1).

One worker thread, `Beamer-audio`, owns every piece of state. Link threads, the helper's reader
thread and the UI only put events on its queue, so none of them ever waits for the helper, the
disk or another link; every `AudioHost.post` happens on the worker. The UI reads `snapshot()`, an
immutable AudioSnapshot swapped whole, and is told of a new one through `subscribe`.

Tests drive the same code without a thread: `start(run_thread=False)` then `pump()`.

A link message is the link's `{"type", "data"}` object. `link_up`/`link_down` are per peer: up when
the peer has a live link (a repeated call refreshes its caps), down when it has none left.
"""

import base64
import dataclasses
import json
import logging
import os
import queue
import secrets
import threading
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Protocol

from core import protocol
from core.audio import helper as helper_module
from core.audio import model, planner, store, wire
from core.audio.model import AudioSnapshot, DeviceRef, MachineAudio, RouteStatus, StreamView

log = logging.getLogger("beamer.audio")

ANSWER_PORT = 24823
RR_MS = 10
OUTBOX_LIMIT = 64          # link_out packets are dropped while the link's outbox holds more (§4.5)
SAVE_DELAY = 0.5
DEV_POLL = 1.0
LEVELS_PERIOD = 0.05       # ≤ 20 Hz to level subscribers
STATS_PERIOD = 0.25        # 4 Hz audio_stats to watching peers
METER_PERIOD_MS = 50
DEV_CMD = "audio-dev-cmd.json"
DEV_STATUS = "audio-dev-status.json"
DEV_EVENTS = "audio-dev-events.jsonl"   # dev only: helper events (no levels) and link changes, for the E2E harness
DEV_EVENTS_MAX = 20_000_000              # bytes; the file is then moved to `.1` and started again
HOWL_NOTICE_HOLD = 120.0   # s: the howl notice stays this long after the guard released its duck


class AudioHost(Protocol):
    def own_id(self) -> str: ...
    def own_name(self) -> str: ...
    def platform(self) -> str: ...                          # "macos" | "windows" | "linux"
    def post(self, peer: str, message: dict) -> bool: ...   # non-blocking, WIRE.md "which link" rule
    def outbox_depth(self, peer: str) -> int: ...
    def peer_host(self, peer: str) -> Optional[str]: ...    # IPv4 from the peer entry; None/loopback = no UDP
    def peer_name(self, peer: str) -> str: ...


@dataclass
class _Flow:
    id: str
    role: str                  # "offer" | "answer"
    key: str
    port: int = 0
    agreed: bool = False
    state: str = "binding"     # binding | probing | up | link | closed
    rtt_ms: Optional[float] = None


@dataclass
class _Peer:
    caps: frozenset
    hello: Optional[dict] = None
    flow: Optional[_Flow] = None
    watching: bool = False

    @property
    def audio(self):
        return "audio" in self.caps and (self.hello is None or self.hello["min_v"] <= wire.VERSION)

    @property
    def direct(self):
        return "audio" in self.caps and self.hello is not None and self.hello["min_v"] <= wire.VERSION


@dataclass
class _Send:
    spec: planner.SendSpec
    stream: int
    key: str
    flow: str
    source: dict
    state: str = "opening"     # opening | starting | running | failed | stopped
    error: Optional[str] = None
    retry_token: object = None
    helper_started: bool = False


@dataclass
class _Recv:
    peer: str
    kind: str
    flow: str
    sink: dict
    state: str = "starting"


@dataclass
class _Loop:
    spec: planner.LoopSpec
    stream: int
    state: str = "starting"


_STOP = object()


def _key():
    return base64.b64encode(secrets.token_bytes(wire.KEY_SIZE)).decode("ascii")


def _test_signal_ok(signal):
    """Dev test source: "A" or "B", optionally with a pilot amplitude ("A:0.2", 0..0.5)."""
    if not isinstance(signal, str):
        return False
    name, _, amp = signal.partition(":")
    if name not in ("A", "B"):
        return False
    if not amp:
        return True
    try:
        return 0.0 <= float(amp) <= 0.5
    except ValueError:
        return False


def _usable_host(host):
    return isinstance(host, str) and host and not host.startswith("127.") and host != "localhost"


def snapshot_data(snap: AudioSnapshot) -> dict:
    """An AudioSnapshot as JSON-ready data (the dev status file)."""
    def status(value):
        return {"state": value.state, "reason": value.reason, "params": dict(value.params)}
    return {"own": snap.own, "machines": {ident: model.machine_data(machine) for ident, machine in snap.machines.items()},
            "online": sorted(snap.online), "direct": sorted(snap.direct), "route": model.route_data(snap.route),
            "mic_status": status(snap.mic_status),
            "output_status": {ident: status(value) for ident, value in snap.output_status.items()},
            "streams": [dataclasses.asdict(stream) for stream in snap.streams], "helper": snap.helper,
            "permission": dict(snap.permission), "aec_mode": snap.aec_mode, "aec_active": snap.aec_active,
            "howls": [dataclasses.asdict(howl) for howl in snap.howls], "vmic_exclusive": snap.vmic_exclusive}


class AudioService:
    def __init__(self, host: AudioHost, state_dir, helper_cmd, *, clock=time.monotonic, wall=time.time,
                 helper_factory=None, logger=None):
        self._host = host
        self._state_dir = Path(state_dir)
        self._helper_cmd = list(helper_cmd) if helper_cmd else None
        self._clock = clock
        self._wall = wall
        self._helper_factory = helper_factory or (lambda cmd, deliver: helper_module.HelperProcess(cmd, deliver))
        self._log = logger or log
        self._queue = queue.Queue()
        self._thread = None
        self._worker = None
        self._subscribers = []
        self._level_subscribers = []
        self._subscriber_lock = threading.Lock()
        self._own = host.own_id()
        self._name = host.own_name()
        self._platform = host.platform()
        self._snapshot = model.empty_snapshot(self._own, "starting" if self._helper_cmd else "no_helper")
        self._began = False
        self._stopped = False
        # Worker state, touched only on the worker.
        self._route = model.empty_route()
        self._machines = {}
        self._own_rev = 0
        self._aec = "auto"
        self._vmic_exclusive = False
        self._helper = None
        self._helper_gen = 0        # events of a helper replaced by a restart are dropped
        self._helper_state = "starting" if self._helper_cmd else "no_helper"
        self._helper_info = {}
        self._devices_event = None
        self._permission = {}
        self._peers = {}
        self._sends = {}
        self._recvs = {}
        self._loop = None
        self._demand_hold = {}
        self._in_use = {}
        self._pending = {}
        self._req = 0
        self._stats = {}
        self._remote_streams = {}
        self._howls = {}            # target -> {"freq_hz", "active", "until"}: our system sound's howl guard
        self._remote_howls = {}     # peer -> [{"to", "freq_hz", "active"}] from its audio_stats
        self._levels = {}
        self._levels_dirty = False
        self._levels_at = 0.0
        self._stats_at = 0.0
        self._watching = False
        self._save_at = None
        self._dev = None
        self._dev_at = 0.0
        self._test_source = None
        self._dev_events = None
        self._wake_at = None
        self._dirty = True
        self.link_out_dropped = 0

    # Any thread ---------------------------------------------------------------------------------

    def start(self, run_thread=True) -> None:
        if self._thread is not None or self._began:
            return
        if run_thread:
            self._thread = threading.Thread(target=self._run, name="Beamer-audio", daemon=True)
            self._thread.start()
        else:
            self._worker = threading.current_thread()
            self._begin()

    def stop(self, timeout: float = 2.0) -> None:
        if self._stopped:
            return
        self._queue.put(_STOP)
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout)
        elif thread is None and self._began:
            self.pump()

    def link_up(self, peer: str, caps) -> None:
        self._queue.put(("link_up", peer, frozenset(caps or ())))

    def link_down(self, peer: str) -> None:
        self._queue.put(("link_down", peer))

    def link_message(self, peer: str, message: dict) -> None:
        self._queue.put(("link", peer, message))

    def snapshot(self) -> AudioSnapshot:
        return self._snapshot

    def subscribe(self, fn: Callable[[], None]) -> Callable[[], None]:
        return self._add(self._subscribers, fn)

    def subscribe_levels(self, fn) -> Callable[[], None]:
        return self._add(self._level_subscribers, fn)

    def _add(self, where, fn):
        with self._subscriber_lock:
            where.append(fn)

        def remove():
            with self._subscriber_lock:
                if fn in where:
                    where.remove(fn)
        return remove

    def set_mic(self, machine: Optional[str], device: Optional[DeviceRef]) -> None:
        self._queue.put(("call", "_set_mic", (machine, device)))

    def set_output(self, machine: str, to: Optional[str], device: Optional[DeviceRef]) -> None:
        self._queue.put(("call", "_set_output", (machine, to, device)))

    def set_form(self, machine: str, device_key: str, form: Optional[str]) -> None:
        self._queue.put(("call", "_set_form", (machine, device_key, form)))

    def set_aec(self, mode: str) -> None:
        self._queue.put(("call", "_set_aec", (mode,)))

    def set_vmic_exclusive(self, on: bool) -> None:
        """Windows, this machine only: render exclusively into CABLE Input (about 35 ms instead of 70,
        but no other app can play into it meanwhile). Restarts the helper: streams drop for ~1 s."""
        self._queue.put(("call", "_set_vmic_exclusive", (bool(on),)))

    def watch(self, on: bool) -> None:
        self._queue.put(("call", "_watch", (bool(on),)))

    def test_input(self, machine: str, device_key: str) -> None:
        self._queue.put(("call", "_test_input", (machine, device_key)))

    def retry_helper(self) -> None:
        self._queue.put(("call", "_retry_helper", ()))

    def pump(self, limit=100000) -> int:
        """Handle everything queued, and what that queues in turn, then the timers that are due: the
        threadless worker step for tests. The number of events handled."""
        handled = 0
        while handled < limit:
            try:
                item = self._queue.get_nowait()
            except queue.Empty:
                self._tick()
                if self._queue.empty():
                    break
                continue
            if item is _STOP:
                self._shutdown()
                break
            self._handle(item)
            handled += 1
        return handled

    # Worker -------------------------------------------------------------------------------------

    def _run(self):
        self._worker = threading.current_thread()
        self._begin()
        while True:
            deadline = self._next_deadline()
            timeout = None if deadline is None else max(0.0, deadline - self._clock())
            try:
                item = self._queue.get(timeout=timeout)
            except queue.Empty:
                item = None
            while item is not None:
                if item is _STOP:
                    self._shutdown()
                    return
                self._handle(item)
                try:
                    item = self._queue.get_nowait()
                except queue.Empty:
                    item = None
            self._tick()

    def _begin(self):
        self._began = True
        stored = store.load(self._state_dir, now=self._wall())
        self._route, self._own_rev, self._aec = stored.route, stored.own_rev, stored.aec
        self._vmic_exclusive = stored.vmic_exclusive
        self._machines = {ident: machine for ident, machine in stored.machines.items() if ident != self._own}
        self._dev = helper_module.dev_config(self._state_dir)
        self._dev_at = self._clock()
        dev_helper = (self._dev or {}).get("helper")
        if self._helper_cmd and dev_helper:
            self._helper_cmd = helper_module.locate(self._state_dir) or self._helper_cmd
        if self._helper_cmd:
            self._start_helper()
        self._dirty = True
        self._tick()

    def _helper_command(self):
        exclusive = self._vmic_exclusive and self._platform == "windows"
        cmd = list(self._helper_cmd)
        if "--control" not in cmd:
            return helper_module.command_line(cmd, self._state_dir, dev=self._dev is not None,
                                              vmic_exclusive=exclusive)
        if exclusive and helper_module.VMIC_EXCLUSIVE_ARG not in cmd:
            cmd.append(helper_module.VMIC_EXCLUSIVE_ARG)
        return cmd

    def _start_helper(self):
        self._helper_gen += 1
        generation = self._helper_gen

        def deliver(event):
            self._queue.put(("helper", event, generation))
        self._helper = self._helper_factory(self._helper_command(), deliver)
        self._helper.start()

    def _restart_helper(self):
        """A new helper with a new command line: the old one quits first (it may hold CABLE Input
        or a muted endpoint), its streams and flows are gone, and `ready` opens them again."""
        old = self._helper
        if old is None:
            return
        self._helper_gen += 1       # whatever the old one still says is dropped
        old.stop()
        self._log.info("audio: restarting the helper (low-latency virtual microphone %s)",
                       "on" if self._vmic_exclusive else "off")
        self._helper_gone("starting")
        self._start_helper()

    def _handle(self, item):
        if self._dev is not None:
            self._dev_event(item)
        try:
            kind = item[0]
            if kind == "link_up":
                self._on_link_up(item[1], item[2])
            elif kind == "link_down":
                self._on_link_down(item[1])
            elif kind == "link":
                self._on_link_message(item[1], item[2])
            elif kind == "helper":
                if len(item) < 3 or item[2] == self._helper_gen:
                    self._on_helper(item[1])
            elif kind == "call":
                getattr(self, item[1])(*item[2])
        except Exception:  # never let one bad event end the worker
            self._log.exception("audio: event %r failed", item[0] if isinstance(item, tuple) else item)
        self._dirty = True

    def _next_deadline(self):
        deadlines = [moment for moment in (self._save_at, self._wake_at) if moment is not None]
        if self._dev is not None:
            deadlines.append(self._dev_at + DEV_POLL)
        if self._levels_dirty:
            deadlines.append(self._levels_at + LEVELS_PERIOD)
        if any(peer.watching for peer in self._peers.values()):
            deadlines.append(self._stats_at + STATS_PERIOD)
        return min(deadlines) if deadlines else None

    def _tick(self):
        now = self._clock()
        if self._wake_at is not None and now >= self._wake_at:
            self._wake_at = None
            self._dirty = True
        if self._dev is not None and now - self._dev_at >= DEV_POLL:
            self._dev_at = now
            self._dev_poll()
        if self._dirty:
            self._dirty = False
            self._settle()
        if self._levels_dirty and now - self._levels_at >= LEVELS_PERIOD:
            self._levels_at = now
            self._levels_dirty = False
            levels = dict(self._levels)
            for fn in self._listeners(self._level_subscribers):
                self._call(fn, levels)
        if now - self._stats_at >= STATS_PERIOD and any(peer.watching for peer in self._peers.values()):
            self._stats_at = now
            self._send_stats()
        if self._save_at is not None and now >= self._save_at:
            self._save_now()

    def _listeners(self, where):
        with self._subscriber_lock:
            return list(where)

    def _call(self, fn, *args):
        try:
            fn(*args)
        except Exception:
            self._log.exception("audio: subscriber failed")

    def _shutdown(self):
        for (kind, peer), send in list(self._sends.items()):
            self._close_send(kind, peer, "shutdown")
        for stream, recv in list(self._recvs.items()):
            self._post(recv.peer, wire.close(stream, "shutdown"))
            self._helper_send({"cmd": "stop", "stream": stream})
        self._recvs.clear()
        if self._loop is not None:
            self._helper_send({"cmd": "stop", "stream": self._loop.stream})
            self._loop = None
        for peer in self._peers.values():
            if peer.flow is not None:
                self._helper_send({"cmd": "flow_close", "flow": peer.flow.id})
                peer.flow = None
        if self._helper is not None:
            self._helper.stop()
        self._save_now()
        if self._dev_events is not None:
            self._dev_events.close()
            self._dev_events = None
        self._stopped = True

    # Helpers on the worker ----------------------------------------------------------------------

    def _audio_peers(self, but=None):
        return [ident for ident, peer in sorted(self._peers.items()) if peer.audio and ident != but]

    def _direct(self):
        return frozenset(ident for ident, peer in self._peers.items() if peer.direct)

    def _post(self, peer, msg):
        state = self._peers.get(peer)
        if state is None or not state.audio:
            return False
        if not wire.size_ok(msg):
            self._log.warning("audio: %s too large, not sent", msg.get("type"))
            return False
        try:
            return bool(self._host.post(peer, msg))
        except Exception:
            self._log.exception("audio: post to a peer failed")
            return False

    def _helper_send(self, command, on_reply=None):
        if self._helper is None or self._helper_state != "ok":
            return False
        if on_reply is not None:
            self._req += 1
            command = dict(command, req=self._req)
            self._pending[self._req] = on_reply
        return self._helper.send(command)

    def _new_stream(self):
        used = set(self._recvs) | {send.stream for send in self._sends.values()}
        if self._loop is not None:
            used.add(self._loop.stream)
        while True:
            stream = secrets.randbits(31)
            if stream and stream not in used:
                return stream

    def _schedule_save(self):
        if self._save_at is None:
            self._save_at = self._clock() + SAVE_DELAY

    def _save_now(self):
        self._save_at = None
        machines = dict(self._machines)
        try:
            store.save(self._state_dir, store.Stored(self._route, self._own_rev, machines, self._aec,
                                                     self._vmic_exclusive))
        except OSError as error:
            self._log.warning("audio: could not save %s: %s", store.FILE_NAME, error)

    def _listen(self):
        return self._platform != "macos"

    def _hello(self):
        helper = self._helper_state if self._helper_state in wire.HELPER_STATES else "failed"
        return wire.hello(self._platform, helper, self._listen(), self._helper_info.get("features", ()))

    # Links --------------------------------------------------------------------------------------

    def _on_link_up(self, peer, caps):
        if peer == self._own:
            return
        existing = self._peers.get(peer)
        if existing is not None:
            was_audio = existing.audio
            existing.caps = caps
            if was_audio or not existing.audio:
                return
        else:
            self._peers[peer] = _Peer(caps)
        if "audio" not in caps:
            return
        self._post(peer, self._hello())
        own = self._machines.get(self._own)
        if own is not None:
            self._post(peer, wire.devices(own))
        for ident, machine in sorted(self._machines.items()):
            if ident not in (self._own, peer):
                self._post(peer, wire.devices(machine))
        self._post(peer, wire.route(self._route))
        if self._watching:
            self._post(peer, wire.watch(True))

    def _on_link_down(self, peer):
        state = self._peers.pop(peer, None)
        if state is None:
            return
        for kind in ("mic", "system"):
            send = self._sends.pop((kind, peer), None)
            if send is not None and send.helper_started:
                self._helper_send({"cmd": "stop", "stream": send.stream})
        for stream, recv in list(self._recvs.items()):
            if recv.peer == peer:
                del self._recvs[stream]
                self._helper_send({"cmd": "stop", "stream": stream})
        if state.flow is not None:
            self._helper_send({"cmd": "flow_close", "flow": state.flow.id})
        self._remote_streams.pop(peer, None)
        self._remote_howls.pop(peer, None)
        self._levels = {key: value for key, value in self._levels.items() if not key.startswith(peer + "/")}

    def _on_link_message(self, peer, message):
        state = self._peers.get(peer)
        if state is None or not state.audio:
            return
        read = wire.read(message, now=self._wall())
        if read is None:
            if wire.is_audio(message):
                self._log.debug("audio: malformed %s ignored", message.get("type"))
            return
        kind, data = read
        handler = getattr(self, "_on_" + kind)
        handler(peer, state, data)

    def _on_audio_hello(self, peer, state, data):
        before = state.hello
        state.hello = data
        if data["min_v"] > wire.VERSION:
            self._log.info("audio: a peer needs audio protocol %d, treating it as without audio", data["min_v"])
            return
        came_up = data["helper"] == "ok" and (before is None or before["helper"] != "ok")
        if came_up and self._helper_state == "ok" and self._offerer(peer):
            self._open_flow(peer)
        elif data["helper"] != "ok" and before is not None and before["helper"] == "ok":
            self._drop_flow(peer)

    def _on_audio_devices(self, peer, state, machine):
        if machine.id == self._own:
            if machine.rev > self._own_rev:
                # A list of ours newer than the one we know: this machine's audio.json went back.
                self._own_rev = machine.rev
                self._machines.pop(self._own, None)
            return
        held = self._machines.get(machine.id)
        if not model.accept_devices(held, machine, direct_from=machine.id == peer) or machine == held:
            return
        self._machines[machine.id] = machine
        self._schedule_save()
        for other in self._audio_peers(but=peer):
            if other != machine.id:
                self._post(other, wire.devices(machine))

    def _on_audio_route(self, peer, state, incoming):
        merged, changed, answer = model.merge_route(self._route, incoming)
        if changed:
            self._route = merged
            self._schedule_save()
            for other in self._audio_peers(but=peer):
                self._post(other, wire.route(merged))
        if answer:
            self._post(peer, wire.route(merged))

    def _broadcast_route(self):
        self._schedule_save()
        for peer in self._audio_peers():
            self._post(peer, wire.route(self._route))

    # Flows (§4.2) -------------------------------------------------------------------------------

    def _offerer(self, peer):
        state = self._peers.get(peer)
        if state is None or state.hello is None:
            return False
        mine, theirs = self._listen(), state.hello["listen"]
        if mine != theirs:
            return not mine
        return (protocol.read_id(self._own) or b"") < (protocol.read_id(peer) or b"")

    def _open_flow(self, peer):
        state = self._peers[peer]
        self._drop_flow(peer)
        flow = _Flow(secrets.token_hex(4), "offer", _key())
        state.flow = flow
        # `host`: the peer's address on the Beamer link, so the helper binds towards it instead of
        # listening on every interface (an unsigned listener on the Mac's LAN side prompts the firewall).
        host = self._host.peer_host(peer)
        self._helper_send({"cmd": "flow_open", "flow": flow.id, "peer": peer, "role": "offer",
                           "port": ANSWER_PORT if self._listen() else 0, "remote": None,
                           "host": host if _usable_host(host) else None, "key": flow.key, "rr_ms": RR_MS}, lambda event, flow=flow: self._flow_reply(peer, flow, event))

    def _flow_reply(self, peer, flow, event):
        if event.get("ev") == "error":
            self._log.warning("audio: flow_open failed: %s", event.get("code"))
            state = self._peers.get(peer)
            if state is not None and state.flow is flow:
                state.flow = None

    def _drop_flow(self, peer, notify=False):
        """Forget the flow with `peer`, and every stream on it."""
        state = self._peers.get(peer)
        if state is None or state.flow is None:
            return
        flow, state.flow = state.flow, None
        for kind in ("mic", "system"):
            send = self._sends.get((kind, peer))
            if send is not None and send.flow == flow.id:
                self._close_send(kind, peer, "helper")
        for stream, recv in list(self._recvs.items()):
            if recv.flow == flow.id:
                del self._recvs[stream]
                self._helper_send({"cmd": "stop", "stream": stream})
        self._helper_send({"cmd": "flow_close", "flow": flow.id})
        if notify:
            self._post(peer, wire.flow_close(flow.id))

    def _on_audio_flow(self, peer, state, data):
        phase = data["phase"]
        if not state.direct:
            return
        if phase == "want":
            # An offer already on its way answers it.
            pending = state.flow is not None and not state.flow.agreed
            if self._helper_state == "ok" and self._offerer(peer) and not pending:
                self._open_flow(peer)
        elif phase == "offer":
            if self._helper_state != "ok" or self._offerer(peer):
                return
            self._drop_flow(peer)
            flow = _Flow(data["flow"], "answer", data["key"])
            state.flow = flow
            host = self._host.peer_host(peer)
            remote = {"host": host, "port": data["port"]} if _usable_host(host) and data["port"] else None
            self._helper_send({"cmd": "flow_open", "flow": flow.id, "peer": peer, "role": "answer",
                               "port": ANSWER_PORT, "remote": remote, "key": flow.key, "rr_ms": RR_MS},
                              lambda event, flow=flow: self._flow_reply(peer, flow, event))
        elif phase == "answer":
            flow = state.flow
            if flow is None or flow.id != data["flow"] or flow.role != "offer":
                return
            flow.agreed = True
            host = self._host.peer_host(peer)
            if _usable_host(host) and data["port"]:
                self._helper_send({"cmd": "flow_remote", "flow": flow.id, "host": host, "port": data["port"]})
        elif phase == "close":
            if state.flow is not None and state.flow.id == data["flow"]:
                self._drop_flow(peer)

    def _flow_peer(self, flow_id):
        for ident, state in self._peers.items():
            if state.flow is not None and state.flow.id == flow_id:
                return ident, state.flow
        return None, None

    # Streams ------------------------------------------------------------------------------------

    def _on_audio_open(self, peer, state, data):
        stream = data["stream"]
        if not state.direct or self._helper_state != "ok" or state.flow is None or state.flow.id != data["flow"]:
            self._post(peer, wire.opened(stream, False, "helper"))
            return
        own = self._own_machine()
        problem = planner.sink_problem(self._route, own, data["sink"]) if own is not None else "helper"
        if problem is not None:
            self._post(peer, wire.opened(stream, False, problem))
            return
        for old, recv in list(self._recvs.items()):
            if recv.peer == peer and recv.kind == data["kind"] or old == stream:
                del self._recvs[old]
                self._helper_send({"cmd": "stop", "stream": old})
        if data["sink"].get("virtual_mic"):
            sink = {"virtual_mic": True}
        else:
            device, _missing = planner.resolve_output(own, data["sink"]["device"])
            sink = {"device": None if data["sink"]["device"] is None else device.key}
        self._recvs[stream] = _Recv(peer, data["kind"], data["flow"], data["sink"])

        def reply(event):
            if event.get("ev") == "ok":
                self._post(peer, wire.opened(stream, True))
                return
            self._recvs.pop(stream, None)
            code = event.get("code")
            self._post(peer, wire.opened(stream, False, code if code in wire.OPEN_ERRORS else "helper"))
        self._helper_send({"cmd": "recv", "stream": stream, "flow": data["flow"], "kind": data["kind"],
                           "codec": data["codec"], "key": data["key"], "sink": sink}, reply)

    def _send_for_stream(self, stream):
        for key, send in self._sends.items():
            if send.stream == stream:
                return key, send
        return None, None

    def _on_audio_opened(self, peer, state, data):
        key, send = self._send_for_stream(data["stream"])
        if send is None or key[1] != peer or send.state != "opening":
            return
        if not data["ok"]:
            send.state, send.error = "failed", data["error"]
            send.retry_token = self._retry_token(peer)
            return
        send.state = "starting"
        command = {"cmd": "send", "stream": send.stream, "flow": send.flow, "kind": send.spec.kind,
                   "source": send.source, "codec": wire.CODECS[send.spec.kind], "key": send.key,
                   "mute_local": send.spec.mute_local,
                   "aec": {"reference": send.spec.aec_reference} if send.spec.aec else None}
        send.helper_started = True

        def reply(event, send=send, key=key):
            if event.get("ev") == "error" and self._sends.get(key) is send:
                send.state, send.error = "failed", event.get("code")
                send.retry_token = self._retry_token(peer)
                send.helper_started = False
                self._post(peer, wire.close(send.stream, "error"))
        if not self._helper_send(command, reply):
            send.state, send.error = "failed", "helper"
            send.helper_started = False

    def _on_audio_close(self, peer, state, data):
        stream = data["stream"]
        recv = self._recvs.get(stream)
        if recv is not None and recv.peer == peer:
            del self._recvs[stream]
            self._helper_send({"cmd": "stop", "stream": stream})
            return
        key, send = self._send_for_stream(stream)
        if send is not None and key[1] == peer:
            if send.helper_started:
                self._helper_send({"cmd": "stop", "stream": stream})
            send.helper_started = False
            send.state, send.error = "failed", data["reason"]
            send.retry_token = self._retry_token(peer)

    def _on_audio_frame(self, peer, state, data):
        if state.flow is not None and state.flow.id == data["flow"]:
            self._helper_send({"cmd": "link_in", "flow": data["flow"], "p": data["p"]})

    def _on_audio_watch(self, peer, state, data):
        state.watching = data["on"]
        if state.watching:
            self._helper_send({"cmd": "meter", "on": True, "period_ms": METER_PERIOD_MS})
        elif not self._watching and not any(other.watching for other in self._peers.values()):
            self._helper_send({"cmd": "meter", "on": False, "period_ms": METER_PERIOD_MS})

    def _on_audio_stats(self, peer, state, data):
        if not self._watching:
            return
        self._levels = {key: value for key, value in self._levels.items() if not key.startswith(peer + "/")}
        for (side, key), level in data["levels"].items():
            self._levels[f"{peer}/{side}/{key}"] = level
        self._remote_streams[peer] = data["streams"]
        self._remote_howls[peer] = data.get("howls", [])
        self._levels_dirty = True

    def _on_audio_test(self, peer, state, data):
        self._helper_send({"cmd": "test_input", "device": data["device"], "seconds": data["seconds"]})

    def _send_stats(self):
        levels = {}
        for name, level in self._levels.items():
            machine, _slash, rest = name.partition("/")
            if machine == self._own:
                side, _slash, key = rest.partition("/")
                levels[f"{side}:{key}"] = [round(level[0], 1), round(level[1], 1)]
        streams = {str(stream): {"path": values.get("path", ""), "jb_ms": values.get("jb_ms"),
                                 "loss_pct": values.get("loss_pct"), "rtt_ms": values.get("rtt_ms")}
                   for stream, values in self._stats.items()}
        howls = [{"to": target, "freq_hz": round(float(howl["freq_hz"]), 1), "active": howl["active"]}
                 for target, howl in sorted(self._howls.items())]
        msg = wire.stats(self._clock() * 1000, levels, streams, howls)
        for ident, peer in self._peers.items():
            if peer.watching:
                self._post(ident, msg)

    def _retry_token(self, peer):
        machine = self._machines.get(peer)
        state = self._peers.get(peer)
        return (model.route_data(self._route).__repr__(), machine.rev if machine else None,
                state.flow.id if state and state.flow else None)

    def _close_send(self, kind, peer, reason):
        send = self._sends.pop((kind, peer), None)
        if send is None:
            return
        if send.state != "failed" or reason == "shutdown":
            self._post(peer, wire.close(send.stream, reason))
        if send.helper_started:
            self._helper_send({"cmd": "stop", "stream": send.stream})
        self._stats.pop(send.stream, None)

    def _source(self, spec):
        if spec.kind == "system":
            return {"system": True}
        if self._test_source is not None and self._dev is not None:
            return {"test": self._test_source}
        return {"device": spec.source}

    def _open_send(self, spec):
        state = self._peers[spec.peer]
        stream = self._new_stream()
        send = _Send(spec, stream, _key(), state.flow.id, self._source(spec))
        self._sends[(spec.kind, spec.peer)] = send
        entry = self._route.mic if spec.kind == "mic" else model.output_of(self._route, self._own)
        sink = spec.sink
        if "device" in sink:
            sink = {"device": model.ref_data(sink["device"])}
        self._post(spec.peer, wire.open_stream(stream, state.flow.id, spec.kind, wire.CODECS[spec.kind], send.key,
                                               sink, entry.stamp))

    def _apply(self, plan):
        wanted = {}
        for key, spec in plan.sends.items():
            state = self._peers.get(spec.peer)
            if state is not None and state.flow is not None and state.flow.agreed and self._helper_state == "ok":
                wanted[key] = spec
        for key, send in list(self._sends.items()):
            spec = wanted.get(key)
            state = self._peers.get(key[1])
            flow = state.flow.id if state is not None and state.flow is not None else None
            if spec is None:
                reason = "demand" if key[0] == "mic" and self._route.mic.machine == self._own else "route"
                self._close_send(*key, reason)
            elif spec != send.spec or flow != send.flow or self._source(spec) != send.source:
                self._close_send(*key, "route")
            elif send.state == "failed" and send.retry_token != self._retry_token(key[1]):
                self._close_send(*key, "route")
        for key, spec in wanted.items():
            if key not in self._sends:
                self._open_send(spec)
        # The local loop (§6.3).
        if self._loop is not None and (plan.loop is None or plan.loop != self._loop.spec):
            self._helper_send({"cmd": "stop", "stream": self._loop.stream})
            self._loop = None
        if plan.loop is not None and self._loop is None and self._helper_state == "ok":
            loop = _Loop(plan.loop, self._new_stream())
            self._loop = loop

            def reply(event, loop=loop):
                if event.get("ev") == "error" and self._loop is loop:
                    loop.state = "failed"
            self._helper_send({"cmd": "loop", "stream": loop.stream, "source": {"device": plan.loop.source},
                               "sink": {"virtual_mic": True}}, reply)
        # Streams this machine takes whose sink went away (§6.4 rule 5).
        own = self._own_machine()
        for stream, recv in list(self._recvs.items()):
            problem = planner.sink_problem(self._route, own, recv.sink) if own is not None else "helper"
            if problem is not None:
                del self._recvs[stream]
                self._helper_send({"cmd": "stop", "stream": stream})
                self._post(recv.peer, wire.close(stream, "device_gone" if problem != "conflict" else "route"))

    # Helper events ------------------------------------------------------------------------------

    def _on_helper(self, event):
        name = event.get("ev")
        if name == "_state":
            self._helper_gone(event.get("state", "starting"))
        elif name == "ready":
            self._helper_state = "ok"
            self._helper_info = {"version": str(event.get("version", "")), "platform": event.get("platform"),
                                 "features": [word for word in event.get("features", []) if isinstance(word, str)]}
            if event.get("restored"):
                self._log.warning("audio: the helper restored %s muted endpoint(s) from its journal",
                                  event.get("restored"))
            self._helper_send({"cmd": "devices"})
            if self._watching or any(peer.watching for peer in self._peers.values()):
                self._helper_send({"cmd": "meter", "on": True, "period_ms": METER_PERIOD_MS})
            for peer in self._audio_peers():
                self._post(peer, self._hello())
            for peer in sorted(self._direct()):
                if self._offerer(peer):
                    self._open_flow(peer)
                else:
                    self._post(peer, wire.flow_want())
        elif name == "devices":
            self._devices_event = event
        elif name in ("ok", "error"):
            reply = self._pending.pop(event.get("req"), None)
            if reply is not None:
                reply(event)
            elif name == "error":
                self._log.debug("audio: helper error %s: %s", event.get("code"), event.get("msg"))
        elif name == "flow_bound":
            self._on_flow_bound(event)
        elif name == "flow_state":
            _peer, flow = self._flow_peer(event.get("flow"))
            if flow is not None:
                flow.state = event.get("state", flow.state)
                flow.rtt_ms = event.get("rtt_ms") if isinstance(event.get("rtt_ms"), (int, float)) else None
        elif name == "link_out":
            self._on_link_out(event)
        elif name == "stream_state":
            self._on_stream_state(event)
        elif name == "levels":
            self._on_levels(event)
        elif name == "stats":
            streams = event.get("streams")
            if isinstance(streams, dict):
                self._stats = {int(key): value for key, value in streams.items()
                               if str(key).isdigit() and isinstance(value, dict)}
        elif name == "howl":
            self._on_howl(event)
        elif name == "permission":
            before = dict(self._permission)
            for what in ("mic", "system_audio"):
                if isinstance(event.get(what), str):
                    self._permission[what] = event[what]
            # A prompt was answered: streams that failed waiting for it open again (§6.4 rule 4 retry).
            answered = set()
            if self._permission.get("mic") == "granted" and before.get("mic") not in (None, "granted"):
                answered.add("mic")
            if before.get("system_audio") == "undetermined" and self._permission.get("system_audio") != "undetermined":
                answered.add("system")
            for send in self._sends.values():
                if send.spec.kind in answered and send.state == "failed":
                    send.retry_token = None
            if "mic" in answered:
                if self._loop is not None and self._loop.state == "failed":
                    self._helper_send({"cmd": "stop", "stream": self._loop.stream})
                    self._loop = None

    def _on_howl(self, event):
        """The howl guard of a system sound stream ducked it, or let go (§3.3 `howl`)."""
        key, send = self._send_for_stream(event.get("stream"))
        if send is None or key[0] != "system" or not isinstance(event.get("active"), bool):
            return
        freq = event.get("freq_hz") if isinstance(event.get("freq_hz"), (int, float)) else 0.0
        target = key[1]
        if event["active"]:
            self._log.warning("audio: feedback at %.0f Hz between %s's speakers and a microphone here; "
                              "this machine's sound to it is turned down", freq, self._host.peer_name(target) or target)
            self._howls[target] = {"freq_hz": float(freq), "active": True, "until": None}
        elif target in self._howls:
            self._howls[target] = dict(self._howls[target], active=False,
                                       until=self._clock() + HOWL_NOTICE_HOLD)

    def _howl_views(self):
        """The howls to show, ours and those watched peers report; ends ours whose stream is gone and
        drops those past their hold. Returns (views, earliest expiry or None)."""
        now = self._clock()
        for target, howl in list(self._howls.items()):
            if howl["active"] and ("system", target) not in self._sends:
                howl.update(active=False, until=now + HOWL_NOTICE_HOLD)
            if not howl["active"] and howl["until"] <= now:
                del self._howls[target]
        views = [model.HowlView(self._own, target, howl["freq_hz"], howl["active"])
                 for target, howl in sorted(self._howls.items())]
        for peer, howls in sorted(self._remote_howls.items()):
            views += [model.HowlView(peer, howl["to"], howl["freq_hz"], howl["active"]) for howl in howls]
        expiry = [howl["until"] for howl in self._howls.values() if howl["until"] is not None]
        return tuple(views), (min(expiry) if expiry else None)

    def _helper_gone(self, state):
        """The helper process is (re)starting or failed: its flows and streams are gone (§5.4)."""
        self._helper_state = state if state in ("starting", "failed") else "starting"
        for (kind, peer) in list(self._sends):
            send = self._sends.pop((kind, peer))
            self._post(peer, wire.close(send.stream, "helper"))
        for stream, recv in list(self._recvs.items()):
            self._post(recv.peer, wire.close(stream, "helper"))
        self._recvs.clear()
        self._loop = None
        self._stats.clear()
        for peer in self._peers.values():
            peer.flow = None
        self._pending.clear()
        for peer in self._audio_peers():
            self._post(peer, self._hello())

    def _on_flow_bound(self, event):
        peer, flow = self._flow_peer(event.get("flow"))
        if flow is None or not isinstance(event.get("port"), int):
            return
        flow.port = event["port"]
        flow.state = "probing"
        if flow.role == "offer":
            self._post(peer, wire.flow_offer(flow.id, flow.port, flow.key))
        else:
            flow.agreed = True
            self._post(peer, wire.flow_answer(flow.id, flow.port))

    def _on_link_out(self, event):
        peer, flow = self._flow_peer(event.get("flow"))
        packets = event.get("p")
        if flow is None or not isinstance(packets, list) or not packets:
            return
        try:
            depth = self._host.outbox_depth(peer)
        except Exception:
            depth = 0
        if depth > OUTBOX_LIMIT:
            self.link_out_dropped += len(packets)
            return
        for start in range(0, len(packets), wire.MAX_FRAME_PACKETS):
            self._post(peer, wire.frame(flow.id, packets[start:start + wire.MAX_FRAME_PACKETS]))

    def _on_stream_state(self, event):
        stream, state = event.get("stream"), event.get("state")
        key, send = self._send_for_stream(stream)
        if send is not None:
            if state == "running":
                send.state = "running"
            elif state in ("failed", "stopped") and send.state != "failed":
                send.state, send.error = "failed", event.get("error") or state
                send.retry_token = self._retry_token(key[1])
                send.helper_started = False
                self._post(key[1], wire.close(stream, "error"))
            return
        recv = self._recvs.get(stream)
        if recv is not None:
            if state == "running":
                recv.state = "running"
            elif state in ("failed", "stopped"):
                del self._recvs[stream]
                self._post(recv.peer, wire.close(stream, "device_gone" if event.get("error") == "no_device" else "error"))
            return
        if self._loop is not None and self._loop.stream == stream:
            self._loop.state = state if state in ("running", "failed", "starting") else "failed"

    def _on_levels(self, event):
        fresh = {}
        for side, name in (("in", "in"), ("out", "out")):
            values = event.get(side)
            if isinstance(values, dict):
                for key, level in values.items():
                    if isinstance(level, list) and len(level) == 2:
                        fresh[f"{self._own}/{name}/{key}"] = (float(level[0]), float(level[1]))
        self._levels = {key: value for key, value in self._levels.items() if not key.startswith(self._own + "/")}
        self._levels.update(fresh)
        self._levels_dirty = True

    # UI calls (on the worker) -------------------------------------------------------------------

    def _set_mic(self, machine, device):
        stamp = model.next_stamp(self._route, self._own, self._wall())
        self._route = dataclasses.replace(self._route, mic=model.MicRoute(machine, device, stamp))
        self._broadcast_route()

    def _set_output(self, machine, to, device):
        if to == machine:
            to, device = None, None
        stamp = model.next_stamp(self._route, self._own, self._wall())
        outputs = dict(self._route.outputs)
        outputs[machine] = model.OutputRoute(to, device if to is not None else None, stamp)
        self._route = dataclasses.replace(self._route, outputs=outputs)
        self._broadcast_route()

    def _set_form(self, machine, device_key, form):
        if form not in (None,) + model.OVERRIDE_FORMS:
            return
        stamp = model.next_stamp(self._route, self._own, self._wall())
        forms = dict(self._route.forms)
        forms[(machine, device_key)] = model.FormEntry(form, stamp)
        self._route = dataclasses.replace(self._route, forms=model.trim_forms(forms))
        self._broadcast_route()

    def _set_aec(self, mode):
        if mode in store.AEC_MODES and mode != self._aec:
            self._aec = mode
            self._schedule_save()

    def _set_vmic_exclusive(self, on):
        if on == self._vmic_exclusive:
            return
        self._vmic_exclusive = on
        self._schedule_save()
        if self._platform == "windows":
            self._restart_helper()

    def _watch(self, on):
        if on == self._watching:
            return
        self._watching = on
        if on or not any(peer.watching for peer in self._peers.values()):
            self._helper_send({"cmd": "meter", "on": on, "period_ms": METER_PERIOD_MS})
        for peer in self._audio_peers():
            self._post(peer, wire.watch(on))
        if not on:
            self._levels = {key: value for key, value in self._levels.items() if key.startswith(self._own + "/")}
            self._remote_howls.clear()

    def _test_input(self, machine, device_key):
        if machine == self._own:
            self._helper_send({"cmd": "test_input", "device": device_key, "seconds": 10})
        elif machine in self._direct():
            self._post(machine, wire.test(device_key, 10))

    def _retry_helper(self):
        if self._helper is not None:
            self._helper.retry()

    # Dev command files (§8.1) -------------------------------------------------------------------

    def _dev_poll(self):
        path = self._state_dir / DEV_CMD
        try:
            text = path.read_text(encoding="utf-8")
            path.unlink()
        except FileNotFoundError:
            text = None
        except OSError as error:
            self._log.debug("audio: dev command unreadable: %s", error)
            text = None
        if text:
            try:
                self._dev_command(json.loads(text))
            except (ValueError, TypeError, KeyError) as error:
                self._log.warning("audio: dev command ignored: %s", error)
            self._settle()
        status = {"snapshot": snapshot_data(self._snapshot), "stats": {str(key): value for key, value in self._stats.items()},
                  "link_out_dropped": self.link_out_dropped, "wall": self._wall(), "test_source": self._test_source}
        temporary = self._state_dir / f".{DEV_STATUS}.tmp"
        try:
            temporary.write_text(json.dumps(status, indent=1, sort_keys=True), encoding="utf-8")
            os.replace(temporary, self._state_dir / DEV_STATUS)
        except OSError as error:
            self._log.debug("audio: dev status not written: %s", error)

    def _dev_event(self, item):
        """Dev mode: append helper events (not levels) and link changes to DEV_EVENTS, stamped with
        this service's clock (the E2E harness puts them on the same timeline as its recordings)."""
        kind = item[0] if isinstance(item, tuple) else None
        if kind == "helper":
            if not isinstance(item[1], dict) or item[1].get("ev") == "levels":
                return
            record = dict(item[1])
        elif kind in ("link_up", "link_down"):
            record = {"ev": "_" + kind, "peer": item[1]}
        else:
            return
        record["mono"] = round(self._clock(), 6)
        record["wall"] = round(self._wall(), 3)
        path = self._state_dir / DEV_EVENTS
        try:
            if self._dev_events is None:
                self._dev_events = open(path, "a", encoding="utf-8")
            self._dev_events.write(json.dumps(record, separators=(",", ":")) + "\n")
            self._dev_events.flush()
            if self._dev_events.tell() > DEV_EVENTS_MAX:
                self._dev_events.close()
                self._dev_events = None
                os.replace(path, path.with_name(DEV_EVENTS + ".1"))
        except (OSError, TypeError, ValueError) as error:
            self._log.debug("audio: dev event not written: %s", error)

    def _dev_command(self, command):
        commands = command if isinstance(command, list) else [command]
        for item in commands:
            if "set_mic" in item:
                value = item["set_mic"]
                self._set_mic(value.get("machine"), model.ref_from(value.get("device")))
            if "set_output" in item:
                value = item["set_output"]
                self._set_output(value["machine"], value.get("to"), model.ref_from(value.get("device")))
            if "set_form" in item:
                value = item["set_form"]
                self._set_form(value["machine"], value["key"], value.get("form"))
            if "set_aec" in item:
                self._set_aec(item["set_aec"])
            if "set_vmic_exclusive" in item:
                self._set_vmic_exclusive(item["set_vmic_exclusive"] is True)
            if "test_source" in item:
                value = item["test_source"]
                signal = value.get("signal") if isinstance(value, dict) else None
                self._test_source = signal if _test_signal_ok(signal) else None
            if item.get("retry_helper"):
                self._retry_helper()
            if "helper_dev" in item and isinstance(item["helper_dev"], dict):
                # the helper's own dev switches (drop_udp, loss_pct, reset_stats); needs `--dev`
                self._helper_send(dict(item["helper_dev"], cmd="dev"))

    # Settling: own list, plan, snapshot ---------------------------------------------------------

    def _own_machine(self):
        return self._machines.get(self._own)

    def _compose_own(self):
        event = self._devices_event or {}
        status = self._helper_state

        def devices(entries, kind, default):
            result = []
            for entry in entries if isinstance(entries, list) else []:
                if not isinstance(entry, dict) or not isinstance(entry.get("key"), str):
                    continue
                key = entry["key"][:wire.MAX_KEY]
                override = model.form_of(self._route, self._own, key)
                form = entry.get("form") if entry.get("form") in model.FORMS else "unknown"
                transport = entry.get("transport") if entry.get("transport") in wire.TRANSPORTS else "other"
                channels = entry.get("channels") if isinstance(entry.get("channels"), int) else 0
                result.append(model.Device(key, str(entry.get("name", ""))[:wire.MAX_NAME], kind, transport,
                                           max(0, min(channels, 64)), override or form, key == default))
            return tuple(result[:wire.MAX_DEVICES])
        default_input = event.get("default_input") if isinstance(event.get("default_input"), str) else None
        default_output = event.get("default_output") if isinstance(event.get("default_output"), str) else None
        vmic = event.get("virtual_mic") if isinstance(event.get("virtual_mic"), dict) else {}
        kind = vmic.get("kind") if isinstance(vmic.get("kind"), str) and vmic.get("kind") else None
        return MachineAudio(
            id=self._own, name=self._name, platform=self._platform, rev=self._own_rev, status=status,
            inputs=devices(event.get("inputs"), "input", default_input),
            outputs=devices(event.get("outputs"), "output", default_output),
            default_input=default_input, default_output=default_output,
            virtual_mic=model.VirtualMic(kind, vmic.get("present") is True, vmic.get("in_use") is True),
            features=frozenset(self._helper_info.get("features", ())), links=self._direct(),
            helper=self._helper_info.get("version", ""))

    def _republish_own(self):
        fresh = self._compose_own()
        held = self._machines.get(self._own)
        if held is not None and dataclasses.replace(fresh, rev=held.rev) == held:
            return
        self._own_rev = max(int(self._wall() * 1000), self._own_rev + 1)
        own = dataclasses.replace(fresh, rev=self._own_rev)
        self._machines[self._own] = own
        self._schedule_save()
        for peer in self._audio_peers():
            self._post(peer, wire.devices(own))

    def _track_demand(self):
        now = self._clock()
        for ident, machine in self._machines.items():
            in_use = machine.virtual_mic.in_use
            if self._in_use.get(ident) and not in_use:
                self._demand_hold[ident] = now + planner.DEMAND_HOLD
            elif in_use:
                self._demand_hold.pop(ident, None)
            self._in_use[ident] = in_use
        self._demand_hold = {ident: until for ident, until in self._demand_hold.items() if until > now}

    def _settle(self):
        self._republish_own()
        self._track_demand()
        direct = self._direct()
        online = model.online(self._own, direct, self._machines)
        plan = planner.plan(self._own, self._machines, self._route, direct, online, self._aec, self._clock(),
                            self._demand_hold)
        self._apply(plan)
        howls, howl_expiry = self._howl_views()
        wakes = [moment for moment in (plan.wake_at, howl_expiry) if moment is not None]
        self._wake_at = min(wakes) if wakes else None
        snap = self._compose_snapshot(plan, direct, online, howls)
        if snap != self._snapshot:
            self._snapshot = snap
            for fn in self._listeners(self._subscribers):
                self._call(fn)

    def _path(self, peer):
        state = self._peers.get(peer)
        if state is None or state.flow is None:
            return ""
        return {"up": "udp", "link": "link"}.get(state.flow.state, "")

    def _rtt(self, peer):
        state = self._peers.get(peer)
        return state.flow.rtt_ms if state is not None and state.flow is not None else None

    def _compose_snapshot(self, plan, direct, online, howls=()):
        views = []
        for (kind, peer), send in sorted(self._sends.items()):
            stats = self._stats.get(send.stream, {})
            views.append(StreamView(send.stream, kind, self._own, peer, send.state, stats.get("path") or self._path(peer),
                                    stats.get("jb_ms"), stats.get("loss_pct"), stats.get("rtt_ms", self._rtt(peer))))
        for stream, recv in sorted(self._recvs.items()):
            stats = self._stats.get(stream, {})
            views.append(StreamView(stream, recv.kind, recv.peer, self._own, recv.state,
                                    stats.get("path") or self._path(recv.peer), stats.get("jb_ms"),
                                    stats.get("loss_pct"), stats.get("rtt_ms", self._rtt(recv.peer))))
        if self._loop is not None:
            views.append(StreamView(self._loop.stream, "mic", self._own, self._own, self._loop.state, "local",
                                    None, None, None))
        mic_status, output_status = plan.mic_status, dict(plan.output_status)
        for (kind, peer), send in self._sends.items():
            if send.state != "failed":
                continue
            failed = RouteStatus("error", send.error, {"to": peer})
            if kind == "mic":
                mic_status = failed
            else:
                output_status[self._own] = failed
        aec_active = any(send.spec.aec and send.state == "running" for (kind, _peer), send in self._sends.items()
                         if kind == "mic")
        return AudioSnapshot(own=self._own, machines=dict(self._machines), online=online, direct=direct,
                             route=self._route, mic_status=mic_status, output_status=output_status,
                             streams=tuple(views), helper=self._helper_state, permission=dict(self._permission),
                             aec_mode=self._aec, aec_active=aec_active, howls=howls,
                             vmic_exclusive=self._vmic_exclusive)
