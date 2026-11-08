# SPDX-License-Identifier: GPL-3.0-only
"""The audio control plane's snapshot types and the pure rules over them (dev/audio/PHASE2.md §8.3).

Pure, with no AppKit, no Qt, no threads and no I/O: both pages read these types, and the service,
the planner and the store build them.

Every type is a frozen dataclass. Mapping fields default to an empty dict of their own, never a
shared one; `AudioSnapshot` takes keyword arguments only, since its Mapping fields come before
fields without a default.

The route is stamped entry by entry (§6.2): newer is a higher `set_at`, or the same `set_at` and the
larger `by` compared as bytes, as `settings_sync` decides the shared layout. Device lists have one
author each, the machine itself, and a higher `rev` wins (§5.3).
"""

from collections import deque
from dataclasses import dataclass, field
from typing import Mapping, Optional

from core import settings_sync

FORMS = ("microphone", "speakers", "headphones", "headset", "unknown")
OVERRIDE_FORMS = ("headset", "speakers")
STATUSES = ("ok", "starting", "failed", "no_helper")
STATES = ("off", "local", "ready", "streaming", "waiting_device", "waiting_machine", "unreachable",
          "no_virtual_mic", "conflict", "error")
MAX_FORMS = 64


@dataclass(frozen=True)
class DeviceRef:
    key: str
    name: str


@dataclass(frozen=True)
class Device:
    key: str
    name: str
    kind: str                # "input" | "output"
    transport: str           # "builtin" | "usb" | "bluetooth" | "virtual" | "hdmi" | "other"
    channels: int
    form: str                # FORMS, override already applied by the device's owner
    default: bool


@dataclass(frozen=True)
class VirtualMic:
    kind: Optional[str]      # "beamer" | "vbcable" | None
    present: bool
    in_use: bool


@dataclass(frozen=True)
class MachineAudio:
    id: str
    name: str
    platform: str
    rev: int
    status: str              # ok | starting | failed | no_helper
    inputs: tuple[Device, ...]
    outputs: tuple[Device, ...]
    default_input: Optional[str]
    default_output: Optional[str]
    virtual_mic: VirtualMic
    features: frozenset[str]
    links: frozenset[str]
    helper: str


@dataclass(frozen=True)
class Stamp:
    set_at: int
    by: str


@dataclass(frozen=True)
class MicRoute:
    machine: Optional[str]
    device: Optional[DeviceRef]
    stamp: Stamp


@dataclass(frozen=True)
class OutputRoute:
    to: Optional[str]
    device: Optional[DeviceRef]
    stamp: Stamp


@dataclass(frozen=True)
class FormEntry:
    form: Optional[str]
    stamp: Stamp


@dataclass(frozen=True)
class Route:
    mic: MicRoute
    outputs: Mapping[str, OutputRoute] = field(default_factory=dict)
    forms: Mapping[tuple[str, str], FormEntry] = field(default_factory=dict)


@dataclass(frozen=True)
class RouteStatus:
    state: str
    reason: Optional[str] = None
    params: Mapping[str, str] = field(default_factory=dict)


@dataclass(frozen=True)
class StreamView:
    stream: int
    kind: str                # "mic" | "system"
    source: str              # machine id
    target: str              # machine id
    state: str               # "opening" | "starting" | "running" | "failed" | "stopped"
    path: str                # "udp" | "link" | ""
    jb_ms: Optional[float]
    loss_pct: Optional[float]
    rtt_ms: Optional[float]


@dataclass(frozen=True)
class HowlView:
    """Feedback the howl guard of `source`'s sound stream to `target` heard (PHASE2 §3.3 `howl`):
    `target`'s speakers reach a microphone whose sound `source` plays into its own output."""
    source: str              # machine id: whose sound was turned down
    target: str              # machine id: whose speakers play it
    freq_hz: float
    active: bool             # still ducked; False: ended within the notice hold


@dataclass(frozen=True, kw_only=True)
class AudioSnapshot:
    own: str
    machines: Mapping[str, MachineAudio] = field(default_factory=dict)
    online: frozenset[str]
    direct: frozenset[str]
    route: Route
    mic_status: RouteStatus
    output_status: Mapping[str, RouteStatus] = field(default_factory=dict)
    streams: tuple[StreamView, ...]
    helper: str
    permission: Mapping[str, str] = field(default_factory=dict)
    aec_mode: str
    aec_active: bool
    howls: tuple[HowlView, ...] = ()
    vmic_exclusive: bool = False    # this machine's local choice (Windows): exclusive render into CABLE Input


ZERO = Stamp(0, "")
MIC_OFF = MicRoute(None, None, ZERO)
LOCAL = OutputRoute(None, None, ZERO)


def empty_route() -> Route:
    """Mic off and every output local: the route of a machine that has never seen one."""
    return Route(MIC_OFF, {}, {})


def empty_snapshot(own: str, helper: str = "starting") -> AudioSnapshot:
    return AudioSnapshot(own=own, machines={}, online=frozenset([own]), direct=frozenset(),
                         route=empty_route(), mic_status=RouteStatus("off"), output_status={},
                         streams=(), helper=helper, permission={}, aec_mode="auto", aec_active=False)


def newer(one: Stamp, other: Stamp) -> bool:
    """Whether `one` is newer than `other`: a higher `set_at`, or the same and the larger `by` as bytes."""
    return settings_sync._newer((one.set_at, one.by), (other.set_at, other.by))


def next_stamp(route: Route, by: str, now: float) -> Stamp:
    """The stamp for a change made here: newer than every stamp in `route` (`settings_sync.next_stamp`)."""
    return Stamp(settings_sync.next_stamp(max_stamp(route), now), by)


def max_stamp(route: Route) -> int:
    stamps = [route.mic.stamp.set_at]
    stamps += [entry.stamp.set_at for entry in route.outputs.values()]
    stamps += [entry.stamp.set_at for entry in route.forms.values()]
    return max(stamps)


def output_of(route: Route, machine: str) -> OutputRoute:
    """The output entry of `machine`, local when the route has none."""
    return route.outputs.get(machine, LOCAL)


def form_of(route: Route, machine: str, key: str) -> Optional[str]:
    """The form override of a device, None when there is none."""
    entry = route.forms.get((machine, key))
    return entry.form if entry is not None else None


def _pick(held, incoming):
    """The newer of two entries. Equal stamps are the same edit; should they ever differ, the larger
    repr wins, so every machine still picks the same one."""
    if held is None:
        return incoming
    if incoming is None or newer(held.stamp, incoming.stamp):
        return held
    if newer(incoming.stamp, held.stamp):
        return incoming
    return incoming if repr(incoming) > repr(held) else held


def trim_forms(forms: Mapping[tuple[str, str], FormEntry]) -> dict:
    """At most MAX_FORMS entries: when there are more, the oldest `form: None` ones go first, then the
    oldest of the rest. Stamps decide alone, so every machine trims the same way."""
    forms = dict(forms)
    if len(forms) <= MAX_FORMS:
        return forms
    order = sorted(forms, key=lambda name: (forms[name].form is not None, forms[name].stamp.set_at,
                                            settings_sync._id(forms[name].stamp.by) or b"", name))
    for name in order[:len(forms) - MAX_FORMS]:
        del forms[name]
    return forms


def merge_route(held: Route, incoming: Route) -> tuple[Route, bool, bool]:
    """Entry-wise merge (§6.2): (merged, changed, answer). `changed`: the merge differs from `held`,
    so it is stored and sent on. `answer`: `held` has an entry newer than the incoming one (or one
    the sender lacks), so the sender should get ours back, as `settings_sync.merge`'s `answer`."""
    mic = _pick(held.mic, incoming.mic)
    outputs = {machine: _pick(held.outputs.get(machine), incoming.outputs.get(machine))
               for machine in sorted(set(held.outputs) | set(incoming.outputs))}
    forms = trim_forms({name: _pick(held.forms.get(name), incoming.forms.get(name))
                        for name in sorted(set(held.forms) | set(incoming.forms))})
    merged = Route(mic, outputs, forms)
    # An entry of the merge that differs from the sender's is one the sender lacks.
    answer = (mic != incoming.mic
              or any(entry != incoming.outputs.get(machine, LOCAL) for machine, entry in outputs.items())
              or any(entry != incoming.forms.get(name, FormEntry(None, ZERO)) for name, entry in forms.items()))
    return merged, merged != held, answer


def accept_devices(held: Optional[MachineAudio], incoming: MachineAudio, direct_from: bool) -> bool:
    """Whether a received device list replaces the one held for its machine (§5.3): a higher `rev`, or
    the list came straight from the machine it describes and differs from ours."""
    if held is None:
        return True
    if incoming.rev > held.rev:
        return True
    return direct_from and incoming != held


def online(own: str, direct: frozenset, machines: Mapping[str, MachineAudio]) -> frozenset:
    """Machines reachable from `own` (§5.3): BFS over `own`'s direct peers and, beyond them, over the
    `links` of held lists, an edge counting only when both ends list each other."""
    seen = {own}
    todo = deque()
    for peer in direct:
        if peer not in seen:
            seen.add(peer)
            todo.append(peer)
    while todo:
        machine = todo.popleft()
        held = machines.get(machine)
        if held is None:
            continue
        for other in held.links:
            if other in seen:
                continue
            listed = machines.get(other)
            if listed is not None and machine in listed.links:
                seen.add(other)
                todo.append(other)
    return frozenset(seen)


def resolve(ref: Optional[DeviceRef], devices) -> Optional[Device]:
    """The device `ref` names among `devices`: the same key, else the only one with its name (a USB
    device moved to another port), else None (none, or an ambiguous name)."""
    if ref is None:
        return None
    devices = tuple(devices)
    for device in devices:
        if device.key == ref.key:
            return device
    named = [device for device in devices if device.name == ref.name]
    return named[0] if len(named) == 1 else None


def device_by_key(devices, key: Optional[str]) -> Optional[Device]:
    if key is None:
        return None
    for device in devices:
        if device.key == key:
            return device
    return None


# Data <-> types. The dicts are the wire's and audio.json's (§5.2, §6.2); `core.audio.wire`
# validates them first, so these assume well-formed input.

def ref_data(ref: Optional[DeviceRef]):
    return None if ref is None else {"key": ref.key, "name": ref.name}


def ref_from(data) -> Optional[DeviceRef]:
    return None if data is None else DeviceRef(data["key"], data["name"])


def stamp_data(stamp: Stamp) -> dict:
    return {"set_at": stamp.set_at, "by": stamp.by}


def route_data(route: Route) -> dict:
    return {
        "mic": {"machine": route.mic.machine, "device": ref_data(route.mic.device), **stamp_data(route.mic.stamp)},
        "outputs": {machine: {"to": entry.to, "device": ref_data(entry.device), **stamp_data(entry.stamp)}
                    for machine, entry in sorted(route.outputs.items())},
        "forms": {f"{machine} {key}": {"form": entry.form, **stamp_data(entry.stamp)}
                  for (machine, key), entry in sorted(route.forms.items())},
    }


def route_from(data) -> Route:
    mic = data.get("mic") or {}
    mic_route = (MicRoute(mic.get("machine"), ref_from(mic.get("device")), Stamp(mic["set_at"], mic["by"]))
                 if "set_at" in mic else MIC_OFF)
    outputs = {machine: OutputRoute(entry.get("to"), ref_from(entry.get("device")),
                                    Stamp(entry["set_at"], entry["by"]))
               for machine, entry in (data.get("outputs") or {}).items()}
    forms = {}
    for name, entry in (data.get("forms") or {}).items():
        machine, _space, key = name.partition(" ")
        forms[(machine, key)] = FormEntry(entry.get("form"), Stamp(entry["set_at"], entry["by"]))
    return Route(mic_route, outputs, trim_forms(forms))


def _device_data(device: Device) -> dict:
    return {"key": device.key, "name": device.name, "transport": device.transport,
            "channels": device.channels, "form": device.form}


def machine_data(machine: MachineAudio) -> dict:
    """`audio_devices` data for a machine (§5.2)."""
    return {"machine": machine.id, "rev": machine.rev, "name": machine.name, "platform": machine.platform,
            "status": machine.status, "helper": machine.helper,
            "inputs": [_device_data(device) for device in machine.inputs],
            "outputs": [_device_data(device) for device in machine.outputs],
            "default_input": machine.default_input, "default_output": machine.default_output,
            "virtual_mic": {"kind": machine.virtual_mic.kind, "present": machine.virtual_mic.present,
                            "in_use": machine.virtual_mic.in_use},
            "features": sorted(machine.features), "links": sorted(machine.links)}


def machine_from(data) -> MachineAudio:
    def devices(entries, kind, default):
        return tuple(Device(entry["key"], entry["name"], kind, entry.get("transport", "other"),
                            int(entry.get("channels", 0)), entry.get("form", "unknown"), entry["key"] == default)
                     for entry in entries)
    vmic = data.get("virtual_mic") or {}
    return MachineAudio(
        id=data["machine"], name=data.get("name", ""), platform=data.get("platform", ""), rev=data["rev"],
        status=data.get("status", "ok"),
        inputs=devices(data.get("inputs", ()), "input", data.get("default_input")),
        outputs=devices(data.get("outputs", ()), "output", data.get("default_output")),
        default_input=data.get("default_input"), default_output=data.get("default_output"),
        virtual_mic=VirtualMic(vmic.get("kind"), bool(vmic.get("present")), bool(vmic.get("in_use"))),
        features=frozenset(data.get("features", ())), links=frozenset(data.get("links", ())),
        helper=data.get("helper", ""))
