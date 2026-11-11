# SPDX-License-Identifier: GPL-3.0-only
"""The Camera module of the Audio & Camera page (dev/camera/PHASE2.md §8.3-§8.5): the camera your
apps see, its quality, its status line, the hint, this Mac's sharing switch and camera access, and
the camera's notices.

Everything shown comes from `core.camera.view.camera_section`; this file only lays it out in AppKit,
with the Microphone module's chrome, fonts and status-line style (mac_app/audio_page.py). The page
owns the service and calls `show(section)` from its refresh; the module calls back with what a
person picked: `on_pick(choice)`, `on_quality(size)`, `on_action(action)`.
"""

from __future__ import annotations

import AppKit

import theme
import widgets
from audio_page import ChoicePopup, _dot_line
from core.camera import model as camera_model
from core.camera import view as camera_view
from core.i18n import tr

LABEL_WIDTH = 130.0
HEADING_WIDTH = 84.0


def _padded(button):
    """A button at the end of a row, as the page's Test buttons are: its title's width plus its
    padding (the row's words take the spare width), one line high."""
    view = button.view
    view.widthAnchor().constraintGreaterThanOrEqualToConstant_(64).setActive_(True)
    view.setContentCompressionResistancePriority_forOrientation_(
        AppKit.NSLayoutPriorityRequired, AppKit.NSLayoutConstraintOrientationHorizontal)
    return widgets.hug(view, horizontal=False)


class _RowView:
    """One row's widgets, kept and updated in place."""
    row = line = heading = label = value = button = None


class CameraModule:
    """The module in `body` (added where the page builds it), hidden until a section says it shows."""

    def __init__(self, body, on_pick, on_quality, on_action, on_menu):
        self.on_pick = on_pick
        self.on_quality = on_quality
        self.on_action = on_action
        self.section = None
        self.rows_signature = None
        self.notices_signature = None
        self.row_buttons = []
        self.row_views = []
        self.notice_buttons = []
        self.module = module = widgets.Module()
        # Hidden rows (the PC's help line on a Mac, an empty detail) take no width or height.
        module.body.setDetachesHiddenViews_(True)
        self.view = module.view
        self.heading = widgets.Label(tr("Camera"), theme.TYPE["body"], 600)
        module.add(self.heading.view)
        widgets.add(body, module.view)

        self.popup = ChoicePopup(self._picked, on_menu, tr("Camera your apps see"))
        row, self.caption = widgets.field_row(tr("Camera your apps see"), self.popup.view)
        self.choice_row = row
        module.add(row)
        self.quality = widgets.Segmented([(size, size) for size in camera_model.QUALITIES], on_change=self._quality)
        row, self.quality_caption = widgets.field_row(tr("Quality"), self.quality.view)
        self.quality_row = row
        module.add(row)
        module.body.setCustomSpacing_afterView_(14, row)

        # The status sentence with its dot, and the line's own action (Retry) beside it.
        self.status = widgets.Label("", theme.TYPE["body"], 500, wrap=True)
        words, self.dot = _dot_line(self.status)
        widgets.hug(words, AppKit.NSLayoutPriorityDefaultLow)
        self.status_row = widgets.stack(vertical=False, spacing=16)
        self.status_row.setAlignment_(AppKit.NSLayoutAttributeCenterY)
        self.status_row.addArrangedSubview_(words)
        self.status_button = widgets.action_button(tr("Retry"), lambda: self._action(self.section.action if self.section else None),
                                                   scale="small")
        self.status_row.addArrangedSubview_(_padded(self.status_button))
        self.status_button.view.setHidden_(True)
        module.add(self.status_row)
        module.body.setCustomSpacing_afterView_(4, self.status_row)
        self.detail = widgets.note(ink="ink_3")
        self.detail_view = widgets.stack(spacing=0)
        self.detail_view.setEdgeInsets_(AppKit.NSEdgeInsets(0, 14, 0, 0))
        widgets.add(self.detail_view, self.detail.view)
        module.add(self.detail_view)
        self.hint = widgets.note(ink="ink_3")
        module.add(self.hint.view)

        # This machine: a hairline, then its rows under its name.
        self.rule = widgets.hairline()
        module.add(self.rule)
        self.rows_host = widgets.stack(spacing=0)
        self.rows_host.setAlignment_(AppKit.NSLayoutAttributeWidth)
        module.add(self.rows_host)
        # PC only (the model never gives the Mac one): the multi-app help line and its link.
        self.help_row = widgets.stack(vertical=False, spacing=16)
        self.help = widgets.note(ink="ink_3", reading=False)
        widgets.hug(self.help.view, AppKit.NSLayoutPriorityDefaultLow)
        self.help_row.addArrangedSubview_(self.help.view)
        self.help_button = widgets.action_button(tr("Camera settings"),
                                                 lambda: self._action(self.section.help_action if self.section else None),
                                                 scale="small")
        self.help_row.addArrangedSubview_(_padded(self.help_button))
        self.help_row.setHidden_(True)
        module.add(self.help_row)
        self.notices_host = widgets.stack(spacing=0)
        self.notices_host.setAlignment_(AppKit.NSLayoutAttributeWidth)
        module.add(self.notices_host)
        self.view.setHidden_(True)

    # Showing.

    def show(self, section):
        """Brings the module up to `section` (a core.camera.view.CameraSection), or hides it for None."""
        if section is None or not section.shown:
            self.section = section
            self.view.setHidden_(True)
            return
        if section == self.section and not self.view.isHidden():
            return
        self.section = section
        self.view.setHidden_(False)
        self.caption.set(section.label)
        self.popup.set(section.choices, section.selected)
        self.quality_caption.set(section.quality_label)
        if self.quality.value != section.quality_selected:
            self.quality.value = section.quality_selected
        self.quality.set_enabled(section.quality_enabled)
        self.status.set(section.line, ink="ink" if section.tone == camera_view.LIVE else section.tone)
        widgets.paint(self.dot, section.tone)
        self.status_button.view.setHidden_(not section.action)
        if section.action:
            self.status_button.set_title(section.action_label)
        self.detail.set(section.detail)
        self.detail_view.setHidden_(not section.detail)
        self.hint.set(section.hint)
        self.hint.view.setHidden_(not section.hint)
        self.help_row.setHidden_(not section.help)
        if section.help:
            self.help.set(section.help)
            self.help_button.set_title(section.help_action_label or tr("Camera settings"))
        signature = (section.this_machine_label, section.this_machine)
        if signature != self.rows_signature:
            self.rows_signature = signature
            self._show_rows(section)
        self.rule.setHidden_(not section.this_machine)
        if section.notices != self.notices_signature:
            self.notices_signature = section.notices
            self._show_notices(section.notices)

    def _clear(self, host):
        for child in list(host.arrangedSubviews()):
            host.removeArrangedSubview_(child)
            child.removeFromSuperview()

    def _show_rows(self, section):
        """This machine's rows, updated in place: a camera state change usually changes a value or a
        button, never the rows' shape, and building a row costs a few ms of the main thread."""
        rows = section.this_machine
        while len(self.row_views) < len(rows):
            self.row_views.append(self._row_view())
        for index, shown in enumerate(self.row_views):
            if index >= len(rows):
                shown.line.setHidden_(True)
                shown.row = None
                continue
            row = rows[index]
            shown.row = row
            shown.line.setHidden_(False)
            shown.heading.set(section.this_machine_label if index == 0 else "")
            shown.label.set(row.label)
            shown.value.set(row.value, ink="ink" if row.tone in (camera_view.PLAIN, camera_view.LIVE) else row.tone,
                            weight=600 if row.tone in (camera_view.FAULT, camera_view.WAIT) else 500)
            shown.button.view.setHidden_(not row.action)
            if row.action:
                shown.button.set_title(row.action_label)
                shown.button.set_style("primary" if row.tone == camera_view.WAIT else "plain")
        self.row_buttons = [(shown.row, shown.button) for shown in self.row_views if shown.row is not None and shown.row.action]

    def _row_view(self):
        shown = _RowView()
        line = widgets.stack(vertical=False, spacing=12)
        line.setAlignment_(AppKit.NSLayoutAttributeCenterY)
        line.setEdgeInsets_(AppKit.NSEdgeInsets(6, 0, 6, 0))
        line.heightAnchor().constraintGreaterThanOrEqualToConstant_(36).setActive_(True)
        shown.heading = widgets.Label("", theme.TYPE["note"], 600, "ink")
        widgets.size(shown.heading.view, HEADING_WIDTH)
        line.addArrangedSubview_(shown.heading.view)
        shown.label = widgets.Label("", theme.TYPE["note"], 400, "ink_2")
        widgets.size(shown.label.view, LABEL_WIDTH)
        line.addArrangedSubview_(shown.label.view)
        shown.value = widgets.Label("", theme.TYPE["note"], 500, "ink", wrap=True)
        widgets.hug(shown.value.view, AppKit.NSLayoutPriorityDefaultLow)
        line.addArrangedSubview_(shown.value.view)
        shown.button = widgets.action_button("", lambda: self._action(shown.row.action if shown.row else None),
                                             scale="small")
        line.addArrangedSubview_(_padded(shown.button))
        shown.line = line
        widgets.add(self.rows_host, line)
        return shown

    def _show_notices(self, notices):
        self._clear(self.notices_host)
        self.notice_buttons = []
        for notice in notices:
            widgets.add(self.notices_host, widgets.hairline())
            line = widgets.stack(vertical=False, spacing=16)
            line.setAlignment_(AppKit.NSLayoutAttributeCenterY)
            line.setEdgeInsets_(AppKit.NSEdgeInsets(10, 0, 10, 0))
            text = widgets.Label(notice.text, theme.TYPE["note"], 500 if notice.tone == camera_view.FAULT else 400,
                                 "ink" if notice.tone in (camera_view.FAULT, camera_view.WAIT) else "ink_2", wrap=True)
            text.view.widthAnchor().constraintLessThanOrEqualToConstant_(theme.READING_WIDTH).setActive_(True)
            words, dot = _dot_line(text)
            widgets.paint(dot, notice.tone)
            widgets.hug(words, AppKit.NSLayoutPriorityDefaultLow)
            line.addArrangedSubview_(words)
            if notice.action:
                button = widgets.action_button(notice.action_label, lambda notice=notice: self._action(notice.action),
                                               scale="small", style="primary" if notice.tone == camera_view.WAIT else "plain")
                line.addArrangedSubview_(_padded(button))
                self.notice_buttons.append((notice, button))
            widgets.add(self.notices_host, line)

    # A person's picks.

    def _picked(self, choice):
        self.on_pick(choice)

    def _quality(self, size):
        self.on_quality(size)

    def _action(self, action):
        if action:
            self.on_action(action)

    @property
    def rows(self):
        """(row, button) for each row with a button, for tests."""
        return list(self.row_buttons)
