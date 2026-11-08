# SPDX-License-Identifier: GPL-3.0-only
"""Audio on Windows: the AudioHost the control plane posts through, and its link bookkeeping
(dev/audio/PHASE2.md §5.1, §8.1).

The window makes one `PcAudio` when this PC announces `audio` (Windows only, the feature flag on and a
helper present, `available`); its `service` is the AudioService the Audio page reads (`window.audio`).
Linux never announces `audio`. A peer reaches this PC over the link this PC opened (LinkSender), the
link the peer opened (LinkResponder), or both; the service is told `link_up` when the first is live
and `link_down` when none is left (`LinkPeers`). Calls from link threads only enqueue. The page
hops the service's callbacks to the GUI thread itself (a Qt signal).
"""

import ipaddress
import logging
import sys
import threading
from pathlib import Path

from core import protocol
from core.audio import helper as audio_helper
from core.audio.service import AudioService

LOGGER = logging.getLogger("beamer.audio")


def platform_name(platform=None) -> str:
    platform = sys.platform if platform is None else platform
    return "windows" if platform.startswith("win") else "linux" if platform.startswith("linux") else platform


def available(state_dir, platform=None) -> bool:
    """Whether this machine announces `audio`: Windows only (a Linux build never does), the feature
    flag on and a helper binary present (the app's own, or the one `audio-dev.enabled` names)."""
    if platform_name(platform) != "windows":
        return False
    try:
        return audio_helper.available(Path(state_dir))
    except Exception:
        LOGGER.exception("audio: could not tell whether the helper exists")
        return False


def udp_host(host):
    """`host` when it is an IPv4 address a UDP flow can go to, else None: loopback, names and IPv6
    mean the flow stays on the link (§4.2)."""
    if not isinstance(host, str) or not host:
        return None
    try:
        address = ipaddress.ip_address(host)
    except ValueError:
        return None
    if address.version != 4 or address.is_loopback or address.is_unspecified or address.is_multicast:
        return None
    return host


LOGGED = ("audio_hello", "audio_devices")


def log_message(direction, name, message):
    """`audio_hello` and `audio_devices` in the log, both ways: rare (link up, a device change), and
    what tells from the log alone that two machines found each other's audio."""
    kind = message.get("type") if isinstance(message, dict) else None
    if kind not in LOGGED:
        return
    data = message.get("data") if isinstance(message.get("data"), dict) else {}
    if kind == "audio_hello":
        LOGGER.info("audio: %s %s %s (v %s, helper %s, platform %s)", kind, direction, name, data.get("v"),
                    data.get("helper"), data.get("platform"))
    else:
        LOGGER.info("audio: %s %s %s (machine %s, rev %s, status %s, %s inputs, %s outputs)", kind, direction, name,
                    data.get("name"), data.get("rev"), data.get("status"), len(data.get("inputs") or ()),
                    len(data.get("outputs") or ()))


class LinkPeers:
    """Per-peer link state from the per-link events of both directions. `kind` is "out" (the link this
    machine opened) or "in" (the peer's). The caps given to the service are those of the link that
    carries this machine's messages: its own when up, else the peer's. (Same as mac_app/audio_mac.py.)"""

    def __init__(self, service):
        self._service = service
        self._lock = threading.Lock()
        self._links = {}      # peer id text: {kind: caps}

    def changed(self, peer, kind, up, caps=()):
        if not peer:
            return
        with self._lock:
            held = self._links.setdefault(peer, {})
            before = self._caps(held)
            if up:
                if kind in held and len(held) == 1:
                    # Its only link was replaced without an end being reported (the responder closes
                    # an earlier link from the same peer): the peer saw that link end, so do we.
                    self._service.link_down(peer)
                    before = None
                held[kind] = frozenset(caps or ())
            else:
                held.pop(kind, None)
            after = self._caps(held)
            if after is None:
                del self._links[peer]
                if before is not None:
                    self._service.link_down(peer)
            elif after != before:
                self._service.link_up(peer, after)

    def peers(self):
        with self._lock:
            return {peer: self._caps(held) for peer, held in self._links.items()}

    @staticmethod
    def _caps(held):
        if "out" in held:
            return held["out"]
        return held.get("in")


class PcAudioHost:
    """The AudioHost of §8.1 over LinkSender (`sender`) and LinkResponder (`server`), both keyed by
    id bytes; the service's peers are id texts."""

    def __init__(self, server, sender, identity, name_of=None, platform=None):
        self._server = server
        self._sender = sender
        own = identity()
        self._own = protocol.id_text(own["id"])
        self._name = own.get("name") or "PC"
        self._platform = platform_name(platform)
        self._name_of = name_of

    def own_id(self) -> str:
        return self._own

    def own_name(self) -> str:
        return self._name

    def platform(self) -> str:
        return self._platform

    def post(self, peer: str, message: dict) -> bool:
        """WIRE.md's "which link" rule: the link this PC opened when it is up, else the peer's."""
        ident = protocol.read_id(peer)
        if ident is None:
            return False
        log_message("to", self.peer_name(peer), message)
        links = self._sender.links
        if links.up(ident):
            if links.post(ident, message):
                return True
            if links.up(ident):
                return False
        return bool(self._server.send(ident, message))

    def outbox_depth(self, peer: str) -> int:
        ident = protocol.read_id(peer)
        if ident is None:
            return 0
        links = self._sender.links
        if links.up(ident):
            return links.pending(ident)
        return self._server.outbox_depth(ident)

    def peer_host(self, peer: str):
        """The peer's IPv4 address as a link reaches it: the address this PC dialled, else the address
        the peer's own link comes from; None for loopback or a name."""
        ident = protocol.read_id(peer)
        if ident is None:
            return None
        host = udp_host(self._sender.links.host(ident))
        return host if host is not None else udp_host(self._server.host_of(ident))

    def peer_name(self, peer: str) -> str:
        if self._name_of is not None:
            try:
                return self._name_of(peer) or peer
            except Exception:
                pass
        return peer


class PcAudio:
    """The service, its host and the link bookkeeping, made before any link is up so no peer's first
    `audio_hello` is missed; `start` runs the worker and the helper."""

    def __init__(self, server, sender, identity, state_dir, *, name_of=None, service_factory=AudioService,
                 platform=None, logger=None):
        self.state_dir = Path(state_dir)
        self.host = PcAudioHost(server, sender, identity, name_of=name_of, platform=platform)
        self.service = service_factory(self.host, self.state_dir, audio_helper.locate(self.state_dir), logger=logger)
        self.peers = LinkPeers(self.service)
        self._started = False
        LOGGER.info("audio: this machine announces audio, helper %s", " ".join(audio_helper.locate(self.state_dir) or ()))

    def start(self):
        if not self._started:
            self._started = True
            self.service.start()

    def stop(self):
        try:
            self.service.stop()
        except Exception:
            LOGGER.exception("audio: the service did not stop cleanly")

    def link(self, peer: bytes, kind, up, caps=()):
        text = protocol.id_text(peer)
        if up:
            LOGGER.info("audio: %s link with %s up, %d capabilities%s", kind, self.host.peer_name(text), len(caps or ()),
                        " including audio" if "audio" in (caps or ()) else ", no audio")
        self.peers.changed(text, kind, up, caps)

    def message(self, peer: bytes, message):
        text = protocol.id_text(peer)
        log_message("from", self.host.peer_name(text), message)
        self.service.link_message(text, message)
