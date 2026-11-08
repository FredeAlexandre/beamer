# SPDX-License-Identifier: GPL-3.0-only
"""N AudioServices on in-memory links, in lockstep rounds (dev/audio/PHASE2.md §11.1).

Each node is a machine: an AudioService with a FakeHelper, or (`audio=False`) a Beamer without the
`audio` cap that only counts what reaches it. A link is up or down; messages posted on it wait in
flight until the next `round()`, which delivers every message then in flight and lets every service
handle everything queued. Taking a link down drops what is in flight on it, as a closed TCP link does.
Every post to a peer whose caps lack `audio` is recorded in `violations`.
"""

import shutil
import tempfile
import threading

from core import protocol
from core.audio import service as service_module
from core.audio.fake_helper import FakeHelper

BASE_CAPS = ("clipboard", "clipboard_image", "gestures", "media_keys", "text", "settings", "design_sync",
             "layout", "profiles", "displays", "ping_rtt", "identify_screens", "motion_path")


def machine_id(seed):
    return protocol.id_text(bytes([seed]) * 16)


class Clock:
    def __init__(self, start=1000.0, wall=1793000000.0):
        self.now = start
        self.wall_now = wall

    def __call__(self):
        return self.now

    def wall(self):
        return self.wall_now

    def advance(self, seconds):
        self.now += seconds
        self.wall_now += seconds


class Host:
    """AudioHost of one node: posts go in flight on the harness's links."""

    def __init__(self, net, node):
        self.net = net
        self.node = node
        self.depth = 0
        self.require_thread = None
        self.posted = []

    def own_id(self):
        return self.node.id

    def own_name(self):
        return self.node.name

    def platform(self):
        return self.node.platform

    def post(self, peer, message):
        if self.require_thread is not None:
            assert threading.current_thread() is self.require_thread(), "post off the worker"
        self.posted.append((peer, message))
        return self.net.post(self.node.id, peer, message)

    def outbox_depth(self, peer):
        return self.depth

    def peer_host(self, peer):
        return self.net.nodes[peer].address if peer in self.net.nodes else None

    def peer_name(self, peer):
        return self.net.nodes[peer].name if peer in self.net.nodes else ""


class Node:
    def __init__(self, net, seed, name, platform, audio, state_dir, devices=None, features=None):
        self.net = net
        self.id = machine_id(seed)
        self.name = name
        self.platform = platform
        self.audio = audio
        self.address = f"192.168.1.{seed}"
        self.caps = frozenset(BASE_CAPS + (("audio",) if audio else ()))
        self.received = []
        self.state_dir = state_dir
        self.host = Host(net, self)
        self.helper = None
        self.service = None
        if audio:
            self.helper = FakeHelper(platform=platform, devices=devices, features=features)
            self.service = self._new_service()

    def _new_service(self):
        def factory(cmd, deliver):
            self.helper.cmd = cmd
            self.helper.bind(deliver)
            return self.helper
        return service_module.AudioService(self.host, self.state_dir, ["beamer-audio"], clock=self.net.clock,
                                           wall=self.net.clock.wall, helper_factory=factory)

    def restart(self):
        """Quit Beamer on this machine and start it again (same state dir, fresh helper)."""
        self.service.stop()
        self.service.pump()
        self.helper = FakeHelper(platform=self.platform, devices=self.helper.engine.devices,
                                 features=self.helper.engine.features)
        self.service = self._new_service()
        self.service.start(run_thread=False)

    def snapshot(self):
        return self.service.snapshot()


class Net:
    def __init__(self):
        self.clock = Clock()
        self.nodes = {}
        self.links = set()
        self.in_flight = []
        self.violations = []
        self.delivered = 0
        self._dirs = []

    def add(self, seed, name, platform="macos", audio=True, devices=None, features=None):
        state_dir = tempfile.mkdtemp(prefix="beamer-audio-")
        self._dirs.append(state_dir)
        node = Node(self, seed, name, platform, audio, state_dir, devices, features)
        self.nodes[node.id] = node
        if node.service is not None:
            node.service.start(run_thread=False)
        return node

    def close(self):
        for node in self.nodes.values():
            if node.service is not None:
                node.service.stop()
        for path in self._dirs:
            shutil.rmtree(path, ignore_errors=True)

    def audio_nodes(self):
        return [node for node in self.nodes.values() if node.audio]

    def up(self, a, b):
        return frozenset((a, b)) in self.links

    def post(self, source, target, message):
        if target not in self.nodes or not self.up(source, target):
            return False
        if "audio" not in self.nodes[target].caps:
            self.violations.append((source, target, message.get("type")))
        self.in_flight.append((source, target, message))
        return True

    def link(self, a, b):
        pair = frozenset((a.id, b.id))
        if pair in self.links:
            return
        self.links.add(pair)
        for one, other in ((a, b), (b, a)):
            if one.service is not None:
                one.service.link_up(other.id, other.caps)

    def unlink(self, a, b):
        pair = frozenset((a.id, b.id))
        if pair not in self.links:
            return
        self.links.discard(pair)
        self.in_flight = [item for item in self.in_flight if frozenset(item[:2]) != pair]
        for one, other in ((a, b), (b, a)):
            if one.service is not None:
                one.service.link_down(other.id)

    def pump(self):
        for node in self.audio_nodes():
            node.service.pump()

    def round(self, step=0.01):
        """Deliver what is in flight, then let every service handle it. The number delivered."""
        batch, self.in_flight = self.in_flight, []
        for source, target, message in batch:
            node = self.nodes[target]
            node.received.append((source, message))
            if node.service is not None:
                node.service.link_message(source, message)
        self.delivered += len(batch)
        self.clock.advance(step)
        self.pump()
        return len(batch)

    def settle(self, limit=200, step=0.01):
        """Rounds until nothing is in flight; the number of rounds that carried messages."""
        self.pump()
        rounds = 0
        while self.in_flight and rounds < limit:
            self.round(step)
            rounds += 1
        return rounds
