# SPDX-License-Identifier: GPL-3.0-only
"""Several machines on in-memory links: routes and device lists converge, a peer without `audio`
gets nothing, routes come back after a reconnect or a restart, and a non-direct pair is handled."""

import random
import unittest

from core.audio import model
from core.audio.model import DeviceRef
from core.tests.audio_cluster_harness import Net


def converged(nodes):
    """Every pair of services holds the same route, the same device lists and the same online set."""
    first = nodes[0].snapshot()
    for node in nodes[1:]:
        snap = node.snapshot()
        if snap.route != first.route or snap.online != first.online:
            return False
        if {ident: m.rev for ident, m in snap.machines.items()} != {ident: m.rev for ident, m in first.machines.items()}:
            return False
    return True


def entries(route):
    result = {("mic",): route.mic}
    result.update({("out", machine): entry for machine, entry in route.outputs.items()})
    result.update({("form",) + name: entry for name, entry in route.forms.items()})
    return result


class ConvergenceTests(unittest.TestCase):
    def run_sequence(self, seed, with_fourth=False):
        rng = random.Random(seed)
        net = Net()
        try:
            a = net.add(1, "mac", "macos")
            b = net.add(2, "lebig", "windows")
            c = net.add(3, "asusrog", "linux", audio=False)
            nodes = [a, b, c]
            if with_fourth:
                nodes.append(net.add(4, "air2", "macos"))
            audio = [node for node in nodes if node.audio]
            pairs = [(x, y) for i, x in enumerate(nodes) for y in nodes[i + 1:]]
            newest = {}
            for _step in range(rng.randint(5, 30)):
                action = rng.random()
                if action < 0.35:
                    node = rng.choice(audio)
                    what = rng.random()
                    if what < 0.4:
                        machine = rng.choice([None] + [n.id for n in nodes])
                        device = rng.choice([None, DeviceRef("BuiltInMicrophoneDevice", "MacBook Air Microphone"),
                                             DeviceRef("{mic-wave3}", "Wave:3 Mic In")])
                        node.service.set_mic(machine, device)
                    elif what < 0.8:
                        owner = rng.choice(audio)
                        to = rng.choice([None] + [n.id for n in nodes if n is not owner])
                        node.service.set_output(owner.id, to, None)
                    else:
                        owner = rng.choice(audio)
                        node.service.set_form(owner.id, rng.choice(["BuiltInSpeakerDevice", "{out-g733}"]),
                                              rng.choice([None, "headset", "speakers"]))
                    node.service.pump()
                    for key, entry in entries(node.snapshot().route).items():
                        held = newest.get(key)
                        if held is None or model.newer(entry.stamp, held.stamp):
                            newest[key] = entry
                elif action < 0.6:
                    x, y = rng.choice(pairs)
                    if net.up(x.id, y.id):
                        net.unlink(x, y)
                    else:
                        net.link(x, y)
                elif action < 0.7:
                    rng.choice(audio).helper.set_in_use(rng.random() < 0.5)
                elif action < 0.75:
                    node = rng.choice(audio)
                    if rng.random() < 0.5:
                        node.helper.set_devices(inputs=[])
                    else:
                        node.helper.set_devices(inputs=[{"key": "usb-mic", "name": "USB Mic", "transport": "usb",
                                                         "channels": 1, "form": "microphone"}])
                elif action < 0.8:
                    node = rng.choice(audio)
                    node.helper.crash(restart=rng.random() < 0.8)
                    if node.helper.state != "ok":
                        node.helper.ready()
                else:
                    for _ in range(rng.randint(0, 3)):
                        net.round()
                    net.clock.advance(rng.random() * 3)
            for x, y in pairs:
                net.link(x, y)
            rounds = net.settle(limit=200)
            self.assertLess(rounds, 50, f"seed {seed}: {rounds} rounds")
            self.assertFalse(net.in_flight, f"seed {seed}: never quiet")
            self.assertTrue(converged(audio), f"seed {seed}: not converged")
            final = entries(audio[0].snapshot().route)
            for key, entry in newest.items():
                if key[0] == "form" and key[1:] not in audio[0].snapshot().route.forms:
                    continue
                self.assertEqual(final[key], entry, f"seed {seed}: {key} lost its newest edit")
            self.assertEqual(net.violations, [], f"seed {seed}: audio_* sent to a peer without the cap")
            for node in audio:
                snap = node.snapshot()
                self.assertEqual(set(snap.machines), {n.id for n in audio})
                # What the service shows running is exactly what its helper runs, and a redirected
                # output that says "streaming" has its stream.
                running = {s.stream for s in snap.streams if s.state == "running"}
                self.assertEqual(set(node.helper.engine.streams), running, f"seed {seed}")
                status = snap.output_status.get(node.id)
                if status is not None and status.state == "streaming":
                    self.assertIn(("system", status.params["to"]),
                                  {(s.kind, s.target) for s in snap.streams if s.source == node.id}, f"seed {seed}")
            return rounds
        finally:
            net.close()

    def test_two_hundred_seeded_sequences_with_a_peer_without_audio(self):
        worst = max(self.run_sequence(seed) for seed in range(200))
        self.assertLess(worst, 50)

    def test_sequences_with_three_audio_machines(self):
        for seed in range(1000, 1060):
            self.run_sequence(seed, with_fourth=True)


class ScenarioTests(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)

    def test_old_peer_receives_nothing(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        c = self.net.add(3, "asusrog", "linux", audio=False)
        self.net.link(a, b)
        self.net.link(b, c)
        self.net.link(a, c)
        a.service.set_mic(a.id, None)
        b.service.set_output(b.id, a.id, None)
        self.net.settle()
        self.assertEqual(c.received, [])
        self.assertEqual(self.net.violations, [])
        self.assertNotIn(c.id, a.snapshot().machines)
        self.assertEqual(a.snapshot().direct, frozenset([b.id]))

    def test_reconnect_reapplies_the_route(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        self.net.link(a, b)
        b.service.set_output(b.id, a.id, None)
        self.net.settle()
        self.assertEqual([s.kind for s in b.snapshot().streams], ["system"])
        self.net.unlink(a, b)
        self.net.settle()
        self.assertEqual(b.snapshot().streams, ())
        self.assertEqual(b.snapshot().output_status[b.id].state, "waiting_machine")
        sends = len(b.helper.commands("send"))
        self.assertEqual(b.helper.engine.streams, {})
        self.net.link(a, b)
        self.net.settle()
        self.assertEqual([(s.kind, s.state) for s in b.snapshot().streams], [("system", "running")])
        self.assertEqual(len(b.helper.commands("send")), sends + 1)

    def test_route_survives_a_restart(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        self.net.link(a, b)
        a.service.set_mic(a.id, None)
        b.helper.set_in_use(True)
        self.net.settle()
        self.net.clock.advance(1)
        self.net.pump()
        self.net.unlink(a, b)
        a.restart()
        b.restart()
        b.helper.set_in_use(True)
        self.assertEqual(a.snapshot().route.mic.machine, a.id)
        self.net.link(a, b)
        self.net.settle()
        self.assertEqual([(s.kind, s.state) for s in a.snapshot().streams], [("mic", "running")])

    def test_unplug_and_replug(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        self.net.link(a, b)
        a.service.set_output(a.id, b.id, DeviceRef("{out-hdmi}", "LG HDR 4K"))
        self.net.settle()
        self.assertEqual(a.snapshot().output_status[a.id].state, "streaming")
        outputs = b.helper.engine.devices["outputs"]
        b.helper.set_devices(outputs=outputs[:1])
        self.net.settle()
        self.assertEqual(a.snapshot().output_status[a.id].state, "waiting_device")
        self.assertEqual(a.snapshot().streams, ())
        moved = dict(outputs[1], key="{out-hdmi-port2}")
        b.helper.set_devices(outputs=[outputs[0], moved])
        self.net.settle()
        self.assertEqual(a.snapshot().output_status[a.id].state, "streaming")
        recv = b.helper.commands("recv")[-1]
        self.assertEqual(recv["sink"], {"device": "{out-hdmi-port2}"})

    def test_three_machines_with_a_non_direct_pair(self):
        a = self.net.add(1, "mac", "macos")
        b = self.net.add(2, "lebig", "windows")
        d = self.net.add(4, "air2", "macos")
        self.net.link(a, b)
        self.net.link(b, d)
        self.net.settle()
        snap = a.snapshot()
        self.assertEqual(snap.online, frozenset([a.id, b.id, d.id]))
        self.assertEqual(snap.direct, frozenset([b.id]))
        self.assertIn(d.id, snap.machines)
        # A sets D's sound to play on B: D sends it, A only edited the route.
        a.service.set_output(d.id, b.id, None)
        self.net.settle()
        self.assertEqual([(s.kind, s.target) for s in d.snapshot().streams], [("system", b.id)])
        self.assertEqual(a.snapshot().output_status[d.id].state, "streaming")
        # A's mic for D: not direct, so unreachable.
        a.service.set_mic(a.id, None)
        d.helper.set_in_use(True)
        self.net.settle()
        self.assertEqual(a.snapshot().mic_status.state, "unreachable")
        # B leaves: D goes offline for A, with no timer.
        self.net.unlink(b, d)
        self.net.settle()
        self.assertEqual(a.snapshot().online, frozenset([a.id, b.id]))
        self.assertIn(d.id, a.snapshot().machines)


if __name__ == "__main__":
    unittest.main()
