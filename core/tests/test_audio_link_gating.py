# SPDX-License-Identifier: GPL-3.0-only
"""The `audio` capability on the links (dev/audio/PHASE2.md §5.1, WP5): `audio_*` messages reach only
peers that announced `audio`, are handed to the audio service only when both ends did, and the per-peer
link bookkeeping and AudioHosts of both apps (mac_app/audio_mac.py, win_app/audio_win.py, loaded by
path: they import nothing app-specific). The last tests run a real OutboundLink and LinkResponder on
loopback, each feeding an AudioService with the in-process fake helper, beside a 1.5.x-style peer."""

import importlib.util
import queue
import shutil
import tempfile
import threading
import time
import unittest
from unittest import mock
from pathlib import Path

from core import feature_flags, protocol, receiver
from core import link as link_module
from core.audio import wire
from core.audio.fake_helper import FakeHelper
from core.audio.service import AudioService
from core.link import OutboundLink
from core.tests.responder_harness import B, C, CAPS, HERE, TOKENS, Initiator, Machine, Settings, entry, wait_for

ROOT = Path(__file__).resolve().parents[2]
AUDIO_CAPS = list(CAPS) + ["audio"]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, ROOT / path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


audio_mac = load("audio_mac_under_test", "mac_app/audio_mac.py")
audio_win = load("audio_win_under_test", "win_app/audio_win.py")


def hello():
    return wire.hello("macos", "ok", False)


class RecordingService:
    """What LinkPeers tells the service, in order."""

    def __init__(self):
        self.calls = []

    def link_up(self, peer, caps):
        self.calls.append(("up", peer, frozenset(caps)))

    def link_down(self, peer):
        self.calls.append(("down", peer))


class CapabilityTableTests(unittest.TestCase):
    def test_every_audio_type_needs_the_audio_capability(self):
        audio = {kind for kind, cap in link_module._CAP_FOR.items() if cap == "audio"}
        self.assertEqual(audio, set(wire.TYPES))

    def test_the_feature_flag_is_on_in_the_fork(self):
        self.assertIs(feature_flags.AUDIO, True)

    def test_linux_never_announces_audio(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder)
        (Path(folder) / "audio-dev.enabled").write_text('{"helper": ["%s"]}' % __file__.replace("\\", "/"))
        self.assertFalse(audio_win.available(folder, platform="linux"))
        self.assertTrue(audio_win.available(folder, platform="win32"))
        self.assertTrue(audio_mac.available(folder))

    def test_no_helper_means_no_audio(self):
        folder = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, folder)
        with mock.patch.dict("os.environ", {"BEAMER_AUDIO_HELPER": ""}):
            with mock.patch.object(audio_mac.audio_helper, "REPO", Path(folder)):
                self.assertFalse(audio_mac.available(folder))
                self.assertFalse(audio_win.available(folder, platform="win32"))


class OutboundGatingTests(unittest.TestCase):
    def link(self, caps):
        settings = Settings([entry(HERE, "Far", token=TOKENS[B], host="127.0.0.1", port=1)])
        book = receiver.PeerBook(settings.load, settings.save)
        link = OutboundLink(TOKENS[B], book, lambda: {}, state=lambda *a: None, up=lambda *a: None, message=lambda *a: None)
        link._live = True
        link._caps = frozenset(caps)
        return link

    def test_audio_is_not_queued_for_a_peer_without_the_capability(self):
        link = self.link(CAPS)
        for kind in wire.TYPES:
            self.assertTrue(link.post({"type": kind, "data": {}}))
        self.assertEqual(len(link._outbox), 0)

    def test_audio_is_queued_for_a_peer_with_it(self):
        link = self.link(AUDIO_CAPS)
        self.assertTrue(link.post(hello()))
        self.assertEqual([message["type"] for message in link._outbox], [wire.HELLO])


class ResponderLinkTests(unittest.TestCase):
    def inbound(self, caps):
        return receiver._Link(None, None, B, b"k" * 16, "192.168.1.138", "Peer", caps, HERE, AUDIO_CAPS, True)

    def test_post_drops_audio_for_a_peer_without_the_capability(self):
        link = self.inbound(CAPS)
        self.assertTrue(link.post(hello()))
        self.assertEqual(len(link.outbox), 0)
        self.assertTrue(link.post(protocol.paired_msg([])))
        self.assertEqual(len(link.outbox), 1)

    def test_post_keeps_audio_for_a_peer_with_it(self):
        link = self.inbound(AUDIO_CAPS)
        self.assertTrue(link.post(hello()))
        self.assertEqual(list(link.outbox), [hello()])

    def test_dispatch_hands_audio_over_only_when_both_ends_announced_it(self):
        seen = []
        responder = receiver.LinkResponder(receiver.PeerBook(lambda: {}, lambda data: None), lambda: {},
                                           status_callback=lambda *a: None,
                                           audio_callback=lambda peer, message: seen.append((peer, message["type"])))
        for caps, own in ((AUDIO_CAPS, AUDIO_CAPS), (CAPS, AUDIO_CAPS), (AUDIO_CAPS, CAPS)):
            link = receiver._Link(None, None, B, b"k" * 16, "h", "Peer", caps, HERE, own, True)
            responder._dispatch(link, hello())
            responder._dispatch(link, {"type": "audio_", "data": None})
            responder._dispatch(link, {"type": 7, "data": {}})
        self.assertEqual(seen, [(B, wire.HELLO), (B, "audio_")])

    def test_set_audio_replaces_the_callbacks(self):
        seen = []
        responder = receiver.LinkResponder(receiver.PeerBook(lambda: {}, lambda data: None), lambda: {},
                                           status_callback=lambda *a: None)
        responder.set_audio(lambda peer, message: seen.append(message["type"]), lambda *a: None)
        link = receiver._Link(None, None, B, b"k" * 16, "h", "Peer", AUDIO_CAPS, HERE, AUDIO_CAPS, True)
        responder._dispatch(link, hello())
        self.assertEqual(seen, [wire.HELLO])
        self.assertIsNone(responder.host_of(B))
        self.assertEqual(responder.outbox_depth(B), 0)


class LinkPeersTests(unittest.TestCase):
    def check(self, module):
        service = RecordingService()
        peers = module.LinkPeers(service)
        a, b = frozenset({"audio", "x"}), frozenset({"audio"})
        peers.changed("P", "in", True, b)
        peers.changed("P", "out", True, a)          # the own link's caps win
        peers.changed("P", "in", False, b)          # still linked: nothing
        peers.changed("P", "out", False)
        self.assertEqual(service.calls, [("up", "P", b), ("up", "P", a), ("down", "P")])
        service.calls.clear()
        peers.changed("P", "in", True, b)
        peers.changed("P", "in", True, b)           # its only link replaced, no end reported
        self.assertEqual(service.calls, [("up", "P", b), ("down", "P"), ("up", "P", b)])
        service.calls.clear()
        peers.changed("P", "out", True, b)
        peers.changed("P", "in", True, b)           # replaced while the other one lives: nothing
        self.assertEqual(service.calls, [])
        peers.changed("Q", "in", False)             # never up: nothing
        peers.changed("", "in", True, b)
        self.assertEqual(service.calls, [])
        self.assertEqual(peers.peers(), {"P": b})

    def test_mac(self):
        self.check(audio_mac)

    def test_windows(self):
        self.check(audio_win)

    def test_udp_host(self):
        for module in (audio_mac, audio_win):
            self.assertEqual(module.udp_host("192.168.1.134"), "192.168.1.134")
            for host in ("127.0.0.1", "localhost", "lebig", "::1", "fe80::1", "0.0.0.0", "", None, "224.0.0.1"):
                self.assertIsNone(module.udp_host(host), host)


class FakeOutbound:
    def __init__(self, live=True, ok=True, dialled=("192.168.1.134", 24820), tunnel=False, pending=3):
        self._live, self.ok, self.dialled, self.via_tunnel, self._pending = live, ok, dialled, tunnel, pending
        self.posted = []

    def live(self):
        return self._live

    def post(self, message):
        self.posted.append(message)
        return self.ok

    def pending(self):
        return self._pending


class FakeResponder:
    def __init__(self, host="192.168.1.134", depth=5):
        self.sent, self.host, self.depth = [], host, depth

    def send(self, peer, message):
        self.sent.append((peer, message))
        return True

    def host_of(self, peer):
        return self.host

    def outbox_depth(self, peer):
        return self.depth


class FakeController:
    def __init__(self, own):
        self._peers_up = {}
        self.responder = FakeResponder()
        self.own = own

    def identity(self):
        return {"id": self.own, "name": "alexandres-macbook-air", "caps": AUDIO_CAPS}

    def _name_of(self, peer):
        return "lebig"


class MacHostTests(unittest.TestCase):
    def setUp(self):
        self.controller = FakeController(B)
        self.host = audio_mac.MacAudioHost(self.controller)
        self.peer = protocol.id_text(HERE)

    def test_identity(self):
        self.assertEqual((self.host.own_id(), self.host.own_name(), self.host.platform()),
                         (protocol.id_text(B), "alexandres-macbook-air", "macos"))
        self.assertEqual(self.host.peer_name(self.peer), "lebig")

    def test_the_own_link_first_else_the_peers(self):
        self.assertTrue(self.host.post(self.peer, hello()))
        self.assertEqual(self.controller.responder.sent, [(HERE, hello())])
        self.assertEqual(self.host.outbox_depth(self.peer), 5)
        own = self.controller._peers_up[self.peer] = FakeOutbound()
        self.assertTrue(self.host.post(self.peer, hello()))
        self.assertEqual(len(own.posted), 1)
        self.assertEqual(len(self.controller.responder.sent), 1)
        self.assertEqual(self.host.outbox_depth(self.peer), 3)
        own.ok = False                              # full: not sent around it, out of order
        self.assertFalse(self.host.post(self.peer, hello()))
        self.assertEqual(len(self.controller.responder.sent), 1)

    def test_peer_host(self):
        self.controller.responder.host = "127.0.0.1"
        self.assertIsNone(self.host.peer_host(self.peer))
        self.controller._peers_up[self.peer] = FakeOutbound(dialled=("127.0.0.1", 24822), tunnel=True)
        self.assertIsNone(self.host.peer_host(self.peer))
        self.controller.responder.host = "192.168.1.134"
        self.assertEqual(self.host.peer_host(self.peer), "192.168.1.134")
        self.controller._peers_up[self.peer] = FakeOutbound(dialled=("192.168.1.99", 24820))
        self.assertEqual(self.host.peer_host(self.peer), "192.168.1.99")


class FakeLinkSet:
    def __init__(self):
        self.live, self.ok, self.address, self.posted = False, True, "192.168.1.138", []

    def up(self, peer):
        return self.live

    def post(self, peer, message):
        self.posted.append((peer, message))
        return self.ok

    def pending(self, peer):
        return 2

    def host(self, peer):
        return self.address if self.live else None


class FakeSender:
    def __init__(self):
        self.links = FakeLinkSet()


class PcHostTests(unittest.TestCase):
    def setUp(self):
        self.server, self.sender = FakeResponder(host="192.168.1.138"), FakeSender()
        self.host = audio_win.PcAudioHost(self.server, self.sender, lambda: {"id": HERE, "name": "lebig"},
                                          name_of=lambda peer: "alexandres-macbook-air", platform="win32")
        self.peer = protocol.id_text(B)

    def test_identity(self):
        self.assertEqual((self.host.own_id(), self.host.own_name(), self.host.platform()),
                         (protocol.id_text(HERE), "lebig", "windows"))
        self.assertEqual(self.host.peer_name(self.peer), "alexandres-macbook-air")

    def test_the_own_link_first_else_the_peers(self):
        self.assertTrue(self.host.post(self.peer, hello()))
        self.assertEqual(self.server.sent, [(B, hello())])
        self.assertEqual(self.host.outbox_depth(self.peer), 5)
        self.sender.links.live = True
        self.assertTrue(self.host.post(self.peer, hello()))
        self.assertEqual(self.sender.links.posted, [(B, hello())])
        self.assertEqual(self.host.outbox_depth(self.peer), 2)
        self.sender.links.ok = False
        self.assertFalse(self.host.post(self.peer, hello()))
        self.assertEqual(len(self.server.sent), 1)
        self.assertFalse(self.host.post("not an id", hello()))

    def test_peer_host(self):
        self.assertEqual(self.host.peer_host(self.peer), "192.168.1.138")
        self.server.host = "127.0.0.1"
        self.assertIsNone(self.host.peer_host(self.peer))
        self.sender.links.live = True
        self.assertEqual(self.host.peer_host(self.peer), "192.168.1.138")


class LiveSide:
    """One machine's audio: an AudioService with the in-process fake helper, fed by LinkPeers."""

    def __init__(self, test, host, platform):
        self.folder = tempfile.mkdtemp()
        test.addCleanup(shutil.rmtree, self.folder, True)
        self.helper = FakeHelper(platform=platform)

        def factory(cmd, deliver):
            self.helper.bind(deliver)
            return self.helper
        self.service = AudioService(host, self.folder, ["beamer-audio"], helper_factory=factory)
        self.peers = audio_win.LinkPeers(self.service)
        test.addCleanup(self.service.stop)

    def direct(self):
        return self.service.snapshot().direct

    def machines(self):
        return set(self.service.snapshot().machines)


class ResponderHost:
    """HERE's AudioHost: only the responder's links (this machine opened none)."""

    def __init__(self, machine, platform="windows"):
        self.machine, self._platform = machine, platform

    def own_id(self):
        return protocol.id_text(self.machine.own)

    def own_name(self):
        return "Here"

    def platform(self):
        return self._platform

    def post(self, peer, message):
        return self.machine.responder.send(protocol.read_id(peer), message)

    def outbox_depth(self, peer):
        return self.machine.responder.outbox_depth(protocol.read_id(peer))

    def peer_host(self, peer):
        return audio_win.udp_host(self.machine.responder.host_of(protocol.read_id(peer)))

    def peer_name(self, peer):
        return peer


class InitiatorHost:
    """B's AudioHost: the one OutboundLink it opened."""

    def __init__(self):
        self.link = None

    def own_id(self):
        return protocol.id_text(B)

    def own_name(self):
        return "Near"

    def platform(self):
        return "macos"

    def post(self, peer, message):
        return self.link is not None and self.link.post(message)

    def outbox_depth(self, peer):
        return self.link.pending() if self.link is not None else 0

    def peer_host(self, peer):
        return None

    def peer_name(self, peer):
        return peer


class LiveLinkTests(unittest.TestCase):
    """A real responder (HERE) with audio callbacks, real initiators on loopback."""

    def machine(self, caps=AUDIO_CAPS):
        machine = Machine([entry(B, "Near"), entry(C, "Old", platform="linux")], caps=caps)
        self.audio_links, self.audio_messages = queue.Queue(), queue.Queue()
        machine.responder.set_audio(lambda peer, message: self.audio_messages.put((peer, message)),
                                    lambda peer, up, caps: self.audio_links.put((peer, up, frozenset(caps))))
        machine.start()
        self.addCleanup(machine.stop)
        return machine

    def initiator(self, machine, peer, caps):
        initiator = Initiator(machine, peer)
        self.addCleanup(initiator.close)
        initiator.handshake(initiator.hello(caps=caps))
        return initiator

    def test_link_events_and_messages_with_an_audio_peer(self):
        machine = self.machine()
        initiator = self.initiator(machine, B, AUDIO_CAPS)
        self.assertEqual(self.audio_links.get(timeout=3), (B, True, frozenset(AUDIO_CAPS)))
        self.assertEqual(machine.responder.host_of(B), "127.0.0.1")
        initiator.send(hello())
        self.assertEqual(self.audio_messages.get(timeout=3), (B, hello()))
        self.assertTrue(machine.responder.send(B, hello()))
        self.assertEqual(initiator.expect(wire.HELLO), hello()["data"])
        initiator.close()
        self.assertEqual(self.audio_links.get(timeout=3), (B, False, frozenset(AUDIO_CAPS)))

    def test_a_peer_without_audio_neither_gets_nor_gives_audio(self):
        machine = self.machine()
        initiator = self.initiator(machine, C, CAPS)
        self.assertEqual(self.audio_links.get(timeout=3), (C, True, frozenset(CAPS)))
        self.assertTrue(machine.responder.send(C, hello()))          # dropped quietly
        self.assertTrue(machine.responder.send(C, protocol.paired_msg([])))
        self.assertIsNotNone(initiator.expect(protocol.MSG_PAIRED))
        initiator.send(hello())
        initiator.send(protocol.ping_msg())
        time.sleep(0.2)
        self.assertTrue(self.audio_messages.empty())
        self.assertFalse(any(message.get("type", "").startswith("audio_") for message in initiator.seen + initiator.unclaimed))

    def test_this_machine_without_audio_hands_nothing_over(self):
        machine = self.machine(caps=CAPS)
        initiator = self.initiator(machine, B, AUDIO_CAPS)
        initiator.send(hello())
        initiator.send(protocol.ping_msg())
        time.sleep(0.2)
        self.assertTrue(self.audio_messages.empty())


class LiveServicesTests(unittest.TestCase):
    """B dials HERE with a real OutboundLink; both run an AudioService. C, a 1.5.x-style Linux peer
    without `audio`, is linked to HERE too and must never see an `audio_*` message."""

    def test_two_audio_machines_meet_and_an_old_peer_hears_nothing(self):
        machine = Machine([entry(B, "Near"), entry(C, "Old", platform="linux")], caps=AUDIO_CAPS)
        here = LiveSide(self, ResponderHost(machine), "windows")
        machine.responder.set_audio(lambda peer, message: here.service.link_message(protocol.id_text(peer), message),
                                    lambda peer, up, caps: here.peers.changed(protocol.id_text(peer), "in", up, caps))
        machine.start()
        self.addCleanup(machine.stop)
        here.service.start()

        old = Initiator(machine, C)
        self.addCleanup(old.close)
        old.handshake(old.hello(caps=CAPS, platform="linux"))

        near_host = InitiatorHost()
        near = LiveSide(self, near_host, "macos")
        settings = Settings([entry(HERE, "Far", token=TOKENS[B], host="127.0.0.1", port=machine.port)])
        settings.data["machine_id"] = protocol.id_text(B)
        book = receiver.PeerBook(settings.load, settings.save)
        identity = {"id": B, "name": "Near", "platform": "macos", "app": "1.5.0", "caps": AUDIO_CAPS, "port": 24820}

        def up(link, fields):
            near.peers.changed(protocol.id_text(fields["id"]), "out", True, link.caps)

        def state(link, is_up, text):
            if not is_up and link.peer_id:
                near.peers.changed(link.peer_id, "out", False)

        def message(link, msg):
            if isinstance(msg.get("type"), str) and msg["type"].startswith("audio_") and "audio" in link.caps:
                near.service.link_message(link.peer_id, msg)
        link = OutboundLink(TOKENS[B], book, lambda: dict(identity), state=state, up=up, message=message,
                            reconnect_seconds=0.05)
        near_host.link = link
        near.service.start()
        link.start()
        self.addCleanup(link.stop)

        here_id, near_id = protocol.id_text(HERE), protocol.id_text(B)
        self.assertTrue(wait_for(lambda: near_id in here.direct() and here_id in near.direct(), 5.0),
                        (here.direct(), near.direct()))
        # audio_devices both ways: each holds the other's list.
        self.assertTrue(wait_for(lambda: near_id in here.machines() and here_id in near.machines(), 5.0))
        # The Mac offers the flow and the PC answers (the fake helper binds nothing).
        self.assertTrue(wait_for(lambda: any(c.get("cmd") == "flow_open" for c in near.helper.sent)
                                 and any(c.get("cmd") == "flow_open" for c in here.helper.sent), 5.0))
        offer = next(c for c in near.helper.sent if c.get("cmd") == "flow_open")
        self.assertEqual(offer["role"], "offer")
        time.sleep(0.3)
        old.send(protocol.ping_msg())
        old.expect(protocol.MSG_ACK, timeout=0.5)
        self.assertFalse([m for m in old.seen + old.unclaimed if str(m.get("type", "")).startswith("audio_")])
        self.assertNotIn(protocol.id_text(C), here.direct())

        link.stop()
        self.assertTrue(wait_for(lambda: near_id not in here.direct(), 5.0))


if __name__ == "__main__":
    unittest.main()
