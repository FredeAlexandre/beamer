# SPDX-License-Identifier: GPL-3.0-only
"""Beamer Camera's elevated install, update, repair and removal (camera PHASE2 §9.2).

The page's buttons call `install` or `remove`. Both return at once; the work runs on a background thread
and `on_done` is called exactly once there with a `Result` (the caller marshals it to its UI thread).

- `install`: `beamer-audio.exe vcam-install --owner-sid <this user's SID>` through ShellExecuteEx "runas"
  (the UAC prompt). Install, update and repair are the same command: the helper copies the DLL it finds by
  the §9.2 lookup rule (its own `camera\\BeamerCameraSource.dll`, built with it, so its version is the
  helper's `kDllVersion`) to `%ProgramFiles%\\Beamer\\Camera\\<version>\\` and re-points the COM class.
- `remove`: `vcam-remove` unelevated (Remove() of this user's camera, best effort), then the elevated
  `vcam-uninstall` (COM class, HKLM key, Camera folders).

After an ok install the page calls `service.refresh_camera_install()`; nothing here talks to the helper
process Beamer runs.
"""

from __future__ import annotations

from dataclasses import dataclass
import logging
from pathlib import Path
import re
import subprocess
import sys
import threading
from typing import Callable, Optional


LOGGER = logging.getLogger(__name__)

# Exit codes of the helper's vcam verbs (camera PHASE2 §3.1).
EXIT_OK, EXIT_BAD_ARGS, EXIT_NOT_ELEVATED, EXIT_FAILED = 0, 2, 3, 4
ERROR_CANCELLED = 1223  # the user said No at the UAC prompt
_CODES = {EXIT_OK: "ok", EXIT_BAD_ARGS: "bad_args", EXIT_NOT_ELEVATED: "not_elevated", EXIT_FAILED: "failed"}
# Elevated verbs copy one small DLL and write a few keys; vcam-uninstall may stop FrameServer (~1 s).
# The bound only keeps on_done from never coming when something hangs; the process is left alone.
ELEVATED_TIMEOUT_S = 120.0
REMOVE_TIMEOUT_S = 30.0
SEE_MASK_NOCLOSEPROCESS, SEE_MASK_NOASYNC = 0x40, 0x100
_SID = re.compile(r"S-1-\d+(?:-\d+)+")


@dataclass(frozen=True)
class Result:
    ok: bool
    code: str  # "ok" | "cancelled" | "not_elevated" | "bad_args" | "failed" | "no_helper"


def install(on_done: Callable[[Result], None], helper: Optional[Path] = None) -> None:
    """Install, update or repair Beamer Camera (one UAC prompt). Returns at once."""
    _start("vcam-install", _install_steps, on_done, helper)


def remove(on_done: Callable[[Result], None], helper: Optional[Path] = None) -> None:
    """Remove this user's Beamer Camera, then unregister it elevated (one UAC prompt). Returns at once."""
    _start("vcam-remove", _remove_steps, on_done, helper)


def _install_steps(exe: Path) -> Result:
    sid = _current_user_sid()
    if not _SID.fullmatch(sid or ""):
        LOGGER.warning("vcam-install: this account's SID could not be read (%r)", sid)
        return Result(False, "failed")
    return _elevated(exe, ["vcam-install", "--owner-sid", sid])


def _remove_steps(exe: Path) -> Result:
    # Best effort: an old helper without the verb, or a camera already gone, must not keep the
    # elevated step from removing the registration.
    try:
        code = _run_unelevated(exe, ["vcam-remove"])
        if code != EXIT_OK:
            LOGGER.warning("vcam-remove exited %s; unregistering anyway", code)
    except (OSError, subprocess.SubprocessError) as exc:
        LOGGER.warning("vcam-remove did not run (%s); unregistering anyway", exc)
    return _elevated(exe, ["vcam-uninstall"])


def _elevated(exe: Path, arguments: list) -> Result:
    try:
        code = _shell_execute_elevated(exe, subprocess.list2cmdline(arguments))
    except OSError as exc:
        if _winerror(exc) == ERROR_CANCELLED:
            LOGGER.info("%s: administrator permission was cancelled", arguments[0])
            return Result(False, "cancelled")
        LOGGER.warning("%s could not start elevated: %s", arguments[0], exc)
        return Result(False, "failed")
    result = Result(code == EXIT_OK, _CODES.get(code, "failed"))
    LOGGER.log(logging.INFO if result.ok else logging.WARNING, "%s exited %s (%s)", arguments[0], code, result.code)
    return result


def _start(name: str, steps: Callable[[Path], Result], on_done: Callable[[Result], None],
           helper: Optional[Path]) -> None:
    def work():
        try:
            exe = _helper_exe(helper)
            result = steps(exe) if exe is not None else Result(False, "no_helper")
        except Exception:
            LOGGER.exception("%s failed", name)
            result = Result(False, "failed")
        try:
            on_done(result)
        except Exception:
            LOGGER.exception("%s: on_done raised", name)

    _spawn(work, name)


def _spawn(target: Callable[[], None], name: str) -> None:
    threading.Thread(target=target, name=f"beamer-{name}", daemon=True).start()


def _helper_exe(helper: Optional[Path]) -> Optional[Path]:
    """The Windows helper: the one given, else the one Beamer runs (beside the frozen Beamer.exe, or the
    dev tree's build output). The dev switch's `helper` is not used: it may name the null helper, which
    has no vcam verbs."""
    if not _is_windows():
        return None
    if helper is None:
        from core.audio import helper as audio_helper
        argv = audio_helper.locate()
        helper = Path(argv[0]) if argv else None
    if helper is None or not Path(helper).is_file():
        LOGGER.warning("vcam: no beamer-audio.exe at %s", helper)
        return None
    return Path(helper).resolve()


def _winerror(exc: BaseException) -> Optional[int]:
    for name in ("winerror", "errno"):
        value = getattr(exc, name, None)
        if isinstance(value, int):
            return value
    args = getattr(exc, "args", ())
    return args[0] if args and isinstance(args[0], int) else None


# OS seams (replaced by the tests).

def _is_windows() -> bool:
    return sys.platform == "win32"


def _current_user_sid() -> str:
    """The SID of the account this (unelevated) Beamer runs as: the camera's owner, even when an
    over-the-shoulder administrator answers the UAC prompt."""
    import win32api
    import win32con
    import win32security
    token = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_QUERY)
    try:
        sid, _ = win32security.GetTokenInformation(token, win32security.TokenUser)
    finally:
        token.Close()
    return win32security.ConvertSidToStringSid(sid)


def _shell_execute_elevated(exe: Path, parameters: str) -> int:
    """Runs `exe parameters` through the UAC prompt, hidden, waits for it and returns its exit code.
    Raises OSError (winerror 1223 when the prompt was refused)."""
    import pythoncom
    import pywintypes
    import win32api
    import win32event
    import win32process
    from win32com.shell import shell
    pythoncom.CoInitializeEx(pythoncom.COINIT_APARTMENTTHREADED)
    try:
        try:
            started = shell.ShellExecuteEx(
                fMask=SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC, lpVerb="runas",
                lpFile=str(exe), lpParameters=parameters, lpDirectory=str(exe.parent), nShow=0)
        except pywintypes.error as exc:
            raise OSError(exc.winerror, exc.strerror) from exc
        process = started.get("hProcess")
        if process is None:
            raise OSError("Windows started no process for the elevated helper")
        try:
            if win32event.WaitForSingleObject(process, int(ELEVATED_TIMEOUT_S * 1000)) != win32event.WAIT_OBJECT_0:
                raise OSError(f"the elevated helper did not finish within {ELEVATED_TIMEOUT_S:.0f} s")
            return win32process.GetExitCodeProcess(process)
        finally:
            win32api.CloseHandle(process)
    finally:
        pythoncom.CoUninitialize()


def _run_unelevated(exe: Path, arguments: list) -> int:
    from core.audio import helper as audio_helper
    completed = subprocess.run(
        [str(exe), *arguments], cwd=str(exe.parent), env=audio_helper.helper_environment(),
        stdin=subprocess.DEVNULL, capture_output=True, timeout=REMOVE_TIMEOUT_S,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
    return completed.returncode
