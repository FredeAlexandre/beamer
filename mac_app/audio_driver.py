# SPDX-License-Identifier: GPL-3.0-only
"""The Beamer Microphone driver (BeamerAudio.driver): what the app carries, what is installed, and
how to install or update it (PHASE2 §10.1).

The app carries the driver in Contents/Resources/audio/ beside install-driver.sh. Installing it
needs root, so it is never done silently:

- dev Macs have /usr/local/libexec/beamer-hal with a NOPASSWD sudo rule; it installs the bundle
  that native/audio/build-mac.sh staged in ~/Developer/beamer-hal-staging. It is used whenever it
  exists, so a dev Mac never shows an administrator prompt.
- everywhere else osascript runs the bundled install-driver.sh "with administrator privileges",
  which shows the standard macOS administrator prompt.

Either way coreaudiod restarts, so every Mac app loses its sound for about a second. The Audio
page warns first and offers the install only while no stream runs (`offer`). `install` blocks
until the person answers the prompt: call it from a worker thread, never the main thread.

Nothing here holds words for the screen: the page turns the states and results into its own
translated text, and passes the translated prompt for the administrator dialog.
"""

from __future__ import annotations

import os
import plistlib
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


BUNDLE_NAME = "BeamerAudio.driver"
BUNDLE_ID = "com.beamer.audio.driver"
INSTALL_SCRIPT = "install-driver.sh"
INSTALLED = Path("/Library/Audio/Plug-Ins/HAL") / BUNDLE_NAME
BEAMER_HAL = Path("/usr/local/libexec/beamer-hal")
SUDO = "/usr/bin/sudo"
OSASCRIPT = "/usr/bin/osascript"
BASH = "/bin/bash"
# Long enough for a person to read the administrator prompt and type a password.
PROMPT_TIMEOUT = 600.0
HAL_TIMEOUT = 60.0

# Driver states.
NO_BUNDLE = "no_bundle"   # this build carries no driver: nothing to offer
MISSING = "missing"       # not installed: offer "install"
OUTDATED = "outdated"     # installed, older than the bundled one: offer "update"
CURRENT = "current"       # installed, same version
NEWER = "newer"           # installed, newer than the bundled one (a later Beamer was here): left alone

# Install paths.
METHOD_HAL = "beamer-hal"
METHOD_OSASCRIPT = "osascript"

# Install results.
INSTALLED_OK = "installed"
CANCELLED = "cancelled"     # the person dismissed the administrator prompt
BUSY = "busy"               # a stream is running: coreaudiod must not restart under it
UNAVAILABLE = "unavailable" # no bundled driver or installer script
FAILED = "failed"


@dataclass(frozen=True)
class DriverStatus:
    state: str
    bundled_version: str | None = None
    installed_version: str | None = None


@dataclass(frozen=True)
class InstallResult:
    result: str
    method: str | None = None
    detail: str = ""

    @property
    def ok(self) -> bool:
        return self.result == INSTALLED_OK


def _repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def resources_dir() -> Path | None:
    """Contents/Resources of the running app, or None when running from source."""
    if not getattr(sys, "frozen", False):
        return None
    resource_path = os.environ.get("RESOURCEPATH")
    if resource_path:
        return Path(resource_path)
    return Path(sys.executable).resolve().parent.parent / "Resources"


def audio_dir() -> Path:
    """Where the driver and its installer live: in the app, or the native build output in a checkout."""
    resources = resources_dir()
    if resources is not None:
        return resources / "audio"
    return _repo_root() / "native" / "audio" / "build" / "mac"


def bundled_driver(folder: Path | None = None) -> Path | None:
    path = (folder or audio_dir()) / BUNDLE_NAME
    return path if path.is_dir() else None


def install_script(folder: Path | None = None) -> Path | None:
    """install-driver.sh; it installs the BeamerAudio.driver beside it, so both share a folder."""
    path = (folder or audio_dir()) / INSTALL_SCRIPT
    return path if path.is_file() else None


def bundle_version(bundle: Path | None) -> str | None:
    """CFBundleVersion of a BeamerAudio.driver, or None if absent, unreadable or not ours."""
    if bundle is None:
        return None
    try:
        with open(bundle / "Contents" / "Info.plist", "rb") as handle:
            info = plistlib.load(handle)
    except (OSError, plistlib.InvalidFileException, ValueError):
        return None
    if not isinstance(info, dict) or info.get("CFBundleIdentifier") != BUNDLE_ID:
        return None
    version = info.get("CFBundleVersion")
    return str(version).strip() if version is not None and str(version).strip() else None


def parse_version(text: str | None) -> tuple[int, ...] | None:
    """"12", "1.2.3" -> (1, 2, 3). Anything else is not a version we can order."""
    if text is None:
        return None
    parts = text.strip().split(".")
    if not parts or not all(part.isdigit() for part in parts):
        return None
    numbers = [int(part) for part in parts]
    while len(numbers) > 1 and numbers[-1] == 0:
        numbers.pop()
    return tuple(numbers)


def compare_versions(a: str | None, b: str | None) -> int | None:
    """-1, 0 or 1 as a < b, a == b, a > b; None when either side cannot be ordered."""
    left, right = parse_version(a), parse_version(b)
    if left is None or right is None:
        return None
    return (left > right) - (left < right)


def status(bundled: Path | None = None, installed: Path = INSTALLED, *, folder: Path | None = None) -> DriverStatus:
    bundled = bundled if bundled is not None else bundled_driver(folder)
    bundled_version = bundle_version(bundled)
    if bundled_version is None:
        return DriverStatus(NO_BUNDLE, None, bundle_version(installed) if installed.is_dir() else None)
    if not installed.is_dir():
        return DriverStatus(MISSING, bundled_version, None)
    installed_version = bundle_version(installed)
    order = compare_versions(installed_version, bundled_version)
    if order is None:
        # Installed but unreadable, foreign or with a version we cannot order: replace it.
        state = OUTDATED
    elif order < 0:
        state = OUTDATED
    elif order > 0:
        state = NEWER
    else:
        state = CURRENT
    return DriverStatus(state, bundled_version, installed_version)


def offer(driver: DriverStatus, streams_running: bool) -> str | None:
    """What the page may offer now: "install", "update" or None."""
    if streams_running:
        return None
    return {MISSING: "install", OUTDATED: "update"}.get(driver.state)


def install_method(beamer_hal: Path = BEAMER_HAL) -> str:
    """beamer-hal where the dev root helper exists, else the administrator prompt."""
    return METHOD_HAL if beamer_hal.is_file() and os.access(beamer_hal, os.X_OK) else METHOD_OSASCRIPT


def _applescript_string(text: str) -> str:
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def install_command(method: str, script: Path | None = None, prompt: str | None = None,
                    beamer_hal: Path = BEAMER_HAL) -> list[str]:
    if method == METHOD_HAL:
        # -n: never ask for a password; the NOPASSWD rule names exactly this command.
        return [SUDO, "-n", str(beamer_hal), "install"]
    if method != METHOD_OSASCRIPT or script is None:
        raise ValueError(f"no install command for {method!r} with script {script}")
    # do shell script runs /bin/sh as root; quoted form of keeps the path one shell word.
    source = (f"do shell script {_applescript_string(BASH + ' ')} & quoted form of "
              f"{_applescript_string(str(script))} with administrator privileges")
    if prompt:
        source += f" with prompt {_applescript_string(prompt)}"
    return [OSASCRIPT, "-e", source]


def install(*, streams_running: bool, prompt: str | None = None, folder: Path | None = None,
            beamer_hal: Path = BEAMER_HAL, runner=subprocess.run) -> InstallResult:
    """Install or update the driver; blocks until done (or the prompt is answered)."""
    if streams_running:
        return InstallResult(BUSY)
    method = install_method(beamer_hal)
    script = None
    if method == METHOD_OSASCRIPT:
        script = install_script(folder)
        if script is None or bundled_driver(script.parent) is None:
            return InstallResult(UNAVAILABLE, method)
    command = install_command(method, script, prompt, beamer_hal)
    try:
        completed = runner(command, capture_output=True, text=True,
                           timeout=HAL_TIMEOUT if method == METHOD_HAL else PROMPT_TIMEOUT)
    except (OSError, subprocess.SubprocessError) as error:
        return InstallResult(FAILED, method, str(error))
    detail = ((completed.stdout or "") + (completed.stderr or "")).strip()
    if completed.returncode == 0:
        return InstallResult(INSTALLED_OK, method, detail)
    if method == METHOD_OSASCRIPT and "-128" in (completed.stderr or ""):
        return InstallResult(CANCELLED, method, detail)
    return InstallResult(FAILED, method, detail)
