# SPDX-License-Identifier: GPL-3.0-only
"""Which streams this machine sends, and every route's status (dev/audio/PHASE2.md §6.4).

Pure. Every machine evaluates the same rules on the same cluster state; each one acts only on the
`sends` and `loop` of its own plan, while the statuses describe the whole cluster for the pages.
"""

from dataclasses import dataclass, field
from typing import Mapping, Optional

from core.audio import model
from core.audio.model import DeviceRef, MachineAudio, Route, RouteStatus

HEADSET_FORMS = ("headphones", "headset")
DEMAND_HOLD = 5.0   # the planner keeps a consumer's demand this long after it stops (the helper adds 3 s)


@dataclass(frozen=True)
class SendSpec:
    kind: str                     # "mic" | "system"
    peer: str
    source: Optional[str]         # input device key, None = default input (followed); None for system
    sink: Mapping = field(default_factory=dict)   # {"virtual_mic": True} | {"device": DeviceRef | None}
    mute_local: bool = False
    aec: bool = False
    aec_reference: Optional[str] = None           # reference output key, None = default output


@dataclass(frozen=True)
class LoopSpec:
    source: Optional[str]         # local input key, None = default input


@dataclass(frozen=True)
class Plan:
    sends: Mapping[tuple[str, str], SendSpec] = field(default_factory=dict)
    loop: Optional[LoopSpec] = None
    mic_status: RouteStatus = RouteStatus("off")
    output_status: Mapping[str, RouteStatus] = field(default_factory=dict)
    wake_at: Optional[float] = None


def peers_of(own: str, machine: str, direct: frozenset, machines: Mapping[str, MachineAudio]) -> frozenset:
    """The direct peers of `machine`: this machine's own links, else what its list says."""
    if machine == own:
        return direct
    held = machines.get(machine)
    return held.links if held is not None else frozenset()


def demand(machine: MachineAudio, now: float, demand_hold: Mapping[str, float]) -> bool:
    """Whether an app on `machine` records its virtual mic, or did within the hold. A machine that
    cannot tell (no `mic_demand`) always wants it while a mic is active."""
    if "mic_demand" not in machine.features:
        return True
    return machine.virtual_mic.in_use or demand_hold.get(machine.id, 0.0) > now


def output_form(route: Route, machine: MachineAudio) -> str:
    """The form of `machine`'s default output, its override first."""
    key = machine.default_output
    override = model.form_of(route, machine.id, key) if key is not None else None
    if override is not None:
        return override
    device = model.device_by_key(machine.outputs, key)
    return device.form if device is not None else "unknown"


def resolve_output(target: MachineAudio, ref: Optional[DeviceRef]):
    """(device or None, missing): T's output that `ref` names, or T's default output for None."""
    if ref is None:
        device = model.device_by_key(target.outputs, target.default_output)
    else:
        device = model.resolve(ref, target.outputs)
    return device, device is None


def conflict(route: Route, target: MachineAudio, device_key: Optional[str]) -> bool:
    """§6.4 rule 2: T mutes its default endpoint to send its own sound away (Windows), so that endpoint
    cannot also play what another machine sends."""
    return ("local_mute_endpoint" in target.features
            and model.output_of(route, target.id).to is not None
            and device_key is not None and device_key == target.default_output)


def sink_problem(route: Route, own: MachineAudio, sink: Mapping) -> Optional[str]:
    """Why this machine cannot take a stream into `sink` (an `audio_opened` error), or None (rule 4)."""
    if sink.get("virtual_mic"):
        return None if own.virtual_mic.present else "no_virtual_mic"
    device, missing = resolve_output(own, sink.get("device"))
    if missing:
        return "no_device"
    if conflict(route, own, device.key):
        return "conflict"
    return None


def speakers_idle(route: Route, holder: MachineAudio, output_status: Mapping[str, RouteStatus]) -> bool:
    """Nothing plays on `holder`'s default output (the echo reference): its own sound streams to another
    machine (muted here), and no other machine's sound streams to that output. Its mic then hears no
    speakers, and an echo canceller would only risk altering it (WP16)."""
    if model.output_of(route, holder.id).to is None or output_status.get(holder.id, RouteStatus("off")).state != "streaming":
        return False
    for machine, entry in route.outputs.items():
        if machine == holder.id or entry.to != holder.id:
            continue
        if output_status.get(machine, RouteStatus("off")).state != "streaming":
            continue
        device, _missing = resolve_output(holder, entry.device)
        if device is not None and device.key == holder.default_output:
            return False
    return True


def _mic(own, machines, route, direct, online, aec_mode, now, demand_hold, sends, output_status):
    mic = route.mic
    source = mic.machine
    if source is None:
        return RouteStatus("off"), None
    holder = machines.get(source)
    if holder is None or source not in online:
        return RouteStatus("waiting_machine", None, {"machine": source}), None
    if mic.device is None:
        if holder.default_input is None:
            return RouteStatus("waiting_device", "no_default", {"machine": source}), None
        source_key = None
    else:
        device = model.resolve(mic.device, holder.inputs)
        if device is None:
            return RouteStatus("waiting_device", None, {"machine": source, "device": mic.device.name}), None
        source_key = device.key
    # Only a helper that lists `aec` cancels echo (§3.3 `ready`); an older one would ignore the field.
    # No echo to cancel from a headset, or from speakers that play nothing (speakers_idle).
    aec = (aec_mode == "auto" and "aec" in holder.features and output_form(route, holder) not in HEADSET_FORMS
           and not speakers_idle(route, holder, output_status))
    reference = holder.default_output if aec else None
    reachable = peers_of(own, source, direct, machines)
    streaming, problems = [], {}
    for target in sorted(online):
        consumer = machines.get(target)
        if target == source or consumer is None or not demand(consumer, now, demand_hold):
            continue
        if target not in reachable:
            problems[target] = "unreachable"
        elif not consumer.virtual_mic.present:
            problems[target] = "no_virtual_mic"
        else:
            streaming.append(target)
            if source == own:
                sends[("mic", target)] = SendSpec("mic", target, source_key, {"virtual_mic": True}, False, aec, reference)
    loop = None
    if demand(holder, now, demand_hold) and holder.virtual_mic.present:
        streaming.append(source)
        if source == own:
            loop = LoopSpec(source_key)
    if streaming:
        return RouteStatus("streaming", None, {"to": " ".join(sorted(streaming))}), loop
    if problems:
        state = "unreachable" if "unreachable" in problems.values() else "no_virtual_mic"
        return RouteStatus(state, None, {"to": " ".join(sorted(problems))}), loop
    return RouteStatus("ready", None, {"machine": source}), loop


def _output(own, machine, machines, route, direct, online, sends):
    entry = model.output_of(route, machine)
    target = entry.to
    if target is None:
        return RouteStatus("local")
    params = {"to": target}
    if entry.device is not None:
        params["device"] = entry.device.name
    holder, receiver = machines.get(machine), machines.get(target)
    if holder is None or receiver is None or machine not in online or target not in online:
        return RouteStatus("waiting_machine", None, params)
    if target not in peers_of(own, machine, direct, machines):
        return RouteStatus("unreachable", None, params)
    if "system_capture" not in holder.features:
        return RouteStatus("error", "no_system_capture", params)
    device, missing = resolve_output(receiver, entry.device)
    if missing:
        return RouteStatus("waiting_device", None, params)
    if conflict(route, receiver, device.key):
        return RouteStatus("conflict", None, params)
    if machine == own:
        sink_ref = None if entry.device is None else model.DeviceRef(device.key, device.name)
        sends[("system", target)] = SendSpec("system", target, None, {"device": sink_ref}, True, False, None)
    return RouteStatus("streaming", None, params)


def plan(own: str, machines: Mapping[str, MachineAudio], route: Route, direct: frozenset, online: frozenset,
         aec_mode: str, now: float, demand_hold: Mapping[str, float]) -> Plan:
    sends = {}
    output_status = {machine: _output(own, machine, machines, route, direct, online, sends)
                     for machine in sorted(machines)}
    mic_status, loop = _mic(own, machines, route, direct, online, aec_mode, now, demand_hold, sends, output_status)
    pending = [until for until in demand_hold.values() if until > now]
    return Plan(sends, loop, mic_status, output_status, min(pending) if pending else None)
