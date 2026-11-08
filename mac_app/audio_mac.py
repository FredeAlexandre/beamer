# SPDX-License-Identifier: GPL-3.0-only
"""Audio on the Mac: the AudioHost the control plane posts through, and its link bookkeeping
(dev/audio/PHASE2.md §5.1, §8.1).

The bridge makes one `MacAudio` when this Mac announces `audio` (the feature flag is on and a helper
exists, `available`). Its `service` is the AudioService the Audio page reads (`controller.audio`).
A peer reaches this Mac over the link this Mac opened, the link the peer opened, or both; the
service is told `link_up` when the first is live and `link_down` when none is left (`LinkPeers`).
Every call made here from a link thread only enqueues: nothing waits on the helper or the disk.
The page hops the service's callbacks to the main thread itself (AppHelper.callAfter).
"""

import ipaddress
import logging
import threading
from pathlib import Path

from core import protocol
from core.audio import helper as audio_helper
from core.audio.service import AudioService

LOGGER = logging.getLogger("beamer.audio")

STATE_DIR = Path.home() / "Library" / "Application Support" / "Beamer"
PLATFORM = "macos"


def available(state_dir=None) -> bool:
    """Whether this Mac announces `audio`: the feature flag is on and a helper binary exists (the
    app's own, or the one `audio-dev.enabled` names)."""
    try:
        return audio_helper.available(Path(state_dir) if state_dir is not None else STATE_DIR)
    except Exception:
        LOGGER.exception("audio: could not tell whether the helper exists")
        return False


def udp_host(host):
    """`host` when it is an IPv4 address a UDP flow can go to, else None: loopback (the SSH tunnel),
    names and IPv6 mean the flow stays on the link (§4.2)."""
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
    carries this machine's messages: its own when up, else the peer's."""

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


class MacAudioHost:
    """The AudioHost of §8.1 over the bridge's links. Peers are id texts; the responder keys its
    links by id bytes."""

    def __init__(self, controller):
        self._controller = controller
        identity = controller.identity()
        self._own = protocol.id_text(identity["id"])
        self._name = identity.get("name") or "Mac"

    def own_id(self) -> str:
        return self._own

    def own_name(self) -> str:
        return self._name

    def platform(self) -> str:
        return PLATFORM

    def _outbound(self, peer):
        link = self._controller._peers_up.get(peer)
        return link if link is not None and link.live() else None

    def _responder(self):
        return getattr(self._controller, "responder", None)

    def post(self, peer: str, message: dict) -> bool:
        """WIRE.md's "which link" rule: the link this Mac opened when it is up, else the peer's."""
        log_message("to", self.peer_name(peer), message)
        link = self._outbound(peer)
        if link is not None:
            if link.post(message):
                return True
            if link.live():
                return False
        responder, ident = self._responder(), protocol.read_id(peer)
        return bool(responder is not None and ident is not None and responder.send(ident, message))

    def outbox_depth(self, peer: str) -> int:
        link = self._outbound(peer)
        if link is not None:
            return link.pending()
        responder, ident = self._responder(), protocol.read_id(peer)
        return responder.outbox_depth(ident) if responder is not None and ident is not None else 0

    def peer_host(self, peer: str):
        """The peer's IPv4 address as a link reaches it: the address this Mac dialled (not through the
        tunnel), else the address the peer's own link comes from; None for loopback or a name."""
        link = self._outbound(peer)
        if link is not None and not getattr(link, "via_tunnel", False):
            dialled = getattr(link, "dialled", None)
            host = udp_host(dialled[0] if dialled else None)
            if host is not None:
                return host
        responder, ident = self._responder(), protocol.read_id(peer)
        if responder is not None and ident is not None:
            return udp_host(responder.host_of(ident))
        return None

    def peer_name(self, peer: str) -> str:
        try:
            return self._controller._name_of(peer)
        except Exception:
            return peer


class MacAudio:
    """The service, its host and the link bookkeeping, made by the bridge before any link is up so no
    peer's first `audio_hello` is missed; `start` runs the worker and the helper."""

    def __init__(self, controller, state_dir, *, service_factory=AudioService, logger=None):
        self.state_dir = Path(state_dir)
        self.host = MacAudioHost(controller)
        self.service = service_factory(self.host, self.state_dir, audio_helper.locate(self.state_dir),
                                       logger=logger)
        self.peers = LinkPeers(self.service)
        self._started = False
        LOGGER.info("audio: this Mac announces audio, %d capabilities, helper %s",
                    len(controller.identity().get("caps") or ()), " ".join(audio_helper.locate(self.state_dir) or ()))

    def start(self):
        if not self._started:
            self._started = True
            self.service.start()

    def stop(self):
        try:
            self.service.stop()
        except Exception:
            LOGGER.exception("audio: the service did not stop cleanly")

    def link(self, peer, kind, up, caps=()):
        if up:
            LOGGER.info("audio: %s link with %s up, %d capabilities%s", kind, self.host.peer_name(peer), len(caps or ()),
                        " including audio" if "audio" in (caps or ()) else ", no audio")
        self.peers.changed(peer, kind, up, caps)

    def message(self, peer, message):
        log_message("from", self.host.peer_name(peer), message)
        self.service.link_message(peer, message)
