# SPDX-License-Identifier: GPL-3.0-only
"""What Beamer Camera shows on a PC while no live picture feeds it (dev/camera/PHASE2.md §7.5, §8.4).

Pure. `placeholder_texts` builds the title and detail line of every placeholder state from a camera
snapshot; `CameraPart` sends them to the PC helper (`vcam placeholders`), which renders them into the
camera picture. Wording reviewed by CW7 with the page's status lines (§8.4); the page may reuse this
function but never forks it.

English in code, every string through `core.i18n` at call time. Machine and camera names go into a
template as `{fields}`, never through `tr()`.
"""

from typing import Callable, Optional

from core.camera.model import CameraSnapshot, camera_by_key
from core.i18n import N_, tr

# Why a stream stopped, as a detail line (the `error` placeholder's `<reason>`): engine and helper
# error codes (§2.4, §3.3) and `camera_opened` / `camera_close` codes (§5.2).
REASONS = {
    "no_camera": N_("The camera is not available"),
    "camera_gone": N_("The camera is not available"),
    "permission": N_("Camera access is not allowed"),
    "not_installed": N_("Beamer Camera is not installed"),
    "outdated": N_("Beamer Camera needs an update"),
    "unsupported": N_("That machine cannot send its camera"),
    "decoder": N_("This PC could not show the picture"),
    "no_receiver": N_("The picture did not reach this PC"),
    "helper": N_("Beamer's media helper stopped"),
    "not_source": N_("The camera choice changed"),
    "internal": N_("Something went wrong in Beamer"),
}
GENERIC_REASON = N_("Open Beamer for details")


def reason_text(code: Optional[str]) -> str:
    """The detail line for an error code; a generic line for an unknown one."""
    return tr(REASONS.get(code or "", GENERIC_REASON))


def _source_text(snap: CameraSnapshot, name: Callable[[str], str]) -> str:
    machine = snap.route.camera.machine
    return name(machine) if machine is not None else tr("another machine")


def _camera_text(snap: CameraSnapshot) -> str:
    """The camera by name: the dev test source, the route's camera, else the preferred camera the
    route follows when that machine's list has it (as the page names it), else "Default camera"."""
    machine = snap.route.camera.machine
    held = snap.machines.get(machine) if machine is not None else None
    if held is not None and held.test_source:
        return tr("Test source")
    device = snap.route.camera.device
    if device is not None and device.name:
        return device.name
    if device is None and held is not None:
        preferred = camera_by_key(held.cameras, held.preferred)
        if preferred is not None and preferred.name:
            return preferred.name
    return tr("Default camera")


def _denied_text(snap: CameraSnapshot, source: str) -> str:
    """Where the camera machine's owner allows the camera: macOS's System Settings pane (camera access
    is a macOS permission; a machine of unknown platform is taken as a Mac), not Beamer."""
    machine = snap.route.camera.machine
    held = snap.machines.get(machine) if machine is not None else None
    if held is not None and held.platform and held.platform != "macos":
        return tr("Allow it in the system settings on {machine}").format(machine=source)
    return tr("Allow it on {machine} in System Settings > Privacy & Security > Camera").format(machine=source)


def _error_reason(snap: CameraSnapshot) -> Optional[str]:
    """The newest error code that explains a stop: the overall status's, else this machine's sink's,
    else the camera machine's capture error."""
    if snap.status.state == "error" and snap.status.reason:
        return snap.status.reason
    own = snap.sinks.get(snap.own)
    if own is not None and own.state == "error" and own.reason:
        return own.reason
    machine = snap.route.camera.machine
    held = snap.machines.get(machine) if machine is not None else None
    if held is not None and held.capture.state == "failed":
        return held.capture.error
    return None


def placeholder_texts(snap: CameraSnapshot, name: Callable[[str], str]) -> dict:
    """{"<state>": {"title": ..., "line": ...}} for every placeholder state (§4.6.4, §8.4).
    `name` is `MediaContext.machine_name`."""
    source = _source_text(snap, name)
    camera = _camera_text(snap)
    reason = _error_reason(snap)

    def entry(title, line):
        return {"title": title, "line": line}
    return {
        "off": entry(tr("Beamer Camera is off"), tr("Choose a camera in Beamer")),
        "waiting": entry(tr("Waiting for {machine}").format(machine=source), camera),
        "waiting_permission": entry(tr("Waiting for camera access"),
                                    tr("Allow it in Beamer on {machine}").format(machine=source)),
        "sharing_off": entry(tr("Camera turned off on {machine}").format(machine=source),
                             tr("Turn it on in Beamer on {machine}").format(machine=source)),
        "denied": entry(tr("{machine} has not allowed the camera").format(machine=source),
                        _denied_text(snap, source)),
        "error": entry(tr("Beamer Camera stopped"), reason_text(reason)),
        "starting": entry(tr("Starting {camera}").format(camera=camera),
                          tr("on {machine}").format(machine=source)),
        "reconnecting": entry(tr("Reconnecting to {machine}").format(machine=source), camera),
    }
