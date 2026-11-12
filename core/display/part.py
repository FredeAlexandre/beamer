# SPDX-License-Identifier: GPL-3.0-only
"""Shared displays inside the AudioService worker (dev/display/DESIGN.md).

The part owns the route (who owns each shared display), this machine's display list and the
`display_*` messages. Every call runs on the AudioService worker, through a small context:

    ctx.own, ctx.clock, ctx.wall, ctx.logger
    ctx.post(peer, message) -> bool
    ctx.peers_with(feature) -> frozenset of direct peers whose audio_hello lists it
    ctx.machine_name(machine) -> str
    ctx.changed()           -> the service tells its subscribers
    ctx.schedule(at)        -> the part's next wake-up (clock time)

The backend is the platform's: `displays()` lists the connected external displays, the ones this
machine turned off included (on=False), and `set_on(key, on)` turns one on or off; both may raise.
"""

import dataclasses
import json
import os
from pathlib import Path
from typing import Dict, Optional, Tuple

from core.display import model

FILE_NAME = "display.json"
POLL_SECONDS = 2.0
SETTLE_SECONDS = 0.5
RETRY_SECONDS = 30.0


@dataclasses.dataclass(frozen=True)
class Choice:
    machine: Optional[str]      # None: not managed
    name: str
    chosen: bool


@dataclasses.dataclass(frozen=True)
class SharedView:
    key: str
    name: str
    owner: Optional[str]
    on_here: bool
    choices: Tuple[Choice, ...]


@dataclasses.dataclass(frozen=True)
class DisplaySnapshot:
    own: str
    shared: Tuple[SharedView, ...] = ()


class DisplayPart:
    def __init__(self, ctx, state_dir, backend):
        self._ctx = ctx
        self._log = ctx.logger
        self._path = Path(state_dir) / FILE_NAME
        self._backend = backend
        self._route: model.Route = {}
        self._turned_off = set()            # keys this machine turned off (persisted)
        self._rev = 0
        self._local: Tuple[model.Display, ...] = ()
        self._peers: Dict[str, Tuple[int, Tuple[model.Display, ...]]] = {}
        self._ready = set()                 # direct peers that announced display1
        self._retry_at: Dict[str, float] = {}
        self._poll_at = None
        self._snapshot = DisplaySnapshot(ctx.own)
        self._started = False

    # Lifecycle ----------------------------------------------------------------------------------

    def start(self):
        self._started = True
        self._load()
        self._poll()
        self._apply()

    def shutdown(self):
        # Nothing is turned back on: a display another machine owns would come back as a hidden
        # screen. The route and what this machine turned off stay on disk for the next start.
        self._save()

    def snapshot(self) -> DisplaySnapshot:
        return self._snapshot

    # Links --------------------------------------------------------------------------------------

    def peer_ready(self, peer):
        """`peer`'s audio_hello lists display1: tell it what this machine has and the route."""
        self._ready.add(peer)
        self._ctx.post(peer, model.state_msg(self._ctx.own, self._rev, self._local))
        self._ctx.post(peer, model.route_msg(self._route))
        self._changed()

    def link_down(self, peer):
        self._ready.discard(peer)
        if self._peers.pop(peer, None) is not None:
            self._apply()
            self._changed()

    def link_message(self, peer, message):
        kind = message.get("type") if isinstance(message, dict) else None
        if kind == model.STATE:
            read = model.read_state(message)
            if read is None or read[0] != peer:     # direct only, each machine about itself
                return
            _machine, rev, displays = read
            held = self._peers.get(peer)
            if held is not None and held == (rev, displays):
                return
            self._peers[peer] = (rev, displays)
            self._log.info("display: %s has %s", self._ctx.machine_name(peer),
                           ", ".join(f"{d.key}({'on' if d.on else 'off'})" for d in displays) or "no external display")
            self._apply()
            self._changed()
        elif kind == model.ROUTE:
            incoming = model.read_route(message)
            if incoming is None:
                return
            merged, learnt, lacked = model.merge(self._route, incoming)
            if learnt:
                self._route = merged
                self._log.info("display: route from %s: %s", self._ctx.machine_name(peer), self._describe_route())
                self._save()
                for other in sorted(self._ready - {peer}):
                    self._ctx.post(other, model.route_msg(self._route))
                self._apply()
                self._changed()
            if lacked:
                self._ctx.post(peer, model.route_msg(self._route))

    # The user's choice --------------------------------------------------------------------------

    def set_owner(self, key, owner):
        if not model.valid_key(key):
            return
        held = self._route.get(key)
        if held is not None and held.owner == owner:
            return
        self._route = model.choose(self._route, key, owner, self._ctx.own, int(self._ctx.wall() * 1000))
        self._log.info("display: %s handed to %s here", key, self._ctx.machine_name(owner) if owner else "nobody (not managed)")
        self._save()
        for peer in sorted(self._ready):
            self._ctx.post(peer, model.route_msg(self._route))
        self._apply()
        self._changed()

    # Timer --------------------------------------------------------------------------------------

    def tick(self):
        now = self._ctx.clock()
        if self._poll_at is not None and now >= self._poll_at:
            self._poll()
            self._apply()
        self._schedule()

    # Inside -------------------------------------------------------------------------------------

    def _poll(self):
        try:
            found = tuple(self._backend.displays())
        except Exception:
            self._log.exception("display: listing the displays failed")
            found = self._local
        keys = [d.key for d in found]
        unique = tuple(d for d in found if keys.count(d.key) == 1)  # two of one model: left alone
        self._poll_at = self._ctx.clock() + POLL_SECONDS
        if unique != self._local:
            self._local = unique
            self._rev += 1
            self._log.info("display: this machine has %s",
                           ", ".join(f"{d.key} {d.name!r} ({'on' if d.on else 'off'})" for d in unique) or "no external display")
            for peer in sorted(self._ready):
                self._ctx.post(peer, model.state_msg(self._ctx.own, self._rev, self._local))
            self._changed()

    def _peer_keys(self):
        return {peer: frozenset(d.key for d in displays) for peer, (_rev, displays) in self._peers.items()
                if peer in self._ready}

    def _apply(self):
        if not self._started:
            return
        now = self._ctx.clock()
        peers = self._peer_keys()
        acted = False
        for display in self._local:
            decision = model.want(self._ctx.own, display.key, self._route, peers)
            if decision.on is None or decision.on == display.on:
                if decision.on and display.on:
                    self._turned_off.discard(display.key)
                continue
            if decision.on and display.key not in self._turned_off:
                continue    # turned off by someone else (the person, another tool): not ours to undo
            if self._retry_at.get(display.key, 0.0) > now:
                continue
            try:
                done = bool(self._backend.set_on(display.key, decision.on))
            except Exception:
                self._log.exception("display: turning %s %s failed", display.key, "on" if decision.on else "off")
                done = False
            self._log.info("display: %s turned %s here (%s)%s", display.key, "on" if decision.on else "off",
                           decision.reason, "" if done else ", failed")
            if not done:
                self._retry_at[display.key] = now + RETRY_SECONDS
                continue
            self._retry_at.pop(display.key, None)
            (self._turned_off.discard if decision.on else self._turned_off.add)(display.key)
            acted = True
            # What we just did is the state now; the next poll confirms it.
            self._local = tuple(dataclasses.replace(d, on=decision.on) if d.key == display.key else d
                                for d in self._local)
        if acted:
            self._rev += 1
            for peer in sorted(self._ready):
                self._ctx.post(peer, model.state_msg(self._ctx.own, self._rev, self._local))
            self._save()
            self._poll_at = now + SETTLE_SECONDS
            self._schedule()
            self._changed()

    def _schedule(self):
        self._ctx.schedule(self._poll_at)

    def _changed(self):
        self._snapshot = self._compose()
        self._ctx.changed()

    def _compose(self):
        peers = self._peer_keys()
        names = {}
        for _peer, (_rev, displays) in self._peers.items():
            for d in displays:
                names.setdefault(d.key, d.name)
        views = []
        for display in self._local:
            holders = sorted(peer for peer, keys in peers.items() if display.key in keys)
            entry = self._route.get(display.key)
            owner = entry.owner if entry is not None else None
            if not holders and owner is None:
                continue
            machines = [self._ctx.own] + holders
            if owner is not None and owner not in machines:
                machines.append(owner)
            choices = tuple(Choice(machine, self._ctx.machine_name(machine), machine == owner) for machine in machines)
            choices += (Choice(None, "", owner is None),)
            name = display.name or names.get(display.key) or display.key
            views.append(SharedView(display.key, name, owner, display.on, choices))
        return DisplaySnapshot(self._ctx.own, tuple(views))

    def _describe_route(self):
        return ", ".join(f"{key} -> {self._ctx.machine_name(entry.owner) if entry.owner else 'not managed'}"
                         for key, entry in sorted(self._route.items())) or "empty"

    # Disk ---------------------------------------------------------------------------------------

    def _load(self):
        try:
            data = json.loads(self._path.read_text(encoding="utf-8"))
        except FileNotFoundError:
            return
        except (OSError, ValueError):
            self._log.warning("display: %s unreadable, starting empty", self._path)
            return
        if not isinstance(data, dict):
            return
        self._route = model.route_from_data(data.get("route") or {})
        self._turned_off = {key for key in data.get("turned_off") or () if model.valid_key(key)}
        rev = data.get("rev")
        self._rev = rev if isinstance(rev, int) and rev >= 0 else 0
        self._log.info("display: route %s, turned off here: %s", self._describe_route(),
                       ", ".join(sorted(self._turned_off)) or "none")

    def _save(self):
        data = {"route": model.route_data(self._route), "turned_off": sorted(self._turned_off), "rev": self._rev}
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            temp = self._path.with_suffix(".tmp")
            temp.write_text(json.dumps(data, indent=1), encoding="utf-8")
            os.replace(temp, self._path)
        except OSError as error:
            self._log.warning("display: could not save %s: %s", self._path, error)
