# SPDX-License-Identifier: GPL-3.0-only
"""core.camera.planner.plan_camera, table-driven (PHASE2 §6.4, §10.1)."""

import unittest
from dataclasses import replace

from core.audio.model import MachineAudio, VirtualMic
from core.camera import model, planner
from core.camera.model import Camera, CameraEntry, CameraRef, CameraRoute, MachineCamera, QualityEntry, Stamp

MAC = "AQEBAQEBAQEBAQEBAQEBAQ"
PC = "AgICAgICAgICAgICAgICAg"
PC2 = "BAQEBAQEBAQEBAQEBAQEBA"
LINUX = "AwMDAwMDAwMDAwMDAwMDAw"
NOW = 1000.0


def mac(**changes):
    machine = MachineCamera(
        id=MAC, platform="macos", rev=1, status="ok", features=frozenset({"camera_capture"}),
        cameras=(Camera("cam-1", "MacBook Air Camera", "builtin", (), True),
                 Camera("cam-2", "Logi C920", "usb", (), False)),
        preferred="cam-1", permission="granted", sharing=True, capture=model.IDLE_CAPTURE,
        virtual_camera=model.NO_VIRTUAL_CAMERA)
    return replace(machine, **changes)


def pc(ident=PC, in_use=False, install="ok", want=None):
    return MachineCamera(
        id=ident, platform="windows", rev=1, status="ok", features=frozenset({"virtual_camera"}), cameras=(),
        preferred=None, permission=None, sharing=None, capture=model.IDLE_CAPTURE,
        virtual_camera=model.VirtualCamera(install, install == "ok", in_use, "Teams" if in_use else None, want))


def linux():
    return MachineCamera(id=LINUX, platform="linux", rev=1, status="no_video", features=frozenset(), cameras=(),
                         preferred=None, permission=None, sharing=None, capture=model.IDLE_CAPTURE,
                         virtual_camera=model.NO_VIRTUAL_CAMERA)


def audio(ident, links):
    return MachineAudio(id=ident, name=ident[:4], platform="", rev=1, status="ok", inputs=(), outputs=(),
                        default_input=None, default_output=None, virtual_mic=VirtualMic(None, False, False),
                        features=frozenset(), links=frozenset(links), helper="")


def route(machine=MAC, device=None, size="720p"):
    return CameraRoute(CameraEntry(machine, device, Stamp(1, MAC)), QualityEntry(size, Stamp(1, MAC)), {})


def plan(own=MAC, machines=None, route_value=None, direct=None, online=None, hold=None, no_udp=frozenset(),
         audio_machines=None, now=NOW):
    machines = machines if machines is not None else {MAC: mac(), PC: pc(in_use=True)}
    online = online if online is not None else frozenset(machines)
    direct = direct if direct is not None else frozenset(online - {own})
    audio_machines = audio_machines if audio_machines is not None else {
        ident: audio(ident, online - {ident}) for ident in online}
    return planner.plan_camera(own, machines, audio_machines, route_value or route(), direct, online, now,
                               hold or {}, no_udp)


class PlannerTests(unittest.TestCase):
    def test_off(self):
        result = plan(route_value=route(None))
        self.assertEqual(result.status.state, "off")
        self.assertEqual(result.sends, {})
        self.assertEqual(result.sinks[PC].state, "off")
        self.assertIsNone(result.warm)
        self.assertEqual(plan(own=PC, route_value=route(None)).idle_placeholder, "off")

    def test_demand_zero_one_two_sinks(self):
        none = plan(machines={MAC: mac(), PC: pc()})
        self.assertEqual(none.sends, {})
        self.assertEqual(none.status.state, "idle")
        self.assertEqual(none.warm, (1280, 720, 2_500_000))       # warm without any demand
        one = plan()
        self.assertEqual(set(one.sends), {PC})
        spec = one.sends[PC]
        self.assertEqual((spec.peer, spec.device, spec.w, spec.h, spec.fps, spec.bitrate),
                         (PC, None, 1280, 720, 30, 2_500_000))
        self.assertEqual(one.status.state, "starting")
        two = plan(machines={MAC: mac(), PC: pc(in_use=True), PC2: pc(PC2, in_use=True)})
        self.assertEqual(set(two.sends), {PC, PC2})
        self.assertEqual(two.status.params["to"], " ".join(sorted([PC, PC2])))

    def test_streaming_follows_the_source_capture(self):
        result = plan(machines={MAC: mac(capture=model.Capture("running", False, (), None)), PC: pc(in_use=True)})
        self.assertEqual(result.status.state, "streaming")
        self.assertEqual(result.sinks[PC].state, "streaming")

    def test_hold_three_seconds(self):
        machines = {MAC: mac(), PC: pc(in_use=False)}
        held = plan(machines=machines, hold={PC: NOW + 2.9})
        self.assertIn(PC, held.sends)
        self.assertEqual(held.wake_at, NOW + 2.9)
        self.assertNotIn(PC, plan(machines=machines, hold={PC: NOW}).sends)
        self.assertEqual(planner.DEMAND_HOLD, 3.0)

    def test_permission(self):
        undetermined = plan(machines={MAC: mac(permission="undetermined"), PC: pc(in_use=True)})
        self.assertEqual(undetermined.sends, {})
        self.assertEqual(undetermined.status.state, "waiting_permission")
        self.assertTrue(undetermined.needs_permission)
        self.assertIsNone(undetermined.warm)
        on_pc = plan(own=PC, machines={MAC: mac(permission="undetermined"), PC: pc(in_use=True)})
        self.assertFalse(on_pc.needs_permission)                 # only S itself needs it
        self.assertEqual(on_pc.idle_placeholder, "waiting_permission")
        self.assertEqual(on_pc.sinks[PC].state, "waiting_permission")
        for value in ("denied", "restricted"):
            denied = plan(machines={MAC: mac(permission=value), PC: pc(in_use=True)})
            self.assertEqual((denied.status.state, denied.sends), ("denied", {}))
            self.assertFalse(denied.needs_permission)
        self.assertEqual(plan(own=PC, machines={MAC: mac(permission="denied"), PC: pc()}).idle_placeholder, "denied")

    def test_sharing_off(self):
        machines = {MAC: mac(sharing=False), PC: pc(in_use=True), PC2: pc(PC2, in_use=True)}
        result = plan(machines=machines)
        self.assertEqual(result.sends, {})
        self.assertEqual(result.status.state, "sharing_off")
        self.assertEqual({result.sinks[PC].state, result.sinks[PC2].state}, {"sharing_off"})
        self.assertIsNone(result.warm)
        self.assertFalse(result.needs_permission)
        self.assertEqual(plan(own=PC, machines=machines).idle_placeholder, "sharing_off")
        # A newer route naming the Mac changes nothing: sharing is not in the route.
        newer = CameraRoute(CameraEntry(MAC, None, Stamp(99, PC)), QualityEntry("1080p", Stamp(99, PC)), {})
        self.assertEqual(plan(machines=machines, route_value=newer).sends, {})
        # Sharing off also blocks the dev test source.
        self.assertEqual(plan(machines={MAC: mac(sharing=False, test_source="synth"), PC: pc(in_use=True)}).sends, {})

    def test_missing_camera_and_return(self):
        chosen = route(device=CameraRef("cam-2", "Logi C920"))
        gone = plan(machines={MAC: mac(cameras=mac().cameras[:1]), PC: pc(in_use=True)}, route_value=chosen)
        self.assertEqual(gone.status.state, "waiting_device")
        self.assertEqual(gone.status.params["device"], "Logi C920")
        self.assertEqual(gone.sends, {})
        moved = mac(cameras=(mac().cameras[0], Camera("cam-2-port2", "Logi C920", "usb", (), False)))
        back = plan(machines={MAC: moved, PC: pc(in_use=True)}, route_value=chosen)
        self.assertEqual(back.sends[PC].device, "cam-2-port2")
        no_camera = plan(machines={MAC: mac(cameras=(), preferred=None), PC: pc(in_use=True)})
        self.assertEqual((no_camera.status.state, no_camera.sends), ("no_camera", {}))

    def test_preferred_camera_followed(self):
        result = plan(machines={MAC: mac(preferred="cam-2"), PC: pc(in_use=True)})
        self.assertIsNone(result.sends[PC].device)              # the helper follows the system choice
        self.assertEqual(plan(route_value=route(device=CameraRef("cam-2", "Logi C920"))).sends[PC].device, "cam-2")

    def test_not_installed_outdated_broken(self):
        for install, state in (("missing", "not_installed"), ("outdated", "outdated"), ("broken", "broken")):
            result = plan(machines={MAC: mac(), PC: pc(in_use=True, install=install)})
            self.assertEqual(result.sends, {}, install)
            self.assertEqual(result.sinks[PC].state, state)
            self.assertEqual(result.status.state, state)
            # Shown even without demand.
            self.assertEqual(plan(machines={MAC: mac(), PC: pc(install=install)}).sinks[PC].state, state)

    def test_not_direct_and_loopback_only(self):
        machines = {MAC: mac(), PC: pc(in_use=True)}
        unreachable = plan(machines=machines, direct=frozenset())
        self.assertEqual((unreachable.status.state, unreachable.sends), ("unreachable", {}))
        loopback = plan(machines=machines, no_udp=frozenset({PC}))
        self.assertEqual((loopback.status.state, loopback.sends), ("no_direct_path", {}))

    def test_size_is_the_cluster_quality_whatever_the_app_wants(self):
        for want in ((640, 360, 30), (1280, 720, 30), (1920, 1080, 30)):
            machines = {MAC: mac(), PC: pc(in_use=True, want=want)}
            spec = plan(machines=machines).sends[PC]
            self.assertEqual((spec.w, spec.h, spec.bitrate), (1280, 720, 2_500_000))
            spec = plan(machines=machines, route_value=route(size="1080p")).sends[PC]
            self.assertEqual((spec.w, spec.h, spec.bitrate), (1920, 1080, 5_000_000))

    def test_mac_as_sink_and_linux_ignored(self):
        other_mac = replace(mac(), id=PC2)
        machines = {MAC: mac(), PC: pc(in_use=True), PC2: other_mac, LINUX: linux()}
        result = plan(machines=machines)
        self.assertEqual(result.sinks[PC2].state, "no_virtual_camera")
        self.assertEqual(result.sinks[MAC].state, "no_virtual_camera")    # T = S
        self.assertNotIn(LINUX, result.sinks)
        self.assertEqual(set(result.sends), {PC})

    def test_source_offline_or_without_capture(self):
        offline = plan(machines={MAC: mac(), PC: pc(in_use=True)}, own=PC, online=frozenset({PC}))
        self.assertEqual(offline.status.state, "waiting_machine")
        self.assertEqual(offline.idle_placeholder, "waiting")
        not_a_source = plan(machines={MAC: mac(), PC: pc(in_use=True)}, route_value=route(PC))
        self.assertEqual(not_a_source.status.state, "no_camera")
        self.assertEqual(not_a_source.sends, {})

    def test_idle_placeholder_per_state(self):
        cases = ((route(None), {}, "off"), (route(), {"sharing": False}, "sharing_off"),
                 (route(), {"permission": "undetermined"}, "waiting_permission"),
                 (route(), {"permission": "denied"}, "denied"),
                 (route(), {"capture": model.Capture("failed", False, (), "no_camera")}, "error"),
                 (route(), {}, "waiting"))
        for route_value, changes, expected in cases:
            result = plan(own=PC, machines={MAC: mac(**changes), PC: pc()}, route_value=route_value)
            self.assertEqual(result.idle_placeholder, expected, changes)
        self.assertIsNone(plan().idle_placeholder)              # S is not a sink

    def test_warm(self):
        self.assertEqual(plan(machines={MAC: mac(), PC: pc()}).warm, (1280, 720, 2_500_000))
        self.assertIsNone(plan(own=PC).warm)                     # only on S
        self.assertIsNone(plan(machines={MAC: mac(sharing=False), PC: pc()}).warm)
        self.assertIsNone(plan(machines={MAC: mac(permission="denied"), PC: pc()}).warm)
        self.assertIsNone(plan(route_value=route(PC)).warm)      # route moved away
        self.assertIsNone(plan(machines={MAC: mac(cameras=(), preferred=None), PC: pc()}).warm)
        self.assertEqual(plan(route_value=route(size="1080p")).warm, (1920, 1080, 5_000_000))

    def test_dev_test_source(self):
        for changes in ({"permission": "undetermined"}, {"permission": "denied"}, {"cameras": (), "preferred": None},
                        {"permission": None, "cameras": (), "preferred": None}):
            result = plan(machines={MAC: mac(test_source="synth", **changes), PC: pc(in_use=True)})
            self.assertEqual(set(result.sends), {PC}, changes)
            self.assertIsNone(result.sends[PC].device)
            self.assertEqual(result.warm, (1280, 720, 2_500_000))
            self.assertNotIn(result.status.state, ("waiting_permission", "denied", "no_camera"))
            self.assertFalse(result.needs_permission)
            without = plan(machines={MAC: mac(**changes), PC: pc(in_use=True)})
            self.assertIn(without.status.state, ("waiting_permission", "denied", "no_camera"), changes)
            self.assertEqual(without.sends, {})

    def test_remote_source_reachability_uses_its_links(self):
        machines = {MAC: mac(), PC: pc(in_use=True), PC2: pc(PC2, in_use=True)}
        online = frozenset(machines)
        audio_machines = {MAC: audio(MAC, {PC}), PC: audio(PC, {MAC, PC2}), PC2: audio(PC2, {PC})}
        result = plan(own=PC, machines=machines, online=online, direct=frozenset({MAC, PC2}),
                      audio_machines=audio_machines)
        self.assertEqual(result.sinks[PC2].state, "unreachable")
        self.assertEqual(result.sinks[PC].state, "starting")
        self.assertEqual(result.sends, {})                       # only S sends


if __name__ == "__main__":
    unittest.main()
