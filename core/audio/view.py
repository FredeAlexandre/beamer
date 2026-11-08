# SPDX-License-Identifier: GPL-3.0-only
"""The Audio page's model, shared by the Mac page and the Qt page (dev/audio/PHASE2.md §9.1).

Pure: no AppKit, no Qt, no threads. `page_model(snapshot, tiles, selected, own_label)` turns the
service's snapshot and the shared Crossing layout into everything a page shows, already worded in
the current language, so both pages only lay it out:

- the microphone selector (`mic_choices`, `mic_selected`), its status line and level meter key;
- the map: one `TileView` per machine on the Crossing layout's tiles, and `Arrow`s for the routes;
- the selected machine's panel: where its sound plays, its inputs and its outputs;
- the notices, each with an optional action.

Colours are palette names from tokens.py (`ink_2`, `ink_3`, `signal`, `amber`, `fault`), the same in
both apps. Level meter keys are the service's: `"<machine>/in/<device key>"`, `"<machine>/out/<key>"`.
A page acts through the `apply_*` helpers, which call the AudioService setters (§8.1).
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Mapping, Optional

from core.audio import model, planner
from core.audio.model import AudioSnapshot, DeviceRef, MachineAudio, RouteStatus
from core.i18n import N_, joined, tr, trn

HEADSET_FORMS = ("headphones", "headset")
FLOOR_DB = -60.0
# A device a machine lists only as the virtual mic's other end is never a microphone to choose.
VIRTUAL_INPUTS = ("Beamer Microphone", "CABLE Output")

PLATFORMS = {"macos": N_("macOS"), "windows": N_("Windows"), "linux": N_("Linux")}
OFF = "off"
HERE = "here"
STREAMS_RUNNING = ("opening", "starting", "running")

# Inks: what a line or a mark is drawn in, by what it says.
QUIET, PLAIN, LIVE, WAIT, FAULT = "ink_3", "ink_2", "signal", "amber", "fault"
TONES = {"off": QUIET, "local": PLAIN, "ready": PLAIN, "streaming": LIVE, "waiting_device": WAIT,
         "waiting_machine": WAIT, "unreachable": WAIT, "no_virtual_mic": WAIT, "conflict": FAULT, "error": FAULT}

# Error codes a status may carry as its `reason` (§3.3, §5.2), as words.
REASONS = {
    "no_device": N_("the device is gone"),
    "blocked_device": N_("Beamer does not use that device"),
    "no_virtual_mic": N_("no virtual microphone there"),
    "device_busy": N_("another app holds the device"),
    "permission": N_("the system does not allow it"),
    "unsupported": N_("that machine cannot do it"),
    "bind_failed": N_("the network port is taken"),
    "helper": N_("the audio helper stopped"),
    "conflict": N_("its output is muted there"),
    "no_system_capture": N_("that machine cannot capture its sound"),
}


@dataclass(frozen=True)
class Choice:
    """One line of a selector. `key` is stable across snapshots, so a page keeps the selection by it.
    A header names the machine the choices under it belong to and cannot be picked."""
    key: str
    label: str                      # in the open menu, under its header
    title: str                      # on the closed selector, where no header says which machine
    enabled: bool = True
    header: bool = False
    machine: Optional[str] = None   # mic: the machine; output: the machine the sound goes to (None = here)
    device: Optional[DeviceRef] = None


@dataclass(frozen=True)
class TileView:
    machine: str
    frame: tuple                    # (x, y, w, h) in the Crossing layout's units
    title: str
    subtitle: str
    platform: str
    own: bool
    online: bool
    selected: bool
    has_active_mic: bool
    mic_line: str                   # the active mic's name on its machine's tile, else ""
    recording: bool                 # an app on this machine records its virtual mic
    output_line: str
    output_tone: str
    meter: Optional[str]            # the level key of the tile's thin bar
    description: str                # one sentence for screen readers
    input_line: str = ""            # the machine's own default microphone, shown when it is not the active one
    listen_line: str = ""           # "Hears This Mac": an app here records the shared mic from another machine
    recording_line: str = ""        # while `recording`: one sentence saying so, for a badge's tooltip or a reader


@dataclass(frozen=True)
class Arrow:
    source: str
    target: str
    kind: str                       # "mic" | "sound"
    state: str                      # "active" | "waiting"
    description: str


@dataclass(frozen=True)
class DeviceRow:
    machine: str
    key: str
    name: str
    detail: str                     # "Default · USB", ...
    kind: str                       # "input" | "output"
    default: bool
    form: str
    meter: str
    active: bool                    # the active mic, or an output that plays another machine's sound
    active_label: str
    test_enabled: bool              # inputs: Test can meter it
    headset: bool                   # outputs: counts as a headset (no echo)
    headset_enabled: bool


@dataclass(frozen=True)
class MachinePanel:
    machine: str
    heading: str
    output_label: str               # "Sound from lebig plays on"
    output_choices: tuple
    output_selected: str
    output_enabled: bool
    mute_note: str                  # "" while the sound plays on the machine itself
    output_line: str
    output_tone: str
    inputs: tuple
    outputs: tuple
    inputs_empty: str
    outputs_empty: str
    # This PC's own "Low-latency virtual microphone" switch (§8.4 `local.vmic_exclusive`): only on a
    # Windows machine's own panel with VB-CABLE present; each machine sets its own, never a peer's.
    vmic_exclusive_shown: bool = False
    vmic_exclusive_on: bool = False
    vmic_exclusive_label: str = ""
    vmic_exclusive_note: str = ""


@dataclass(frozen=True)
class Notice:
    code: str
    text: str
    tone: str
    action: Optional[str] = None    # see apply_notice
    action_label: str = ""
    machine: Optional[str] = None
    target: Optional[str] = None


@dataclass(frozen=True)
class AudioPage:
    mic_label: str
    mic_choices: tuple
    mic_selected: str
    mic_line: str
    mic_tone: str
    mic_meter: Optional[str]
    mic_hint: str
    tiles: tuple                    # reading order: rows top to bottom, each left to right
    arrows: tuple
    selected: str
    panel: MachinePanel
    notices: tuple
    map_description: str
    meters: frozenset = field(default_factory=frozenset)


# Words for one machine, devices and keys.

def meter_key(machine: str, kind: str, key: str) -> str:
    return f"{machine}/{'in' if kind == 'input' else 'out'}/{key}"


def level_fraction(db: Optional[float]) -> float:
    """dBFS -60..0 as 0..1 along a meter."""
    if db is None:
        return 0.0
    return min(1.0, max(0.0, (float(db) - FLOOR_DB) / -FLOOR_DB))


def meter_reading(levels: Mapping, key: Optional[str]) -> Optional[tuple]:
    """(rms, peak) as 0..1 for `key` in the service's levels map, or None when it has none."""
    if key is None or key not in levels:
        return None
    rms, peak = levels[key]
    return level_fraction(rms), level_fraction(peak)


def layout_tiles(layout: Mapping) -> dict:
    """settings["layout"] (tiles as {x, y, w, h, ...}) as page_model's `tiles`; (x, y, w, h) tuples
    pass through."""
    tiles = {}
    for machine, tile in (layout or {}).items():
        try:
            values = (tile["x"], tile["y"], tile["w"], tile["h"]) if isinstance(tile, Mapping) else tuple(tile)[:4]
            tiles[machine] = tuple(float(value) for value in values)
        except (KeyError, TypeError, ValueError):
            continue
        if len(tiles[machine]) != 4:
            del tiles[machine]
    return tiles


def _choice_key(*parts) -> str:
    return "\t".join("" if part is None else str(part) for part in parts)


def _title(machine: str, device: str) -> str:
    """A choice on the closed selector, where no header says whose device it is."""
    # Translators: a device chosen in a selector, after the machine it belongs to: "lebig · Wave:3".
    return tr("{machine} · {device}").format(machine=machine, device=device)


def _headset(form: Optional[str]) -> bool:
    return form in HEADSET_FORMS


class _Names:
    def __init__(self, snap: AudioSnapshot, own_label: str):
        self.snap = snap
        self.own_label = own_label

    def __call__(self, machine: Optional[str]) -> str:
        if machine == self.snap.own:
            return self.own_label
        held = self.snap.machines.get(machine)
        if held is not None and held.name:
            return held.name
        return tr("Machine · {id}").format(id=(machine or "")[-6:])


def _inputs(machine: Optional[MachineAudio]):
    if machine is None:
        return ()
    return tuple(device for device in machine.inputs
                 if not any(device.name.startswith(name) for name in VIRTUAL_INPUTS))


def _device_named(devices, ref: Optional[DeviceRef], default_key: Optional[str]):
    """The device `ref` resolves to, or the default one for None; None when it is not there."""
    if ref is None:
        return model.device_by_key(devices, default_key)
    return model.resolve(ref, devices)


def _mic_device_name(snap: AudioSnapshot) -> str:
    mic = snap.route.mic
    held = snap.machines.get(mic.machine)
    device = _device_named(_inputs(held), mic.device, held.default_input if held else None)
    if device is not None:
        return device.name
    if mic.device is not None:
        return mic.device.name
    return tr("its default microphone")


def _usable(snap: AudioSnapshot, machine: str) -> bool:
    held = snap.machines.get(machine)
    return machine in snap.online and held is not None and held.status in ("ok", "starting")


def _ids(text) -> list:
    return [part for part in str(text or "").split() if part]


# The microphone.

def _mic_choices(snap: AudioSnapshot, names: _Names, order):
    choices = [Choice(OFF, tr("Off"), tr("Off"))]
    mic = snap.route.mic
    selected = OFF
    for machine in order:
        held = snap.machines.get(machine)
        if held is None:
            continue
        inputs = _inputs(held)
        usable = _usable(snap, machine)
        name = names(machine)
        group = [Choice(_choice_key("mic-head", machine), name, name, enabled=False, header=True, machine=machine)]
        default = model.device_by_key(inputs, held.default_input)
        label = (tr("Default ({name})").format(name=default.name) if default is not None
                 else tr("Default microphone"))
        key = _choice_key("mic", machine, "")
        group.append(Choice(key, label, _title(name, label),
                            enabled=usable, machine=machine, device=None))
        if mic.machine == machine and mic.device is None:
            selected = key
        chosen = model.resolve(mic.device, inputs) if mic.machine == machine else None
        for device in inputs:
            key = _choice_key("mic", machine, device.key)
            ref = DeviceRef(device.key, device.name)
            group.append(Choice(key, device.name, _title(name, device.name),
                                enabled=usable, machine=machine, device=ref))
            if chosen is not None and chosen.key == device.key and mic.device is not None:
                selected = key
        if mic.machine == machine and mic.device is not None and chosen is None:
            # The chosen mic is unplugged: still the selection, so nothing has to be set again.
            key = _choice_key("mic", machine, mic.device.key)
            label = tr("{device} (not connected)").format(device=mic.device.name)
            group.append(Choice(key, label, _title(name, label),
                                enabled=False, machine=machine, device=mic.device))
            selected = key
        choices.extend(group)
    if mic.machine is not None and selected == OFF:
        name = names(mic.machine)
        label = mic.device.name if mic.device is not None else tr("Default microphone")
        key = _choice_key("mic", mic.machine, mic.device.key if mic.device else "")
        choices.append(Choice(_choice_key("mic-head", mic.machine), name, name, enabled=False, header=True,
                              machine=mic.machine))
        choices.append(Choice(key, label, _title(name, label),
                              enabled=False, machine=mic.machine, device=mic.device))
        selected = key
    return tuple(choices), selected


def _mic_targets(snap: AudioSnapshot) -> list:
    """The machines hearing the active mic: the status's `to` (planner.py), else the running mic streams,
    and the mic's own machine while an app there records it (the local loop)."""
    source = snap.route.mic.machine
    params = snap.mic_status.params
    targets = _ids(params.get("to") or params.get("targets"))
    if not targets:
        targets = [stream.target for stream in snap.streams
                   if stream.kind == "mic" and stream.source == source and stream.state in ("running", "starting")]
        held = snap.machines.get(source)
        if held is not None and held.virtual_mic.in_use and source not in targets:
            targets.append(source)
    return list(dict.fromkeys(targets))


def _mic_line(snap: AudioSnapshot, names: _Names) -> str:
    status = snap.mic_status
    source = snap.route.mic.machine
    if source is None or status.state == "off":
        return tr("No microphone is shared: apps that record it hear silence.")
    device, machine = _mic_device_name(snap), names(source)
    targets = joined(names(target) for target in _mic_targets(snap))
    state = status.state
    if state == "streaming" and targets:
        return tr("{device} on {machine} is live for {targets}.").format(device=device, machine=machine, targets=targets)
    if state in ("ready", "streaming"):
        return tr("{device} on {machine} is ready. It turns on when an app starts recording.").format(
            device=device, machine=machine)
    if state == "waiting_device":
        return tr("{device} is not connected to {machine}. Beamer uses it again as soon as it is back.").format(
            device=device, machine=machine)
    if state == "waiting_machine":
        return tr("{machine} is offline. Its microphone comes back with it.").format(machine=machine)
    if state == "unreachable":
        return (tr("{targets} cannot hear {machine}: they are not linked to it directly.").format(
            targets=targets, machine=machine) if targets else
            tr("A machine recording it is not linked to {machine} directly.").format(machine=machine))
    if state == "no_virtual_mic":
        return (tr("{targets} cannot hear {machine}: no virtual microphone there.").format(
            targets=targets, machine=machine) if targets else
            tr("A machine recording it has no virtual microphone."))
    reason = REASONS.get(status.reason or "")
    return (tr("{device} on {machine} could not start: {reason}.").format(device=device, machine=machine, reason=tr(reason))
            if reason else tr("{device} on {machine} could not start.").format(device=device, machine=machine))


def _mic_meter(snap: AudioSnapshot) -> Optional[str]:
    source = snap.route.mic.machine
    held = snap.machines.get(source)
    if held is None:
        return None
    device = _device_named(_inputs(held), snap.route.mic.device, held.default_input)
    return meter_key(source, "input", device.key) if device is not None else None


# Where each machine's sound plays.

def _output_status(snap: AudioSnapshot, machine: str) -> RouteStatus:
    entry = model.output_of(snap.route, machine)
    if entry.to is None:
        return RouteStatus("local")
    return snap.output_status.get(machine, RouteStatus("waiting_machine"))


def _sink_name(snap: AudioSnapshot, entry) -> str:
    """The device a machine's sound goes to on its target, by name."""
    held = snap.machines.get(entry.to)
    device = _device_named(held.outputs if held else (), entry.device, held.default_output if held else None)
    if device is not None:
        return device.name
    if entry.device is not None:
        return entry.device.name
    return tr("its default output")


def _tile_output(snap: AudioSnapshot, names: _Names, machine: str):
    entry = model.output_of(snap.route, machine)
    if entry.to is None:
        return tr("Sound plays here"), PLAIN
    state = _output_status(snap, machine).state
    target = names(entry.to)
    if state == "streaming":
        return tr("Sound → {target}").format(target=target), LIVE
    if state == "conflict":
        return tr("Sound → {target}, blocked").format(target=target), FAULT
    return tr("Sound → {target}, waiting").format(target=target), WAIT


def _panel_output_line(snap: AudioSnapshot, names: _Names, machine: str):
    entry = model.output_of(snap.route, machine)
    status = _output_status(snap, machine)
    name = names(machine)
    if entry.to is None:
        held = snap.machines.get(machine)
        device = model.device_by_key(held.outputs, held.default_output) if held else None
        return (tr("Plays on {device}.").format(device=device.name) if device is not None else ""), PLAIN
    target, device = names(entry.to), _sink_name(snap, entry)
    state = status.state
    if state == "streaming":
        line = tr("Playing on {device} on {target}.")
    elif state == "waiting_device":
        line = tr("{device} is not connected to {target}. {machine} plays its sound itself until it is back.")
    elif state == "waiting_machine":
        line = tr("{target} is offline. {machine} plays its sound itself until it is back.")
    elif state == "unreachable":
        line = tr("{machine} and {target} are not linked directly, so {machine} plays its sound itself.")
    elif state == "conflict":
        line = tr("{target} sends its own sound elsewhere, which mutes {device} there. Choose another output on {target}.")
    else:
        reason = REASONS.get(status.reason or "")
        line = (tr("{machine}'s sound could not be sent to {target} ({reason}). It plays on {machine}.")
                if reason else tr("{machine}'s sound could not be sent to {target}. It plays on {machine}."))
        return line.format(machine=name, target=target, reason=tr(reason) if reason else ""), TONES["error"]
    return line.format(machine=name, target=target, device=device), TONES.get(state, PLAIN)


def _peers_of(snap: AudioSnapshot, machine: str) -> list:
    """The machines `machine` is linked to directly, which are the only ones its sound can go to."""
    if machine == snap.own:
        peers = set(snap.direct)
    else:
        held = snap.machines.get(machine)
        peers = set(held.links) if held is not None else set()
        if machine in snap.direct:
            peers.add(snap.own)
    peers.discard(machine)
    return [peer for peer in peers if peer in snap.machines]


def _output_choices(snap: AudioSnapshot, names: _Names, machine: str, order):
    held = snap.machines.get(machine)
    entry = model.output_of(snap.route, machine)
    name = names(machine)
    local = model.device_by_key(held.outputs, held.default_output) if held else None
    here = (tr("Here, on {device}").format(device=local.name) if local is not None else tr("Here"))
    choices = [Choice(HERE, here, name if local is None else _title(name, local.name))]
    selected = HERE
    can_send = held is not None and "system_capture" in held.features and machine in snap.online
    peers = _peers_of(snap, machine)
    for target in [peer for peer in order if peer in peers]:
        other = snap.machines[target]
        usable = can_send and _usable(snap, target)
        target_name = names(target)
        choices.append(Choice(_choice_key("out-head", target), target_name, target_name, enabled=False, header=True,
                              machine=target))
        default = model.device_by_key(other.outputs, other.default_output)
        label = (tr("Default ({name})").format(name=default.name) if default is not None
                 else tr("Default output"))
        key = _choice_key("out", target, "")
        choices.append(Choice(key, label, _title(target_name, label),
                              enabled=usable, machine=target, device=None))
        if entry.to == target and entry.device is None:
            selected = key
        chosen = model.resolve(entry.device, other.outputs) if entry.to == target else None
        for device in other.outputs:
            key = _choice_key("out", target, device.key)
            choices.append(Choice(key, device.name, _title(target_name, device.name),
                                  enabled=usable, machine=target, device=DeviceRef(device.key, device.name)))
            if chosen is not None and chosen.key == device.key and entry.device is not None:
                selected = key
        if entry.to == target and entry.device is not None and chosen is None:
            key = _choice_key("out", target, entry.device.key)
            label = tr("{device} (not connected)").format(device=entry.device.name)
            choices.append(Choice(key, label, _title(target_name, label),
                                  enabled=False, machine=target, device=entry.device))
            selected = key
    if entry.to is not None and selected == HERE:
        target_name = names(entry.to)
        label = (tr("{device} (not connected)").format(device=entry.device.name) if entry.device is not None
                 else tr("Default output"))
        key = _choice_key("out", entry.to, entry.device.key if entry.device else "")
        choices.append(Choice(_choice_key("out-head", entry.to), target_name, target_name, enabled=False, header=True,
                              machine=entry.to))
        choices.append(Choice(key, label, _title(target_name, label),
                              enabled=False, machine=entry.to, device=entry.device))
        selected = key
    return tuple(choices), selected, can_send


def _transport(transport: str) -> str:
    return tr({"builtin": N_("Built-in"), "usb": N_("USB"), "bluetooth": N_("Bluetooth"), "virtual": N_("Virtual"),
               "hdmi": N_("HDMI")}.get(transport, N_("Other")))


def _rows(snap: AudioSnapshot, names: _Names, machine: str):
    held = snap.machines.get(machine)
    if held is None:
        return (), ()
    usable = _usable(snap, machine) and held.status == "ok" and (machine == snap.own or machine in snap.direct)
    mic = snap.route.mic
    active_mic = None
    if mic.machine == machine:
        device = _device_named(_inputs(held), mic.device, held.default_input)
        active_mic = device.key if device is not None else None
    receiving = {}
    for source, entry in snap.route.outputs.items():
        if entry.to == machine and _output_status(snap, source).state == "streaming":
            device = _device_named(held.outputs, entry.device, held.default_output)
            if device is not None:
                receiving.setdefault(device.key, []).append(names(source))
    inputs = []
    for device in _inputs(held):
        detail = (tr("Default · {transport}") if device.default else tr("{transport}")).format(
            transport=_transport(device.transport))
        active = device.key == active_mic
        inputs.append(DeviceRow(machine, device.key, device.name, detail, "input", device.default, device.form,
                                meter_key(machine, "input", device.key), active,
                                tr("Active microphone") if active else "", usable, False, False))
    outputs = []
    for device in held.outputs:
        detail = (tr("Default · {transport}") if device.default else tr("{transport}")).format(
            transport=_transport(device.transport))
        sources = receiving.get(device.key, [])
        label = tr("Plays {machines}").format(machines=joined(sources)) if sources else ""
        outputs.append(DeviceRow(machine, device.key, device.name, detail, "output", device.default, device.form,
                                 meter_key(machine, "output", device.key), bool(sources), label, False,
                                 _headset(device.form), usable))
    return tuple(inputs), tuple(outputs)


def _vmic_exclusive(snap: AudioSnapshot, machine: str) -> dict:
    """The low-latency switch of this machine's panel: a Windows machine renders the shared mic into
    CABLE Input shared (~70 ms) unless this local choice asks its helper for exclusive (~35 ms, WP12).
    The helper names `vmic_exclusive` in its features only while started with it, so a helper that
    was asked and does not name it cannot do it."""
    held = snap.machines.get(machine)
    if machine != snap.own or held is None or held.platform != "windows" or not held.virtual_mic.present:
        return {}
    note = tr("About 35 ms instead of 70 ms, but no other app can play into CABLE Input meanwhile.")
    if snap.vmic_exclusive and held.status == "ok" and "vmic_exclusive" not in held.features:
        note += " " + tr("This version of the audio helper cannot do it.")
    return dict(vmic_exclusive_shown=True, vmic_exclusive_on=snap.vmic_exclusive,
                vmic_exclusive_label=tr("Low-latency virtual microphone"), vmic_exclusive_note=note)


def _panel(snap: AudioSnapshot, names: _Names, machine: str, order) -> MachinePanel:
    name = names(machine)
    choices, selected, can_send = _output_choices(snap, names, machine, order)
    entry = model.output_of(snap.route, machine)
    line, tone = _panel_output_line(snap, names, machine)
    inputs, outputs = _rows(snap, names, machine)
    held = snap.machines.get(machine)
    if held is None:
        empty = tr("{machine} has not listed its devices yet.").format(machine=name)
    elif machine not in snap.online:
        empty = tr("{machine} is offline.").format(machine=name)
    else:
        empty = ""
    if not can_send and held is not None and machine in snap.online and "system_capture" not in held.features:
        line = line or tr("{machine} cannot send its sound to another machine.").format(machine=name)
    return MachinePanel(
        machine=machine, heading=name,
        output_label=tr("Sound from {machine} plays on").format(machine=name),
        output_choices=choices, output_selected=selected,
        output_enabled=selected != HERE or (can_send and any(choice.enabled and choice.key != HERE for choice in choices)),
        mute_note=(tr("Muted on {machine} while it plays elsewhere.").format(machine=name) if entry.to is not None else ""),
        output_line=line, output_tone=tone, inputs=inputs, outputs=outputs,
        inputs_empty=empty or (tr("No microphones.") if not inputs else ""),
        outputs_empty=empty or (tr("No outputs.") if not outputs else ""),
        **_vmic_exclusive(snap, machine))


# The map.

def _frames(order_ids, tiles: Mapping) -> dict:
    """Each machine's tile (x, y, w, h); machines the layout lacks go in a row under it."""
    frames = {}
    for machine in order_ids:
        tile = tiles.get(machine)
        if isinstance(tile, Mapping):
            tile = layout_tiles({machine: tile}).get(machine)
        if tile is not None:
            frames[machine] = tuple(float(value) for value in tile[:4])
    missing = [machine for machine in order_ids if machine not in frames]
    if missing:
        if frames:
            left = min(frame[0] for frame in frames.values())
            top = max(frame[1] + frame[3] for frame in frames.values()) + 24.0
            width = sorted(frame[2] for frame in frames.values())[len(frames) // 2]
            height = sorted(frame[3] for frame in frames.values())[len(frames) // 2]
        else:
            left, top, width, height = 0.0, 0.0, 160.0, 100.0
        for index, machine in enumerate(missing):
            frames[machine] = (left + index * (width + 24.0), top, width, height)
    return frames


def reading_order(frames: Mapping) -> list:
    """Tiles in rows top to bottom, each row left to right, as the Crossing map reads them."""
    placed = sorted((frame[1] + frame[3] / 2, frame[0] + frame[2] / 2, frame[3], key) for key, frame in frames.items())
    rows = []
    for item in placed:
        if not rows or item[0] - rows[-1][0][0] > min(item[2], rows[-1][0][2]) / 2:
            rows.append([item])
        else:
            rows[-1].append(item)
    return [item[3] for row in rows for item in sorted(row, key=lambda entry: entry[1])]


def _tile(snap: AudioSnapshot, names: _Names, machine: str, frame, selected: str) -> TileView:
    held = snap.machines.get(machine)
    own = machine == snap.own
    online = machine in snap.online
    platform = held.platform if held is not None else ""
    platform_name = tr(PLATFORMS[platform]) if platform in PLATFORMS else tr("Computer")
    if own:
        machine_name = held.name if held is not None and held.name else ""
        subtitle = (tr("{name} · {platform}").format(name=machine_name, platform=platform_name) if machine_name
                    else platform_name)
    else:
        subtitle = tr("{platform} · {status}").format(platform=platform_name,
                                                       status=tr("Linked") if online else tr("Offline"))
    if held is None:
        subtitle = tr("Audio starting…") if own and snap.helper in ("starting", "ok") else tr("No audio")
    elif held.status == "starting":
        subtitle = tr("Audio starting…")
    elif held.status in ("failed", "no_helper"):
        subtitle = tr("Audio unavailable")
    has_mic = snap.route.mic.machine == machine
    mic_line = _mic_device_name(snap) if has_mic else ""
    default_input = model.device_by_key(_inputs(held), held.default_input) if held is not None else None
    input_line = default_input.name if default_input is not None and not has_mic else ""
    source = snap.route.mic.machine
    listening = (not has_mic and source is not None and snap.mic_status.state == "streaming"
                 and machine in _mic_targets(snap))
    listen_line = tr("Hears {machine}").format(machine=names(source)) if listening else ""
    recording = held is not None and held.virtual_mic.in_use
    recording_line = tr("An app here is recording Beamer's microphone.") if recording else ""
    output_line, output_tone = _tile_output(snap, names, machine)
    if has_mic:
        meter = _mic_meter(snap)
    elif held is not None and held.default_output:
        meter = meter_key(machine, "output", held.default_output)
    else:
        meter = None
    name = names(machine)
    parts = [tr("{machine}, {state}.").format(machine=name, state=tr("online") if online else tr("offline"))]
    if has_mic:
        parts.append(tr("Its {device} is the shared microphone.").format(device=mic_line))
    parts.append(output_line + ".")
    if recording_line:
        parts.append(recording_line)
    return TileView(machine=machine, frame=frame, title=name, subtitle=subtitle, platform=platform, own=own,
                    online=online, selected=machine == selected, has_active_mic=has_mic, mic_line=mic_line,
                    recording=recording, output_line=output_line, output_tone=output_tone if online else QUIET,
                    meter=meter, description=" ".join(parts), input_line=input_line,
                    listen_line=listen_line, recording_line=recording_line)


def _arrows(snap: AudioSnapshot, names: _Names, shown) -> tuple:
    arrows = []
    source = snap.route.mic.machine
    if source in shown:
        state = snap.mic_status.state
        for target in _mic_targets(snap):
            if target == source or target not in shown:
                continue
            live = state == "streaming"
            arrows.append(Arrow(source, target, "mic", "active" if live else "waiting",
                                tr("{target} hears the microphone of {source}.").format(target=names(target), source=names(source))))
    for machine in shown:
        entry = model.output_of(snap.route, machine)
        if entry.to is None or entry.to not in shown or entry.to == machine:
            continue
        live = _output_status(snap, machine).state == "streaming"
        arrows.append(Arrow(machine, entry.to, "sound", "active" if live else "waiting",
                            tr("{source}'s sound plays on {target}.").format(source=names(machine), target=names(entry.to))))
    return tuple(arrows)


# Notices.

def _output_form(snap: AudioSnapshot, machine: str) -> Optional[str]:
    held = snap.machines.get(machine)
    device = model.device_by_key(held.outputs, held.default_output) if held else None
    return device.form if device is not None else None


def _speaker_devices(snap: AudioSnapshot, machine: str) -> list:
    """The outputs of `machine` that play aloud: its default output while its own sound plays there,
    and every device other machines' sound plays on."""
    held = snap.machines.get(machine)
    if held is None:
        return []
    devices = []
    if model.output_of(snap.route, machine).to is None:
        devices.append(model.device_by_key(held.outputs, held.default_output))
    for source, entry in snap.route.outputs.items():
        if entry.to == machine and _output_status(snap, source).state == "streaming":
            devices.append(_device_named(held.outputs, entry.device, held.default_output))
    return [device for device in devices if device is not None and not _headset(device.form)]


def _wave_link(machine: Optional[MachineAudio]) -> bool:
    """Whether `machine` lists an Elgato or Wave Link device: Wave Link's monitor mix may then play a
    microphone into its sound."""
    devices = (machine.inputs + machine.outputs) if machine is not None else ()
    return any("elgato" in device.name.lower() or "wave link" in device.name.lower() for device in devices)


def _howl_notices(snap: AudioSnapshot, names: _Names) -> list:
    """Feedback the howl guard caught (§3.3 `howl`): one notice per sound stream, while ducked and
    for a while after. Beamer cannot see whether the target's speakers are muted or turned down, and
    the guard can mistake a sound for feedback, so the notice says what Beamer did and gives the
    likely cause as a condition, never as a fact; it does not mention speakers when the sound plays
    on headphones."""
    notices, seen = [], set()
    for howl in snap.howls:
        if (howl.source, howl.target) in seen:
            continue
        seen.add((howl.source, howl.target))
        wave_link = _wave_link(snap.machines.get(howl.source))
        held = snap.machines.get(howl.target)
        sink = (_device_named(held.outputs, model.output_of(snap.route, howl.source).device, held.default_output)
                if held is not None else None)
        if sink is not None and _headset(sink.form):
            text = (tr("Beamer heard what sounds like feedback in {source}'s sound playing on {target}, and turned it "
                       "down. If Wave Link's monitor plays a microphone into the system output on {source}, turn it "
                       "off.") if wave_link else
                    tr("Beamer heard what sounds like feedback in {source}'s sound playing on {target}, and turned it "
                       "down. If {source} plays a microphone through its own sound, stop it."))
        elif wave_link:
            text = tr("Beamer heard what sounds like feedback in {source}'s sound playing on {target}, and turned it "
                      "down. If a microphone on {source} can hear {target}'s speakers, use headphones on {target}, or "
                      "turn off Wave Link's monitor to the system output on {source}.")
        else:
            text = tr("Beamer heard what sounds like feedback in {source}'s sound playing on {target}, and turned it "
                      "down. If a microphone on {source} can hear {target}'s speakers, use headphones on {target}, or "
                      "stop {source} from playing that microphone through its own sound.")
        notices.append(Notice("howl", text.format(source=names(howl.source), target=names(howl.target)), WAIT,
                              machine=howl.source, target=howl.target))
    return notices


def _notices(snap: AudioSnapshot, names: _Names, platform: str, without_audio, driver) -> tuple:
    notices = []
    own = snap.machines.get(snap.own)
    if snap.helper == "failed":
        notices.append(Notice("helper_failed", tr("Beamer's audio helper stopped and did not start again."), FAULT,
                              "retry_helper", tr("Retry")))
    # The Mac driver: installing or updating it restarts coreaudiod, so it is offered only while no
    # stream runs, and the notice says first that every app goes quiet for a moment.
    busy = any(stream.state in STREAMS_RUNNING for stream in snap.streams)
    restart = tr("Every app on this Mac goes quiet for about a second while it installs.")
    later = tr("Beamer is sending or playing sound now; it can be installed once that stops.")
    missing = (own is not None and not own.virtual_mic.present and driver not in ("current", "newer")) or driver == "missing"
    if platform == "macos" and missing:
        offered = driver != "no_bundle" and not busy
        text = tr("Beamer Microphone is not installed, so apps on this Mac cannot hear another machine's microphone.")
        if driver == "no_bundle":
            text += " " + tr("This build of Beamer does not carry it.")
        else:
            text += " " + (later if busy else restart)
        notices.append(Notice("driver_missing", text, WAIT, "install_driver" if offered else None,
                              tr("Install Beamer Microphone") if offered else ""))
    elif platform == "macos" and driver == "outdated":
        offered = not busy
        notices.append(Notice("driver_outdated", tr("This Beamer carries a newer Beamer Microphone.") + " "
                              + (later if busy else restart), PLAIN, "install_driver" if offered else None,
                              tr("Update Beamer Microphone") if offered else ""))
    if own is not None and not own.virtual_mic.present:
        if platform == "windows":
            notices.append(Notice("vbcable_missing", tr("To use another machine's mic here, install VB-CABLE "
                                                        "(vb-audio.com), then choose 'CABLE Output' as the microphone "
                                                        "in your apps."), WAIT))
    mic_state = snap.permission.get("mic")
    if mic_state in ("denied", "suspect"):
        text = (tr("Beamer may not use this Mac's microphones. Allow it in System Settings, Privacy & Security, "
                   "Microphone.") if platform == "macos" else
                tr("Beamer may not use this PC's microphones. Allow it in Settings, Privacy & security, Microphone."))
        if mic_state == "suspect":
            text = tr("A microphone here sends only silence. Check that Beamer may use the microphone.")
        notices.append(Notice("permission_mic", text, FAULT, "open_mic_settings", tr("Open privacy settings")))
    if snap.permission.get("system_audio") in ("denied", "suspect"):
        notices.append(Notice("permission_system",
                              tr("Beamer may not record this Mac's sound, so it cannot play on another machine. "
                                 "Allow it in System Settings, Privacy & Security, Screen & System Audio Recording."),
                              FAULT, "open_system_audio_settings", tr("Open privacy settings")))
    for machine, status in sorted(snap.output_status.items()):
        entry = model.output_of(snap.route, machine)
        if status.state == "conflict" and entry.to is not None:
            notices.append(Notice("conflict", tr("{machine}'s sound cannot play on {device} on {target}: {target} sends "
                                                 "its own sound elsewhere, which mutes that output.").format(
                machine=names(machine), device=_sink_name(snap, entry), target=names(entry.to)), FAULT,
                machine=machine, target=entry.to))
    notices += _howl_notices(snap, names)
    source = snap.route.mic.machine
    targets = [target for target in _mic_targets(snap) if target != source] if snap.mic_status.state == "streaming" else []
    # Beamer cancels echo only where the helper can (`aec` in its features), it is not turned off
    # (this machine's own setting; another machine's is not known here) and the default output plays
    # something: the planner's rule (§6.4).
    held_source = snap.machines.get(source) if source is not None else None
    cancels = (held_source is not None and "aec" in held_source.features
               and (source != snap.own or snap.aec_mode == "auto")
               and not planner.speakers_idle(snap.route, held_source, snap.output_status))
    if source is not None and targets and _speaker_devices(snap, source):
        if cancels:
            notices.append(Notice("echo_aec", tr("Beamer cancels the echo of {machine}'s speakers. A headset on "
                                                 "{machine} sounds better.").format(machine=names(source)), PLAIN,
                                  machine=source))
        else:
            notices.append(Notice("echo_no_aec", tr("{machine}'s microphone can hear its speakers, and Beamer does not "
                                                    "cancel that echo there. Use a headset on {machine}.").format(
                machine=names(source)), WAIT, machine=source))
    mic_name = _mic_device_name(snap) if source is not None else ""
    for target in targets:
        if (model.output_of(snap.route, target).to is None and not _headset(_output_form(snap, target))
                and _output_form(snap, target) is not None):
            can_send = "system_capture" in snap.machines[target].features if target in snap.machines else False
            if not cancels:
                notices.append(Notice(
                    "echo_cross", tr("If {target}'s speakers can be heard by {mic}, people in a call on {target} hear "
                                     "themselves. Use a headset on {target}.").format(target=names(target), mic=mic_name),
                    WAIT, machine=target, target=source))
                continue
            notices.append(Notice(
                "echo_cross", tr("If {target}'s speakers can be heard by {mic}, people in a call on {target} hear "
                                 "themselves. Use a headset, or send {target}'s sound to {source}, where Beamer "
                                 "cancels it.").format(target=names(target), mic=mic_name, source=names(source)),
                WAIT, "send_sound" if can_send and source in _peers_of(snap, target) else None,
                tr("Send {target}'s sound to {source}").format(target=names(target), source=names(source)),
                machine=target, target=source))
    if (source is not None and own is not None and own.virtual_mic.present and not own.virtual_mic.in_use
            and snap.mic_status.state not in ("off", "waiting_machine")):
        notices.append(Notice("pick_virtual_mic",
                              tr("In your call app on this Mac, choose Beamer Microphone as the microphone.")
                              if platform == "macos" else
                              tr("In your call app on this PC, choose CABLE Output as the microphone."), PLAIN))
    without_audio = [name for name in without_audio if name]
    if without_audio:
        notices.append(Notice("peer_no_audio", trn(
            "{names} runs a Beamer without audio. Update Beamer there to use its microphones and speakers.",
            "{names} run a Beamer without audio. Update Beamer there to use their microphones and speakers.",
            len(without_audio)).format(names=joined(without_audio)), PLAIN))
    return tuple(notices)


def page_model(snap: AudioSnapshot, tiles: Mapping, selected: Optional[str], own_label: str,
               peers_without_audio=(), driver: Optional[str] = None) -> AudioPage:
    """Everything the Audio page shows (§9.1). `tiles`: machine id -> (x, y, w, h), or the layout's
    {x, y, w, h} dicts; `selected`: the machine whose panel shows, else this one; `own_label`:
    "This Mac" / "This PC"; `peers_without_audio`: names of linked machines whose Beamer has no
    audio (the snapshot lists audio machines only); `driver`: the Mac's Beamer Microphone driver as
    mac_app/audio_driver.py states it ("missing", "outdated", "current", "newer", "no_bundle"), None
    when unknown (the snapshot's virtual mic then decides)."""
    names = _Names(snap, own_label)
    shown = [snap.own] + [machine for machine in snap.machines if machine != snap.own]
    frames = _frames(shown, tiles or {})
    order = reading_order(frames)
    if selected not in frames:
        selected = snap.own
    own = snap.machines.get(snap.own)
    platform = own.platform if own is not None else ""
    choices, mic_selected = _mic_choices(snap, names, [snap.own] + [machine for machine in order if machine != snap.own])
    tile_views = tuple(_tile(snap, names, machine, frames[machine], selected) for machine in order)
    arrows = _arrows(snap, names, set(order))
    panel = _panel(snap, names, selected, order)
    meters = {tile.meter for tile in tile_views if tile.meter}
    meters.update(row.meter for row in panel.inputs + panel.outputs)
    mic_meter = _mic_meter(snap)
    if mic_meter:
        meters.add(mic_meter)
    return AudioPage(
        mic_label=tr("Microphone your apps hear"),
        mic_choices=choices, mic_selected=mic_selected,
        mic_line=_mic_line(snap, names), mic_tone=TONES.get(snap.mic_status.state, PLAIN) if snap.route.mic.machine else QUIET,
        mic_meter=mic_meter,
        mic_hint=tr("Apps use 'Beamer Microphone' on a Mac and 'CABLE Output' on a PC."),
        tiles=tile_views, arrows=arrows, selected=selected, panel=panel,
        notices=_notices(snap, names, platform, peers_without_audio, driver),
        map_description=" ".join([tile.description for tile in tile_views] + [arrow.description for arrow in arrows]),
        meters=frozenset(meters))


# Actions: what a page calls when a person changes something.

def choice_by_key(choices, key: str) -> Optional[Choice]:
    return next((choice for choice in choices if choice.key == key and not choice.header), None)


def apply_mic(service, choice: Choice) -> None:
    """The microphone selector picked `choice`."""
    if choice.key == OFF:
        service.set_mic(None, None)
    elif not choice.header:
        service.set_mic(choice.machine, choice.device)


def apply_output(service, machine: str, choice: Choice) -> None:
    """`machine`'s output selector picked `choice`."""
    if choice.key == HERE:
        service.set_output(machine, None, None)
    elif not choice.header:
        service.set_output(machine, choice.machine, choice.device)


def apply_headset(service, row: DeviceRow, on: bool) -> None:
    """The Headset switch of an output: an explicit form either way, so the switch always shows
    what was set, whatever the system says about the device."""
    service.set_form(row.machine, row.key, "headset" if on else "speakers")


def apply_vmic_exclusive(service, panel: MachinePanel, on: bool) -> None:
    """The low-latency virtual microphone switch: this machine's own, so only from its own panel.
    The service restarts the helper; streams drop for about a second."""
    if panel.vmic_exclusive_shown:
        service.set_vmic_exclusive(on)


def apply_test(service, row: DeviceRow) -> None:
    service.test_input(row.machine, row.key)


def apply_notice(service, notice: Notice) -> bool:
    """Runs a notice's action through the service; False when the action is the app's own
    ("install_driver", which installs or updates the Mac driver off the main thread,
    "open_mic_settings", "open_system_audio_settings") or there is none."""
    if notice.action == "retry_helper":
        service.retry_helper()
        return True
    if notice.action == "send_sound" and notice.machine and notice.target:
        service.set_output(notice.machine, notice.target, None)
        return True
    return False
