# SPDX-License-Identifier: GPL-3.0-only
"""Which video streams this machine sends, and the camera route's status (dev/camera/PHASE2.md §6.4).

Pure. Every machine evaluates the same rules on the same cluster state and acts only on its own part
of the plan (`sends`, `warm`, `idle_placeholder`); the statuses describe the whole cluster for the
pages.

The active camera S feeds every online machine T != S whose Beamer Camera is installed, only while an
app there has it open (or for DEMAND_HOLD after), at the one cluster size (§6.3); S's own sharing
switch and camera permission gate everything (§6.1, rule 2). The dev synthetic source replaces the
camera and permission conditions, never the sharing switch.
"""

from dataclasses import dataclass, field
from typing import Mapping, Optional

from core.audio.model import MachineAudio
from core.camera import model
from core.camera.model import CameraRoute, CameraStatus, MachineCamera

DEMAND_HOLD = 3.0       # s: S keeps a sink's demand this long after its app closed the camera (§4.7)


@dataclass(frozen=True)
class VideoSendSpec:
    peer: str
    device: Optional[str]    # camera key, None = S's system-preferred camera (followed)
    w: int
    h: int
    fps: int
    bitrate: int


@dataclass(frozen=True)
class CameraPlan:
    sends: Mapping[str, VideoSendSpec] = field(default_factory=dict)   # peer -> spec, streams this machine sends
    status: CameraStatus = CameraStatus("off")                         # the active camera's overall state
    sinks: Mapping[str, CameraStatus] = field(default_factory=dict)    # per machine with or lacking a vcam
    needs_permission: bool = False       # own machine is S and its permission is undetermined (§8.5)
    idle_placeholder: Optional[str] = None   # own machine is a sink: vcam `idle` when no sink is open
    warm: Optional[tuple[int, int, int]] = None   # own machine is S, ready to send: standby (w, h, bitrate)
    wake_at: Optional[float] = None


INSTALL_STATUS = {"missing": "not_installed", "outdated": "outdated", "broken": "broken",
                  "unsupported": "no_virtual_camera"}
_DEMAND_PROBLEMS = ("unreachable", "no_direct_path")
_INSTALL_PROBLEMS = ("not_installed", "outdated", "broken")


def demand(machine: MachineCamera, now: float, demand_hold: Mapping[str, float]) -> bool:
    """Whether an app on `machine` has Beamer Camera open, or closed it less than DEMAND_HOLD ago."""
    return machine.virtual_camera.in_use or demand_hold.get(machine.id, 0.0) > now


def peers_of(own: str, machine: str, direct: frozenset, audio_machines: Mapping[str, MachineAudio]) -> frozenset:
    """The direct peers of `machine`: this machine's own, else what its audio list says (§5.3)."""
    if machine == own:
        return direct
    held = audio_machines.get(machine)
    return held.links if held is not None else frozenset()


def source_problem(own: str, src: Optional[MachineCamera], route: CameraRoute, online: frozenset):
    """(status or None, device key or None) for S per rule 2. None status: S can send; the key is the
    camera to open (None = follow S's preferred camera, or the dev synthetic source)."""
    machine = route.camera.machine
    params = {"machine": machine}
    if src is None or machine not in online:
        return CameraStatus("waiting_machine", None, params), None
    if "camera_capture" not in src.features:
        if src.status in ("starting",):
            return CameraStatus("waiting_machine", "starting", params), None
        if src.status in ("failed", "no_video") and src.platform == "macos":
            return CameraStatus("error", "helper", params), None
        return CameraStatus("no_camera", "unsupported", params), None
    if src.status != "ok":
        state = "waiting_machine" if src.status == "starting" else "error"
        return CameraStatus(state, "helper" if state == "error" else "starting", params), None
    if src.sharing is False:
        return CameraStatus("sharing_off", None, params), None
    if src.test_source:
        return None, None
    device = route.camera.device
    if device is None:
        if src.preferred is None or model.camera_by_key(src.cameras, src.preferred) is None:
            return CameraStatus("no_camera", None, params), None
        key = None
    else:
        found = model.resolve_camera(device, src.cameras)
        if found is None:
            return CameraStatus("waiting_device", None, dict(params, device=device.name)), None
        key = found.key
    if src.permission == "granted":
        return None, key
    if src.permission in ("denied", "restricted"):
        return CameraStatus("denied", None, params), None
    return CameraStatus("waiting_permission", None, params), None


def _idle_placeholder(status: CameraStatus) -> str:
    """Rule 7: what a sink with no open stream shows."""
    if status.state in ("off", "sharing_off", "waiting_permission", "denied", "error"):
        return status.state
    return "waiting"


def plan_camera(own: str, machines: Mapping[str, MachineCamera], audio_machines: Mapping[str, MachineAudio],
                route: CameraRoute, direct: frozenset, online: frozenset, now: float,
                demand_hold: Mapping[str, float], no_udp: frozenset) -> CameraPlan:
    """The plan of machine `own` (§6.4). `machines` holds every camera list, `own`'s included;
    `direct` are `own`'s direct peers that speak `camera1`; `no_udp` those of them with no UDP path."""
    pending = [until for until in demand_hold.values() if until > now]
    wake_at = min(pending) if pending else None
    own_machine = machines.get(own)
    own_is_sink = own_machine is not None and "virtual_camera" in own_machine.features
    shown = {ident: machine for ident, machine in machines.items()
             if ident in online and machine.features & set(model.FEATURES)}
    source = route.camera.machine
    if source is None:
        sinks = {ident: CameraStatus("off") for ident, machine in shown.items() if "virtual_camera" in machine.features}
        return CameraPlan({}, CameraStatus("off"), sinks, False, "off" if own_is_sink else None, None, wake_at)

    src = machines.get(source)
    problem, device_key = source_problem(own, src, route, online)
    quality = model.size_for(route.quality.size)
    w, h, bitrate = quality
    needs_permission = (source == own and src is not None and "camera_capture" in src.features
                        and not src.test_source and src.sharing is not False and src.permission == "undetermined")
    warm = quality if source == own and problem is None else None

    reachable = peers_of(own, source, direct, audio_machines)
    capture = src.capture if src is not None else model.IDLE_CAPTURE
    sinks, sends = {}, {}
    for target in sorted(shown):
        machine = shown[target]
        if target == source or "virtual_camera" not in machine.features:
            sinks[target] = CameraStatus("no_virtual_camera", None, {"machine": target})
            continue
        params = {"machine": source, "to": target}
        install = machine.virtual_camera.install
        if install != "ok":
            sinks[target] = CameraStatus(INSTALL_STATUS.get(install, "broken"), None, params)
            continue
        if problem is not None:
            sinks[target] = CameraStatus(problem.state, problem.reason, dict(problem.params, to=target))
            continue
        if not demand(machine, now, demand_hold):
            sinks[target] = CameraStatus("idle", None, params)
            continue
        if target not in reachable:
            sinks[target] = CameraStatus("unreachable", None, params)
            continue
        if source == own and target in no_udp:
            sinks[target] = CameraStatus("no_direct_path", None, params)
            continue
        if capture.state == "failed":
            sinks[target] = CameraStatus("error", capture.error, params)
        elif capture.state in ("running", "interrupted"):
            sinks[target] = CameraStatus("streaming", None, params)
        else:
            sinks[target] = CameraStatus("starting", None, params)
        if source == own:
            sends[target] = VideoSendSpec(target, device_key, w, h, model.FPS, bitrate)

    status = overall(source, problem, sinks)
    idle = _idle_placeholder(status) if own_is_sink else None
    if own_is_sink and capture.state == "failed" and problem is None:
        idle = "error"
    return CameraPlan(sends, status, sinks, needs_permission, idle, warm, wake_at)


def overall(source: str, problem: Optional[CameraStatus], sinks: Mapping[str, CameraStatus]) -> CameraStatus:
    """The route's one status line (§8.4): S's problem first, then what the sinks do."""
    if problem is not None:
        return problem
    def having(*states):
        return sorted(target for target, status in sinks.items() if status.state in states)
    for state in ("streaming", "starting"):
        targets = having(state)
        if targets:
            return CameraStatus(state, None, {"machine": source, "to": " ".join(targets)})
    failed = [(target, status) for target, status in sorted(sinks.items()) if status.state == "error"]
    if failed:
        return CameraStatus("error", failed[0][1].reason, {"machine": source, "to": failed[0][0]})
    for state in _DEMAND_PROBLEMS:
        targets = having(state)
        if targets:
            return CameraStatus(state, None, {"machine": source, "to": " ".join(targets)})
    if having("idle"):
        return CameraStatus("idle", None, {"machine": source, "to": " ".join(having("idle"))})
    for state in _INSTALL_PROBLEMS:
        targets = having(state)
        if targets:
            return CameraStatus(state, None, {"machine": source, "to": " ".join(targets)})
    return CameraStatus("no_virtual_camera", None, {"machine": source})
