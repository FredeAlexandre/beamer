# SPDX-License-Identifier: GPL-3.0-only
"""`<state_dir>/camera.json` (dev/camera/PHASE2.md §7.3): the camera route, this machine's last
`camera_devices` revision, its sharing switch (the Mac-local kill switch, applied before the first
plan) and the camera lists held for every machine.

A file of its own; `audio.json` is not touched. Written atomically (a temporary file, then
`os.replace`). A missing file is a first run: camera off, sharing on (§6.1). A file that exists but
cannot be read, is malformed, is of another version or has no boolean `sharing` fails closed (R4 #4):
sharing off, `problem` "unreadable", so the Mac page says so. Peer lists come back with no demand
(R4 #2): an app that used Beamer Camera before the restart says so again over a live link.
"""

import json
import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Mapping, Optional

from core.camera import model, wire

FILE_NAME = "camera.json"
VERSION = 1

log = logging.getLogger("beamer.camera")


@dataclass(frozen=True)
class Stored:
    route: model.CameraRoute = field(default_factory=model.empty_route)
    own_rev: int = 0
    sharing: bool = True
    machines: Mapping[str, model.MachineCamera] = field(default_factory=dict)
    seen: Mapping[str, float] = field(default_factory=dict)   # machine -> wall time it was last online
    problem: Optional[str] = None                              # "unreadable", else None


def path_in(state_dir) -> Path:
    return Path(state_dir) / FILE_NAME


UNREADABLE = Stored(sharing=False, problem="unreadable")


def load(state_dir, now=None) -> Stored:
    path = path_in(state_dir)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return Stored()
    except (OSError, ValueError) as error:
        log.warning("camera: %s unreadable, camera sharing starts off: %s", path, error)
        return UNREADABLE
    if not isinstance(data, dict) or data.get("version") != VERSION or not isinstance(data.get("sharing"), bool):
        log.warning("camera: %s is not a version %d file with a sharing switch; camera sharing starts off", path, VERSION)
        return UNREADABLE
    now = time.time() if now is None else now
    route = wire.read_route(data.get("route"), now=now) if "route" in data else None
    machines = {}
    held = data.get("machines") if isinstance(data.get("machines"), dict) else {}
    for ident, entry in held.items():
        machine = wire.read_devices(entry, now=now)
        if machine is not None and machine.id == ident:
            machines[ident] = model.without_demand(machine)
    stored_seen = data.get("seen") if isinstance(data.get("seen"), dict) else {}
    seen = {ident: float(at) for ident, at in stored_seen.items()
            if ident in machines and isinstance(at, (int, float)) and not isinstance(at, bool)}
    own_rev = data.get("own_rev")
    return Stored(route=route or model.empty_route(),
                  own_rev=own_rev if type(own_rev) is int and own_rev >= 0 else 0,
                  sharing=data["sharing"], machines=machines, seen=seen)


def save(state_dir, stored: Stored) -> None:
    path = path_in(state_dir)
    data = {"version": VERSION, "route": model.route_data(stored.route), "own_rev": stored.own_rev,
            "sharing": bool(stored.sharing),
            "machines": {ident: model.machine_data(machine) for ident, machine in sorted(stored.machines.items())},
            "seen": {ident: at for ident, at in sorted(stored.seen.items()) if ident in stored.machines}}
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{FILE_NAME}.{os.getpid()}.tmp")
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=1, sort_keys=True)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
