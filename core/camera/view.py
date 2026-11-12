# SPDX-License-Identifier: GPL-3.0-only
"""The Camera module's model and the map's camera extras, shared by the Mac page and the Qt page
(dev/camera/PHASE2.md §8.2-§8.6).

Pure: no AppKit, no Qt, no threads. `camera_section(snapshot, audio, own_label, platform)` turns the
camera snapshot (core.camera.model, §7.2) and the audio snapshot (for machine names and who is
online) into everything the Camera module shows, already worded in the current language;
`map_extras(snapshot)` is what the shared machine map adds for the camera (a badge on the camera
machine's tile, a line on each tile whose apps use Beamer Camera, camera arrows), passed to
`core.audio.view.page_model(..., camera=...)`. `menu_model` is the Mac menu bar's camera part (§8.5).

The placeholder pictures' texts are not built here: `core/camera/texts.py` builds them and the part
sends them to the PC helper (§7.5).

Tones are core.audio.view's palette names (`ink_3` muted, `ink_2` plain, `signal` live, `amber`
waiting, `fault`), the same in both apps. A page acts through the `apply_*` helpers, which call the
AudioService's camera setters (§7.1); actions an app runs itself (a settings pane, the PC's elevated
install) return False from `apply_camera_action` and are named by the `ACTION_*` constants.

This file is a contract (PHASE2 §12): CW8's Qt module codes against these dataclasses.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Mapping, NamedTuple, Optional

from core.audio.view import Choice, Notice, _Names, _choice_key, _title
from core.camera import model
from core.camera.model import CameraRef, CameraSnapshot, CameraStatus, MachineCamera
from core.i18n import N_, joined, tr, trn

OFF = "off"
QUIET, PLAIN, LIVE, WAIT, FAULT = "ink_3", "ink_2", "signal", "amber", "fault"

# Tone of the status line by state (§8.4).
TONES = {
    "off": QUIET, "idle": PLAIN, "starting": LIVE, "streaming": LIVE, "reduced": WAIT, "sharing_off": QUIET,
    "waiting_permission": WAIT, "denied": FAULT, "waiting_device": WAIT, "no_camera": WAIT,
    "waiting_machine": WAIT, "unreachable": WAIT, "no_direct_path": WAIT, "not_installed": WAIT,
    "outdated": WAIT, "broken": WAIT, "no_virtual_camera": WAIT, "error": FAULT,
}
LIVE_STATES = ("streaming", "reduced")
STREAMS_RUNNING = ("opening", "starting", "running")

# Actions a line, a row or a notice may carry. The service runs the first group (apply_camera_action
# returns True); the app runs the rest itself.
ACTION_SHARE = "share_camera"                     # service.set_camera_sharing(True)
ACTION_STOP_SHARING = "stop_sharing"              # service.set_camera_sharing(False)
ACTION_ALLOW = "allow_camera"                     # service.ask_camera_permission(): the macOS prompt
ACTION_RETRY = "retry_camera"                     # the same route again: a new stamp re-opens (§6.5)
ACTION_RETRY_HELPER = "retry_helper"              # service.retry_helper()
ACTION_PRIVACY = "open_camera_privacy"            # Mac: System Settings, Privacy & Security, Camera
ACTION_CAMERA_SETTINGS = "open_camera_settings"   # PC: ms-settings:camera (§8.6)
ACTION_INSTALL = "install_vcam"                   # PC: win_app/vcam_install.install (UAC), §9.2
ACTION_UPDATE = "update_vcam"                     #     the same call
ACTION_REPAIR = "repair_vcam"                     #     the same call
ACTION_REMOVE = "remove_vcam"                     # PC: win_app/vcam_install.remove (UAC), §9.2
SERVICE_ACTIONS = (ACTION_SHARE, ACTION_STOP_SHARING, ACTION_ALLOW, ACTION_RETRY, ACTION_RETRY_HELPER)
APP_ACTIONS = (ACTION_PRIVACY, ACTION_CAMERA_SETTINGS, ACTION_INSTALL, ACTION_UPDATE, ACTION_REPAIR, ACTION_REMOVE)
# Where the app-run actions go.
PRIVACY_URL = "x-apple.systempreferences:com.apple.preference.security?Privacy_Camera"
CAMERA_SETTINGS_URL = "ms-settings:camera"

QUALITY_KEYS = model.QUALITIES

# Error codes a status may carry as its `reason` (§2.4, §3.3, §5.2), as words inside a sentence.
REASONS = {
    "no_camera": N_("the camera is not available"),
    "camera_gone": N_("the camera is not available"),
    "permission": N_("camera access is not allowed"),
    "not_installed": N_("Beamer Camera is not installed"),
    "outdated": N_("Beamer Camera needs an update"),
    "unsupported": N_("that machine cannot send its camera"),
    "decoder": N_("the picture could not be shown"),
    "no_receiver": N_("the picture did not arrive"),
    "helper": N_("Beamer's media helper stopped"),
    "not_source": N_("the camera choice changed"),
    "busy": N_("another app holds the camera"),
    "internal": N_("something went wrong in Beamer"),
}


class Row(NamedTuple):
    """One row about this machine (§8.3, "This Mac" / "This PC"): a NamedTuple, so it is also the
    plain `(label, value, tone, action, action_label)` tuple of §8.2."""
    label: str                      # "Camera sharing"
    value: str                      # "On"
    tone: str
    action: Optional[str] = None    # ACTION_*
    action_label: str = ""          # "Stop Sharing"


@dataclass(frozen=True)
class CameraSection:
    label: str                      # "Camera your apps see"
    choices: tuple                  # core.audio.view.Choice: "Off", headers per machine, cameras (+ "Default camera")
    selected: str
    line: str                       # status sentence (§8.4)
    tone: str                       # its tone (TONES)
    detail: str                     # "1280x720 · 30 fps · 72 ms" while streaming, where to allow it if denied
    quality_label: str              # "Quality"
    quality_choices: tuple          # Choice "720p", "1080p"
    quality_selected: str
    hint: str                       # "Apps on a PC choose 'Beamer Camera'. Macs cannot ..."
    help: str                       # PC only (§8.6): the multi-app help line, else ""
    help_action: Optional[str]      # ACTION_CAMERA_SETTINGS with `help`, else None
    this_machine: tuple             # Row, about this machine
    notices: tuple                  # core.audio.view.Notice
    # Additions to §8.2 (CW7 milestone 1), all with defaults.
    shown: bool = True              # False: hide the whole module (no machine has a camera side, §8.3)
    quality_enabled: bool = True    # False while Off (§8.3)
    action: Optional[str] = None    # the status line's own action: Retry on `error` (§8.4)
    action_label: str = ""
    help_action_label: str = ""     # "Camera settings"
    this_machine_label: str = ""    # "This Mac" / "This PC": the rows' heading; "" when there are none
    state: str = "off"              # the status's state, for tests and screen readers


@dataclass(frozen=True)
class CameraMapExtras:
    source: Optional[str]           # the camera machine (route), None while Off
    source_line: str                # its tile's badge: "MacBook Air Camera"
    watching: Mapping[str, str]     # sink machine -> "Teams uses the camera" / "Camera ready"
    arrows: tuple                   # (source, target, state "active"|"waiting")
    live: bool = False              # the camera is sent to at least one machine: badge in the accent colour


@dataclass(frozen=True)
class CameraMenu:
    """The Mac menu bar's camera items (§8.5)."""
    shown: bool                     # this Mac has `camera_capture` (the page checks the feature flag)
    line: str                       # "Camera: Teams on lebig" while this Mac sends video, else ""
    sharing_title: str              # "Stop Sharing the Camera" / "Share the Camera"
    sharing_on: bool
    allow_shown: bool               # "Allow Camera Access…" while needs_permission
    allow_title: str = ""


def sharing_title(on: bool) -> str:
    """The menu's sharing item's words while sharing is `on` (or off)."""
    return tr("Stop Sharing the Camera") if on else tr("Share the Camera")


class SharingItem:
    """The Mac menu's sharing item acts on its label (R4 #3): a click sends the action the shown title
    promised, never the opposite of state read at click time, and the title turns at once to the
    other action. Until the snapshot agrees (or HOLD seconds pass), the menu keeps showing what the
    click asked for, so a status tick that still carries the old state does not bring the old title
    back. Used on the AppKit main thread only."""

    HOLD = 2.0

    def __init__(self):
        self.action: Optional[str] = None      # ACTION_STOP_SHARING / ACTION_SHARE of the shown title
        self._pending = None                   # (sharing on asked for, until)

    def title(self) -> str:
        return sharing_title(self.action == ACTION_STOP_SHARING)

    def show(self, menu: CameraMenu, now: float) -> str:
        """The title for `menu` (a status tick); remembers its action."""
        on = menu.sharing_on
        if self._pending is not None:
            wanted, until = self._pending
            if wanted == on or now >= until:
                self._pending = None
            else:
                on = wanted
        self.action = ACTION_STOP_SHARING if on else ACTION_SHARE
        return sharing_title(on)

    def click(self, now: float) -> Optional[str]:
        """The action the clicked title promised (None before any title was shown); the title then
        shows the other action."""
        action = self.action
        if action is None:
            return None
        on = action == ACTION_SHARE
        self._pending = (on, now + self.HOLD)
        self.action = ACTION_STOP_SHARING if on else ACTION_SHARE
        return action


# Words.

def _reason(code: Optional[str]) -> str:
    text = REASONS.get(code or "")
    return tr(text) if text else ""


def _ids(text) -> list:
    return [part for part in str(text or "").split() if part]


def _capitalised(text: str) -> str:
    return text[:1].upper() + text[1:] if text else text


def _machine(snap: CameraSnapshot, ident: Optional[str]) -> Optional[MachineCamera]:
    return snap.machines.get(ident) if ident is not None else None


def _source_camera(snap: CameraSnapshot):
    """The Camera the route resolves to on its machine, or None (followed preferred camera absent,
    unplugged, or the route is off)."""
    held = _machine(snap, snap.route.camera.machine)
    if held is None:
        return None
    if snap.route.camera.device is None:
        return model.camera_by_key(held.cameras, held.preferred)
    return model.resolve_camera(snap.route.camera.device, held.cameras)


def camera_name(snap: CameraSnapshot) -> str:
    """The active camera by name: the dev test source, the resolved camera, the route's name for an
    unplugged one, else "Default camera"."""
    held = _machine(snap, snap.route.camera.machine)
    if held is not None and held.test_source:
        return tr("Test source")
    found = _source_camera(snap)
    if found is not None:
        return found.name
    device = snap.route.camera.device
    if device is not None and device.name:
        return device.name
    return tr("Default camera")


def _app_phrase(snap: CameraSnapshot, names, target: str, capital: bool) -> str:
    """"Microsoft Teams on lebig" / "an app on lebig"."""
    held = _machine(snap, target)
    app = held.virtual_camera.app if held is not None else None
    if app:
        return tr("{app} on {machine}").format(app=app, machine=names(target))
    text = tr("an app on {machine}").format(machine=names(target))
    return _capitalised(text) if capital else text


def _targets(snap: CameraSnapshot, *states) -> list:
    """The sinks in `states`: the status's own `to` first (planner.overall), then any sink in those
    states, in a stable order."""
    found = [target for target in _ids(snap.status.params.get("to"))
             if snap.sinks.get(target, snap.status).state in states]
    found += sorted(target for target, status in snap.sinks.items() if status.state in states and target not in found)
    return found


def _source_name(snap: CameraSnapshot, names, platform: str, sentence_start: bool) -> str:
    """The camera machine's name; this machine's own as "this Mac" inside a sentence."""
    source = snap.route.camera.machine
    if source == snap.own and not sentence_start:
        return _this(platform)
    return names(source) if source is not None else tr("another machine")


def _this(platform: str) -> str:
    return tr("this PC") if platform == "windows" else tr("this Mac")


def _using_line(snap: CameraSnapshot, names, targets) -> str:
    phrases = [_app_phrase(snap, names, target, index == 0) for index, target in enumerate(targets)]
    return trn("{apps} is using it.", "{apps} are using it.", len(phrases)).format(apps=joined(phrases))


class _InSentence:
    """Machine names inside a sentence: this machine is "this PC" / "this Mac", not its label."""

    def __init__(self, snap: CameraSnapshot, names, platform: str):
        self.snap, self.names, self.platform = snap, names, platform

    def __call__(self, ident: Optional[str]) -> str:
        return _this(self.platform) if ident == self.snap.own else self.names(ident)


def _status_line(snap: CameraSnapshot, names, platform: str):
    """(line, tone, action, action_label) for the overall status (§8.4)."""
    names = _InSentence(snap, names, platform)
    status = snap.status
    state = status.state
    source = snap.route.camera.machine
    if source is None or state == "off":
        return tr("Off. Apps that open Beamer Camera see a 'camera off' picture."), QUIET, None, ""
    own_source = source == snap.own
    s_name = _source_name(snap, names, platform, False)
    tone = TONES.get(state, PLAIN)
    target_ids = _ids(status.params.get("to"))
    t_names = joined(names(target) for target in target_ids)
    if state == "idle":
        idle = _targets(snap, "idle") or target_ids
        if not idle:
            return tr("Ready. No app is using Beamer Camera."), tone, None, ""
        return tr("Ready. No app on {machines} is using Beamer Camera.").format(
            machines=joined(names(target) for target in idle)), tone, None, ""
    if state == "starting":
        targets = _targets(snap, "starting") or target_ids
        phrases = [_app_phrase(snap, names, target, False) for target in targets]
        return tr("Starting for {apps}…").format(apps=joined(phrases)), tone, None, ""
    if state in LIVE_STATES:
        targets = _targets(snap, *LIVE_STATES) or target_ids
        line = _using_line(snap, names, targets) if targets else tr("An app is using it.")
        if state == "reduced" or any(snap.sinks.get(target, status).state == "reduced" for target in targets):
            return line + " " + tr("Reduced quality: the network is slow."), WAIT, None, ""
        return line, tone, None, ""
    if state == "sharing_off":
        if own_source:
            return tr("Camera sharing is off on this Mac."), tone, None, ""
        return tr("Turned off on {machine}.").format(machine=s_name), tone, None, ""
    if state == "waiting_permission":
        if own_source:
            return tr("Allow camera access on this Mac."), tone, None, ""
        return tr("Allow camera access on {machine}.").format(machine=s_name), tone, None, ""
    if state == "denied":
        if own_source:
            return tr("This Mac does not allow Beamer to use its camera."), tone, None, ""
        return tr("{machine} does not allow Beamer to use its camera.").format(machine=names(source)), tone, None, ""
    if state == "waiting_device":
        device = status.params.get("device") or camera_name(snap)
        return tr("{camera} is not connected to {machine}.").format(camera=device, machine=s_name), tone, None, ""
    if state == "no_camera":
        if own_source:
            return tr("This Mac has no camera."), tone, None, ""
        return tr("{machine} has no camera.").format(machine=names(source)), tone, None, ""
    if state == "waiting_machine":
        if status.reason == "starting":
            return _capitalised(tr("{machine} is starting its camera side.").format(machine=names(source))), tone, None, ""
        return _capitalised(tr("{machine} is offline.").format(machine=names(source))), tone, None, ""
    if state == "unreachable":
        targets = _targets(snap, "unreachable") or target_ids
        return _capitalised(tr("{source} and {targets} are not linked directly.").format(
            source=names(source), targets=joined(names(target) for target in targets) or tr("the machine using it")
        )), tone, None, ""
    if state == "no_direct_path":
        targets = _targets(snap, "no_direct_path") or target_ids
        return tr("Video needs a direct network path between {source} and {targets}.").format(
            source=s_name, targets=joined(names(target) for target in targets) or tr("the machine using it")
        ), tone, None, ""
    if state in ("not_installed", "outdated", "broken"):
        targets = _targets(snap, state) or target_ids
        where = joined(names(target) for target in targets) or t_names
        text = {"not_installed": tr("Beamer Camera is not installed on {machines}."),
                "outdated": tr("Beamer Camera needs an update on {machines}."),
                "broken": tr("Beamer Camera is damaged on {machines}.")}[state]
        return text.format(machines=where), tone, None, ""
    if state == "no_virtual_camera":
        return tr("No linked PC can show it: apps on a PC use Beamer Camera."), tone, None, ""
    # error, and any state this version does not know.
    reason = _reason(status.reason)
    if reason:
        line = tr("The camera stopped on {machine}: {reason}.").format(machine=s_name, reason=reason)
    else:
        line = tr("The camera stopped on {machine}.").format(machine=s_name)
    if status.reason == "helper" and own_source:
        return line, FAULT, ACTION_RETRY_HELPER, tr("Retry")
    return line, FAULT, ACTION_RETRY, tr("Retry")


def _detail(snap: CameraSnapshot, names) -> str:
    """"1280x720 · 30 fps · 72 ms" for the first live stream, with the link fallback said; while the
    camera machine denies access, where its owner allows it (macOS's System Settings, not Beamer)."""
    if snap.status.state == "denied":
        source = snap.route.camera.machine
        if source == snap.own:
            return tr("Allow it in System Settings > Privacy & Security > Camera.")
        held = _machine(snap, source)
        name = names(source) if source is not None else tr("another machine")
        if held is not None and held.platform and held.platform != "macos":
            return tr("Allow it in the system settings on {machine}.").format(machine=name)
        return tr("Allow it on {machine} in System Settings > Privacy & Security > Camera.").format(machine=name)
    if snap.status.state not in LIVE_STATES:
        return ""
    live = [stream for stream in snap.streams if stream.state == "running"]
    if not live:
        return ""
    stream = live[0]
    parts = []
    if stream.w and stream.h:
        parts.append(tr("{width}x{height}").format(width=stream.w, height=stream.h))
    if stream.fps is not None:
        parts.append(tr("{fps} fps").format(fps=round(stream.fps)))
    if stream.delay_ms is not None:
        parts.append(tr("{ms} ms").format(ms=round(stream.delay_ms)))
    if stream.path == "link":
        parts.append(tr("through the Beamer link"))
    return " · ".join(parts)


# The selector.

def _cameras_online(snap: CameraSnapshot, audio, ident: str) -> bool:
    held = _machine(snap, ident)
    online = ident == snap.own or ident in audio.online
    return online and held is not None and held.status in ("ok", "starting")


def _order(snap: CameraSnapshot, names) -> list:
    others = sorted((ident for ident in snap.machines if ident != snap.own), key=lambda ident: (names(ident).lower(), ident))
    return ([snap.own] if snap.own in snap.machines else []) + others


def _choices(snap: CameraSnapshot, audio, names):
    choices = [Choice(OFF, tr("Off"), tr("Off"))]
    entry = snap.route.camera
    selected = OFF
    for ident in _order(snap, names):
        held = snap.machines[ident]
        if "camera_capture" not in held.features:
            continue
        name = names(ident)
        usable = _cameras_online(snap, audio, ident)
        choices.append(Choice(_choice_key("cam-head", ident), name, name, enabled=False, header=True, machine=ident))
        preferred = model.camera_by_key(held.cameras, held.preferred)
        label = (tr("Default camera ({name})").format(name=preferred.name) if preferred is not None
                 else tr("Default camera"))
        key = _choice_key("cam", ident, "")
        choices.append(Choice(key, label, _title(name, label), enabled=usable, machine=ident, device=None))
        if entry.machine == ident and entry.device is None:
            selected = key
        chosen = model.resolve_camera(entry.device, held.cameras) if entry.machine == ident else None
        for camera in held.cameras:
            key = _choice_key("cam", ident, camera.key)
            choices.append(Choice(key, camera.name, _title(name, camera.name), enabled=usable, machine=ident,
                                  device=CameraRef(camera.key, camera.name)))
            if chosen is not None and chosen.key == camera.key and entry.device is not None:
                selected = key
        if entry.machine == ident and entry.device is not None and chosen is None:
            # The chosen camera is unplugged: still the selection, so nothing has to be set again.
            key = _choice_key("cam", ident, entry.device.key)
            label = tr("{device} (not connected)").format(device=entry.device.name)
            choices.append(Choice(key, label, _title(name, label), enabled=False, machine=ident, device=entry.device))
            selected = key
    if entry.machine is not None and selected == OFF:
        # The route names a machine this one holds no camera list for (offline, or no camera side).
        name = names(entry.machine)
        label = entry.device.name if entry.device is not None else tr("Default camera")
        key = _choice_key("cam", entry.machine, entry.device.key if entry.device else "")
        choices.append(Choice(_choice_key("cam-head", entry.machine), name, name, enabled=False, header=True,
                              machine=entry.machine))
        choices.append(Choice(key, label, _title(name, label), enabled=False, machine=entry.machine, device=entry.device))
        selected = key
    return tuple(choices), selected


def _quality_choices():
    return tuple(Choice(size, size, size) for size in QUALITY_KEYS)


# This machine.

def _mac_rows(snap: CameraSnapshot, held: MachineCamera) -> list:
    rows = []
    if held.sharing is False:
        rows.append(Row(tr("Camera sharing"), tr("Off"), QUIET, ACTION_SHARE, tr("Share the Camera")))
    else:
        rows.append(Row(tr("Camera sharing"), tr("On"), PLAIN, ACTION_STOP_SHARING, tr("Stop Sharing")))
    permission = held.permission if held.permission is not None else snap.permission
    if permission == "granted":
        rows.append(Row(tr("Camera access"), tr("Allowed"), PLAIN))
    elif permission in ("denied", "restricted"):
        rows.append(Row(tr("Camera access"), tr("Not allowed"), FAULT, ACTION_PRIVACY, tr("Open Privacy Settings")))
    elif permission == "undetermined":
        # While the route names this Mac, the notice carries the button (one button on the page).
        if snap.needs_permission:
            rows.append(Row(tr("Camera access"), tr("Not asked yet"), WAIT))
        else:
            rows.append(Row(tr("Camera access"), tr("Not asked yet"), PLAIN, ACTION_ALLOW, tr("Allow Camera Access…")))
    else:
        rows.append(Row(tr("Camera access"), tr("Unknown"), QUIET))
    if held.test_source:
        rows.append(Row(tr("Test source"), tr("On, sent instead of the camera"), WAIT))
    return rows


def _pc_rows(snap: CameraSnapshot, held: MachineCamera) -> list:
    install = held.virtual_camera.install
    busy = any(stream.state in STREAMS_RUNNING for stream in snap.streams)
    offer = snap.vcam_install_offer
    local = snap.vcam_local
    if install == "ok":
        restart = (local is not None and local.loaded_version and local.dll_version
                   and local.loaded_version != local.dll_version)
        if restart:
            return [Row(tr("Beamer Camera"), tr("Close apps that use the camera to finish updating Beamer Camera."), WAIT)]
        return [Row(tr("Beamer Camera"), tr("Installed"), PLAIN, None if busy else ACTION_REMOVE,
                    "" if busy else tr("Remove…"))]
    if install == "unsupported":
        return [Row(tr("Beamer Camera"), tr("This version of Windows cannot show it."), QUIET)]
    value = {"missing": tr("Not installed"), "outdated": tr("Needs an update"), "broken": tr("Damaged")}.get(
        install, tr("Damaged"))
    action = {"install": ACTION_INSTALL, "update": ACTION_UPDATE, "repair": ACTION_REPAIR}.get(
        offer or "", {"missing": ACTION_INSTALL, "outdated": ACTION_UPDATE}.get(install, ACTION_REPAIR))
    label = {ACTION_INSTALL: tr("Install Beamer Camera…"), ACTION_UPDATE: tr("Update Beamer Camera…"),
             ACTION_REPAIR: tr("Repair Beamer Camera…")}[action]
    if busy:
        return [Row(tr("Beamer Camera"), value, WAIT)]
    return [Row(tr("Beamer Camera"), value, WAIT, action, label)]


# Notices.

def _notices(snap: CameraSnapshot, audio, names, platform: str) -> tuple:
    notices = []
    own = _machine(snap, snap.own)
    source = snap.route.camera.machine
    if snap.store_problem == "unreadable" and own is not None and "camera_capture" in own.features:
        # R4 #4: camera.json could not be read, so sharing started off (fail closed).
        # The sharing row below carries "Share the Camera": one button for it on the page.
        notices.append(Notice("camera_store_unreadable",
                              tr("Camera sharing is off because Beamer could not read its saved camera settings."),
                              WAIT, machine=snap.own))
    elif snap.store_problem == "save_failed":
        notices.append(Notice("camera_store_save_failed",
                              tr("Beamer could not save the camera settings. After a restart, they may not be as you set them."),
                              FAULT, machine=snap.own))
    if snap.needs_permission:
        notices.append(Notice("camera_permission", tr("Beamer needs camera access to share this Mac's camera."), WAIT,
                              ACTION_ALLOW, tr("Allow Camera Access…"), machine=snap.own))
    if own is not None and "camera_capture" in own.features and own.capture.by_other and source == snap.own \
            and snap.status.state in LIVE_STATES:
        notices.append(Notice("camera_shared_use", tr("Another app on this Mac uses the camera at the same time."),
                              PLAIN, machine=snap.own))
    if source is not None:
        # Each other PC that cannot show the camera, named, with where to fix it.
        for ident in _order(snap, names):
            held = snap.machines[ident]
            if ident == snap.own or "virtual_camera" not in held.features:
                continue
            if ident not in audio.online:
                continue
            text = {
                "missing": tr("{machine}: Beamer Camera is not installed there. Install it in Beamer on {machine}."),
                "outdated": tr("{machine}: Beamer Camera needs an update there. Update it in Beamer on {machine}."),
                "broken": tr("{machine}: Beamer Camera is damaged there. Repair it in Beamer on {machine}."),
            }.get(held.virtual_camera.install)
            if text:
                notices.append(Notice("vcam_" + held.virtual_camera.install, text.format(machine=names(ident)), WAIT,
                                      machine=ident))
    return tuple(notices)


def section_shown(snap: CameraSnapshot) -> bool:
    """§8.3: the module shows while any machine announces a camera or a virtual camera."""
    return any(held.features & set(model.FEATURES) for held in snap.machines.values())


def camera_section(snap: CameraSnapshot, audio, own_label: str, platform: str) -> CameraSection:
    """Everything the Camera module shows (§8.2-§8.6). `audio`: the AudioSnapshot (names, who is
    online); `own_label`: "This Mac" / "This PC"; `platform`: this machine's, "macos" | "windows"."""
    names = _Names(audio, own_label)
    choices, selected = _choices(snap, audio, names)
    line, tone, action, action_label = _status_line(snap, names, platform)
    own = _machine(snap, snap.own)
    rows = []
    if own is not None and "camera_capture" in own.features:
        rows = _mac_rows(snap, own)
    elif own is not None and "virtual_camera" in own.features:
        rows = _pc_rows(snap, own)
    hint = tr("Apps on a PC choose 'Beamer Camera'. Macs cannot show another machine's camera yet.")
    help_line, help_action, help_label = "", None, ""
    if platform == "windows" and own is not None and own.virtual_camera.install == "ok":
        help_line = tr("One app at a time can use Beamer Camera unless Windows allows several: Settings > Bluetooth "
                       "& devices > Cameras > Beamer Camera.")
        help_action, help_label = ACTION_CAMERA_SETTINGS, tr("Camera settings")
    return CameraSection(
        label=tr("Camera your apps see"), choices=choices, selected=selected,
        line=line, tone=tone, detail=_detail(snap, names),
        quality_label=tr("Quality"), quality_choices=_quality_choices(),
        quality_selected=snap.route.quality.size if snap.route.quality.size in QUALITY_KEYS else model.DEFAULT_QUALITY,
        hint=hint, help=help_line, help_action=help_action,
        this_machine=tuple(rows), notices=_notices(snap, audio, names, platform),
        shown=section_shown(snap), quality_enabled=snap.route.camera.machine is not None,
        action=action, action_label=action_label, help_action_label=help_label,
        this_machine_label=own_label if rows else "", state=snap.status.state)


# The map.

def map_extras(snap: CameraSnapshot) -> CameraMapExtras:
    """The camera's marks on the shared machine map: the badge on the camera machine's tile, a line
    on each tile whose apps use (or could use) Beamer Camera, and an arrow to each machine whose
    apps have it open, active while the picture flows."""
    source = snap.route.camera.machine
    if source is None:
        return CameraMapExtras(None, "", {}, ())
    watching, arrows = {}, []
    live = False
    for ident in sorted(snap.machines):
        held = snap.machines[ident]
        if ident == source or "virtual_camera" not in held.features or held.virtual_camera.install != "ok":
            continue
        status = snap.sinks.get(ident)
        state = status.state if status is not None else "idle"
        vcam = held.virtual_camera
        if vcam.in_use:
            watching[ident] = (tr("{app} uses the camera").format(app=vcam.app) if vcam.app
                               else tr("An app uses the camera"))
            active = state in LIVE_STATES
            live = live or active
            arrows.append((source, ident, "active" if active else "waiting"))
        elif state == "idle":
            watching[ident] = tr("Camera ready")
    return CameraMapExtras(source, camera_name(snap), watching, tuple(arrows), live)


# The Mac menu bar (§8.5).

def menu_model(snap: CameraSnapshot, audio, own_label: str = "") -> CameraMenu:
    names = _Names(audio, own_label)
    own = _machine(snap, snap.own)
    if own is None or "camera_capture" not in own.features:
        return CameraMenu(False, "", "", False, False)
    sharing = own.sharing is not False
    line = ""
    if snap.route.camera.machine == snap.own and snap.status.state in LIVE_STATES + ("starting",):
        targets = _targets(snap, *(LIVE_STATES + ("starting",))) or _ids(snap.status.params.get("to"))
        phrases = []
        for target in targets:
            held = _machine(snap, target)
            app = held.virtual_camera.app if held is not None else None
            phrases.append(tr("{app} on {machine}").format(app=app, machine=names(target)) if app
                           else tr("in use on {machine}").format(machine=names(target)))
        if phrases:
            line = tr("Camera: {users}").format(users=joined(phrases))
    return CameraMenu(True, line, sharing_title(sharing), sharing, snap.needs_permission, tr("Allow Camera Access…"))


# Actions: what a page calls when a person changes something.

def apply_camera(service, choice: Choice) -> None:
    """The camera selector picked `choice`."""
    if choice.key == OFF:
        service.set_camera(None, None)
    elif not choice.header and choice.enabled:
        service.set_camera(choice.machine, choice.device)


def apply_quality(service, size: str) -> None:
    if size in QUALITY_KEYS:
        service.set_quality(size)


def apply_camera_action(service, action: Optional[str], snap: Optional[CameraSnapshot] = None) -> bool:
    """Runs a camera action through the service; False when the action is the app's own (APP_ACTIONS:
    a settings pane, the PC's elevated install) or there is none. `snap` is needed for Retry (the
    route to set again)."""
    if action == ACTION_SHARE:
        service.set_camera_sharing(True)
        return True
    if action == ACTION_STOP_SHARING:
        service.set_camera_sharing(False)
        return True
    if action == ACTION_ALLOW:
        service.ask_camera_permission()
        return True
    if action == ACTION_RETRY_HELPER:
        service.retry_helper()
        return True
    if action == ACTION_RETRY and snap is not None and snap.route.camera.machine is not None:
        service.set_camera(snap.route.camera.machine, snap.route.camera.device)
        return True
    return False


def apply_camera_notice(service, notice: Notice, snap: Optional[CameraSnapshot] = None) -> bool:
    """A camera notice's button (§8.2): permission / sharing / settings / install / retry actions."""
    return apply_camera_action(service, notice.action, snap)


def apply_row(service, row: Row, snap: Optional[CameraSnapshot] = None) -> bool:
    """A "This Mac" / "This PC" row's button."""
    return apply_camera_action(service, row.action, snap)
