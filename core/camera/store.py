# SPDX-License-Identifier: GPL-3.0-only
"""`<state_dir>/camera.json` (dev/camera/PHASE2.md §7.3): the camera route, this machine's last
`camera_devices` revision, its sharing switch (the Mac-local kill switch, applied before the first
plan) and the camera lists held for every machine.

A file of its own; `audio.json` is not touched. Written atomically (a temporary file, then
`os.replace`). A missing, unreadable or malformed file loads as empty: camera off, sharing on.
"""

import json
import logging
import os
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Mapping

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


def path_in(state_dir) -> Path:
    return Path(state_dir) / FILE_NAME


def load(state_dir, now=None) -> Stored:
    path = path_in(state_dir)
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        return Stored()
    except (OSError, ValueError) as error:
        log.warning("camera: %s unreadable, starting empty: %s", path, error)
        return Stored()
    if not isinstance(data, dict) or data.get("version") != VERSION:
        return Stored()
    now = time.time() if now is None else now
    route = wire.read_route(data.get("route"), now=now) if "route" in data else None
    machines = {}
    held = data.get("machines") if isinstance(data.get("machines"), dict) else {}
    for ident, entry in held.items():
        machine = wire.read_devices(entry, now=now)
        if machine is not None and machine.id == ident:
            machines[ident] = machine
    own_rev = data.get("own_rev")
    return Stored(route=route or model.empty_route(),
                  own_rev=own_rev if type(own_rev) is int and own_rev >= 0 else 0,
                  sharing=data.get("sharing") is not False, machines=machines)


def save(state_dir, stored: Stored) -> None:
    path = path_in(state_dir)
    data = {"version": VERSION, "route": model.route_data(stored.route), "own_rev": stored.own_rev,
            "sharing": bool(stored.sharing),
            "machines": {ident: model.machine_data(machine) for ident, machine in sorted(stored.machines.items())}}
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{FILE_NAME}.{os.getpid()}.tmp")
    with open(temporary, "w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=1, sort_keys=True)
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(temporary, path)
