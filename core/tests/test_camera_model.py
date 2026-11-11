# SPDX-License-Identifier: GPL-3.0-only
"""core.camera.model: route merge, device-list acceptance, camera resolution, sizes (PHASE2 §7.2, §10.1)."""

import random
import unittest
from dataclasses import replace

from core.camera import model
from core.camera.model import (Camera, CameraEntry, CameraRef, CameraRoute, Capture, MachineCamera, QualityEntry,
                               Stamp, VirtualCamera)

MAC = "AA" * 16
PC = "BB" * 16
AIR = "CC" * 16


def mac(rev=1, **changes):
    machine = MachineCamera(
        id=MAC, platform="macos", rev=rev, status="ok", features=frozenset({"camera_capture"}),
        cameras=(Camera("cam-1", "MacBook Air Camera", "builtin", ("1920x1080@30",), True),),
        preferred="cam-1", permission="granted", sharing=True, capture=model.IDLE_CAPTURE,
        virtual_camera=model.NO_VIRTUAL_CAMERA)
    return replace(machine, **changes)


def route(machine=None, set_at=0, by="", size="720p", size_at=0, size_by="", device=None, extra=None):
    return CameraRoute(CameraEntry(machine, device, Stamp(set_at, by)), QualityEntry(size, Stamp(size_at, size_by)),
                       extra or {})


class RouteMergeTests(unittest.TestCase):
    def test_newer_entry_wins_per_entry(self):
        held = route(MAC, 10, MAC, "720p", 50, PC)
        incoming = route(None, 20, PC, "1080p", 40, MAC)
        merged, changed, answer = model.merge_route(held, incoming)
        self.assertEqual(merged.camera.machine, None)       # incoming newer
        self.assertEqual(merged.quality.size, "720p")        # held newer
        self.assertTrue(changed)
        self.assertTrue(answer)                              # the sender lacks our quality

    def test_tie_broken_by_larger_by(self):
        one = route(MAC, 10, MAC)
        other = route(PC, 10, PC)
        merged_a, _c, _a = model.merge_route(one, other)
        merged_b, _c, _a = model.merge_route(other, one)
        self.assertEqual(merged_a, merged_b)
        self.assertEqual(merged_a.camera.machine, PC if PC.encode() > MAC.encode() else MAC)

    def test_idempotent_and_no_answer_for_equal(self):
        held = route(MAC, 10, MAC, "1080p", 11, MAC)
        merged, changed, answer = model.merge_route(held, held)
        self.assertEqual(merged, held)
        self.assertFalse(changed)
        self.assertFalse(answer)

    def test_unknown_entries_kept_and_forwarded(self):
        sinks = {"sinks": {PC: {"from": MAC}, "set_at": 5, "by": MAC}}
        held = route(MAC, 10, MAC)
        incoming = route(MAC, 10, MAC, extra=sinks)
        merged, changed, answer = model.merge_route(held, incoming)
        self.assertEqual(merged.extra, sinks)
        self.assertTrue(changed)
        self.assertFalse(answer)
        # Round trip through the data keeps it.
        self.assertEqual(model.route_from(model.route_data(merged)), merged)
        newer = {"sinks": {PC: {"from": None}, "set_at": 6, "by": PC}}
        merged2, _c, answer2 = model.merge_route(merged, route(MAC, 10, MAC, extra=newer))
        self.assertEqual(merged2.extra, newer)
        self.assertFalse(answer2)

    def test_commutative_associative_on_random_routes(self):
        rng = random.Random(7)
        ids = [MAC, PC, AIR, ""]
        def any_route():
            extra = {} if rng.random() < 0.6 else {"x": {"v": rng.randint(0, 3), "set_at": rng.randint(0, 3),
                                                         "by": rng.choice(ids)}}
            return route(rng.choice([None, MAC, PC]), rng.randint(0, 5), rng.choice(ids),
                         rng.choice(model.QUALITIES), rng.randint(0, 5), rng.choice(ids),
                         rng.choice([None, CameraRef("k", "n")]), extra)
        for _ in range(300):
            a, b, c = any_route(), any_route(), any_route()
            ab = model.merge_route(a, b)[0]
            ba = model.merge_route(b, a)[0]
            self.assertEqual(ab, ba)
            self.assertEqual(model.merge_route(ab, c)[0], model.merge_route(a, model.merge_route(b, c)[0])[0])
            self.assertEqual(model.merge_route(ab, ab)[0], ab)
            self.assertFalse(model.merge_route(ab, a)[1])     # a's entries are already in ab

    def test_next_stamp_is_newer_than_everything(self):
        held = route(MAC, 1793000000, MAC, "720p", 1793000005, PC)
        stamp = model.next_stamp(held, AIR, 1793000001.0)
        self.assertTrue(model.newer(stamp, held.camera.stamp))
        self.assertTrue(model.newer(stamp, held.quality.stamp))

    def test_route_data_round_trip(self):
        held = route(MAC, 3, PC, "1080p", 4, MAC, CameraRef("cam-1", "MacBook Air Camera"))
        self.assertEqual(model.route_from(model.route_data(held)), held)
        self.assertEqual(model.route_from({}), model.empty_route())


class DevicesTests(unittest.TestCase):
    def test_higher_rev_wins(self):
        self.assertTrue(model.accept_devices(None, mac(1), direct_from=False))
        self.assertTrue(model.accept_devices(mac(1), mac(2), direct_from=False))
        self.assertFalse(model.accept_devices(mac(2), mac(1, permission="denied"), direct_from=False))

    def test_direct_from_its_machine_replaces_a_differing_list(self):
        self.assertTrue(model.accept_devices(mac(2), mac(1, permission="denied"), direct_from=True))
        self.assertFalse(model.accept_devices(mac(2), mac(2), direct_from=True))

    def test_sharing_only_on_a_capture_machine(self):
        pc = MachineCamera(id=PC, platform="windows", rev=1, status="ok", features=frozenset({"virtual_camera"}),
                           cameras=(), preferred=None, permission=None, sharing=False, capture=model.IDLE_CAPTURE,
                           virtual_camera=VirtualCamera("ok", True, False, None, None), test_source="synth")
        clean = model.sanitize(pc)
        self.assertIsNone(clean.sharing)
        self.assertIsNone(clean.test_source)
        self.assertIs(model.sanitize(mac(sharing=False)).sharing, False)
        self.assertIs(model.sanitize(mac(sharing=None)).sharing, True)

    def test_machine_data_round_trip(self):
        machine = mac(5, capture=Capture("running", True, ("center_stage",), None),
                      virtual_camera=VirtualCamera("unsupported", False, False, None, None), test_source="synth")
        self.assertEqual(model.machine_from(model.machine_data(machine)), machine)
        pc = MachineCamera(id=PC, platform="windows", rev=1, status="ok", features=frozenset({"virtual_camera"}),
                           cameras=(), preferred=None, permission=None, sharing=None, capture=model.IDLE_CAPTURE,
                           virtual_camera=VirtualCamera("ok", True, True, "Microsoft Teams", (1280, 720, 30)))
        self.assertEqual(model.machine_from(model.machine_data(pc)), pc)


class ResolveTests(unittest.TestCase):
    CAMS = (Camera("a", "FaceTime HD", "builtin", (), True), Camera("b", "Logi C920", "usb", (), False),
            Camera("c", "Logi C920", "usb", (), False))

    def test_key_first(self):
        self.assertEqual(model.resolve_camera(CameraRef("b", "anything"), self.CAMS).key, "b")

    def test_unique_name_when_the_key_moved(self):
        self.assertEqual(model.resolve_camera(CameraRef("gone", "FaceTime HD"), self.CAMS).key, "a")

    def test_ambiguous_or_missing_name(self):
        self.assertIsNone(model.resolve_camera(CameraRef("gone", "Logi C920"), self.CAMS))
        self.assertIsNone(model.resolve_camera(CameraRef("gone", "Other"), self.CAMS))
        self.assertIsNone(model.resolve_camera(None, self.CAMS))


class SizeTests(unittest.TestCase):
    def test_sizes(self):
        self.assertEqual(model.size_for("720p"), (1280, 720, 2_500_000))
        self.assertEqual(model.size_for("1080p"), (1920, 1080, 5_000_000))
        self.assertEqual(model.size_for("4k"), (1280, 720, 2_500_000))

    def test_snapshot_types_are_frozen_with_own_mappings(self):
        one, two = model.CameraStatus("off"), model.CameraStatus("off")
        self.assertIsNot(one.params, two.params)
        with self.assertRaises(Exception):
            one.state = "idle"
        snap = model.empty_snapshot(MAC)
        self.assertEqual(snap.status.state, "off")
        self.assertEqual(snap.route, model.empty_route())


if __name__ == "__main__":
    unittest.main()
