# SPDX-License-Identifier: GPL-3.0-only
"""The Camera module of the Qt Audio & Camera page (dev/camera/PHASE2.md §8.3, §8.4, §8.6), Windows only.

Everything it shows comes from `core.camera.view.camera_section` (shared with the Mac page); this file
lays it out with the Microphone module's chrome (audio_page_win.py) and carries out the actions only
this app can run:

- the camera selector and the Quality choice, through the service's camera setters;
- the status line, its detail, the hint, and the line's own Retry;
- the "This PC" row with Install / Update / Repair / Remove (win_app/vcam_install.py, one UAC prompt):
  `on_done` arrives on vcam_install's background thread and is relayed to the Qt thread; after an ok
  install the module calls `service.refresh_camera_install()`; a refused prompt says "Installation was
  cancelled." under the row until the row itself changes;
- the static multi-app help line and its "Camera settings" link (`ms-settings:camera`);
- the camera notices.

On Linux (and on a Mac running the Qt app for tests) `section_for` returns nothing, so the module stays
hidden and the map carries no camera marks; the Qt page never shows a camera there.
"""

from __future__ import annotations

import sys
from dataclasses import dataclass
from typing import Callable, Optional

from PySide6.QtCore import QObject, QPointF, QRectF, Qt, QUrl, Signal
from PySide6.QtGui import QColor, QDesktopServices, QPainter, QPainterPath, QPen
from PySide6.QtWidgets import QGridLayout, QHBoxLayout, QMessageBox, QPushButton, QSizePolicy, QVBoxLayout, QWidget

import theme
import tokens
import widgets
from core import feature_flags
from core.i18n import N_, tr

# Words this module adds to the model's (§8.6): the row's note after an elevated step.
NOTES = {
    ("install", "running"): N_("Waiting for Windows to install Beamer Camera…"),
    ("remove", "running"): N_("Waiting for Windows to remove Beamer Camera…"),
    ("install", "cancelled"): N_("Installation was cancelled."),
    ("remove", "cancelled"): N_("Removal was cancelled."),
    ("install", "failed"): N_("Beamer Camera could not be installed. Try again, or see Beamer's log."),
    ("remove", "failed"): N_("Beamer Camera could not be removed. Try again, or see Beamer's log."),
    ("install", "no_helper"): N_("Beamer's media helper is missing, so Beamer Camera cannot be installed."),
    ("remove", "no_helper"): N_("Beamer's media helper is missing, so Beamer Camera cannot be removed."),
}
REMOVE_TITLE = N_("Remove Beamer Camera?")
REMOVE_TEXT = N_("Apps on this PC will no longer list Beamer Camera. Windows asks for administrator permission, "
                 "and every camera on this PC may pause for about a second.")
REMOVE_BUTTON = N_("Remove")
LEGEND_CAMERA = N_("Camera")
ROW_HEADING_WIDTH = 84
ROW_LABEL_WIDTH = 130
# Two or three routes between the same tiles run side by side in this order (as on the Mac page): the
# order of the tile's lines, the camera's above the mic's above the sound's.
ARROW_LANES = ("camera", "mic", "sound")
CAMERA_DASHES = [0.3, 2.2]


def camera_on() -> bool:
    """The camera feature flag (core/feature_flags.py CAMERA, set by the fork)."""
    return bool(getattr(feature_flags, "CAMERA", False))


def camera_platform() -> Optional[str]:
    """This app's platform as core.camera.view names it, or None where the Qt app shows no camera
    (Linux: no virtual camera and no camera source in this version, §8.3)."""
    return "windows" if sys.platform == "win32" else None


@dataclass(frozen=True)
class CameraState:
    """What the page needs from the camera for one refresh."""
    snapshot: object = None         # core.camera.model.CameraSnapshot, or None
    section: object = None          # core.camera.view.CameraSection, or None: no module
    extras: object = None           # core.camera.view.CameraMapExtras for page_model(camera=), or None


NO_CAMERA = CameraState()


def section_for(service, audio, own_label: str) -> CameraState:
    """The camera's part of a page refresh: its snapshot, the module's model and the map's extras,
    or NO_CAMERA without the flag, on a platform without a camera, without a camera part in the
    service, or when the snapshot cannot be read (a camera fault never takes the Audio page down)."""
    platform = camera_platform()
    reader = getattr(service, "camera_snapshot", None) if service is not None else None
    if not camera_on() or platform is None or reader is None:
        return NO_CAMERA
    try:
        snap = reader()
        if snap is None:
            return NO_CAMERA
        from core.camera import view as camera_view
        section = camera_view.camera_section(snap, audio, own_label, platform)
        extras = camera_view.map_extras(snap) if section.shown else None
    except Exception:  # noqa: BLE001 - see the docstring
        import logging
        logging.getLogger(__name__).exception("camera: page model")
        return NO_CAMERA
    return CameraState(snap, section, extras)


# -- the map's camera marks (used by audio_page_win's AudioMap) -----------------------------------------

def paint_camera_glyph(painter: QPainter, x: float, y: float, colour: QColor, size: float = 12.0) -> None:
    """A video camera, `size` tall, drawn like the mic glyph: a rounded body and a lens wedge."""
    unit = size / 12.0
    painter.save()
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(colour)
    painter.drawRoundedRect(QRectF(x, y + 3 * unit, 8.5 * unit, 7 * unit), 1.6 * unit, 1.6 * unit)
    lens = QPainterPath(QPointF(x + 9.2 * unit, y + 6.5 * unit))
    lens.lineTo(QPointF(x + 12.5 * unit, y + 3.6 * unit))
    lens.lineTo(QPointF(x + 12.5 * unit, y + 9.4 * unit))
    lens.closeSubpath()
    painter.drawPath(lens)
    painter.restore()


def camera_pen(pen: QPen) -> QPen:
    """The camera arrow's dotted line (the mic's is dashed, the sound's solid)."""
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    pen.setDashPattern(CAMERA_DASHES)
    return pen


def lane_order(arrows, indexes) -> list:
    """`indexes` of one pair of machines in lane order: camera, mic, sound when a camera arrow is
    among them, else as given (so the audio-only map draws as before)."""
    kinds = [arrows[index].kind for index in indexes]
    if "camera" not in kinds:
        return list(indexes)
    rank = {kind: position for position, kind in enumerate(ARROW_LANES)}
    return sorted(indexes, key=lambda index: rank.get(arrows[index].kind, len(ARROW_LANES)))


def tile_lines(tile, watched: bool, mic_text: str, mic_important: bool, top: float, mic_y: float,
               step: float = 19.0) -> dict:
    """Where the tile's mic and camera lines go: line name -> y. The camera's line sits above the
    mic's; short of room the important one wins (the shared mic or camera, who hears or sees it), then
    the mic. Without a camera line this is the audio page's own rule."""
    camera_text = tile.camera_line or tile.watching_line
    camera_important = bool(tile.camera_line or (tile.watching_line and watched))
    lines = ([("mic", mic_important)] if mic_text else []) + ([("camera", camera_important)] if camera_text else [])
    lines.sort(key=lambda line: (not line[1], line[0] != "mic"))
    placed, y = {}, mic_y
    for name, wanted in lines:
        if y >= top + 48 or (wanted and y >= top + 30):
            placed[name] = y
            y -= step
    return placed


# -- the module -----------------------------------------------------------------------------------------

class _Relay(QObject):
    """vcam_install's `on_done`, from its background thread to the Qt thread."""
    done = Signal(str, object)


class _Row(QWidget):
    """One "This PC" row: heading (first row only), label, value, its button, and the module's note."""

    def __init__(self, on_action: Callable[[str], None], parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setProperty("vernier", "plain")
        self.row = None
        self._weight = None
        grid = QGridLayout(self)
        grid.setContentsMargins(0, 6, 0, 6)
        grid.setHorizontalSpacing(12)
        grid.setVerticalSpacing(4)
        self.heading = widgets.label("", "note")
        self.heading.setFont(theme.font(tokens.TYPE["note"], 600))
        self.heading.setFixedWidth(ROW_HEADING_WIDTH)
        self.label = widgets.label("", "note")
        self.label.setFixedWidth(ROW_LABEL_WIDTH)
        self.value = widgets.label("", "note", wrap=True)
        self.value.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.note = widgets.label("", "note-amber", wrap=True)
        self.note.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.note.hide()
        self.button = QPushButton("")
        self.button.setProperty("vernier", "small")
        self.button.clicked.connect(lambda: on_action(self.row.action) if self.row is not None and self.row.action else None)
        grid.addWidget(self.heading, 0, 0, Qt.AlignmentFlag.AlignVCenter)
        grid.addWidget(self.label, 0, 1, Qt.AlignmentFlag.AlignVCenter)
        grid.addWidget(self.value, 0, 2, Qt.AlignmentFlag.AlignVCenter)
        grid.addWidget(self.button, 0, 3, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
        grid.addWidget(self.note, 1, 2, 1, 2)
        grid.setColumnStretch(2, 1)
        self.setMinimumHeight(36)

    def show_row(self, heading: str, row, note: str, busy: bool) -> None:
        from audio_page_win import note_role
        from core.camera import view as camera_view
        self.row = row
        _text(self.heading, heading)
        _text(self.label, row.label)
        _text(self.value, row.value)
        widgets.set_role(self.value, "note" if row.tone in (camera_view.PLAIN, camera_view.LIVE) else note_role(row.tone))
        weight = 600 if row.tone in (camera_view.WAIT, camera_view.FAULT) else 500
        if weight != self._weight:
            self._weight = weight
            self.value.setFont(theme.font(tokens.TYPE["note"], weight))
        self.button.setVisible(bool(row.action))
        if row.action:
            if self.button.text() != row.action_label:
                self.button.setText(row.action_label)
            self.button.setAccessibleName(row.action_label)
            widgets.set_role(self.button, "primary" if row.tone == camera_view.WAIT else "small")
            self.button.setEnabled(not busy)
        _text(self.note, note)
        self.note.setVisible(bool(note))


def _text(label, text: str) -> None:
    if label.text() != text:
        label.setText(text)


class CameraModule(widgets.Module):
    """The Camera module (§8.3), hidden until `show_section(section)` gets a section that shows.

    `service_of()` returns the AudioService (or None); `snapshot_of()` the camera snapshot the section
    was built from (for Retry). `installer` is win_app/vcam_install.py's API (`install(on_done)`,
    `remove(on_done)`), `open_url(url)` opens a link, `confirm_remove()` asks before removing; all three
    are replaced by the tests."""

    def __init__(self, service_of: Callable[[], object], snapshot_of: Callable[[], object] = lambda: None, *,
                 installer=None, open_url: Optional[Callable[[str], None]] = None,
                 confirm_remove: Optional[Callable[[], bool]] = None, parent: QWidget | None = None) -> None:
        super().__init__(tr("Camera"), parent)
        self.service_of = service_of
        self.snapshot_of = snapshot_of
        self._installer = installer
        self.open_url = open_url or (lambda url: QDesktopServices.openUrl(QUrl(url)))
        self.confirm_remove = confirm_remove or self._ask_remove
        self.section = None
        self.running = None             # "install" | "remove" while an elevated step runs
        self.outcome = None             # (kind, code, row) of the last step that did not succeed
        self.notices = []
        self._notice_keys = None
        self.row_views = []
        self._relay = _Relay(self)
        self._relay.done.connect(self._finished, Qt.ConnectionType.QueuedConnection)
        self._build()
        self.hide()

    # -- building -----------------------------------------------------------------------------------

    def _build(self) -> None:
        from audio_page_win import MenuSelect
        self.select = MenuSelect(self._picked)
        self.select_label = widgets.label("", "key")
        self.select_row = widgets.FormRow(self.select_label, self.select)
        self.body.addWidget(self.select_row)

        from core.camera import model as camera_model
        self.quality = widgets.Choice([(size, size) for size in camera_model.QUALITIES], 0,
                                      camera_model.DEFAULT_QUALITY, self._quality_chosen)
        self.quality_label = widgets.label("", "key")
        # The status sentence with its own action (Retry), the stream's detail and the hint, under the
        # controls as the Microphone module has them.
        status = QWidget()
        status.setProperty("vernier", "plain")
        line = QHBoxLayout(status)
        line.setContentsMargins(0, 0, 0, 0)
        line.setSpacing(12)
        self.line = widgets.label("", "note", wrap=True)
        self.line.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        line.addWidget(self.line, 1)
        self.line_button = QPushButton("")
        self.line_button.setProperty("vernier", "small")
        self.line_button.clicked.connect(lambda: self.act(self.section.action if self.section else None))
        self.line_button.hide()
        line.addWidget(self.line_button, 0, Qt.AlignmentFlag.AlignVCenter)
        self.status = status
        self.detail = widgets.label("", "note-quiet", wrap=True)
        self.hint = widgets.label("", "note-quiet", wrap=True)
        self.quality_row = widgets.FormRow(self.quality_label, self.quality.view, (status, self.detail, self.hint))
        self.body.addWidget(self.quality_row)

        # This PC: a seam, then its rows; the help line and its link; the camera's notices.
        self.rule = widgets.rule(False)
        self.body.addWidget(self.rule)
        self.rows_box = QVBoxLayout()
        self.rows_box.setSpacing(0)
        self.body.addLayout(self.rows_box)
        self.help_row = QWidget()
        self.help_row.setProperty("vernier", "plain")
        help_line = QHBoxLayout(self.help_row)
        help_line.setContentsMargins(0, 0, 0, 0)
        help_line.setSpacing(12)
        self.help = widgets.label("", "note-quiet", wrap=True)
        self.help.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        help_line.addWidget(self.help, 1)
        self.help_button = QPushButton("")
        self.help_button.setProperty("vernier", "small")
        self.help_button.clicked.connect(lambda: self.act(self.section.help_action if self.section else None))
        help_line.addWidget(self.help_button, 0, Qt.AlignmentFlag.AlignVCenter)
        self.help_row.hide()
        self.body.addWidget(self.help_row)
        self.notices_box = QVBoxLayout()
        self.notices_box.setSpacing(8)
        self.body.addLayout(self.notices_box)

    # -- showing ------------------------------------------------------------------------------------

    def show_section(self, section) -> None:
        """Brings the module up to `section` (core.camera.view.CameraSection), hidden for None or a
        section that does not show (no machine with a camera side)."""
        if section is None or not getattr(section, "shown", False):
            self.section = section
            self.setVisible(False)
            return
        self.section = section
        self._show_section(section)
        self.setVisible(True)

    def _show_section(self, section) -> None:
        from audio_page_win import note_role
        _text(self.select_label, section.label)
        self.select.set_choices(section.choices, section.selected)
        self.select.setAccessibleName(section.label)
        _text(self.quality_label, section.quality_label)
        if self.quality.value != section.quality_selected:
            self.quality.set_value(section.quality_selected)
        self.quality.set_enabled(section.quality_enabled)
        self.quality.set_names(section.quality_label)
        _text(self.line, section.line)
        widgets.set_role(self.line, note_role(section.tone))
        self.line.setAccessibleName(section.line)
        self.line_button.setVisible(bool(section.action))
        if section.action:
            _text(self.line_button, section.action_label)
            self.line_button.setAccessibleName(section.action_label)
        _text(self.detail, section.detail)
        self.detail.setVisible(bool(section.detail))
        _text(self.hint, section.hint)
        self.hint.setVisible(bool(section.hint))
        self._show_rows(section)
        self.rule.setVisible(bool(section.this_machine))
        self.help_row.setVisible(bool(section.help))
        if section.help:
            _text(self.help, section.help)
            label = section.help_action_label or tr("Camera settings")
            _text(self.help_button, label)
            self.help_button.setAccessibleName(label)
            self.help_button.setAccessibleDescription(section.help)
            self.help_button.setVisible(bool(section.help_action))
        self._show_notices(section.notices)

    def _show_rows(self, section) -> None:
        rows = section.this_machine
        while len(self.row_views) < len(rows):
            view = _Row(self.act)
            self.rows_box.addWidget(view)
            self.row_views.append(view)
        for index, view in enumerate(self.row_views):
            if index >= len(rows):
                view.row = None
                view.hide()
                continue
            row = rows[index]
            view.show_row(section.this_machine_label if index == 0 else "", row, self._note_for(index, row),
                          self.running is not None)
            view.show()

    def _note_for(self, index: int, row) -> str:
        """The note under the PC's install row: the running step, else the last one's failure while the
        row is still the one it was about (a new install state clears it)."""
        if index != 0:
            return ""
        if self.running is not None:
            return tr(NOTES[(self.running, "running")])
        if self.outcome is not None:
            kind, code, about = self.outcome
            if about == row:
                text = NOTES.get((kind, code)) or NOTES[(kind, "failed")]
                return tr(text)
            self.outcome = None
        return ""

    def _show_notices(self, notices) -> None:
        from audio_page_win import NoticeView
        keys = tuple((notice.code, notice.text, notice.tone, notice.action, notice.action_label) for notice in notices)
        if keys == self._notice_keys:
            for widget, notice in zip(self.notices, notices):
                widget.notice = notice
            return
        self._notice_keys = keys
        while self.notices_box.count():
            item = self.notices_box.takeAt(0)
            if item.widget() is not None:
                item.widget().hide()
                item.widget().deleteLater()
        self.notices = []
        for notice in notices:
            widget = NoticeView(notice, lambda shown: self.act(shown.action))
            self.notices_box.addWidget(widget)
            self.notices.append(widget)

    def controls(self) -> list:
        """Every control a person can reach now (for the keyboard test)."""
        found = [self.select] + [button for button in self.quality._buttons.values() if button.isEnabled()]
        found += [self.line_button] if self.line_button.isVisibleTo(self) else []
        found += [view.button for view in self.row_views
                  if view.isVisibleTo(self) and view.button.isVisibleTo(self) and view.button.isEnabled()]
        found += [self.help_button] if self.help_row.isVisibleTo(self) and self.help_button.isVisibleTo(self) else []
        found += [widget.action for widget in self.notices if widget.action is not None]
        return found

    # -- a person's picks ---------------------------------------------------------------------------

    def _picked(self, choice) -> None:
        service = self.service_of()
        if service is not None:
            from core.camera import view as camera_view
            camera_view.apply_camera(service, choice)

    def _quality_chosen(self, size: str) -> None:
        service = self.service_of()
        if service is not None:
            from core.camera import view as camera_view
            camera_view.apply_quality(service, size)

    def act(self, action: Optional[str]) -> None:
        """A row's, notice's, help line's or status line's button: the service's action, else this
        app's own (the elevated install and removal, the Camera settings link)."""
        if not action:
            return
        from core.camera import view as camera_view
        service = self.service_of()
        if service is not None and camera_view.apply_camera_action(service, action, self.snapshot_of()):
            return
        if action in (camera_view.ACTION_INSTALL, camera_view.ACTION_UPDATE, camera_view.ACTION_REPAIR):
            self._run("install")
        elif action == camera_view.ACTION_REMOVE:
            if self.confirm_remove():
                self._run("remove")
        elif action == camera_view.ACTION_CAMERA_SETTINGS:
            self.open_url(camera_view.CAMERA_SETTINGS_URL)

    # -- the elevated steps -------------------------------------------------------------------------

    def installer(self):
        if self._installer is None:
            import vcam_install
            self._installer = vcam_install
        return self._installer

    def _run(self, kind: str) -> None:
        if self.running is not None:
            return
        self.running = kind
        self.outcome = None
        if self.section is not None:
            self._show_rows(self.section)
        relay = self._relay

        def on_done(result) -> None:
            # vcam_install's background thread: post, nothing else.
            try:
                relay.done.emit(kind, result)
            except RuntimeError:  # the page is gone
                pass

        step = self.installer().install if kind == "install" else self.installer().remove
        try:
            step(on_done)
        except Exception:  # noqa: BLE001 - a broken helper path must not leave the row waiting
            import logging
            logging.getLogger(__name__).exception("camera: %s", kind)
            self._finished(kind, _Failed())

    def _finished(self, kind: str, result) -> None:
        """Qt thread: the step's Result arrived."""
        self.running = None
        ok = bool(getattr(result, "ok", False))
        code = getattr(result, "code", "failed")
        row = self.section.this_machine[0] if self.section is not None and self.section.this_machine else None
        self.outcome = None if ok else (kind, code, row)
        if ok:
            service = self.service_of()
            refresh = getattr(service, "refresh_camera_install", None) if service is not None else None
            if refresh is not None:
                refresh()
        if self.section is not None:
            self._show_rows(self.section)
        self.done_hook(kind, result)

    def done_hook(self, kind: str, result) -> None:
        """Called on the Qt thread after each elevated step (tests)."""

    def _ask_remove(self) -> bool:
        box = QMessageBox(QMessageBox.Icon.Question, tr(REMOVE_TITLE), tr(REMOVE_TEXT), parent=self.window())
        remove = box.addButton(tr(REMOVE_BUTTON), QMessageBox.ButtonRole.DestructiveRole)
        box.addButton(QMessageBox.StandardButton.Cancel)
        box.setDefaultButton(QMessageBox.StandardButton.Cancel)
        box.exec()
        return box.clickedButton() is remove


class _Failed:
    ok = False
    code = "failed"
