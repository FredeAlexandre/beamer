# SPDX-License-Identifier: GPL-3.0-only
"""Camera snapshots for the Camera module's tests and renders in both apps (dev/camera/PHASE2.md
§8, §10.1, §12 CW7/CW8).

Each fixture is a whole page state: an audio fixture (core/tests/audio_fixtures.py: the machines,
their names, the map) and the camera snapshot the service would hold beside it. `FIXTURES` are the
eight acceptance states seen from the Mac (CW7); `FIXTURES_PC` the same seen from lebig plus the PC's
own install states (CW8). `FakeService` adds the camera API of §7.1 to the audio one and records
every call.

The machines are Alex's: this MacBook Air (the camera machine) and the PC lebig (Beamer Camera).
"""

from __future__ import annotations

from dataclasses import dataclass, replace

from core.audio.model import MicRoute, RouteStatus
from core.camera import model
from core.camera.model import (Camera, CameraEntry, CameraRef, CameraRoute, CameraSnapshot, CameraStatus, Capture,
                               LocalVcam, MachineCamera, QualityEntry, Stamp, VideoStreamView, VirtualCamera)
from core.tests import audio_fixtures as fx
from core.tests.audio_fixtures import MAC, PC, ROG, T0

AIR_CAMERA = Camera("47B4B64B70674B9CAD2BAE273A71F4B5", "MacBook Air Camera", "builtin", ("1920x1080@30", "1280x720@30"),
                    True)
IPHONE_CAMERA = Camera("8C1E5A2D-0000-4C3A-9F00-iphone", "iPhone Camera", "continuity", ("1920x1080@30",), False)
CAMERAS = (AIR_CAMERA, IPHONE_CAMERA)
TEAMS = "Microsoft Teams"
STREAM = 3123500001


def mac_camera(*, permission="granted", sharing=True, capture="idle", cameras=CAMERAS, test_source=None,
               status="ok") -> MachineCamera:
    preferred = next((camera.key for camera in cameras if camera.preferred), None)
    return MachineCamera(id=MAC, platform="macos", rev=T0 * 1000 + 11, status=status,
                         features=frozenset({"camera_capture"}), cameras=tuple(cameras), preferred=preferred,
                         permission=permission, sharing=sharing, capture=Capture(capture, False, (), None),
                         virtual_camera=model.NO_VIRTUAL_CAMERA, test_source=test_source)


def pc_camera(*, install="ok", in_use=False, app=None, ident=PC) -> MachineCamera:
    return MachineCamera(id=ident, platform="windows", rev=T0 * 1000 + 12, status="ok",
                         features=frozenset({"virtual_camera"}), cameras=(), preferred=None, permission=None,
                         sharing=None, capture=model.IDLE_CAPTURE,
                         virtual_camera=VirtualCamera(install, install == "ok", in_use, app,
                                                      (1280, 720, 30) if in_use else None))


def route(machine=MAC, device=None, quality="720p", by=MAC) -> CameraRoute:
    camera = model.CAMERA_OFF if machine is None else CameraEntry(machine, device, Stamp(T0 + 40, by))
    return CameraRoute(camera, QualityEntry(quality, Stamp(T0 + 41, by)), {})


def status(state, to=PC, reason=None, machine=MAC) -> CameraStatus:
    params = {"machine": machine}
    if to:
        params["to"] = to
    return CameraStatus(state, reason, params)


def send_stream(*, path="udp", fps=30.0, kbps=2450.0, delay_ms=72.0, size=(1280, 720)) -> VideoStreamView:
    return VideoStreamView(STREAM, MAC, PC, "running", path, size[0], size[1], fps, kbps, delay_ms, 0.0)


def camera_snapshot(machines, route_, status_, sinks, *, own=MAC, streams=(), needs_permission=False):
    machines = {machine.id: machine for machine in machines}
    held = machines.get(own)
    return CameraSnapshot(own=own, machines=machines, route=route_, status=status_, sinks=dict(sinks),
                          streams=tuple(streams), permission=held.permission if held else None,
                          needs_permission=needs_permission, sharing=held.sharing if held else None,
                          vcam_install_offer=None, vcam_local=None)


@dataclass(frozen=True)
class Fixture:
    name: str
    audio: fx.Fixture
    camera: CameraSnapshot


def _linked_audio() -> fx.Fixture:
    """The Mac and lebig linked directly, the Mac's microphone shared and ready (no app records it)."""
    snap = fx.snapshot([fx.mac(links=(PC,)), fx.pc()], direct=(PC,), mic=MicRoute(MAC, None, fx.stamp(10)),
                       mic_status=RouteStatus("ready"),
                       output_status={MAC: RouteStatus("local"), PC: RouteStatus("local")})
    return fx.Fixture("linked", snap, {MAC: fx.TILES[MAC], PC: fx.TILES[PC]}, None,
                      {f"{MAC}/out/{fx.MAC_SPEAKERS.key}": (-34.0, -24.0)})


def _call_audio() -> fx.Fixture:
    """A call on lebig: it records the Mac's microphone (audio_fixtures.mic_streaming)."""
    return fx.mic_streaming()


def off() -> Fixture:
    """No camera chosen: lebig's apps see the 'camera off' picture."""
    snap = camera_snapshot([mac_camera(), pc_camera()], route(None), CameraStatus("off"),
                           {PC: CameraStatus("off"), MAC: status("no_virtual_camera", None, machine=MAC)})
    return Fixture("off", _linked_audio(), snap)


def idle() -> Fixture:
    """The Mac's default camera chosen, no app on lebig has Beamer Camera open."""
    snap = camera_snapshot([mac_camera(), pc_camera()], route(), status("idle"),
                           {PC: status("idle"), MAC: status("no_virtual_camera", None, machine=MAC)})
    return Fixture("idle", _linked_audio(), snap)


def streaming() -> Fixture:
    """Teams on lebig uses Beamer Camera: the Mac's camera streams at 720p over UDP."""
    snap = camera_snapshot([mac_camera(capture="running"), pc_camera(in_use=True, app=TEAMS)], route(),
                           status("streaming"), {PC: status("streaming"), MAC: status("no_virtual_camera", None)},
                           streams=(send_stream(),))
    return Fixture("streaming", _call_audio(), snap)


def permission_from_pc() -> Fixture:
    """lebig's page chose the Mac's camera while the Mac has never been asked: no prompt, the Mac
    shows the notice and the menu item, lebig shows 'Allow camera access on MacBook Air'."""
    snap = camera_snapshot([mac_camera(permission="undetermined"), pc_camera(in_use=True, app=TEAMS)],
                           route(by=PC), status("waiting_permission", None),
                           {PC: status("waiting_permission"), MAC: status("no_virtual_camera", None)},
                           needs_permission=True)
    return Fixture("permission_from_pc", _call_audio(), snap)


def sharing_off() -> Fixture:
    """'Stop Sharing the Camera' on the Mac: the route still names it, nothing is sent."""
    snap = camera_snapshot([mac_camera(sharing=False), pc_camera(in_use=True, app=TEAMS)], route(),
                           status("sharing_off", None), {PC: status("sharing_off"), MAC: status("no_virtual_camera", None)})
    return Fixture("sharing_off", _call_audio(), snap)


def pc_not_installed() -> Fixture:
    """Beamer Camera is not installed on lebig."""
    snap = camera_snapshot([mac_camera(), pc_camera(install="missing")], route(), status("not_installed"),
                           {PC: status("not_installed"), MAC: status("no_virtual_camera", None)})
    return Fixture("pc_not_installed", _linked_audio(), snap)


def reduced() -> Fixture:
    """Streaming through the Beamer link at 15 fps: UDP between the machines stopped."""
    snap = camera_snapshot([mac_camera(capture="running"), pc_camera(in_use=True, app=TEAMS)], route(),
                           status("reduced"), {PC: status("reduced"), MAC: status("no_virtual_camera", None)},
                           streams=(send_stream(path="link", fps=15.0, kbps=1000.0, delay_ms=180.0),))
    return Fixture("reduced", _call_audio(), snap)


def denied() -> Fixture:
    """The Mac's camera access was refused in System Settings."""
    snap = camera_snapshot([mac_camera(permission="denied"), pc_camera(in_use=True, app=TEAMS)], route(),
                           status("denied", None), {PC: status("denied"), MAC: status("no_virtual_camera", None)})
    return Fixture("denied", _call_audio(), snap)


# The eight acceptance states (CW7), as this Mac's Beamer shows them.
FIXTURES = {
    "off": off,
    "idle": idle,
    "streaming": streaming,
    "permission_from_pc": permission_from_pc,
    "sharing_off": sharing_off,
    "pc_not_installed": pc_not_installed,
    "reduced": reduced,
    "denied": denied,
}

OFFERS = {"missing": "install", "outdated": "update", "broken": "repair"}


def seen_from_pc(fixture: Fixture, *, local=LocalVcam(1, 1, 1)) -> Fixture:
    """The same cluster from lebig's Beamer: its own id, its own permission/sharing (none), its
    install offer and versions, and the receive side of the stream."""
    snap = fixture.camera
    own = snap.machines[PC]
    streams = tuple(replace(stream, source=MAC, target=PC) for stream in snap.streams)
    camera = replace(snap, own=PC, permission=None, needs_permission=False, sharing=None,
                     vcam_install_offer=OFFERS.get(own.virtual_camera.install), vcam_local=local, streams=streams)
    return Fixture(f"{fixture.name}_from_pc", fx.seen_from(fixture.audio, PC), camera)


def restart_needed() -> Fixture:
    """lebig installed a new Beamer Camera while Frame Server still runs the old one (§9.2)."""
    return replace(seen_from_pc(idle(), local=LocalVcam(1, 2, 2)), name="restart_needed")


def outdated_pc() -> Fixture:
    snap = camera_snapshot([mac_camera(), pc_camera(install="outdated")], route(), status("outdated"),
                           {PC: status("outdated"), MAC: status("no_virtual_camera", None)})
    return seen_from_pc(Fixture("outdated", _linked_audio(), snap))


# The same states as lebig's Beamer shows them (CW8), with its own install states.
FIXTURES_PC = {name: (lambda make=make: seen_from_pc(make())) for name, make in FIXTURES.items()}
FIXTURES_PC["restart_needed"] = restart_needed
FIXTURES_PC["outdated"] = outdated_pc


def three_machines() -> Fixture:
    """asusrog (a second PC, linked to lebig only) also has Beamer Camera, not installed; lebig streams."""
    audio = fx.three_machines()
    snap = camera_snapshot([mac_camera(capture="running"), pc_camera(in_use=True, app=TEAMS),
                            pc_camera(install="missing", ident=ROG)], route(), status("streaming"),
                           {PC: status("streaming"), ROG: status("not_installed", ROG), MAC: status("no_virtual_camera", None)},
                           streams=(send_stream(),))
    return Fixture("three_machines", audio, snap)


class FakeService(fx.FakeService):
    """The AudioService surface the pages use, with the camera part of §7.1: `camera_snapshot()`,
    the camera setters (recorded as (name, args)), and `push_camera(snapshot)`, which calls the
    subscribers as a camera change does."""

    def __init__(self, audio, camera: CameraSnapshot):
        super().__init__(audio)
        self._camera = camera

    def camera_snapshot(self) -> CameraSnapshot:
        return self._camera

    def push_camera(self, snap: CameraSnapshot) -> None:
        self._camera = snap
        for fn in list(self._subscribers):
            fn()

    def set_camera(self, machine, device):
        self._record("set_camera", machine, device)

    def set_quality(self, size):
        self._record("set_quality", size)

    def set_camera_sharing(self, on):
        self._record("set_camera_sharing", on)

    def ask_camera_permission(self):
        self._record("ask_camera_permission")

    def refresh_camera_install(self):
        self._record("refresh_camera_install")


def service_for(fixture: Fixture) -> FakeService:
    return FakeService(fixture.audio.snapshot, fixture.camera)


__all__ = ["AIR_CAMERA", "IPHONE_CAMERA", "CAMERAS", "TEAMS", "Fixture", "FIXTURES", "FIXTURES_PC", "FakeService",
           "mac_camera", "pc_camera", "route", "status", "camera_snapshot", "seen_from_pc", "service_for",
           "three_machines", "MAC", "PC", "ROG", "CameraRef"]
