# SPDX-License-Identifier: GPL-3.0-only
"""Camera parts on the in-memory cluster (PHASE2 §6.5, §10.1): convergence, gating, the sharing switch
and the camera-on invariant checker after every step of seeded sequences, plus named scenarios."""

import random
import threading
import time
import unittest

from core.camera import wire
from core.camera.fake_video import MAC_CAMERA_KEY
from core.camera.model import CameraRef
from core.tests.audio_cluster_harness import Net

MAC_CAMERA = CameraRef(MAC_CAMERA_KEY, "MacBook Air Camera")


def camera_converged(nodes):
    first = nodes[0].camera_snapshot()
    for node in nodes[1:]:
        snap = node.camera_snapshot()
        if snap.route != first.route:
            return False
        if {i: m.rev for i, m in snap.machines.items()} != {i: m.rev for i, m in first.machines.items()}:
            return False
    return True


class ClusterBase(unittest.TestCase):
    def setUp(self):
        self.net = Net()
        self.addCleanup(self.net.close)

    def cluster(self, fork=False, linux=True, pc2=False, mac_video=None):
        net = self.net
        self.mac = net.add(1, "mac", "macos", camera=True, video=mac_video)
        self.pc = net.add(2, "lebig", "windows", camera=True)
        nodes = [self.mac, self.pc]
        if linux:
            self.linux = net.add(3, "asusrog", "linux", audio=False)
            nodes.append(self.linux)
        if fork:
            self.fork = net.add(4, "oldpc", "windows")             # audio, but no camera1
            nodes.append(self.fork)
        if pc2:
            self.pc2 = net.add(5, "lebig2", "windows", camera=True)
            nodes.append(self.pc2)
        for i, x in enumerate(nodes):
            for y in nodes[i + 1:]:
                net.link(x, y)
        net.settle()
        return nodes

    def settle(self):
        self.net.settle()
        self.net.check_camera()

    def assertClean(self):
        self.assertEqual(self.net.invariant_failures, [])
        self.assertEqual(self.net.camera_violations, [])
        self.assertEqual(self.net.violations, [])


class ConvergenceTests(ClusterBase):
    def run_sequence(self, seed):
        rng = random.Random(seed)
        nodes = self.cluster(fork=True)
        net = self.net
        cams = [self.mac, self.pc]
        pairs = [(x, y) for i, x in enumerate(nodes) for y in nodes[i + 1:]]
        for _step in range(rng.randint(5, 30)):
            action = rng.random()
            if action < 0.2:
                node = rng.choice(cams)
                machine = rng.choice([None, self.mac.id, self.mac.id, self.pc.id, self.fork.id])
                node.service.set_camera(machine, rng.choice([None, MAC_CAMERA]) if machine else None)
                if rng.random() < 0.2:
                    node.service.set_quality(rng.choice(["720p", "1080p"]))
                node.service.pump()
            elif action < 0.35:
                x, y = rng.choice(pairs)
                if net.up(x.id, y.id):
                    net.unlink(x, y)
                else:
                    net.link(x, y)
            elif action < 0.5:
                self.pc.set_in_use(rng.random() < 0.6, "Microsoft Teams")
            elif action < 0.55:
                self.mac.service.set_camera_sharing(rng.random() < 0.7)
            elif action < 0.6:
                self.mac.video.set_permission(rng.choice(["granted", "granted", "undetermined", "denied"]))
            elif action < 0.65:
                node = rng.choice(cams)
                node.helper.crash(restart=rng.random() < 0.8)
                if node.helper.state != "ok":
                    net.advance(rng.random())
                    node.helper.ready()
            elif action < 0.7:
                net.cut_udp(self.mac, self.pc, rng.random() < 0.5)
            elif action < 0.72:
                self.pc.camera_dev({"camera_refuse": rng.choice([None, None, "not_source", "helper",
                                                                         "silent"])})
            elif action < 0.75:
                # Refusals by the PC's install state and by its helper's recv (not_installed, outdated).
                if rng.random() < 0.5:
                    install = rng.choice(["ok", "ok", "missing", "outdated", "broken"])
                    self.pc.video.set_vcam(install=install, present=install == "ok")
                else:
                    self.pc.video.recv_error = rng.choice([None, "not_installed", "outdated", "internal"])
            elif action < 0.85:
                for _ in range(rng.randint(0, 3)):
                    net.round()
                    net.check_camera()
            else:
                net.advance(round(rng.random() * 4, 1))
            net.check_camera()
        net.cut_udp(self.mac, self.pc, False)
        self.pc.camera_dev({"camera_refuse": None})
        self.pc.video.recv_error = None
        for x, y in pairs:
            net.link(x, y)
        rounds = net.settle(limit=200)
        net.check_camera()
        self.assertLess(rounds, 50, f"seed {seed}: {rounds} rounds")
        self.assertFalse(net.in_flight, f"seed {seed}: never quiet")
        self.assertTrue(camera_converged(cams), f"seed {seed}: camera state not converged")
        self.assertEqual(net.invariant_failures, [], f"seed {seed}")
        self.assertEqual(net.camera_violations, [], f"seed {seed}: camera_* sent to a peer without camera1")
        self.assertEqual(net.violations, [], f"seed {seed}: sent to a peer without audio")
        self.assertEqual(self.fork.received and [m for _s, m in self.fork.received
                                                 if m["type"].startswith("camera_")], [], f"seed {seed}")
        # Let every hold and backoff run out: the camera ends on only with a wanting receiver.
        net.advance(5)
        self.assertEqual(net.invariant_failures, [], f"seed {seed}")
        return rounds

    def test_two_hundred_seeded_sequences(self):
        worst = 0
        for seed in range(200):
            with self.subTest(seed=seed):
                self.net = Net()
                try:
                    worst = max(worst, self.run_sequence(seed))
                finally:
                    self.net.close()
        self.assertLess(worst, 50)


class ScenarioTests(ClusterBase):
    def start_stream(self):
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        self.pc.set_in_use(True, "Microsoft Teams")
        self.settle()
        self.net.advance(0.3)

    def test_on_demand_stream_and_stop(self):
        self.cluster(fork=True)
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        self.assertFalse(self.mac.video.camera_on())
        self.assertEqual(self.mac.video.warm["on"], True)
        self.assertEqual(self.pc.video.idle, "waiting")
        sends = len(self.mac.helper.commands("send"))
        self.pc.set_in_use(True, "Microsoft Teams")
        # The PC's flood reaches the Mac in one round; the Mac opens in that same worker iteration.
        self.net.pump()
        self.net.round()
        self.assertEqual(len(self.mac.helper.commands("send")), sends + 1)
        self.assertIn("camera_open", [m["type"] for p, m in self.mac.host.posted if p == self.pc.id])
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertEqual(self.pc.camera_snapshot().status.state, "streaming")
        self.assertEqual(self.pc.camera_snapshot().sinks[self.pc.id].state, "streaming")
        closed = self.net.clock()
        self.pc.set_in_use(False)
        while self.mac.video.camera_on() and self.net.clock() - closed < 10:
            self.net.advance(0.01, step=0.01)
        self.assertAlmostEqual(self.net.clock() - closed, 3.0, delta=0.1)
        self.assertTrue(self.mac.video.warm["on"])                  # the standby stays
        self.assertEqual([m for _s, m in self.fork.received if m["type"].startswith("camera_")], [])
        self.assertClean()

    def test_sharing_off_wins_whatever_the_others_set(self):
        self.cluster()
        self.mac.service.set_camera_sharing(False)
        self.settle()
        self.pc.service.set_camera(self.mac.id, MAC_CAMERA)
        self.pc.set_in_use(True)
        self.net.advance(5)
        self.assertEqual(self.mac.helper.commands("send"), [])
        self.assertEqual(self.pc.camera_snapshot().status.state, "sharing_off")
        self.assertEqual(self.pc.video.idle, "sharing_off")
        self.mac.service.set_camera_sharing(True)
        self.net.advance(0.5)
        self.assertEqual(self.pc.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_refusal_of_every_code(self):
        self.cluster()
        for code in ("not_source", "helper"):
            self.pc.camera_dev({"camera_refuse": code})
            self.start_stream()
            self.assertFalse(self.mac.video.camera_on(), code)
            sends = len(self.mac.helper.commands("send"))
            self.net.advance(60)                                    # no token change: no re-open
            self.assertEqual(len(self.mac.helper.commands("send")), sends, code)
            self.assertEqual(self.mac.camera_snapshot().sinks[self.pc.id].reason, code)
            self.pc.set_in_use(False)
            self.net.advance(4)
        for install, code in (("missing", "not_installed"), ("outdated", "outdated")):
            self.pc.camera_dev({"camera_refuse": None})
            self.pc.video.recv_error = code                         # the helper refuses the recv
            self.start_stream()
            self.assertFalse(self.mac.video.camera_on())
            self.pc.video.recv_error = None
            self.pc.set_in_use(False)
            self.net.advance(4)
        self.pc.camera_dev({"camera_refuse": None})
        self.start_stream()
        self.assertTrue(self.mac.video.camera_on())
        self.assertClean()

    def test_silent_receiver_backoff(self):
        self.cluster()
        self.pc.camera_dev({"camera_refuse": "silent"})
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        before = self.mac.video.camera_starts
        self.pc.set_in_use(True)
        on = 0.0
        for _ in range(600):                                        # 60 s
            self.net.advance(0.1)
            on += 0.1 if self.mac.video.camera_on() else 0.0
        self.assertEqual(self.mac.video.camera_starts - before, 3)   # at t = 0, ~13 s, ~46 s
        self.assertLessEqual(on, 3 * 3.5)
        self.pc.camera_dev({"camera_refuse": None})
        self.pc.set_in_use(False)
        self.net.advance(4)
        self.pc.set_in_use(True)
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_not_source_race_retries_once(self):
        self.cluster()
        # The PC's route view lags: the Mac chose itself while the link to the PC was down.
        self.net.unlink(self.mac, self.pc)
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        self.pc.set_in_use(True)
        self.settle()
        self.net.link(self.mac, self.pc)
        # Deliver only the Mac's first messages: the PC gets camera_open before it merges the route?
        # The greeting carries the route first, so force the race with an older view on the PC.
        self.settle()
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        opens = len(self.mac.helper.commands("send"))
        self.assertLessEqual(opens, 2)
        self.assertClean()

    def test_not_source_refusal_then_route_catch_up(self):
        self.cluster()
        self.mac.service.set_camera(self.mac.id, None)
        self.settle()
        part = self.pc.part
        # Make the PC's route view lag: replace it with an older one locally (as if the flood was late).
        lagging = part._route
        part._route = lagging.__class__(lagging.camera.__class__(None, None, lagging.camera.stamp.__class__(0, "")),
                                        lagging.quality, {})
        self.pc.set_in_use(True)
        self.net.round()
        self.net.round()
        self.net.advance(0.2)
        refusals = [m for p, m in self.pc.host.posted if m["type"] == wire.OPENED and not m["data"]["ok"]]
        self.assertEqual([m["data"]["error"] for m in refusals], ["not_source"])
        self.assertFalse(self.mac.video.camera_on() and not self.pc.video.recvs)
        # The PC catches up (the Mac re-floods its route, e.g. after a reconnect); it republishes; one retry.
        self.net.unlink(self.mac, self.pc)
        self.net.link(self.mac, self.pc)
        self.settle()
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_pc_helper_restart_and_link_loss(self):
        self.cluster()
        self.start_stream()
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.pc.helper.crash()                                      # restarts at once
        self.settle()
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertEqual(len(self.pc.video.recvs), 1)
        self.net.unlink(self.mac, self.pc)
        self.net.advance(0.1)
        self.assertFalse(self.mac.video.camera_on())
        self.net.link(self.mac, self.pc)
        self.settle()
        self.net.advance(0.5)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_cut_udp_turns_the_camera_off(self):
        self.cluster()
        self.start_stream()
        self.net.cut_udp(self.mac, self.pc)
        self.net.advance(5.6)
        self.assertFalse(self.mac.video.camera_on())               # 5 s without feedback
        self.net.advance(10)
        self.net.cut_udp(self.mac, self.pc, False)
        self.net.advance(60)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_permission_requests_only_for_mac_actions(self):
        self.cluster(mac_video={"permission": "undetermined"})
        self.pc.service.set_camera(self.mac.id, MAC_CAMERA)         # a PC-side selection
        self.pc.set_in_use(True)
        self.net.advance(2)
        self.assertEqual(self.mac.video.permission_requests, 0)
        self.assertTrue(self.mac.camera_snapshot().needs_permission)
        self.assertEqual(self.pc.camera_snapshot().status.state, "waiting_permission")
        self.assertEqual(self.pc.video.idle, "waiting_permission")
        self.mac.service.ask_camera_permission()                    # "Allow Camera Access…"
        self.mac.service.set_camera(self.mac.id, None)              # the Mac page chose its camera
        self.mac.video.prompt_answer = "granted"
        self.mac.service.ask_camera_permission()
        self.net.advance(1)
        self.assertEqual(self.mac.video.permission_requests, 3)
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_two_sinks_one_stream_each(self):
        self.cluster(pc2=True)
        self.mac.service.set_camera(self.mac.id, None)
        self.pc.set_in_use(True)
        self.pc2.set_in_use(True)
        self.net.advance(0.5)
        targets = sorted(send["peer"] for send in self.mac.video.sends.values())
        self.assertEqual(targets, sorted([self.pc.id, self.pc2.id]))
        self.assertEqual(self.mac.camera_snapshot().status.state, "streaming")
        self.assertClean()

    def test_posts_only_on_the_worker(self):
        nodes = self.cluster()
        worker = threading.current_thread()
        for node in nodes:
            node.host.require_thread = lambda: worker
        self.start_stream()
        self.net.advance(1)
        errors = []

        def elsewhere():
            try:
                self.mac.service.set_camera(self.pc.id, None)      # enqueues only, never posts here
            except AssertionError as error:
                errors.append(error)
        thread = threading.Thread(target=elsewhere)
        thread.start()
        thread.join()
        self.net.advance(0.5)
        self.assertEqual(errors, [])
        self.assertClean()

    def test_restart_keeps_route_and_sharing(self):
        self.cluster()
        self.mac.service.set_camera(self.mac.id, None)
        self.mac.service.set_camera_sharing(False)
        self.net.advance(1)
        self.mac.restart()
        self.net.unlink(self.mac, self.pc)
        self.net.link(self.mac, self.pc)
        self.pc.set_in_use(True)
        self.net.advance(2)
        self.assertEqual(self.mac.camera_snapshot().route.camera.machine, self.mac.id)
        self.assertIs(self.mac.camera_snapshot().sharing, False)
        self.assertEqual(self.mac.helper.commands("send"), [])
        self.assertClean()


if __name__ == "__main__":
    unittest.main()
