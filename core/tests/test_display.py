# SPDX-License-Identifier: GPL-3.0-only
"""Shared displays (core/display): the model, the part with a fake backend, the menus, and two real
AudioServices exchanging `display_*` over an in-memory link."""

import json
import logging
import shutil
import tempfile
import unittest

from core import feature_flags
from core import link as link_module
from core.audio.service import AudioService
from core.display import model, view
from core.display.part import DisplayPart, FILE_NAME
from core.tests.audio_cluster_harness import Clock, machine_id

MAC = machine_id(1)
PC = machine_id(2)
GN10 = "HKC2716"
MSI = "MSI3BA0"


class FakeBackend:
    """Displays a machine has; `set_on` flips them and records the calls."""

    def __init__(self, *displays):
        self.state = {key: [name, on] for key, name, on in displays}
        self.calls = []
        self.fail = False

    def displays(self):
        return [model.Display(key, name, on) for key, (name, on) in sorted(self.state.items())]

    def set_on(self, key, on):
        self.calls.append((key, on))
        if self.fail or key not in self.state:
            return False
        self.state[key][1] = on
        return True


class Ctx:
    def __init__(self, own, clock, names=None):
        self.own = own
        self.clock = clock
        self.wall = clock.wall
        self.logger = logging.getLogger("test.display")
        self.posted = []
        self.wake = None
        self.changes = 0
        self.names = names or {MAC: "MacBook", PC: "lebig"}

    def post(self, peer, message):
        self.posted.append((peer, message))
        return True

    def machine_name(self, machine):
        return self.names.get(machine, machine)

    def changed(self):
        self.changes += 1

    def schedule(self, at):
        self.wake = at


class ModelTests(unittest.TestCase):
    def test_keys(self):
        self.assertEqual(model.pnp_letters(0x2163), "HKC")
        self.assertEqual(model.key_from_numbers(0x2163, 0x2716), GN10)
        self.assertEqual(model.key_from_device_path(r"\\?\DISPLAY#HKC2716#7&1bd4b3b4&0&UID260#{e6f0}"), GN10)
        self.assertEqual(model.key_from_device_path(r"\\?\DISPLAY#MSI3BA0#7&1"), MSI)
        self.assertIsNone(model.key_from_device_path("garbage"))
        self.assertIsNone(model.key_from_numbers(0, 0))

    def test_route_merge_by_stamp(self):
        old = {GN10: model.Owner(MAC, 100, MAC)}
        new = {GN10: model.Owner(PC, 200, PC)}
        merged, learnt, lacked = model.merge(old, new)
        self.assertEqual(merged[GN10].owner, PC)
        self.assertTrue(learnt)
        self.assertFalse(lacked)
        merged, learnt, lacked = model.merge(new, old)
        self.assertEqual(merged[GN10].owner, PC)
        self.assertFalse(learnt)
        self.assertTrue(lacked)

    def test_choose_is_always_newer(self):
        route = {GN10: model.Owner(PC, 5000, PC)}
        chosen = model.choose(route, GN10, MAC, MAC, now_ms=10)    # a clock behind the held stamp
        self.assertGreater(chosen[GN10].set_at, 5000)
        self.assertEqual(chosen[GN10].owner, MAC)

    def test_messages_round_trip(self):
        displays = (model.Display(GN10, "GN10", True),)
        self.assertEqual(model.read_state(model.state_msg(PC, 3, displays)), (PC, 3, displays))
        route = {GN10: model.Owner(PC, 123, MAC)}
        self.assertEqual(model.read_route(model.route_msg(route)), route)
        self.assertIsNone(model.read_route({"type": model.ROUTE, "data": {"owners": {"bad": {}}}}))
        self.assertIsNone(model.read_state({"type": model.STATE, "data": {"machine": PC, "rev": -1, "displays": []}}))

    def test_want_never_leaves_a_hidden_screen(self):
        route = {GN10: model.Owner(PC, 1, MAC)}
        self.assertEqual(model.want(MAC, GN10, {}, {}).on, True)
        self.assertEqual(model.want(MAC, GN10, route, {PC: frozenset({GN10})}).on, False)
        self.assertEqual(model.want(PC, GN10, route, {MAC: frozenset({GN10})}).on, True)
        # The owner is away: the display stays off, it would be a screen nobody sees.
        self.assertEqual(model.want(MAC, GN10, route, {}).on, False)
        # The owner says it does not have that display: back on here.
        self.assertEqual(model.want(MAC, GN10, route, {PC: frozenset({MSI})}).on, True)
        self.assertEqual(model.want(MAC, GN10, {GN10: model.Owner(None, 2, MAC)}, {PC: frozenset({GN10})}).on, True)

    def test_display_types_ride_the_audio_capability(self):
        self.assertTrue(all(link_module._cap_for(kind) == "audio" for kind in model.TYPES))
        self.assertIs(feature_flags.SHARED_DISPLAY, True)


class PartTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.folder, True)
        self.clock = Clock()

    def make(self, own, backend):
        ctx = Ctx(own, self.clock)
        part = DisplayPart(ctx, self.folder, backend)
        part.start()
        return part, ctx

    def test_handing_the_display_to_the_peer_turns_it_off_here(self):
        backend = FakeBackend((GN10, "GN10", True))
        part, ctx = self.make(MAC, backend)
        part.peer_ready(PC)
        part.link_message(PC, model.state_msg(PC, 1, [model.Display(GN10, "GN10", True), model.Display(MSI, "MSI", True)]))
        self.assertEqual(backend.calls, [])
        shared = part.snapshot().shared
        self.assertEqual([v.key for v in shared], [GN10])
        self.assertEqual([c.machine for c in shared[0].choices], [MAC, PC, None])
        part.set_owner(GN10, PC)
        self.assertEqual(backend.calls, [(GN10, False)])
        self.assertTrue(any(peer == PC and m["type"] == model.ROUTE for peer, m in ctx.posted))
        saved = json.loads((self.folder + "/" + FILE_NAME and open(f"{self.folder}/{FILE_NAME}").read()))
        self.assertEqual(saved["turned_off"], [GN10])
        # Back to this Mac: on again.
        part.set_owner(GN10, MAC)
        self.assertEqual(backend.calls[-1], (GN10, True))

    def test_owner_going_away_keeps_the_display_off(self):
        backend = FakeBackend((GN10, "GN10", True))
        part, _ctx = self.make(MAC, backend)
        part.peer_ready(PC)
        part.link_message(PC, model.state_msg(PC, 1, [model.Display(GN10, "GN10", True)]))
        part.set_owner(GN10, PC)
        part.link_down(PC)
        self.clock.advance(3600)
        part.tick()
        self.assertEqual(backend.calls, [(GN10, False)])
        self.assertFalse(backend.state[GN10][1])
        # The snapshot still offers the choice, so the person can take it back without the PC.
        self.assertEqual([v.key for v in part.snapshot().shared], [GN10])

    def test_a_route_from_the_peer_is_applied_and_forwarded(self):
        backend = FakeBackend((GN10, "GN10", True))
        part, ctx = self.make(PC, backend)
        part.peer_ready(MAC)
        part.link_message(MAC, model.state_msg(MAC, 1, [model.Display(GN10, "GN10", True)]))
        part.link_message(MAC, model.route_msg({GN10: model.Owner(MAC, 10, MAC)}))
        self.assertEqual(backend.calls, [(GN10, False)])
        # A stale route from the peer is answered with ours.
        ctx.posted.clear()
        part.link_message(MAC, model.route_msg({}))
        self.assertEqual([m["type"] for _p, m in ctx.posted], [model.ROUTE])

    def test_a_display_turned_off_by_someone_else_is_left_alone(self):
        backend = FakeBackend((GN10, "GN10", False))
        part, _ctx = self.make(MAC, backend)
        self.assertEqual(backend.calls, [])

    def test_state_only_from_the_machine_itself(self):
        backend = FakeBackend((GN10, "GN10", True))
        part, _ctx = self.make(MAC, backend)
        part.peer_ready(PC)
        part.link_message(PC, model.state_msg(machine_id(9), 1, [model.Display(GN10, "GN10", True)]))
        self.assertEqual(part.snapshot().shared, ())

    def test_failure_backs_off(self):
        backend = FakeBackend((GN10, "GN10", True))
        backend.fail = True
        part, _ctx = self.make(MAC, backend)
        part.peer_ready(PC)
        part.link_message(PC, model.state_msg(PC, 1, [model.Display(GN10, "GN10", True)]))
        part.set_owner(GN10, PC)
        self.clock.advance(5)
        part.tick()
        self.assertEqual(backend.calls, [(GN10, False)])
        self.clock.advance(30)
        part.tick()
        self.assertEqual(backend.calls, [(GN10, False), (GN10, False)])

    def test_restart_remembers_the_route(self):
        backend = FakeBackend((GN10, "GN10", True))
        part, _ctx = self.make(MAC, backend)
        part.peer_ready(PC)
        part.link_message(PC, model.state_msg(PC, 1, [model.Display(GN10, "GN10", True)]))
        part.set_owner(GN10, PC)
        part.shutdown()
        backend.state[GN10][1] = True       # macOS brought it back (a restart, a replugged cable)
        again, _ctx = self.make(MAC, backend)
        self.assertEqual(backend.calls[-1], (GN10, False))

    def test_menus(self):
        backend = FakeBackend((GN10, "GN10", True))
        part, _ctx = self.make(MAC, backend)
        part.peer_ready(PC)
        part.link_message(PC, model.state_msg(PC, 1, [model.Display(GN10, "GN10", True)]))
        part.set_owner(GN10, PC)
        menus = view.menus(part.snapshot(), "This Mac")
        self.assertEqual(menus[0].title, "Screen GN10: lebig")
        self.assertEqual([(c.label, c.checked) for c in menus[0].choices],
                         [("This Mac", False), ("lebig", True), ("Not managed", False)])
        self.assertEqual(view.menus(None, "This Mac"), ())


class MemoryHost:
    def __init__(self, own, name, platform):
        self.own, self.name, self._platform = own, name, platform
        self.other = None

    def own_id(self):
        return self.own

    def own_name(self):
        return self.name

    def platform(self):
        return self._platform

    def post(self, peer, message):
        self.other.link_message(self.own, json.loads(json.dumps(message)))
        return True

    def outbox_depth(self, peer):
        return 0

    def peer_host(self, peer):
        return None

    def peer_name(self, peer):
        return {MAC: "MacBook", PC: "lebig"}[peer]


class ServiceTests(unittest.TestCase):
    def side(self, own, name, platform, backend):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        host = MemoryHost(own, name, platform)
        service = AudioService(host, folder, None, display_backend=backend)
        service.start(run_thread=False)
        return host, service

    def test_two_services_hand_the_display_over(self):
        mac_backend = FakeBackend((GN10, "GN10", True))
        pc_backend = FakeBackend((GN10, "GN10", True), (MSI, "MSI G24C6", True))
        mac_host, mac = self.side(MAC, "MacBook", "macos", mac_backend)
        pc_host, pc = self.side(PC, "lebig", "windows", pc_backend)
        mac_host.other, pc_host.other = pc, mac
        mac.link_up(PC, frozenset({"audio"}))
        pc.link_up(MAC, frozenset({"audio"}))
        for _ in range(4):
            mac.pump()
            pc.pump()
        self.assertEqual([v.key for v in mac.display_snapshot().shared], [GN10])
        self.assertEqual([v.key for v in pc.display_snapshot().shared], [GN10])
        mac.set_display_owner(GN10, PC)
        for _ in range(4):
            mac.pump()
            pc.pump()
        self.assertEqual(mac_backend.calls, [(GN10, False)])
        self.assertEqual(pc_backend.calls, [])
        self.assertEqual(pc.display_snapshot().shared[0].owner, PC)
        pc.set_display_owner(GN10, MAC)
        for _ in range(4):
            mac.pump()
            pc.pump()
        self.assertEqual(mac_backend.calls, [(GN10, False), (GN10, True)])
        self.assertEqual(pc_backend.calls, [(GN10, False)])
        self.assertTrue(pc_backend.state[MSI][1])

    def test_without_a_backend_no_feature_word(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder, True)
        service = AudioService(MemoryHost(MAC, "MacBook", "macos"), folder, None)
        self.assertIsNone(service.display_snapshot())
        self.assertNotIn("display1", service._hello()["data"]["features"])


if __name__ == "__main__":
    unittest.main()
