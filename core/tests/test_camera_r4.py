# SPDX-License-Identifier: GPL-3.0-only
"""Regression tests for the R4 Python review (dev/camera/reviews/r4-python.md) and the operator's
rulings on it (dev/camera/reviews/r4-resolution.md): each fails on the code the review read.

- #1: "Off" on the PC always stops the Mac camera: a bad route entry from a peer (an unvalidated
  `extra` stamp, an `extra` over 16 KiB) never blocks later changes, and a sink whose own route no
  longer names the sender closes the stream (a peer clock more than a day ahead).
- #2: demand learned before a link loss, a restart or a helper restart never turns the camera on.
- #4: the sharing switch fails closed on persistence problems.
- #5: held camera lists are bounded and purged.
- #8: a sink's normal close is no error on the source.
"""

import json
import logging
import os
import shutil
import tempfile
import unittest
from unittest import mock

from core.camera import model, store, wire
from core.camera import part as part_module
from core.tests.audio_cluster_harness import machine_id
from core.tests.test_camera_cluster import ClusterBase
from core.tests.test_camera_part import MAC, PC, Base, Ctx, mac_list, pc_list

DAY = 86400


def stored_text(node):
    return (node.part._state_dir / store.FILE_NAME).read_text(encoding="utf-8")


class OffAlwaysStopsTheMacCamera(ClusterBase):
    """R4 #1, the three confirmed scenarios: Alex sets Off on the PC while an app there uses the Mac
    camera; the Mac camera must stop at once and stay off."""

    def streaming(self):
        self.cluster(linux=False)
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        self.pc.set_in_use(True, "Microsoft Teams")
        self.net.advance(0.5)
        self.assertTrue(self.mac.video.camera_on())
        self.assertEqual(self.pc.camera_snapshot().status.state, "streaming")

    def poison_pc(self, name, value):
        """A peer (here: the Mac's id, any direct peer would do) sends the PC a route with an unknown
        entry it should not keep."""
        message = wire.route(self.pc.part._route)
        message["data"][name] = value
        self.pc.service.link_message(self.mac.id, message)
        self.settle()

    def off_on_the_pc(self):
        self.pc.service.set_camera(None, None)
        self.net.advance(0.3)
        self.assertFalse(self.mac.video.camera_on(), "the Mac camera is still on after Off on the PC")
        self.assertIsNone(self.pc.camera_snapshot().route.camera.machine)
        self.assertEqual(self.pc.camera_snapshot().status.state, "off")
        sends = len(self.mac.helper.commands("send"))
        self.net.advance(30)
        self.assertFalse(self.mac.video.camera_on())
        self.assertEqual(len(self.mac.helper.commands("send")), sends)
        self.assertNotEqual(self.mac.camera_snapshot().status.state, "error")
        self.assertClean()

    def test_scenario_a_unvalidated_extra_stamp(self):
        self.streaming()
        self.poison_pc("future", {"set_at": 2 ** 53, "by": ""})
        self.off_on_the_pc()
        # The route reached the Mac too: both views say Off, and nothing bad was kept or saved.
        self.assertIsNone(self.mac.camera_snapshot().route.camera.machine)
        self.net.advance(1)
        for node in (self.mac, self.pc):
            self.assertNotIn("future", node.camera_snapshot().route.extra)
            self.assertNotIn("future", stored_text(node))
        # Later local changes still propagate.
        self.pc.service.set_quality("1080p")
        self.settle()
        self.assertEqual(self.mac.camera_snapshot().route.quality.size, "1080p")

    def test_scenario_b_extra_over_16_kib(self):
        self.streaming()
        self.poison_pc("blob", {"data": "x" * 17_000})
        self.off_on_the_pc()
        self.assertIsNone(self.mac.camera_snapshot().route.camera.machine)
        self.net.advance(1)
        for node in (self.mac, self.pc):
            self.assertNotIn("blob", node.camera_snapshot().route.extra)
            self.assertLess(len(stored_text(node)), 16_000)

    def test_scenario_c_pc_clock_more_than_a_day_ahead(self):
        self.streaming()
        ahead = lambda: self.net.clock.wall() + 2 * DAY      # noqa: E731 - a CMOS reset on the PC
        self.pc.service._wall = ahead
        self.pc.part._ctx.wall = ahead
        with self.assertLogs("beamer", logging.WARNING) as logs:
            self.off_on_the_pc()
        self.assertTrue(any("camera_route" in line and "dropped" in line for line in logs.output), logs.output)
        # The Mac could not take the PC's change (its stamp is two days ahead); the PC enforced it.
        self.assertEqual(self.mac.camera_snapshot().route.camera.machine, self.mac.id)
        self.assertEqual(self.pc.video.recvs, {})


class NoStaleDemand(ClusterBase):
    """R4 #2: the PC app closes while the Mac cannot hear about it; the Mac camera never turns on."""

    def streaming(self):
        self.cluster(linux=False)
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        self.pc.set_in_use(True, "Microsoft Teams")
        self.net.advance(0.5)
        self.assertTrue(self.mac.video.camera_on())

    def assert_never_on(self, seconds=6.0):
        starts = self.mac.video.camera_starts
        for _ in range(int(seconds / 0.05)):
            self.net.advance(0.05, step=0.05)
            self.assertFalse(self.mac.video.camera_on(), f"camera on at {self.net.clock():.2f}")
        self.assertEqual(self.mac.video.camera_starts, starts)
        self.assertClean()

    def test_app_closed_during_a_link_outage(self):
        self.streaming()
        self.net.unlink(self.mac, self.pc)
        self.net.advance(0.2)
        self.assertFalse(self.mac.video.camera_on())
        self.pc.set_in_use(False)                                   # Teams closes while the link is down
        self.net.advance(1)
        self.net.link(self.mac, self.pc)
        self.assert_never_on()
        # A new call still works at once.
        self.pc.set_in_use(True, "Microsoft Teams")
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_app_closed_while_the_mac_beamer_restarts(self):
        self.streaming()
        self.mac.restart()                                          # Beamer quits on the Mac, starts again
        self.net.unlink(self.mac, self.pc)
        self.pc.set_in_use(False)
        self.net.advance(1)
        self.net.link(self.mac, self.pc)
        self.assert_never_on()

    def test_app_closed_while_the_pc_helper_restarts(self):
        self.streaming()
        self.pc.helper.crash(restart=False)
        self.net.advance(0.2)
        self.pc.video.set_in_use(False)                             # Teams closes during the restart
        self.pc.demand_until = min(self.pc.demand_until, self.net.clock())
        self.net.advance(0.2)
        self.assertFalse(self.mac.video.camera_on())
        self.pc.helper.ready()
        self.assert_never_on()


class SinkCloseIsNoError(ClusterBase):
    """R4 #8: a `route` close from the sink is a normal handover, shown as no error on the Mac."""

    def test_route_close_from_the_sink(self):
        self.cluster(linux=False)
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        self.pc.set_in_use(True)
        self.net.advance(0.5)
        stream = next(iter(self.mac.video.sends))
        self.mac.service.link_message(self.pc.id, wire.close(stream, "route"))
        self.net.advance(0.1)
        self.assertFalse(self.mac.video.camera_on())
        snap = self.mac.camera_snapshot()
        self.assertNotEqual(snap.sinks[self.pc.id].state, "error")
        self.assertNotEqual(snap.status.state, "error")


class PartTests(Base):
    def stored(self):
        with open(os.path.join(self.dir, store.FILE_NAME), encoding="utf-8") as handle:
            return json.load(handle)

    def write_store(self, data):
        path = os.path.join(self.dir, store.FILE_NAME)
        with open(path, "w", encoding="utf-8") as handle:
            handle.write(data if isinstance(data, str) else json.dumps(data))

    # R4 #2: lists loaded from camera.json carry no demand.
    def test_loaded_lists_have_no_demand(self):
        mac = self.mac()
        self.route_to_mac(mac)
        self.demand(mac, 1)
        self.assertTrue(mac.video.camera_on())
        mac.part.shutdown()
        self.assertTrue(self.stored()["machines"][PC]
                        ["virtual_camera"]["in_use"])                 # the file may say so ...
        again = Ctx(MAC, "macos", self.dir, self.clock)               # ... the restarted part does not use it
        again.add_peer(PC)
        again.flush()
        again.advance(5)
        self.assertEqual(again.commands("send"), [])

    def test_link_down_drops_demand_without_a_hold(self):
        mac = self.mac()
        self.route_to_mac(mac)
        self.demand(mac, 1)
        del mac.peers[PC]
        mac.part.link_down(PC)
        mac.flush()
        self.assertEqual(mac.part._demand_hold, {})
        self.assertFalse(mac.part.snapshot().machines[PC].virtual_camera.in_use)
        mac.add_peer(PC)
        mac.flush()
        self.assertEqual(len(mac.commands("send")), 1)                # only the first one
        self.demand(mac, 2, in_use=False)                             # the fresh list: no hold either
        self.assertEqual(mac.part._demand_hold, {})
        self.assertEqual(len(mac.commands("send")), 1)

    def test_a_live_end_still_holds_three_seconds(self):
        mac = self.mac()
        self.route_to_mac(mac)
        self.demand(mac, 1)
        self.demand(mac, 2, in_use=False)
        self.assertIn(PC, mac.part._demand_hold)
        self.assertTrue(mac.video.camera_on())

    # R4 #4: the sharing switch fails closed.
    def test_sharing_off_is_written_at_once(self):
        mac = self.mac()
        mac.part.set_sharing(False)
        # No flush, no debounce: a force quit now keeps it.
        self.assertIs(self.stored()["sharing"], False)

    def test_unreadable_or_other_version_file_starts_with_sharing_off(self):
        for content in ("{not json", json.dumps({"version": 2, "sharing": True}), json.dumps([1]),
                        json.dumps({"version": 1, "sharing": "yes"})):
            with self.subTest(content=content):
                self.write_store(content)
                mac = Ctx(MAC, "macos", self.dir, self.clock)
                mac.add_peer(PC)
                mac.flush()
                snap = mac.part.snapshot()
                self.assertIs(snap.sharing, False)
                self.assertEqual(snap.store_problem, "unreadable")
                mac.part.set_camera(MAC, None)
                mac.receive(PC, wire.devices(pc_list(1)))
                self.assertEqual(mac.commands("send"), [])
                mac.part.set_sharing(True)                            # Alex turns it back on: the notice goes
                mac.flush()
                self.assertIsNone(mac.part.snapshot().store_problem)
                os.remove(os.path.join(self.dir, store.FILE_NAME))

    def test_missing_file_is_a_first_run_with_sharing_on(self):
        mac = self.mac()
        self.assertIs(mac.part.snapshot().sharing, True)
        self.assertIsNone(mac.part.snapshot().store_problem)

    def test_a_failed_save_is_surfaced_and_retried(self):
        mac = self.mac()
        with mock.patch.object(store, "save", side_effect=OSError(28, "No space left on device")):
            mac.part.set_sharing(False)
            mac.flush()
            self.assertEqual(mac.part.snapshot().store_problem, "save_failed")
        mac.advance(6)                                                # the retry
        self.assertIsNone(mac.part.snapshot().store_problem)
        self.assertIs(self.stored()["sharing"], False)

    # R4 #5: held lists are bounded and purged.
    def test_held_lists_are_bounded(self):
        pc = self.pc()
        pc.receive(MAC, wire.devices(mac_list(1)))
        for seed in range(100, 100 + part_module.MAX_HELD + 10):                  # offline machines, flooded by a peer
            pc.receive(MAC, wire.devices(pc_list(seed, in_use=False, ident=machine_id(seed))))
        held = [ident for ident in pc.part.snapshot().machines if ident != PC]
        self.assertEqual(len(held), part_module.MAX_HELD)
        self.assertIn(MAC, held)                                      # an online machine is never evicted
        self.assertIn(machine_id(100 + part_module.MAX_HELD + 9), held)           # the newest one got room

    def test_a_machine_offline_for_the_ttl_is_purged(self):
        pc = self.pc()
        gone = machine_id(77)
        pc.receive(MAC, wire.devices(pc_list(5, in_use=False, ident=gone)))
        self.assertIn(gone, pc.part.snapshot().machines)
        self.clock.advance(part_module.HELD_TTL + 10)
        pc.flush()
        self.assertNotIn(gone, pc.part.snapshot().machines)
        pc.part.shutdown()
        self.assertNotIn(gone, json.dumps(self.stored()))

    # R4 #1: an own list far ahead of this clock does not move our revs.
    def test_own_rev_never_jumps_far_ahead(self):
        mac = self.mac()
        rev = mac.part._own_rev
        forged = wire.devices(pc_list(1))
        forged["data"].update(machine=MAC, rev=int((self.clock.wall() + DAY - 60) * 1000))
        mac.receive(PC, forged)
        self.assertLess(mac.part._own_rev, (self.clock.wall() + 3600) * 1000)
        self.assertGreaterEqual(mac.part._own_rev, rev)

    # R4 #1 (a): the sink enforces its own route view on a stream it took.
    def test_sink_closes_a_stream_its_route_no_longer_names(self):
        pc = self.pc()
        entry = model.CameraEntry(MAC, None, model.Stamp(10, MAC))
        pc.receive(MAC, wire.route(model.CameraRoute(entry, model.QUALITY_DEFAULT, {})))
        pc.receive(MAC, wire.open_stream(77, "0a0b0c0d", wire.codec(1280, 720, 2_500_000),
                                         "A" * 43 + "=", None, model.Stamp(10, MAC)))
        self.assertEqual(pc.part.stream_ids(), frozenset({77}))
        pc.part.set_camera(None, None)
        pc.flush()
        self.assertEqual(pc.part.stream_ids(), frozenset())
        self.assertEqual(pc.sent(wire.CLOSE, MAC)[-1]["data"], {"stream": 77, "reason": "route"})
        self.assertEqual(pc.commands("stop")[-1]["stream"], 77)

    def test_sink_refuses_an_open_for_another_camera(self):
        pc = self.pc()
        chosen = model.CameraRef("ext-1", "Studio Display Camera")
        entry = model.CameraEntry(MAC, chosen, model.Stamp(10, MAC))
        pc.receive(MAC, wire.route(model.CameraRoute(entry, model.QUALITY_DEFAULT, {})))
        other = model.CameraRef("builtin", "MacBook Air Camera")
        pc.receive(MAC, wire.open_stream(78, "0a0b0c0d", wire.codec(1280, 720, 2_500_000),
                                         "A" * 43 + "=", other, model.Stamp(9, MAC)))
        self.assertEqual(pc.sent(wire.OPENED, MAC)[-1]["data"]["error"], "not_source")
        pc.receive(MAC, wire.open_stream(79, "0a0b0c0d", wire.codec(1280, 720, 2_500_000),
                                         "A" * 43 + "=", chosen, model.Stamp(10, MAC)))
        self.assertEqual(pc.sent(wire.OPENED, MAC)[-1]["data"]["ok"], True)


if __name__ == "__main__":
    unittest.main()
