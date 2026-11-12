"""Shared displays on macOS: list the external displays and turn one off or on (core/display/part.py).

macOS has no public call to turn a display off. CGSConfigureDisplayEnabled (private, exported by
CoreGraphics; SkyLight's SLSConfigureDisplayEnabled on newer systems) is what Lunar and BetterDisplay
use: inside a display configuration, completed for the session only, so a restart or replugging the
cable always brings the display back. A display turned off leaves CGGetOnlineDisplayList, so this
module remembers each one it turned off (id and name, in display-mac.json) to list it and turn it back
on; CGDisplayVendorNumber and CGDisplayModelNumber still answer for such an id, which tells whether a
remembered id is still that monitor.
"""

import ctypes
import json
import logging
import os
import time
from pathlib import Path

from core.display import model

LOGGER = logging.getLogger("beamer.display")
FILE_NAME = "display-mac.json"
_U32 = ctypes.c_uint32
_VP = ctypes.c_void_p
_CONFIGURE_FOR_SESSION = 1
# A display turned back on takes a few seconds to be online again; until then it is listed as on and
# can be turned off again by its remembered id.
COMING_SECONDS = 15.0


def _load_cg():
    cg = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
    cg.CGBeginDisplayConfiguration.argtypes = [ctypes.POINTER(_VP)]
    cg.CGCompleteDisplayConfiguration.argtypes = [_VP, _U32]
    cg.CGCancelDisplayConfiguration.argtypes = [_VP]
    cg.CGGetOnlineDisplayList.argtypes = [_U32, ctypes.POINTER(_U32), ctypes.POINTER(_U32)]
    cg.CGGetActiveDisplayList.argtypes = [_U32, ctypes.POINTER(_U32), ctypes.POINTER(_U32)]
    for name in ("CGDisplayVendorNumber", "CGDisplayModelNumber", "CGDisplayIsBuiltin", "CGDisplayIsOnline"):
        getattr(cg, name).argtypes = [_U32]
        getattr(cg, name).restype = _U32
    try:
        enable = cg.CGSConfigureDisplayEnabled
    except AttributeError:
        sky = ctypes.cdll.LoadLibrary("/System/Library/PrivateFrameworks/SkyLight.framework/SkyLight")
        enable = sky.SLSConfigureDisplayEnabled
    enable.argtypes = [_VP, _U32, ctypes.c_bool]
    enable.restype = ctypes.c_int32
    return cg, enable


def _product_name(display_id):
    """The display's own name ("GN10") from CoreDisplay, or "" (it is empty while the display is off)."""
    try:
        import objc
        core_display = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreDisplay.framework/CoreDisplay")
        create = core_display.CoreDisplay_DisplayCreateInfoDictionary
        create.restype = _VP
        create.argtypes = [_U32]
        pointer = create(display_id)
        if not pointer:
            return ""
        info = objc.objc_object(c_void_p=pointer)
        names = info.get("DisplayProductName") or {}
        return str(names.get("en_US") or next((value for value in names.values() if value), "") or "")
    except Exception:
        LOGGER.debug("display: no product name for %s", display_id, exc_info=True)
        return ""


class MacDisplays:
    def __init__(self, state_dir):
        self._path = Path(state_dir) / FILE_NAME
        self._cg, self._enable = _load_cg()
        self._names = {}        # display id -> product name, read once while the display is on
        self._off = self._load()
        self._coming = {}       # key -> (id, name, until): turned on, not online yet

    # The backend --------------------------------------------------------------------------------

    def displays(self):
        found = {}
        for display_id in self._online():
            if self._cg.CGDisplayIsBuiltin(display_id):
                continue
            key = self._key(display_id)
            if key is None:
                continue
            if display_id not in self._names:
                self._names[display_id] = _product_name(display_id)
            found[key] = model.Display(key, self._names[display_id], True)
            if key in self._off:
                self._forget(key)       # back on (cable replugged, restart, someone else): no longer ours
        for key, held in list(self._off.items()):
            if key in found:
                continue
            if self._key(held["id"]) != key:
                LOGGER.info("display: %s (id %s) is gone, forgetting it", key, held["id"])
                self._forget(key)
                continue
            found[key] = model.Display(key, held.get("name") or "", False)
        now = time.monotonic()
        for key, (display_id, name, until) in list(self._coming.items()):
            if key in found or now > until:
                del self._coming[key]
            else:
                found[key] = model.Display(key, name, True)
        return [found[key] for key in sorted(found)]

    def set_on(self, key, on):
        if on:
            held = self._off.get(key)
            if held is None:
                return False
            ok = self._configure(held["id"], True)
            if ok:
                self._coming[key] = (held["id"], held.get("name") or "", time.monotonic() + COMING_SECONDS)
                self._forget(key)
            return ok
        target = None
        for display_id in self._online():
            if not self._cg.CGDisplayIsBuiltin(display_id) and self._key(display_id) == key:
                target = (display_id, self._names.get(display_id) or _product_name(display_id))
                break
        if target is None and key in self._coming:
            target = self._coming[key][:2]
        if target is None:
            return False
        display_id, name = target
        if len([other for other in self._active() if other != display_id]) == 0:
            LOGGER.warning("display: not turning %s off, it is this Mac's only active display (lid closed?)", key)
            return False
        ok = self._configure(display_id, False)
        if ok:
            self._coming.pop(key, None)
            self._off[key] = {"id": int(display_id), "name": name}
            self._save()
        return ok

    # Inside -------------------------------------------------------------------------------------

    def _online(self):
        ids, count = (_U32 * 32)(), _U32()
        if self._cg.CGGetOnlineDisplayList(32, ids, ctypes.byref(count)) != 0:
            return []
        return list(ids[:count.value])

    def _active(self):
        ids, count = (_U32 * 32)(), _U32()
        if self._cg.CGGetActiveDisplayList(32, ids, ctypes.byref(count)) != 0:
            return []
        return list(ids[:count.value])

    def _key(self, display_id):
        vendor = self._cg.CGDisplayVendorNumber(display_id)
        if not vendor:
            return None
        return model.key_from_numbers(vendor, self._cg.CGDisplayModelNumber(display_id))

    def _configure(self, display_id, on):
        config = _VP()
        if self._cg.CGBeginDisplayConfiguration(ctypes.byref(config)) != 0:
            return False
        result = self._enable(config, display_id, bool(on))
        if result != 0:
            self._cg.CGCancelDisplayConfiguration(config)
            LOGGER.warning("display: CGSConfigureDisplayEnabled(%s, %s) returned %s", display_id, on, result)
            return False
        completed = self._cg.CGCompleteDisplayConfiguration(config, _CONFIGURE_FOR_SESSION)
        if completed != 0:
            LOGGER.warning("display: CGCompleteDisplayConfiguration returned %s", completed)
        return completed == 0

    def _forget(self, key):
        if self._off.pop(key, None) is not None:
            self._save()

    def _load(self):
        try:
            data = json.loads(self._path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return {}
        if not isinstance(data, dict):
            return {}
        return {key: {"id": int(value["id"]), "name": str(value.get("name") or "")} for key, value in data.items()
                if model.valid_key(key) and isinstance(value, dict) and isinstance(value.get("id"), int)}

    def _save(self):
        try:
            self._path.parent.mkdir(parents=True, exist_ok=True)
            temp = self._path.with_suffix(".tmp")
            temp.write_text(json.dumps(self._off, indent=1), encoding="utf-8")
            os.replace(temp, self._path)
        except OSError as error:
            LOGGER.warning("display: could not save %s: %s", self._path, error)


def backend(state_dir):
    """The Mac backend, or None when CoreGraphics' display switch is missing (logged)."""
    try:
        return MacDisplays(state_dir)
    except Exception:
        LOGGER.exception("display: shared displays unavailable on this Mac")
        return None
