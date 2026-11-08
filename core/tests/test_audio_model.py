# SPDX-License-Identifier: GPL-3.0-only
"""The audio snapshot types and their pure rules: route merge, device lists, reachability, resolve."""

import dataclasses
import itertools
import random
import unittest

from core import protocol
from core.audio import model
from core.audio.model import (AudioSnapshot, Device, DeviceRef, FormEntry, MachineAudio, MicRoute,
                              OutputRoute, Route, RouteStatus, Stamp, VirtualMic)


def mid(seed):
    return protocol.id_text(bytes([seed]) * 16)


A, B, C, D = mid(1), mid(2), mid(3), mid(4)


def machine(ident, rev=1, links=(), inputs=(), outputs=(), **extra):
    values = dict(id=ident, name=ident[:4], platform="macos", rev=rev, status="ok", inputs=tuple(inputs),
                  outputs=tuple(outputs), default_input=None, default_output=None,
                  virtual_mic=VirtualMic("beamer", True, False), features=frozenset(), links=frozenset(links),
                  helper="0.1.0")
    values.update(extra)
    return MachineAudio(**values)


def dev(key, name, kind="input"):
    return Device(key, name, kind, "usb", 1, "microphone", False)


class ShapeTests(unittest.TestCase):
    def test_fields_match_the_contract(self):
        names = lambda cls: [f.name for f in dataclasses.fields(cls)]
        self.assertEqual(names(DeviceRef), ["key", "name"])
        self.assertEqual(names(Device), ["key", "name", "kind", "transport", "channels", "form", "default"])
        self.assertEqual(names(VirtualMic), ["kind", "present", "in_use"])
        self.assertEqual(names(MachineAudio), ["id", "name", "platform", "rev", "status", "inputs", "outputs",
                                               "default_input", "default_output", "virtual_mic", "features",
                                               "links", "helper"])
        self.assertEqual(names(Stamp), ["set_at", "by"])
        self.assertEqual(names(MicRoute), ["machine", "device", "stamp"])
        self.assertEqual(names(OutputRoute), ["to", "device", "stamp"])
        self.assertEqual(names(FormEntry), ["form", "stamp"])
        self.assertEqual(names(Route), ["mic", "outputs", "forms"])
        self.assertEqual(names(RouteStatus), ["state", "reason", "params"])
        self.assertEqual(names(model.StreamView), ["stream", "kind", "source", "target", "state", "path",
                                                   "jb_ms", "loss_pct", "rtt_ms"])
        self.assertEqual(names(AudioSnapshot), ["own", "machines", "online", "direct", "route", "mic_status",
                                                "output_status", "streams", "helper", "permission", "aec_mode",
                                                "aec_active"])

    def test_frozen_and_mapping_defaults_never_shared(self):
        one, two = RouteStatus("off"), RouteStatus("off")
        self.assertEqual(one.params, {})
        self.assertIsNot(one.params, two.params)
        self.assertIsNot(model.empty_route().outputs, model.empty_route().outputs)
        with self.assertRaises(dataclasses.FrozenInstanceError):
            one.state = "local"
        snap = model.empty_snapshot(A)
        self.assertEqual(snap.online, frozenset([A]))
        self.assertEqual(snap.route.mic, model.MIC_OFF)


def random_route(rng, by_choices):
    def stamp():
        return Stamp(rng.randint(0, 5), rng.choice(by_choices))
    mic = MicRoute(rng.choice([None, A, B]), rng.choice([None, DeviceRef("k1", "Mic")]), stamp())
    outputs = {m: OutputRoute(rng.choice([None, A, B, C]), None, stamp())
               for m in rng.sample([A, B, C], rng.randint(0, 3))}
    forms = {(m, "k"): FormEntry(rng.choice([None, "headset"]), stamp()) for m in rng.sample([A, B], rng.randint(0, 2))}
    return Route(mic, outputs, forms)


class MergeTests(unittest.TestCase):
    def test_per_entry_newer_wins_and_both_concurrent_edits_survive(self):
        held = Route(model.MIC_OFF, {A: OutputRoute(B, None, Stamp(10, A))}, {})
        incoming = Route(model.MIC_OFF, {B: OutputRoute(A, None, Stamp(11, B))}, {})
        merged, changed, answer = model.merge_route(held, incoming)
        self.assertTrue(changed)
        self.assertTrue(answer)   # the sender lacks A's output
        self.assertEqual(merged.outputs[A].to, B)
        self.assertEqual(merged.outputs[B].to, A)

    def test_tie_goes_to_the_larger_by(self):
        held = Route(MicRoute(A, None, Stamp(10, A)))
        incoming = Route(MicRoute(B, None, Stamp(10, B)))
        merged, changed, answer = model.merge_route(held, incoming)
        self.assertEqual(merged.mic.machine, B)
        self.assertTrue(changed)
        self.assertFalse(answer)
        merged, changed, answer = model.merge_route(incoming, held)
        self.assertEqual(merged.mic.machine, B)
        self.assertFalse(changed)
        self.assertTrue(answer)

    def test_older_incoming_changes_nothing_and_asks_for_an_answer(self):
        held = Route(MicRoute(A, None, Stamp(20, A)))
        merged, changed, answer = model.merge_route(held, Route(MicRoute(None, None, Stamp(10, B))))
        self.assertEqual(merged, held)
        self.assertFalse(changed)
        self.assertTrue(answer)

    def test_same_route_is_quiet(self):
        held = Route(MicRoute(A, None, Stamp(20, A)), {A: OutputRoute(B, None, Stamp(3, B))})
        self.assertEqual(model.merge_route(held, held), (held, False, False))
        self.assertEqual(model.merge_route(model.empty_route(), model.empty_route())[1:], (False, False))

    def test_commutative_associative_idempotent(self):
        rng = random.Random(7)
        for _ in range(300):
            x, y, z = (random_route(rng, [A, B, C]) for _ in range(3))
            xy = model.merge_route(x, y)[0]
            self.assertEqual(xy, model.merge_route(y, x)[0])
            self.assertEqual(model.merge_route(xy, z)[0], model.merge_route(x, model.merge_route(y, z)[0])[0])
            self.assertEqual(model.merge_route(xy, xy), (xy, False, False))
            # The answer goes back to the sender: then both ends hold the same route.
            merged, _changed, answer = model.merge_route(x, y)
            back = model.merge_route(y, merged)[0] if answer else model.merge_route(y, x)[0]
            self.assertEqual(back, merged)

    def test_forms_trimmed_oldest_null_first(self):
        forms = {(A, f"k{i}"): FormEntry(None if i < 10 else "headset", Stamp(100 + i, A)) for i in range(70)}
        route = Route(model.MIC_OFF, {}, {})
        merged, changed, _answer = model.merge_route(route, Route(model.MIC_OFF, {}, forms))
        self.assertEqual(len(merged.forms), model.MAX_FORMS)
        self.assertTrue(changed)
        gone = set(forms) - set(merged.forms)
        self.assertEqual(gone, {(A, f"k{i}") for i in range(6)})

    def test_next_stamp_beats_everything_held(self):
        route = Route(MicRoute(A, None, Stamp(500, B)), {A: OutputRoute(None, None, Stamp(900, A))})
        self.assertEqual(model.next_stamp(route, A, 100), Stamp(901, A))
        self.assertEqual(model.next_stamp(route, A, 2000), Stamp(2000, A))

    def test_round_trip_through_data(self):
        route = Route(MicRoute(A, DeviceRef("k", "Mic"), Stamp(5, A)),
                      {B: OutputRoute(A, DeviceRef("o", "Out"), Stamp(6, B))},
                      {(A, "k 2"): FormEntry("headset", Stamp(7, C))})
        self.assertEqual(model.route_from(model.route_data(route)), route)
        self.assertEqual(model.route_from({}), model.empty_route())


class DeviceListTests(unittest.TestCase):
    def test_accept_by_rev_or_straight_from_its_machine(self):
        held = machine(A, rev=10)
        self.assertTrue(model.accept_devices(None, held, direct_from=False))
        self.assertTrue(model.accept_devices(held, machine(A, rev=11), direct_from=False))
        self.assertFalse(model.accept_devices(held, machine(A, rev=10), direct_from=False))
        self.assertFalse(model.accept_devices(held, machine(A, rev=9), direct_from=False))
        self.assertTrue(model.accept_devices(held, machine(A, rev=9), direct_from=True))
        self.assertFalse(model.accept_devices(held, machine(A, rev=10), direct_from=True))

    def test_machine_round_trip(self):
        one = machine(A, rev=3, links=[B], inputs=[Device("i", "Mic", "input", "usb", 1, "microphone", True)],
                      outputs=[Device("o", "Out", "output", "builtin", 2, "speakers", False)],
                      default_input="i", features=frozenset({"aec"}))
        self.assertEqual(model.machine_from(model.machine_data(one)), one)


class OnlineTests(unittest.TestCase):
    def test_direct_peers_and_mutual_links_beyond(self):
        machines = {B: machine(B, links=[A, C]), C: machine(C, links=[B, D]), D: machine(D, links=[])}
        self.assertEqual(model.online(A, frozenset([B]), machines), frozenset([A, B, C]))

    def test_one_sided_link_does_not_count(self):
        machines = {B: machine(B, links=[A, C]), C: machine(C, links=[])}
        self.assertEqual(model.online(A, frozenset([B]), machines), frozenset([A, B]))

    def test_alone_and_cut_off(self):
        machines = {B: machine(B, links=[C]), C: machine(C, links=[B])}
        self.assertEqual(model.online(A, frozenset(), machines), frozenset([A]))


class ResolveTests(unittest.TestCase):
    def test_key_then_unique_name(self):
        devices = [dev("k1", "USB Mic"), dev("k2", "Built-in"), dev("k3", "Twin"), dev("k4", "Twin")]
        self.assertEqual(model.resolve(DeviceRef("k2", "x"), devices).key, "k2")
        self.assertEqual(model.resolve(DeviceRef("gone", "USB Mic"), devices).key, "k1")
        self.assertIsNone(model.resolve(DeviceRef("gone", "Twin"), devices))
        self.assertIsNone(model.resolve(DeviceRef("gone", "Nope"), devices))
        self.assertIsNone(model.resolve(None, devices))


class ExhaustiveMergeTests(unittest.TestCase):
    def test_every_order_of_three_routes_converges(self):
        rng = random.Random(11)
        routes = [random_route(rng, [A, B]) for _ in range(3)]
        results = set()
        for order in itertools.permutations(routes):
            held = model.empty_route()
            for route in order:
                held = model.merge_route(held, route)[0]
            results.add(repr(model.route_data(held)))
        self.assertEqual(len(results), 1)


if __name__ == "__main__":
    unittest.main()
