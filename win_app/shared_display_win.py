"""Shared displays on Windows: list the connected monitors and turn one off or on (core/display/part.py).

Through the CCD API (QueryDisplayConfig / SetDisplayConfig), in the signed-in session where Beamer
runs. Off: the monitor's path is left out of the active configuration, the remaining displays moved
so one of them sits at (0,0) and becomes primary; nothing is saved to Windows' display database.
On: the extend topology from that database, which is the layout Windows keeps for this set of
monitors, so the monitor comes back where it was (primary included). That also turns on any other
connected monitor someone had turned off by hand; a known limit.
"""

import ctypes
import logging
import sys
from ctypes import wintypes

from core.display import model

LOGGER = logging.getLogger("beamer.display")

QDC_ALL_PATHS = 0x1
QDC_ONLY_ACTIVE_PATHS = 0x2
SDC_TOPOLOGY_EXTEND = 0x4
SDC_USE_SUPPLIED_DISPLAY_CONFIG = 0x20
SDC_APPLY = 0x80
SDC_ALLOW_CHANGES = 0x400
PATH_ACTIVE = 0x1
GET_TARGET_NAME = 2
OUTPUT_INTERNAL = 0x80000000


class LUID(ctypes.Structure):
    _fields_ = [("LowPart", wintypes.DWORD), ("HighPart", wintypes.LONG)]


class SOURCE(ctypes.Structure):
    _fields_ = [("adapterId", LUID), ("id", ctypes.c_uint32), ("modeInfoIdx", ctypes.c_uint32),
                ("statusFlags", ctypes.c_uint32)]


class TARGET(ctypes.Structure):
    _fields_ = [("adapterId", LUID), ("id", ctypes.c_uint32), ("modeInfoIdx", ctypes.c_uint32),
                ("outputTechnology", ctypes.c_uint32), ("rotation", ctypes.c_uint32), ("scaling", ctypes.c_uint32),
                ("refreshNum", ctypes.c_uint32), ("refreshDen", ctypes.c_uint32), ("scanLineOrdering", ctypes.c_uint32),
                ("targetAvailable", wintypes.BOOL), ("statusFlags", ctypes.c_uint32)]


class PATH(ctypes.Structure):
    _fields_ = [("sourceInfo", SOURCE), ("targetInfo", TARGET), ("flags", ctypes.c_uint32)]


class SOURCE_MODE(ctypes.Structure):
    _fields_ = [("width", ctypes.c_uint32), ("height", ctypes.c_uint32), ("pixelFormat", ctypes.c_uint32),
                ("x", ctypes.c_int32), ("y", ctypes.c_int32)]


class MODE_UNION(ctypes.Union):
    _fields_ = [("source", SOURCE_MODE), ("raw", ctypes.c_uint64 * 6)]


class MODE(ctypes.Structure):
    _fields_ = [("infoType", ctypes.c_uint32), ("id", ctypes.c_uint32), ("adapterId", LUID), ("u", MODE_UNION)]


class TARGET_NAME(ctypes.Structure):
    _fields_ = [("type", ctypes.c_uint32), ("size", ctypes.c_uint32), ("adapterId", LUID), ("id", ctypes.c_uint32),
                ("flags", ctypes.c_uint32), ("outputTechnology", ctypes.c_uint32), ("edidManufactureId", ctypes.c_uint16),
                ("edidProductCodeId", ctypes.c_uint16), ("connectorInstance", ctypes.c_uint32),
                ("monitorFriendlyDeviceName", wintypes.WCHAR * 64), ("monitorDevicePath", wintypes.WCHAR * 128)]


class WindowsDisplays:
    def __init__(self):
        assert ctypes.sizeof(PATH) == 72 and ctypes.sizeof(MODE) == 64
        self._user32 = ctypes.WinDLL("user32")

    def displays(self):
        paths, _modes = self._query(QDC_ALL_PATHS)
        found = {}
        for path in paths:
            if not path.targetInfo.targetAvailable or path.targetInfo.outputTechnology == OUTPUT_INTERNAL:
                continue
            key, name = self._target(path)
            if key is None:
                continue
            on = bool(path.flags & PATH_ACTIVE)
            held = found.get((key, path.targetInfo.adapterId.LowPart, path.targetInfo.id))
            found[(key, path.targetInfo.adapterId.LowPart, path.targetInfo.id)] = model.Display(
                key, name, on or (held is not None and held.on))
        return sorted(found.values(), key=lambda display: display.key)

    def set_on(self, key, on):
        return self._turn_on(key) if on else self._turn_off(key)

    # Inside -------------------------------------------------------------------------------------

    def _turn_off(self, key):
        paths, modes = self._query(QDC_ONLY_ACTIVE_PATHS)
        keep = [path for path in paths if self._target(path)[0] != key]
        if len(keep) == len(paths) or not keep:
            return False    # not on, or the last display: never leave Windows without a screen
        sources = [modes[path.sourceInfo.modeInfoIdx].u.source for path in keep]
        if not any(source.x == 0 and source.y == 0 for source in sources):
            dx, dy = sources[0].x, sources[0].y
            for path in keep:
                source = modes[path.sourceInfo.modeInfoIdx].u.source
                source.x, source.y = source.x - dx, source.y - dy
        result = self._user32.SetDisplayConfig(len(keep), (PATH * len(keep))(*keep), len(modes), (MODE * len(modes))(*modes),
                                               SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES)
        if result != 0:
            LOGGER.warning("display: SetDisplayConfig without %s returned %s", key, result)
        return result == 0

    def _turn_on(self, key):
        result = self._user32.SetDisplayConfig(0, None, 0, None, SDC_APPLY | SDC_TOPOLOGY_EXTEND)
        if result != 0:
            LOGGER.warning("display: SetDisplayConfig(extend) for %s returned %s", key, result)
            return False
        return any(display.key == key and display.on for display in self.displays())

    def _query(self, flags):
        for _attempt in range(3):   # the configuration can change between the two calls
            count_paths, count_modes = ctypes.c_uint32(), ctypes.c_uint32()
            result = self._user32.GetDisplayConfigBufferSizes(flags, ctypes.byref(count_paths), ctypes.byref(count_modes))
            if result != 0:
                raise OSError(f"GetDisplayConfigBufferSizes returned {result}")
            paths, modes = (PATH * count_paths.value)(), (MODE * count_modes.value)()
            result = self._user32.QueryDisplayConfig(flags, ctypes.byref(count_paths), paths, ctypes.byref(count_modes),
                                                     modes, None)
            if result == 0:
                return list(paths[:count_paths.value]), list(modes[:count_modes.value])
            if result != 122:   # ERROR_INSUFFICIENT_BUFFER
                raise OSError(f"QueryDisplayConfig returned {result}")
        raise OSError("QueryDisplayConfig kept changing")

    def _target(self, path):
        name = TARGET_NAME(type=GET_TARGET_NAME, size=ctypes.sizeof(TARGET_NAME), adapterId=path.targetInfo.adapterId,
                           id=path.targetInfo.id)
        if self._user32.DisplayConfigGetDeviceInfo(ctypes.byref(name)) != 0:
            return None, ""
        return model.key_from_device_path(name.monitorDevicePath), name.monitorFriendlyDeviceName


def backend(platform=None):
    """The Windows backend, or None elsewhere (Linux) or when the CCD API is missing (logged)."""
    if (platform or sys.platform) != "win32":
        return None
    try:
        return WindowsDisplays()
    except Exception:
        LOGGER.exception("display: shared displays unavailable on this PC")
        return None
