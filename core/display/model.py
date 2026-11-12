# SPDX-License-Identifier: GPL-3.0-only
"""Shared displays: the pure part (dev/display/DESIGN.md). Display keys, the `display_*` messages, the
ownership route and the decision of which local displays are on.

A display is named across machines by its EDID identity, `<PNP id><product code>`, e.g. "HKC2716":
the three letters of the manufacturer and the product code as four upper-case hex digits. macOS gives
both as numbers (CGDisplayVendorNumber, CGDisplayModelNumber), Windows in the monitor's device path
("\\\\?\\DISPLAY#HKC2716#..."). Two monitors of the same model on one machine share a key and are left
alone (`duplicates`).

The route maps a display key to its owner (a machine id, or None for "not managed") with a stamp per
entry; newer is a higher `set_at` (milliseconds), a tie goes to the larger `by`. Entries are never
deleted, so a merge never loses a choice.
"""

import dataclasses
import re
from typing import Dict, FrozenSet, Iterable, Optional, Tuple

STATE = "display_state"     # direct: this machine's displays
ROUTE = "display_route"     # flooded, merged entry by entry
TYPES = (STATE, ROUTE)
FEATURE = "display1"        # audio_hello.features word

MAX_DISPLAYS = 16
MAX_ROUTE = 32
MAX_NAME = 128
_KEY = re.compile(r"[A-Z]{3}[0-9A-F]{4}")
_ID = re.compile(r"[0-9A-Za-z_.:-]{1,64}")



def pnp_letters(vendor: int) -> str:
    """The EDID manufacturer id (three 5-bit letters, 'A' = 1) as text: 0x2163 -> "HKC"."""
    vendor = int(vendor) & 0x7FFF
    return "".join(chr(64 + ((vendor >> shift) & 0x1F)) for shift in (10, 5, 0))


def key_from_numbers(vendor: int, product: int) -> Optional[str]:
    key = f"{pnp_letters(vendor)}{int(product) & 0xFFFF:04X}"
    return key if _KEY.fullmatch(key) else None


def key_from_device_path(path: str) -> Optional[str]:
    r"""The key in a Windows monitor path, "\\?\DISPLAY#HKC2716#7&1bd...#{e6f0...}" -> "HKC2716"."""
    parts = (path or "").split("#")
    if len(parts) >= 2:
        candidate = parts[1].upper()
        if _KEY.fullmatch(candidate):
            return candidate
    return None


def valid_key(key) -> bool:
    return isinstance(key, str) and bool(_KEY.fullmatch(key))


@dataclasses.dataclass(frozen=True)
class Display:
    """One display of a machine as it is published: `on` is False while it is connected but turned off."""
    key: str
    name: str
    on: bool


@dataclasses.dataclass(frozen=True)
class Owner:
    owner: Optional[str]     # machine id, or None: not managed
    set_at: int              # milliseconds
    by: str

    def newer_than(self, other: Optional["Owner"]) -> bool:
        if other is None:
            return True
        return (self.set_at, self.by.encode()) > (other.set_at, other.by.encode())


Route = Dict[str, Owner]


def merge(held: Route, incoming: Route) -> Tuple[Route, bool, bool]:
    """(merged, we_learnt_something, incoming_lacked_something)."""
    merged = dict(held)
    learnt = False
    for key, entry in incoming.items():
        if entry.newer_than(merged.get(key)):
            merged[key] = entry
            learnt = True
    lacked = any(held_entry.newer_than(incoming.get(key)) for key, held_entry in held.items()
                 if incoming.get(key) != held_entry)
    return merged, learnt, lacked


def choose(route: Route, key: str, owner: Optional[str], by: str, now_ms: int) -> Route:
    """The route with `key` handed to `owner` (None: not managed), stamped after any held entry."""
    held = route.get(key)
    set_at = max(int(now_ms), held.set_at + 1 if held is not None else 0)
    return dict(route, **{key: Owner(owner, set_at, by)})


# Messages ----------------------------------------------------------------------------------------


def state_msg(machine: str, rev: int, displays: Iterable[Display]) -> dict:
    return {"type": STATE, "data": {"machine": machine, "rev": int(rev),
                                    "displays": [{"key": d.key, "name": d.name, "on": bool(d.on)} for d in displays]}}


def route_msg(route: Route) -> dict:
    return {"type": ROUTE, "data": {"owners": {key: {"owner": entry.owner, "set_at": entry.set_at, "by": entry.by}
                                               for key, entry in sorted(route.items())}}}


def read_state(message) -> Optional[Tuple[str, int, Tuple[Display, ...]]]:
    """(machine, rev, displays) of a well-formed `display_state`, else None."""
    data = _data(message, STATE)
    if data is None:
        return None
    machine, rev, items = data.get("machine"), data.get("rev"), data.get("displays")
    if not _is_id(machine) or not isinstance(rev, int) or isinstance(rev, bool) or rev < 0:
        return None
    if not isinstance(items, list) or len(items) > MAX_DISPLAYS:
        return None
    displays = []
    for item in items:
        if not isinstance(item, dict) or not valid_key(item.get("key")) or not isinstance(item.get("on"), bool):
            return None
        name = item.get("name")
        name = name[:MAX_NAME] if isinstance(name, str) else ""
        displays.append(Display(item["key"], name, item["on"]))
    return machine, rev, tuple(displays)


def read_route(message) -> Optional[Route]:
    data = _data(message, ROUTE)
    if data is None:
        return None
    owners = data.get("owners")
    if not isinstance(owners, dict) or len(owners) > MAX_ROUTE:
        return None
    route = {}
    for key, entry in owners.items():
        if not valid_key(key) or not isinstance(entry, dict):
            return None
        owner, set_at, by = entry.get("owner"), entry.get("set_at"), entry.get("by")
        if owner is not None and not _is_id(owner):
            return None
        if not isinstance(set_at, int) or isinstance(set_at, bool) or set_at < 0 or not _is_id(by):
            return None
        route[key] = Owner(owner, set_at, by)
    return route


def route_data(route: Route) -> dict:
    return route_msg(route)["data"]["owners"]


def route_from_data(owners) -> Route:
    return read_route({"type": ROUTE, "data": {"owners": owners}}) or {}


def _data(message, kind):
    if not isinstance(message, dict) or message.get("type") != kind or not isinstance(message.get("data"), dict):
        return None
    return message["data"]


def _is_id(value) -> bool:
    return isinstance(value, str) and bool(_ID.fullmatch(value))


# The decision ------------------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class Want:
    on: Optional[bool]       # None: leave it as it is
    reason: str


def want(own: str, key: str, route: Route, peers: Dict[str, FrozenSet[str]]) -> Want:
    """Whether this machine's display `key` should be on.

    `peers` maps each linked peer that speaks display1, and has said which displays it has, to those
    keys. The rule is "never a hidden screen": a monitor turned back on here would not take the
    monitor's input back from its owner (automatic input selection keeps the input that has a signal),
    so it would be a screen nobody sees. A display owned by another machine therefore stays off here,
    even while that machine is away; it comes back on only when the route says so (this machine, or
    not managed) or when the owner says it does not have that display."""
    entry = route.get(key)
    if entry is None or entry.owner is None:
        return Want(True, "unmanaged")
    if entry.owner == own:
        return Want(True, "owner")
    owner_keys = peers.get(entry.owner)
    if owner_keys is not None and key not in owner_keys:
        return Want(True, "owner_lacks_it")
    return Want(False, "other_owner" if owner_keys is not None else "owner_away")


def shared(own_keys: Iterable[str], peers: Dict[str, FrozenSet[str]]) -> Dict[str, Tuple[str, ...]]:
    """Each own display key some linked peer has too -> the peers that have it, sorted."""
    result = {}
    for key in sorted(set(own_keys)):
        holders = tuple(sorted(peer for peer, keys in peers.items() if key in keys))
        if holders:
            result[key] = holders
    return result
