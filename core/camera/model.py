# SPDX-License-Identifier: GPL-3.0-only
"""The camera control plane's snapshot types and the pure rules over them (dev/camera/PHASE2.md §7.2).

Pure, with no AppKit, no Qt, no threads and no I/O: both pages read these types, and the part, the
planner and the store build them. This file is a contract (PHASE2 §12): CW6 and CW7 code against it,
so its types change only with the operator.

Every type is a frozen dataclass; Mapping fields default to an empty dict of their own, never a
shared one. The route is stamped entry by entry (§6.2), with the audio route's rules: newer is a
higher `set_at`, or the same `set_at` and the larger `by` compared as bytes (`settings_sync`). Device
lists have one author each, the machine itself, and a higher `rev` wins (§5.3).
"""

import json
from dataclasses import dataclass, field, replace
from typing import Any, Mapping, Optional

from core import settings_sync

# Values the wire and the planner use (§5.2, §6.4).
STATUSES = ("ok", "starting", "failed", "no_video")          # a machine's camera side (camera_devices.status)
FEATURES = ("camera_capture", "virtual_camera")               # backend words a camera_devices list announces
TRANSPORTS = ("builtin", "usb", "continuity", "virtual", "other")
PERMISSIONS = ("granted", "denied", "undetermined", "restricted", "unknown")
CAPTURE_STATES = ("idle", "starting", "running", "interrupted", "failed")
INSTALL_STATES = ("ok", "missing", "outdated", "broken", "unsupported")
EFFECTS = ("center_stage", "portrait", "studio_light", "background", "reactions", "edge_light")
QUALITIES = ("720p", "1080p")
DEFAULT_QUALITY = "720p"
STATES = ("off", "idle", "starting", "streaming", "reduced", "sharing_off", "waiting_permission", "denied",
          "waiting_device", "no_camera", "waiting_machine", "unreachable", "no_direct_path", "not_installed",
          "outdated", "broken", "no_virtual_camera", "error")
PLACEHOLDER_STATES = ("off", "waiting", "waiting_permission", "sharing_off", "denied", "error", "starting",
                      "reconnecting")
IDLE_STATES = ("off", "waiting", "waiting_permission", "sharing_off", "denied", "error")

# Quality -> (width, height, bitrate in bit/s) of the one size the cluster sends (§4.3, §6.1).
SIZES = {"720p": (1280, 720, 2_500_000), "1080p": (1920, 1080, 5_000_000)}
FPS = 30


@dataclass(frozen=True)
class CameraRef:
    key: str
    name: str


@dataclass(frozen=True)
class Camera:
    key: str
    name: str
    transport: str           # TRANSPORTS
    native: tuple[str, ...]  # "1920x1080@30", diagnostic only
    preferred: bool          # macOS systemPreferredCamera


@dataclass(frozen=True)
class Capture:
    state: str               # CAPTURE_STATES
    by_other: bool
    effects: tuple[str, ...]
    error: Optional[str]


@dataclass(frozen=True)
class VirtualCamera:
    install: str             # INSTALL_STATES
    present: bool
    in_use: bool             # demand: an app on this machine has Beamer Camera open
    app: Optional[str]       # best effort, None = unknown
    want: Optional[tuple[int, int, int]]   # (w, h, fps) of the app's media type while in use


@dataclass(frozen=True)
class MachineCamera:
    id: str
    platform: str
    rev: int
    status: str              # STATUSES
    features: frozenset[str]
    cameras: tuple[Camera, ...]
    preferred: Optional[str]
    permission: Optional[str]
    sharing: Optional[bool]  # the Mac-local kill switch, authored only by that machine (§6.1); None elsewhere
    capture: Capture
    virtual_camera: VirtualCamera
    test_source: Optional[str] = None   # dev synthetic source (§6.4 rule 2, §7.4); None in release


@dataclass(frozen=True)
class Stamp:
    set_at: int
    by: str


@dataclass(frozen=True)
class CameraEntry:
    machine: Optional[str]   # None = off
    device: Optional[CameraRef]   # None = that machine's system-preferred camera, followed
    stamp: Stamp


@dataclass(frozen=True)
class QualityEntry:
    size: str                # QUALITIES
    stamp: Stamp


@dataclass(frozen=True)
class CameraRoute:
    camera: CameraEntry
    quality: QualityEntry
    extra: Mapping[str, Any] = field(default_factory=dict)   # unknown top-level entries, kept and forwarded


@dataclass(frozen=True)
class CameraStatus:
    state: str               # STATES
    reason: Optional[str] = None
    params: Mapping[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class VideoStreamView:
    stream: int
    source: str              # machine id
    target: str              # machine id
    state: str               # "opening" | "starting" | "running" | "failed" | "stopped"
    path: str                # "udp" | "link" | ""
    w: int
    h: int
    fps: Optional[float]
    kbps: Optional[float]
    delay_ms: Optional[float]
    frozen_ms: Optional[float]


@dataclass(frozen=True)
class LocalVcam:
    loaded_version: int
    dll_version: int
    bundled_version: int


@dataclass(frozen=True)
class CameraSnapshot:
    own: str
    machines: Mapping[str, MachineCamera]
    route: CameraRoute
    status: CameraStatus
    sinks: Mapping[str, CameraStatus]
    streams: tuple[VideoStreamView, ...]
    permission: Optional[str]           # own machine
    needs_permission: bool              # own machine
    sharing: Optional[bool]             # own machine
    vcam_install_offer: Optional[str]   # "install" | "update" | "repair" | None (this PC only)
    vcam_local: Optional[LocalVcam]     # this PC only: versions for "Restart to finish the update" (§9.2)


ZERO = Stamp(0, "")
CAMERA_OFF = CameraEntry(None, None, ZERO)
QUALITY_DEFAULT = QualityEntry(DEFAULT_QUALITY, ZERO)
IDLE_CAPTURE = Capture("idle", False, (), None)
NO_VIRTUAL_CAMERA = VirtualCamera("unsupported", False, False, None, None)


def empty_route() -> CameraRoute:
    """Camera off at 720p: the route of a machine that has never seen one."""
    return CameraRoute(CAMERA_OFF, QUALITY_DEFAULT, {})


def empty_snapshot(own: str) -> CameraSnapshot:
    return CameraSnapshot(own=own, machines={}, route=empty_route(), status=CameraStatus("off"), sinks={},
                          streams=(), permission=None, needs_permission=False, sharing=None,
                          vcam_install_offer=None, vcam_local=None)


def size_for(quality: str) -> tuple[int, int, int]:
    """(width, height, bitrate) sent at `quality`; an unknown value is the default."""
    return SIZES.get(quality, SIZES[DEFAULT_QUALITY])


def newer(one: Stamp, other: Stamp) -> bool:
    """Whether `one` is newer than `other`: a higher `set_at`, or the same and the larger `by` as bytes."""
    return settings_sync._newer((one.set_at, one.by), (other.set_at, other.by))


def max_stamp(route: CameraRoute) -> int:
    stamps = [route.camera.stamp.set_at, route.quality.stamp.set_at]
    for value in route.extra.values():
        if isinstance(value, dict) and type(value.get("set_at")) is int:
            stamps.append(value["set_at"])
    return max(stamps)


def next_stamp(route: CameraRoute, by: str, now: float) -> Stamp:
    """The stamp for a change made here: newer than every stamp in `route` (`settings_sync.next_stamp`)."""
    return Stamp(settings_sync.next_stamp(max_stamp(route), now), by)


def _pick(held, incoming):
    """The newer of two entries. Equal stamps are the same edit; should they ever differ, the larger
    repr wins, so every machine picks the same one."""
    if held is None:
        return incoming
    if incoming is None or newer(held.stamp, incoming.stamp):
        return held
    if newer(incoming.stamp, held.stamp):
        return incoming
    return incoming if repr(incoming) > repr(held) else held


def _extra_stamp(value) -> Stamp:
    if isinstance(value, dict) and type(value.get("set_at")) is int and isinstance(value.get("by", ""), str):
        return Stamp(value["set_at"], value.get("by", ""))
    return ZERO


def _pick_extra(held, incoming):
    """An unknown entry (e.g. a later `sinks`): the stamped newer one when both carry stamps, else the
    larger canonical repr, so every machine keeps the same value without understanding it."""
    if held is None:
        return incoming
    if incoming is None or held == incoming:
        return held
    one, other = _extra_stamp(held), _extra_stamp(incoming)
    if newer(one, other):
        return held
    if newer(other, one):
        return incoming
    return incoming if _canonical(incoming) > _canonical(held) else held


def _canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), default=str)


def merge_route(held: CameraRoute, incoming: CameraRoute) -> tuple[CameraRoute, bool, bool]:
    """Entry-wise merge (§6.2): (merged, changed, answer). `changed`: the merge differs from `held`, so
    it is stored and sent on. `answer`: the merge has an entry the sender lacks or holds older, so the
    sender should get ours back. Commutative and idempotent."""
    camera = _pick(held.camera, incoming.camera)
    quality = _pick(held.quality, incoming.quality)
    extra = {name: _pick_extra(held.extra.get(name), incoming.extra.get(name))
             for name in sorted(set(held.extra) | set(incoming.extra))}
    merged = CameraRoute(camera, quality, extra)
    answer = (camera != incoming.camera or quality != incoming.quality
              or any(incoming.extra.get(name) != value for name, value in extra.items()))
    return merged, merged != held, answer


def accept_devices(held: Optional[MachineCamera], incoming: MachineCamera, direct_from: bool) -> bool:
    """Whether a received `camera_devices` list replaces the one held for its machine (§5.3): a higher
    `rev`, or the list came straight from the machine it describes and differs from ours.

    A list only ever speaks for the machine it describes, so its `sharing` is that machine's own
    switch (§6.1): `sanitize` has already cleared it for a machine without `camera_capture`, and the
    part never takes a list describing itself (its own switch is local state)."""
    if held is None:
        return True
    if incoming.rev > held.rev:
        return True
    return direct_from and incoming != held


def sanitize(machine: MachineCamera) -> MachineCamera:
    """The list as the planner may trust it: `sharing` only on a machine with `camera_capture`
    (default on when it says nothing), `test_source` only there too."""
    if "camera_capture" not in machine.features:
        if machine.sharing is None and machine.test_source is None:
            return machine
        return replace(machine, sharing=None, test_source=None)
    if machine.sharing is None:
        return replace(machine, sharing=True)
    return machine


def resolve_camera(ref: Optional[CameraRef], cameras) -> Optional[Camera]:
    """The camera `ref` names among `cameras`: the same key, else the only one with its name, else None
    (none, or an ambiguous name). None for a None ref: the caller follows the preferred camera."""
    if ref is None:
        return None
    cameras = tuple(cameras)
    for camera in cameras:
        if camera.key == ref.key:
            return camera
    named = [camera for camera in cameras if camera.name == ref.name]
    return named[0] if len(named) == 1 else None


def camera_by_key(cameras, key: Optional[str]) -> Optional[Camera]:
    if key is None:
        return None
    for camera in cameras:
        if camera.key == key:
            return camera
    return None


# Data <-> types. The dicts are the wire's and camera.json's (§5.2, §6.2); `core.camera.wire`
# validates them first, so these assume well-formed input.

def ref_data(ref: Optional[CameraRef]):
    return None if ref is None else {"key": ref.key, "name": ref.name}


def ref_from(data) -> Optional[CameraRef]:
    return None if data is None else CameraRef(data["key"], data["name"])


def stamp_data(stamp: Stamp) -> dict:
    return {"set_at": stamp.set_at, "by": stamp.by}


def route_data(route: CameraRoute) -> dict:
    data = {name: value for name, value in sorted(route.extra.items())}
    data["camera"] = {"machine": route.camera.machine, "device": ref_data(route.camera.device),
                      **stamp_data(route.camera.stamp)}
    data["quality"] = {"size": route.quality.size, **stamp_data(route.quality.stamp)}
    return data


def route_from(data) -> CameraRoute:
    camera = data.get("camera") or {}
    entry = (CameraEntry(camera.get("machine"), ref_from(camera.get("device")), Stamp(camera["set_at"], camera["by"]))
             if "set_at" in camera else CAMERA_OFF)
    quality = data.get("quality") or {}
    size = (QualityEntry(quality["size"], Stamp(quality["set_at"], quality["by"]))
            if "set_at" in quality and quality.get("size") in QUALITIES else QUALITY_DEFAULT)
    extra = {name: value for name, value in data.items() if name not in ("camera", "quality")}
    return CameraRoute(entry, size, extra)


def machine_data(machine: MachineCamera) -> dict:
    """`camera_devices` data for a machine (§5.2)."""
    vcam = machine.virtual_camera
    want = None if vcam.want is None else {"w": vcam.want[0], "h": vcam.want[1], "fps": vcam.want[2]}
    return {"machine": machine.id, "rev": machine.rev, "platform": machine.platform, "status": machine.status,
            "features": sorted(machine.features), "test_source": machine.test_source,
            "cameras": [{"key": camera.key, "name": camera.name, "transport": camera.transport,
                         "native": list(camera.native)} for camera in machine.cameras],
            "preferred": machine.preferred, "permission": machine.permission, "sharing": machine.sharing,
            "capture": {"state": machine.capture.state, "by_other": machine.capture.by_other,
                        "effects": list(machine.capture.effects), "error": machine.capture.error},
            "virtual_camera": {"install": vcam.install, "present": vcam.present, "in_use": vcam.in_use,
                               "app": vcam.app, "want": want}}


def machine_from(data) -> MachineCamera:
    preferred = data.get("preferred")
    cameras = tuple(Camera(entry["key"], entry["name"], entry.get("transport", "other"),
                           tuple(entry.get("native", ())), entry["key"] == preferred)
                    for entry in data.get("cameras", ()))
    capture = data.get("capture") or {}
    vcam = data.get("virtual_camera") or {}
    want = vcam.get("want")
    return MachineCamera(
        id=data["machine"], platform=data.get("platform", ""), rev=data["rev"], status=data.get("status", "ok"),
        features=frozenset(data.get("features", ())), cameras=cameras, preferred=preferred,
        permission=data.get("permission"), sharing=data.get("sharing"),
        capture=Capture(capture.get("state", "idle"), bool(capture.get("by_other")),
                        tuple(capture.get("effects", ())), capture.get("error")),
        virtual_camera=VirtualCamera(vcam.get("install", "unsupported"), bool(vcam.get("present")),
                                     bool(vcam.get("in_use")), vcam.get("app"),
                                     None if want is None else (want["w"], want["h"], want["fps"])),
        test_source=data.get("test_source"))
