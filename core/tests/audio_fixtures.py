# SPDX-License-Identifier: GPL-3.0-only
"""Audio snapshots for the page tests and renders of both apps (dev/audio/PHASE2.md §9, §13 WP6/WP7).

Each fixture is a whole page state: the snapshot the service would hold, the Crossing layout's tiles,
the selected machine and a levels map. `FIXTURES` names the four the acceptance renders use;
`FakeService` stands in for core.audio.service.AudioService (§8.1) and records every setter call.

The machines are Alex's: this MacBook Air, the PC lebig (Windows, linked directly) and asusrog
(Windows, linked to lebig only).
"""

from __future__ import annotations

import base64
import threading
from dataclasses import dataclass, field, replace
from typing import Mapping, Optional

from core.audio import model
from core.audio.model import (AudioSnapshot, Device, DeviceRef, MachineAudio, MicRoute, OutputRoute, Route,
                              RouteStatus, Stamp, StreamView, VirtualMic)


def machine_id(seed: str) -> str:
    """A machine id as protocol.id_text makes them: 16 bytes, URL-safe base64 without padding."""
    raw = (seed.encode("ascii") * 16)[:16]
    return base64.urlsafe_b64encode(raw).decode("ascii").rstrip("=")


MAC = machine_id("mac-air")
PC = machine_id("lebig")
ROG = machine_id("asusrog")

MAC_MIC = Device("BuiltInMicrophoneDevice", "MacBook Air Microphone", "input", "builtin", 1, "microphone", True)
AIRPODS_MIC = Device("AirPods-Pro-mic", "AirPods Pro", "input", "bluetooth", 1, "headset", False)
MAC_SPEAKERS = Device("BuiltInSpeakerDevice", "MacBook Air Speakers", "output", "builtin", 2, "speakers", True)
AIRPODS = Device("AirPods-Pro-out", "AirPods Pro", "output", "bluetooth", 2, "headphones", False)

WAVE = Device("{0.0.1.00000000}.{6b1d0a43-wave3}", "Microphone (Wave:3)", "input", "usb", 1, "microphone", True)
G733_MIC = Device("{0.0.1.00000000}.{a5c2e0d1-g733}", "Microphone (Logitech G733)", "input", "usb", 1, "headset", False)
G733 = Device("{0.0.0.00000000}.{a5c2e0d1-g733}", "Speakers (Logitech G733)", "output", "usb", 2, "headset", True)
REALTEK = Device("{0.0.0.00000000}.{3f9e11aa-rtk}", "Speakers (Realtek(R) Audio)", "output", "builtin", 2, "speakers", False)
DUALSENSE = Device("{0.0.0.00000000}.{77d0c1e2-ds}", "Speakers (DualSense Wireless Controller)", "output", "usb", 2,
                   "headphones", False)

ROG_MIC = Device("{0.0.1.00000000}.{1234-rog-mic}", "Microphone Array (Realtek(R) Audio)", "input", "builtin", 2,
                 "microphone", True)
ROG_SPEAKERS = Device("{0.0.0.00000000}.{1234-rog-out}", "Speakers (Realtek(R) Audio)", "output", "builtin", 2,
                      "speakers", True)

MAC_FEATURES = frozenset({"system_capture", "local_mute_tap", "mic_demand", "virtual_mic", "aec"})
PC_FEATURES = frozenset({"system_capture", "local_mute_endpoint", "mic_demand", "virtual_mic", "aec"})

# The Crossing layout: lebig's wide screen left of the Mac, asusrog under lebig.
TILES = {
    PC: (-192.0, -8.0, 192.0, 108.0),
    MAC: (0.0, 0.0, 160.0, 100.0),
    ROG: (-192.0, 100.0, 192.0, 108.0),
}

T0 = 1793000000


def mac(*, links=(), vmic_present=True, in_use=False, outputs=(MAC_SPEAKERS, AIRPODS), status="ok") -> MachineAudio:
    defaults = [device.key for device in outputs if device.default]
    return MachineAudio(id=MAC, name="MacBook Air", platform="macos", rev=T0 * 1000 + 1, status=status,
                        inputs=(MAC_MIC, AIRPODS_MIC), outputs=tuple(outputs),
                        default_input=MAC_MIC.key, default_output=defaults[0] if defaults else None,
                        virtual_mic=VirtualMic("beamer" if vmic_present else None, vmic_present, in_use),
                        features=MAC_FEATURES if vmic_present else MAC_FEATURES - {"virtual_mic"},
                        links=frozenset(links), helper="0.1.0")


def pc(*, links=(MAC,), in_use=False, outputs=(G733, REALTEK, DUALSENSE), status="ok") -> MachineAudio:
    defaults = [device.key for device in outputs if device.default]
    return MachineAudio(id=PC, name="lebig", platform="windows", rev=T0 * 1000 + 2, status=status,
                        inputs=(WAVE, G733_MIC), outputs=tuple(outputs),
                        default_input=WAVE.key, default_output=defaults[0] if defaults else None,
                        virtual_mic=VirtualMic("vbcable", True, in_use), features=PC_FEATURES,
                        links=frozenset(links), helper="0.1.0")


def rog(*, links=(PC,)) -> MachineAudio:
    return MachineAudio(id=ROG, name="asusrog", platform="windows", rev=T0 * 1000 + 3, status="ok",
                        inputs=(ROG_MIC,), outputs=(ROG_SPEAKERS,), default_input=ROG_MIC.key,
                        default_output=ROG_SPEAKERS.key, virtual_mic=VirtualMic("vbcable", True, False),
                        features=PC_FEATURES, links=frozenset(links), helper="0.1.0")


def stamp(offset=0) -> Stamp:
    return Stamp(T0 + offset, MAC)


def snapshot(machines, *, own=MAC, direct=(), mic=None, outputs=None, mic_status=None, output_status=None,
             streams=(), helper="ok", permission=None, aec_active=False) -> AudioSnapshot:
    machines = {machine.id: machine for machine in machines}
    direct = frozenset(direct)
    route = Route(mic or model.MIC_OFF, dict(outputs or {}), {})
    return AudioSnapshot(own=own, machines=machines, online=model.online(own, direct, machines), direct=direct,
                         route=route, mic_status=mic_status or RouteStatus("off"),
                         output_status=dict(output_status or {}), streams=tuple(streams), helper=helper,
                         permission=dict(permission or {"mic": "granted", "system_audio": "granted"}),
                         aec_mode="auto", aec_active=aec_active)


@dataclass(frozen=True)
class Fixture:
    name: str
    snapshot: AudioSnapshot
    tiles: Mapping
    selected: Optional[str]
    levels: Mapping = field(default_factory=dict)
    own_label: str = "This Mac"
    peers_without_audio: tuple = ()


def no_peers() -> Fixture:
    """A Mac on its own: its devices, the mic off, nothing to route."""
    snap = snapshot([mac()])
    levels = {f"{MAC}/out/{MAC_SPEAKERS.key}": (-31.0, -22.5)}
    return Fixture("no_peers", snap, {MAC: TILES[MAC]}, None, levels)


def mic_streaming() -> Fixture:
    """The Mac's microphone feeds a call on lebig; each machine plays its own sound (lebig on its
    G733 headset, the Mac on its speakers, so Beamer cancels their echo)."""
    route_mic = MicRoute(MAC, None, stamp(10))
    streams = (StreamView(2882400018, "mic", MAC, PC, "running", "udp", 23.0, 0.1, 5.2),)
    snap = snapshot([mac(links=(PC,)), pc(in_use=True)], direct=(PC,), mic=route_mic,
                    mic_status=RouteStatus("streaming", None, {"to": PC}),
                    output_status={MAC: RouteStatus("local"), PC: RouteStatus("local")}, streams=streams,
                    aec_active=True)
    levels = {f"{MAC}/in/{MAC_MIC.key}": (-22.0, -9.5), f"{MAC}/out/{MAC_SPEAKERS.key}": (-38.0, -27.0),
              f"{PC}/out/{G733.key}": (-26.0, -14.0), f"{PC}/in/{WAVE.key}": (-58.0, -50.0)}
    return Fixture("mic_streaming", snap, {MAC: TILES[MAC], PC: TILES[PC]}, None, levels)


def sound_redirected() -> Fixture:
    """Alex's echo case: lebig's sound plays on the Mac's speakers (muted on lebig) while the Mac's
    mic feeds lebig's call. Beamer cancels the echo on the Mac; the panel shows lebig."""
    route_mic = MicRoute(MAC, None, stamp(10))
    outputs = {PC: OutputRoute(MAC, None, stamp(20))}
    streams = (StreamView(2882400018, "mic", MAC, PC, "running", "udp", 23.0, 0.1, 5.2),
               StreamView(1517325410, "system", PC, MAC, "running", "udp", 31.0, 0.0, 5.0))
    snap = snapshot([mac(links=(PC,)), pc(in_use=True, outputs=(replace(REALTEK, default=True), replace(G733, default=False),
                                                               DUALSENSE))],
                    direct=(PC,), mic=route_mic, outputs=outputs,
                    mic_status=RouteStatus("streaming", None, {"to": PC}),
                    output_status={MAC: RouteStatus("local"), PC: RouteStatus("streaming")}, streams=streams,
                    aec_active=True)
    levels = {f"{MAC}/in/{MAC_MIC.key}": (-25.0, -11.0), f"{MAC}/out/{MAC_SPEAKERS.key}": (-18.0, -6.0),
              f"{PC}/out/{REALTEK.key}": (-19.0, -7.0)}
    return Fixture("sound_redirected", snap, {MAC: TILES[MAC], PC: TILES[PC]}, PC, levels)


def driver_missing() -> Fixture:
    """Beamer Microphone is not installed on the Mac; lebig's Wave:3 is the chosen mic."""
    route_mic = MicRoute(PC, DeviceRef(WAVE.key, WAVE.name), stamp(10))
    snap = snapshot([mac(links=(PC,), vmic_present=False), pc()], direct=(PC,), mic=route_mic,
                    mic_status=RouteStatus("ready"),
                    output_status={MAC: RouteStatus("local"), PC: RouteStatus("local")})
    levels = {f"{PC}/in/{WAVE.key}": (-41.0, -30.0)}
    return Fixture("driver_missing", snap, {MAC: TILES[MAC], PC: TILES[PC]}, None, levels)


def three_machines() -> Fixture:
    """asusrog is linked to lebig only: online through the flood, not a direct peer of the Mac.
    Its mic is active and lebig records; the Mac's sound is sent to lebig but lebig is offline-
    waiting on a device."""
    route_mic = MicRoute(ROG, None, stamp(10))
    outputs = {MAC: OutputRoute(PC, DeviceRef("{gone}", "Headphones (Arctis 7)"), stamp(30))}
    streams = (StreamView(7, "mic", ROG, PC, "running", "udp", 20.0, 0.0, 3.0),)
    snap = snapshot([mac(links=(PC,)), pc(links=(MAC, ROG), in_use=True), rog()], direct=(PC,), mic=route_mic,
                    outputs=outputs, mic_status=RouteStatus("streaming", None, {"to": PC}),
                    output_status={MAC: RouteStatus("waiting_device"), PC: RouteStatus("local"),
                                   ROG: RouteStatus("local")},
                    streams=streams)
    return Fixture("three_machines", snap, dict(TILES), MAC, {})


FIXTURES = {
    "no_peers": no_peers,
    "mic_streaming": mic_streaming,
    "sound_redirected": sound_redirected,
    "driver_missing": driver_missing,
}


class FakeService:
    """The AudioService surface the pages use (§8.1), recording each setter call as (name, args).
    `push(snapshot)` and `push_levels(levels)` call the subscribers on the calling thread, as the
    service's worker would on its own."""

    def __init__(self, snap: AudioSnapshot):
        self._snapshot = snap
        self.calls = []
        self._subscribers = []
        self._level_subscribers = []
        self.lock = threading.Lock()

    def snapshot(self) -> AudioSnapshot:
        return self._snapshot

    def subscribe(self, fn):
        self._subscribers.append(fn)
        return lambda: self._subscribers.remove(fn) if fn in self._subscribers else None

    def subscribe_levels(self, fn):
        self._level_subscribers.append(fn)
        return lambda: self._level_subscribers.remove(fn) if fn in self._level_subscribers else None

    def push(self, snap: AudioSnapshot) -> None:
        self._snapshot = snap
        for fn in list(self._subscribers):
            fn()

    def push_levels(self, levels) -> None:
        for fn in list(self._level_subscribers):
            fn(levels)

    def _record(self, name, *args):
        with self.lock:
            self.calls.append((name, args))

    def set_mic(self, machine, device):
        self._record("set_mic", machine, device)

    def set_output(self, machine, to, device):
        self._record("set_output", machine, to, device)

    def set_form(self, machine, device_key, form):
        self._record("set_form", machine, device_key, form)

    def set_aec(self, mode):
        self._record("set_aec", mode)

    def watch(self, on):
        self._record("watch", on)

    def test_input(self, machine, device_key):
        self._record("test_input", machine, device_key)

    def retry_helper(self):
        self._record("retry_helper")
