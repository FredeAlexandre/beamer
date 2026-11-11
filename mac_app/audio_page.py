# SPDX-License-Identifier: GPL-3.0-only
"""The Audio page (dev/audio/PHASE2.md §9.2): the microphone your apps hear, notices, the machines on
the Crossing map with their routes and levels, and the selected machine's output and devices.

Everything shown comes from `core.audio.view.page_model`; this file only lays it out in AppKit.
The service's callbacks arrive on its worker thread and hop here with `AppHelper.callAfter`. A
snapshot rebuilds what changed (never while one of the page's menus is open); levels only move the
meters, at most 20 times a second, with Core Animation actions off so each update is a few layer
frames. While the page is on screen the page asks the service to `watch`, which turns the meters on.
"""

from __future__ import annotations

import logging
import threading
import weakref

import AppKit
import Foundation
import Quartz
import objc
from PyObjCTools import AppHelper

import theme
import widgets
from core import feature_flags
from core.audio import model, view
from core.i18n import tr
from diagram import _box, _rect, _set_layer_frame_if_changed, _text_layer

LOG = logging.getLogger("beamer.audio.page")

# System Settings panes a notice's action opens.
SETTINGS_URLS = {
    "open_mic_settings": "x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone",
    "open_system_audio_settings": "x-apple.systempreferences:com.apple.preference.security?Privacy_AudioCapture",
}
# Camera (dev/camera/PHASE2.md §8): how each route kind's arrow is drawn, and the order two or three
# routes between the same tiles run side by side.
ARROW_DASHES = {"mic": [5, 4], "camera": [0.5, 4.5], "sound": None}
ARROW_LANES = ("mic", "camera", "sound")


def camera_on():
    """The camera feature flag (core/feature_flags.py CAMERA, set by the fork)."""
    return bool(getattr(feature_flags, "CAMERA", False))


def _quantised(reading):
    """Meter readings to 1/400 of the bar, so an unchanged level moves no layer."""
    if reading is None:
        return None
    return tuple(round(value * 400) / 400 for value in reading)


def _symbol_mask(name, size):
    """An SF Symbol as a mask layer, so the glyph takes a palette colour (and follows a switch) from
    the layer it masks."""
    image = AppKit.NSImage.imageWithSystemSymbolName_accessibilityDescription_(name, None)
    mask = Quartz.CALayer.layer()
    if image is None:
        return mask
    configuration = AppKit.NSImageSymbolConfiguration.configurationWithPointSize_weight_(size, AppKit.NSFontWeightSemibold)
    image = image.imageWithSymbolConfiguration_(configuration) or image
    mask.setContents_(image)
    mask.setContentsGravity_(Quartz.kCAGravityResizeAspect)
    mask.setContentsScale_(2.0)
    return mask


def _glyph(name, size, ink):
    layer = Quartz.CALayer.layer()
    theme.tint(layer, background=ink)
    mask = _symbol_mask(name, size)
    layer.setMask_(mask)
    return layer


def _place_glyph(layer, frame):
    _set_layer_frame_if_changed(layer, _rect(frame))
    _set_layer_frame_if_changed(layer.mask(), ((0, 0), (frame[2], frame[3])))


class LevelMeter(AppKit.NSView):
    """A horizontal level: the RMS fills the track, a tick marks the peak, -60..0 dBFS. Empty (only
    the track) when the service has no level for it."""

    HEIGHT = 6.0

    def isFlipped(self):
        return True

    @objc.python_method
    def setup(self, width=None, height=None):
        self.setTranslatesAutoresizingMaskIntoConstraints_(False)
        self.setWantsLayer_(True)
        self.height = height or self.HEIGHT
        self.heightAnchor().constraintEqualToConstant_(self.height).setActive_(True)
        if width is not None:
            self.widthAnchor().constraintEqualToConstant_(width).setActive_(True)
        layer = self.layer()
        theme.tint(layer, background="well")
        layer.setCornerRadius_(self.height / 2)
        layer.setMasksToBounds_(True)
        self.fill = _box("signal")
        self.peak = _box("ink_2")
        for sublayer in (self.fill, self.peak):
            layer.addSublayer_(sublayer)
        self.reading = None
        self.key = None
        self.setAccessibilityElement_(True)
        self.setAccessibilityRole_(AppKit.NSAccessibilityLevelIndicatorRole)
        self.setAccessibilityLabel_(tr("Level"))
        self._place()
        return self

    @objc.python_method
    def set_reading(self, reading):
        """(rms, peak) as 0..1, or None."""
        reading = _quantised(reading)
        if reading == self.reading:
            return
        self.reading = reading
        self._place()

    def layout(self):
        objc.super(LevelMeter, self).layout()
        self._place()

    @objc.python_method
    def _place(self):
        width, height = self.bounds().size.width, self.bounds().size.height
        rms, peak = self.reading or (0.0, 0.0)
        Quartz.CATransaction.begin()
        Quartz.CATransaction.setDisableActions_(True)
        _set_layer_frame_if_changed(self.fill, ((0, 0), (width * rms, height)))
        _set_layer_frame_if_changed(self.peak, ((max(0.0, width * peak - 2), 0), (2, height)))
        self.peak.setHidden_(self.reading is None or peak <= 0)
        Quartz.CATransaction.commit()


def level_meter(width=None, height=None):
    return LevelMeter.alloc().init().setup(width, height)


class AudioMap(AppKit.NSView):
    """The machines as tiles on the Crossing layout, drawn as the Crossing map draws them, with the
    active microphone and each machine's output on its tile and arrows for the routes: sound solid,
    microphone dashed, grey while waiting. Read-only: a click or an arrow key selects a machine."""

    HEIGHT = 260.0
    MAX_HEIGHT = 420.0
    ROW_SCALE = 1.45
    INSET = 16.0
    MAX_SCALE = 1.6
    # Half the gap opened between tiles that touch on the layout, so a route's arrow has room.
    GAP = (20.0, 14.0)

    def isFlipped(self):
        return True

    @objc.python_method
    def setup(self):
        self.setTranslatesAutoresizingMaskIntoConstraints_(False)
        self.height_constraint = self.heightAnchor().constraintEqualToConstant_(self.HEIGHT)
        self.height_constraint.setActive_(True)
        self.setWantsLayer_(True)
        theme.tint(self.layer(), background="well", border="edge")
        self.layer().setBorderWidth_(1)
        self.layer().setCornerRadius_(4)
        self.tiles = ()
        self.arrows = ()
        self.selected = None
        self.levels = {}
        self.parts = {}
        self.arrow_layers = []
        self.on_select = None
        self.on_window = None
        self.keyboard_focus = False
        self.focus_layer = _box(None, "ink_2", 6)
        self.focus_layer.setBorderWidth_(2)
        self.focus_layer.setHidden_(True)
        self.focus_layer.setZPosition_(30)
        self.layer().addSublayer_(self.focus_layer)
        self.setAccessibilityElement_(True)
        self.setAccessibilityRole_(AppKit.NSAccessibilityGroupRole)
        self.setAccessibilityLabel_(tr("Machines and where their sound goes"))
        return self

    def acceptsFirstResponder(self):
        return True

    def becomeFirstResponder(self):
        self.refresh_keyboard_focus()
        return True

    def resignFirstResponder(self):
        self.keyboard_focus = False
        self._draw()
        return True

    @objc.python_method
    def refresh_keyboard_focus(self):
        self.keyboard_focus = widgets.keyboard_focus_visible(self)
        self._draw()

    def viewDidMoveToWindow(self):
        if self.on_window is not None:
            self.on_window()

    @objc.python_method
    def show(self, tiles, arrows, selected, description=""):
        if (tiles, arrows, selected) == (self.tiles, self.arrows, self.selected):
            return
        self.tiles, self.arrows, self.selected = tiles, arrows, selected
        if tiles:
            # Taller for a layout of several rows, so each tile keeps room for its lines.
            span = max(t.frame[1] + t.frame[3] for t in tiles) - min(t.frame[1] for t in tiles)
            wanted = span * self.ROW_SCALE + 2 * self.INSET - 2 * self.GAP[1]
            height = min(self.MAX_HEIGHT, max(self.HEIGHT, round(wanted)))
            if self.height_constraint.constant() != height:
                self.height_constraint.setConstant_(height)
        shown = {tile.machine for tile in tiles}
        for machine in [machine for machine in self.parts if machine not in shown]:
            for layer in self.parts.pop(machine).values():
                layer.removeFromSuperlayer()
        if description:
            self.setAccessibilityValue_(description)
        self._draw()

    @objc.python_method
    def set_levels(self, levels):
        self.levels = levels
        frames = self._frames()
        Quartz.CATransaction.begin()
        Quartz.CATransaction.setDisableActions_(True)
        for tile in self.tiles:
            if tile.machine in frames:
                self._place_bar(tile, frames[tile.machine])
        Quartz.CATransaction.commit()

    def layout(self):
        objc.super(AudioMap, self).layout()
        self._draw()

    # Geometry: the Crossing map's scale-to-fit, then each tile pulled in from its neighbours.

    @objc.python_method
    def _frames(self):
        width, height = self.bounds().size.width, self.bounds().size.height
        if not self.tiles or width <= 0 or height <= 0:
            return {}
        left = min(tile.frame[0] for tile in self.tiles)
        right = max(tile.frame[0] + tile.frame[2] for tile in self.tiles)
        top = min(tile.frame[1] for tile in self.tiles)
        bottom = max(tile.frame[1] + tile.frame[3] for tile in self.tiles)
        gap_x, gap_y = self.GAP
        scale = min((width - 2 * self.INSET + 2 * gap_x) / max(1.0, right - left),
                    (height - 2 * self.INSET + 2 * gap_y) / max(1.0, bottom - top), self.MAX_SCALE)
        offset_x = width / 2 - (left + right) / 2 * scale
        offset_y = height / 2 - (top + bottom) / 2 * scale
        frames = {}
        for tile in self.tiles:
            x, y, w, h = tile.frame
            frames[tile.machine] = (round(x * scale + offset_x + gap_x), round(y * scale + offset_y + gap_y),
                                    round(w * scale - 2 * gap_x), round(h * scale - 2 * gap_y))
        return frames

    @objc.python_method
    def tile_at(self, point):
        for machine, (x, y, w, h) in self._frames().items():
            if x <= point.x <= x + w and y <= point.y <= y + h:
                return machine
        return None

    # Drawing.

    @objc.python_method
    def _part(self, machine):
        if machine in self.parts:
            return self.parts[machine]
        parts = {
            "tile": _box("panel", "edge", 4),
            "title": _text_layer(theme.TYPE["body"], "ink"),
            "dot": _box("signal", None, 3),
            "subtitle": _text_layer(theme.TYPE["eyebrow"], "ink_3", Quartz.kCAAlignmentLeft),
            "mic": _glyph("mic.fill", 11, "signal"),
            "mic_text": _text_layer(theme.TYPE["note"], "ink", Quartz.kCAAlignmentLeft),
            "speaker": _glyph("speaker.wave.2.fill", 11, "ink_3"),
            "output": _text_layer(theme.TYPE["note"], "ink_2", Quartz.kCAAlignmentLeft),
            "track": _box("well", None, 1.5),
            "bar": _box("signal", None, 1.5),
            "recording": _glyph("record.circle", 12, "signal"),
            "camera": _glyph("web.camera.fill", 11, "ink_3"),
            "camera_text": _text_layer(theme.TYPE["note"], "ink_2", Quartz.kCAAlignmentLeft),
        }
        parts["title"].setFont_(theme.font(theme.TYPE["body"], 600))
        parts["mic_text"].setFont_(theme.font(theme.TYPE["note"], 500))
        for name in ("tile", "title", "dot", "subtitle", "mic", "mic_text", "speaker", "output", "track", "bar",
                     "recording", "camera", "camera_text"):
            self.layer().addSublayer_(parts[name])
        self.parts[machine] = parts
        return parts

    @objc.python_method
    def _draw(self):
        frames = self._frames()
        Quartz.CATransaction.begin()
        Quartz.CATransaction.setDisableActions_(True)
        for tile in self.tiles:
            frame = frames.get(tile.machine)
            if frame is None:
                continue
            self._draw_tile(tile, frame)
        self._draw_arrows(frames)
        self.removeAllToolTips()
        self.tooltips = {}
        for tile in self.tiles:
            frame = frames.get(tile.machine)
            if frame is not None and tile.recording_line:
                self.addToolTipRect_owner_userData_(
                    AppKit.NSMakeRect(frame[0] + frame[2] - 28, frame[1] + 6, 22, 22), self, len(self.tooltips))
                self.tooltips[len(self.tooltips)] = tile.recording_line
        focus = frames.get(self.selected) if self.keyboard_focus else None
        self.focus_layer.setHidden_(focus is None)
        if focus is not None:
            _set_layer_frame_if_changed(self.focus_layer, _rect((focus[0] - 4, focus[1] - 4, focus[2] + 8, focus[3] + 8)))
        Quartz.CATransaction.commit()

    @objc.python_method
    def _draw_tile(self, tile, frame):
        parts = self._part(tile.machine)
        x, y, w, h = frame
        box = parts["tile"]
        _set_layer_frame_if_changed(box, _rect(frame))
        box.setBorderWidth_(2 if tile.selected else 1)
        theme.tint(box, background="well" if tile.own else "panel", border="ink_2" if tile.selected else "edge")
        dim = 0.42 if not tile.online else 1.0
        for name in ("tile", "title", "dot", "subtitle", "mic", "mic_text", "speaker", "output", "track", "bar",
                     "camera", "camera_text"):
            parts[name].setOpacity_(dim)
        title = parts["title"]
        title.setString_(tile.title)
        # An app here records Beamer's microphone: a mark in the corner, the title kept centred.
        inset = 30 if tile.recording else 10
        _set_layer_frame_if_changed(title, ((x + inset, y + 10), (max(1, w - 2 * inset), 20)))
        parts["recording"].setHidden_(not tile.recording)
        _place_glyph(parts["recording"], (x + w - 24, y + 12, 14, 14))
        theme.tint(parts["dot"], background="signal" if tile.online else "ink_3")
        _set_layer_frame_if_changed(parts["dot"], ((x + 12, y + 37), (6, 6)))
        parts["subtitle"].setString_(tile.subtitle)
        _set_layer_frame_if_changed(parts["subtitle"], ((x + 24, y + 32), (max(1, w - 34), 16)))
        # The microphone and the output in two lines over the level bar, the camera's line above the
        # microphone's; a short tile keeps the output.
        mic_text = tile.mic_line or tile.listen_line or tile.input_line
        out_y = y + h - 36
        mic_y = out_y - 21
        # Short of room, the shared mic (or who hears it) beats the subtitle, and the subtitle
        # beats the machine's own default mic; the shared camera (or an app using it) counts as the
        # shared mic does, and gives way to it.
        important = bool(tile.mic_line or tile.listen_line)
        camera_text = tile.camera_line or tile.watching_line
        camera_important = bool(tile.camera_line or (tile.watching_line and self._watched(tile.machine)))
        lines = sorted(([("mic", important)] if mic_text else []) + ([("camera", camera_important)] if camera_text else []),
                       key=lambda line: (not line[1], line[0] != "mic"))
        placed, next_y = {}, mic_y
        for name, wanted in lines:
            if next_y >= y + 50 or (wanted and next_y >= y + 30):
                placed[name] = next_y
                next_y -= 20
        show_mic = "mic" in placed
        mic_y = placed.get("mic", mic_y)
        squeezed = any(line_y < y + 50 for line_y in placed.values())
        parts["subtitle"].setHidden_(squeezed)
        parts["dot"].setHidden_(squeezed)
        for name in ("mic", "mic_text"):
            parts[name].setHidden_(not show_mic)
        for name in ("camera", "camera_text"):
            parts[name].setHidden_("camera" not in placed)
        if "camera" in placed:
            camera_y = placed["camera"]
            lit = tile.camera_live or (bool(tile.watching_line) and self._watched(tile.machine))
            _place_glyph(parts["camera"], (x + 11, camera_y + 1, 15, 14))
            theme.tint(parts["camera"], background="signal" if lit else "ink_3")
            parts["camera_text"].setString_(camera_text)
            parts["camera_text"].setFont_(theme.font(theme.TYPE["note"], 600 if tile.has_active_camera else 400))
            theme.tint(parts["camera_text"], foreground="ink" if tile.has_active_camera else "signal" if lit else "ink_3")
            _set_layer_frame_if_changed(parts["camera_text"], ((x + 30, camera_y), (max(1, w - 40), 17)))
        _place_glyph(parts["mic"], (x + 12, mic_y + 1, 12, 14))
        theme.tint(parts["mic"], background="signal" if tile.has_active_mic or tile.listen_line else "ink_3")
        parts["mic_text"].setString_(mic_text)
        parts["mic_text"].setFont_(theme.font(theme.TYPE["note"], 600 if tile.has_active_mic else 400))
        theme.tint(parts["mic_text"], foreground="ink" if tile.has_active_mic else "signal" if tile.listen_line else "ink_3")
        _set_layer_frame_if_changed(parts["mic_text"], ((x + 30, mic_y), (max(1, w - 40), 17)))
        _place_glyph(parts["speaker"], (x + 11, out_y + 1, 15, 14))
        theme.tint(parts["speaker"], background=tile.output_tone if tile.output_tone != view.PLAIN else "ink_3")
        parts["output"].setString_(tile.output_line)
        theme.tint(parts["output"], foreground=tile.output_tone)
        _set_layer_frame_if_changed(parts["output"], ((x + 30, out_y), (max(1, w - 40), 17)))
        _set_layer_frame_if_changed(parts["track"], ((x + 12, y + h - 13), (max(1, w - 24), 3)))
        parts["track"].setHidden_(tile.meter is None)
        self._place_bar(tile, frame)

    @objc.python_method
    def _watched(self, machine):
        """A live camera arrow ends at `machine`: an app there sees the camera now."""
        return any(arrow.kind == "camera" and arrow.target == machine and arrow.state == "active" for arrow in self.arrows)

    @objc.python_method
    def _place_bar(self, tile, frame):
        parts = self.parts.get(tile.machine)
        if parts is None:
            return
        reading = _quantised(view.meter_reading(self.levels, tile.meter))
        bar = parts["bar"]
        bar.setHidden_(reading is None)
        if reading is None:
            return
        x, y, w, h = frame
        _set_layer_frame_if_changed(bar, ((x + 12, y + h - 13), (max(0.0, (w - 24) * reading[0]), 3)))

    @objc.python_method
    def _draw_arrows(self, frames):
        wanted = []
        pairs = {}
        for arrow in self.arrows:
            pairs.setdefault(frozenset((arrow.source, arrow.target)), []).append(arrow)
        for arrow in self.arrows:
            if arrow.source not in frames or arrow.target not in frames:
                continue
            siblings = pairs[frozenset((arrow.source, arrow.target))]
            # Routes between the same machines run side by side: the mic, the camera, the sound.
            kinds = sorted({sibling.kind for sibling in siblings},
                           key=lambda kind: ARROW_LANES.index(kind) if kind in ARROW_LANES else len(ARROW_LANES))
            if len(kinds) == 1:
                lane = 0.0
            elif len(kinds) == 2:
                lane = -1.0 if kinds.index(arrow.kind) == 0 else 1.0
            else:
                lane = (kinds.index(arrow.kind) - (len(kinds) - 1) / 2) * 1.6
            wanted.append((arrow, _route(frames[arrow.source], frames[arrow.target], lane * 11)))
        while len(self.arrow_layers) < len(wanted):
            line = Quartz.CAShapeLayer.layer()
            line.setFillColor_(None)
            line.setLineWidth_(2)
            line.setLineCap_(Quartz.kCALineCapRound)
            line.setZPosition_(20)
            head = Quartz.CAShapeLayer.layer()
            head.setZPosition_(20)
            self.layer().addSublayer_(line)
            self.layer().addSublayer_(head)
            self.arrow_layers.append((line, head))
        for index, (line, head) in enumerate(self.arrow_layers):
            hidden = index >= len(wanted)
            line.setHidden_(hidden)
            head.setHidden_(hidden)
            if hidden:
                continue
            arrow, (start, control, end, tip) = wanted[index]
            ink = "signal" if arrow.state == "active" else "ink_3"
            theme.tint(line, stroke=ink)
            theme.tint(head, fill=ink)
            line.setLineDashPattern_(ARROW_DASHES.get(arrow.kind))
            path = Quartz.CGPathCreateMutable()
            Quartz.CGPathMoveToPoint(path, None, *start)
            if control is None:
                Quartz.CGPathAddLineToPoint(path, None, *end)
            else:
                Quartz.CGPathAddQuadCurveToPoint(path, None, control[0], control[1], *end)
            line.setPath_(path)
            head.setPath_(_head_path(tip, end))

    def view_stringForToolTip_point_userData_(self, _view, _tag, _point, data):
        return getattr(self, "tooltips", {}).get(data, "")

    # Input.

    def mouseDown_(self, event):
        point = self.convertPoint_fromView_(event.locationInWindow(), None)
        machine = self.tile_at(point)
        if machine is not None:
            self.select(machine)

    @objc.python_method
    def select(self, machine):
        if machine != self.selected and self.on_select is not None:
            self.on_select(machine)

    def keyDown_(self, event):
        order = [tile.machine for tile in self.tiles]
        key = event.charactersIgnoringModifiers()
        steps = {AppKit.NSLeftArrowFunctionKey: -1, AppKit.NSUpArrowFunctionKey: -1,
                 AppKit.NSRightArrowFunctionKey: 1, AppKit.NSDownArrowFunctionKey: 1}
        step = steps.get(key[:1]) if key else None
        if step is None or not order:
            objc.super(AudioMap, self).keyDown_(event)
            return
        index = order.index(self.selected) if self.selected in order else 0
        self.select(order[min(max(index + step, 0), len(order) - 1)])

    def accessibilityPerformPress(self):
        return True


def _edge_point(frame, towards, lane):
    """Where the line from `frame`'s centre towards `towards` leaves the frame, moved `lane` points
    across the line so two routes between the same tiles run side by side."""
    x, y, w, h = frame
    cx, cy = x + w / 2, y + h / 2
    dx, dy = towards[0] - cx, towards[1] - cy
    length = max(1e-6, (dx * dx + dy * dy) ** 0.5)
    nx, ny = -dy / length * lane, dx / length * lane
    cx, cy = cx + nx, cy + ny
    scale = min(w / 2 / abs(dx) if dx else float("inf"), h / 2 / abs(dy) if dy else float("inf"))
    return cx + dx * scale, cy + dy * scale


def _route(source, target, lane):
    """(start, control or None, end, tip direction point) for an arrow from tile `source` to `target`.
    Level across the gap between side-by-side tiles (upright between stacked ones), in the middle
    of the span they share; otherwise centre to centre, bowed when the tiles are far apart."""
    pad = 3.0
    sx, sy, sw, sh = source
    tx, ty, tw, th = target
    top, bottom = max(sy, ty), min(sy + sh, ty + th)
    left, right = max(sx, tx), min(sx + sw, tx + tw)
    if bottom - top > 24 and (tx >= sx + sw or sx >= tx + tw):
        y = (top + bottom) / 2 + lane
        forward = tx >= sx + sw
        start = (sx + sw + pad if forward else sx - pad, y)
        end = (tx - pad - 7 if forward else tx + tw + pad + 7, y)
        if abs(end[0] - start[0]) >= 12:
            return start, None, end, start
    if right - left > 24 and (ty >= sy + sh or sy >= ty + th):
        x = (left + right) / 2 + lane
        forward = ty >= sy + sh
        start = (x, sy + sh + pad if forward else sy - pad)
        end = (x, ty - pad - 7 if forward else ty + th + pad + 7)
        if abs(end[1] - start[1]) >= 12:
            return start, None, end, start
    sc = (source[0] + source[2] / 2, source[1] + source[3] / 2)
    tc = (target[0] + target[2] / 2, target[1] + target[3] / 2)
    # A consistent side for the lane whichever way the arrow runs.
    sign = 1.0 if (sc[0], sc[1]) < (tc[0], tc[1]) else -1.0
    start = _edge_point(source, tc, lane * sign)
    end = _edge_point(target, sc, -lane * sign)
    dx, dy = end[0] - start[0], end[1] - start[1]
    length = max(1e-6, (dx * dx + dy * dy) ** 0.5)
    ux, uy = dx / length, dy / length
    start = (start[0] + ux * pad, start[1] + uy * pad)
    end = (end[0] - ux * (pad + 7), end[1] - uy * (pad + 7))
    control = None
    if length > 160:
        bow = min(60.0, length / 4)
        mid = ((start[0] + end[0]) / 2 - uy * bow * sign, (start[1] + end[1]) / 2 + ux * bow * sign)
        control = mid
        tail = mid
    else:
        tail = start
    return start, control, end, tail


def _head_path(tail, end):
    """A filled arrowhead whose base sits at `end`, pointing away from `tail`."""
    dx, dy = end[0] - tail[0], end[1] - tail[1]
    length = max(1e-6, (dx * dx + dy * dy) ** 0.5)
    ux, uy = dx / length, dy / length
    tip = (end[0] + ux * 8, end[1] + uy * 8)
    left = (end[0] - uy * 4.5, end[1] + ux * 4.5)
    right = (end[0] + uy * 4.5, end[1] - ux * 4.5)
    path = Quartz.CGPathCreateMutable()
    Quartz.CGPathMoveToPoint(path, None, *tip)
    Quartz.CGPathAddLineToPoint(path, None, *left)
    Quartz.CGPathAddLineToPoint(path, None, *right)
    Quartz.CGPathCloseSubpath(path)
    return path


class _MenuWatch(AppKit.NSObject):
    """A popup's menu delegate: tells the page while a menu is open, so no refresh rebuilds it."""

    def initWithOwner_(self, owner):
        self = objc.super(_MenuWatch, self).init()
        if self is None:
            return None
        self.owner = weakref.ref(owner)
        return self

    def menuWillOpen_(self, _menu):
        owner = self.owner()
        if owner is not None:
            owner.menu_open(True)

    def menuDidClose_(self, _menu):
        owner = self.owner()
        if owner is not None:
            owner.menu_open(False)


class ChoicePopup:
    """A popup of `view.Choice`s: machine headers, the choices under them indented, and on the
    closed button the chosen one's full title ("lebig's Wave:3", not just "Wave:3")."""

    def __init__(self, on_pick, on_menu, label):
        self.on_pick = on_pick
        self.on_menu = on_menu
        self.choices = ()
        self.selected = None
        self.view = widgets.box()
        self.popup = widgets._autolayout(AppKit.NSPopUpButton.alloc().initWithFrame_pullsDown_(((0, 0), (0, 0)), False))
        self.popup.setFont_(theme.font(theme.TYPE["body"]))
        self.popup.setAutoenablesItems_(False)
        self.popup.setAccessibilityLabel_(label)
        self.popup.cell().setUsesItemFromMenu_(False)
        self.shown = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_("", None, "")
        self.popup.cell().setMenuItem_(self.shown)
        self.view.addSubview_(self.popup)
        widgets.pin(self.popup, self.view, (0, 4, 0, 4))
        owner = weakref.ref(self)

        def picked(_sender):
            menu = owner()
            if menu is not None:
                menu._picked()

        self.target = widgets._Target.alloc().initWithCallback_(picked)
        self.popup.setTarget_(self.target)
        self.popup.setAction_("fire:")
        self.watch = _MenuWatch.alloc().initWithOwner_(self)
        self.popup.menu().setDelegate_(self.watch)
        self.view.widthAnchor().constraintLessThanOrEqualToConstant_(380).setActive_(True)
        preference = self.view.widthAnchor().constraintEqualToConstant_(380)
        preference.setPriority_(255)
        preference.setActive_(True)
        self.view.heightAnchor().constraintGreaterThanOrEqualToConstant_(30).setActive_(True)
        widgets.squeeze(self.view)
        widgets.squeeze(self.popup)

    def menu_open(self, is_open):
        self.on_menu(is_open)

    def set(self, choices, selected, enabled=True):
        if choices != self.choices:
            self.choices = choices
            menu = self.popup.menu()
            menu.removeAllItems()
            under_header = False
            for choice in choices:
                if choice.header:
                    header = getattr(AppKit.NSMenuItem, "sectionHeaderWithTitle_", None)
                    item = header(choice.label) if header is not None else None
                    if item is None:
                        item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(choice.label, None, "")
                        item.setAttributedTitle_(widgets.attributed(choice.label, theme.TYPE["small"], 700, "ink_3"))
                    item.setEnabled_(False)
                    if menu.numberOfItems():
                        menu.addItem_(AppKit.NSMenuItem.separatorItem())
                    menu.addItem_(item)
                    under_header = True
                    continue
                item = AppKit.NSMenuItem.alloc().initWithTitle_action_keyEquivalent_(choice.label, None, "")
                item.setEnabled_(choice.enabled)
                item.setIndentationLevel_(1 if under_header else 0)
                item.setToolTip_(choice.title)
                item.setRepresentedObject_(choice.key)
                menu.addItem_(item)
            self.selected = None
        if selected != self.selected or self.shown.title() != self._title(selected):
            self.selected = selected
            index = self._index(selected)
            if index >= 0:
                self.popup.selectItemAtIndex_(index)
            self.shown.setTitle_(self._title(selected))
            self.popup.setAccessibilityValue_(self._title(selected))
            self.popup.setNeedsDisplay_(True)
        self.popup.setEnabled_(enabled)

    def _title(self, key):
        choice = next((choice for choice in self.choices if choice.key == key), None)
        return choice.title if choice is not None else ""

    def _index(self, key):
        menu = self.popup.menu()
        for index in range(menu.numberOfItems()):
            if menu.itemAtIndex_(index).representedObject() == key:
                return index
        return -1

    def _picked(self):
        item = self.popup.selectedItem()
        key = item.representedObject() if item is not None else None
        choice = view.choice_by_key(self.choices, key)
        if choice is None:
            return
        self.selected = key
        self.shown.setTitle_(choice.title)
        self.on_pick(choice)

    def pick(self, key):
        """As if a person chose `key` in the menu (tests, keyboard)."""
        index = self._index(key)
        if index >= 0:
            self.popup.selectItemAtIndex_(index)
            self._picked()


class _Observer(AppKit.NSObject):
    def initWithCallback_(self, callback):
        self = objc.super(_Observer, self).init()
        if self is None:
            return None
        self.callback = callback
        return self

    def changed_(self, _notification):
        self.callback()


def _legend_swatch(dashed, ink="signal", dashes=None):
    swatch = widgets.box()
    widgets.size(swatch, 22, 10)
    line = Quartz.CAShapeLayer.layer()
    path = Quartz.CGPathCreateMutable()
    Quartz.CGPathMoveToPoint(path, None, 1, 5)
    Quartz.CGPathAddLineToPoint(path, None, 15, 5)
    line.setPath_(path)
    line.setLineWidth_(2)
    line.setLineCap_(Quartz.kCALineCapRound)
    line.setFillColor_(None)
    theme.tint(line, stroke=ink)
    if dashes:
        line.setLineDashPattern_(dashes)
    elif dashed:
        line.setLineDashPattern_([4, 3])
    head = Quartz.CAShapeLayer.layer()
    head.setPath_(_head_path((0, 5), (14, 5)))
    theme.tint(head, fill=ink)
    swatch.layer().addSublayer_(line)
    swatch.layer().addSublayer_(head)
    return swatch


def _legend_item(swatch, text, ink="ink_2"):
    item = widgets.stack(vertical=False, spacing=6)
    item.addArrangedSubview_(swatch)
    item.addArrangedSubview_(widgets.Label(text, theme.TYPE["note"], 500, ink).view)
    return item


class _RowView:
    """One device row's widgets, updated in place while its device stays."""

    def __init__(self, row, line):
        self.row = row
        self.line = line
        self.name = self.detail = self.active = self.meter = self.control = None

    def update(self, row):
        self.row = row
        self.name.set(row.name, weight=600 if row.active else 500)
        self.detail.set(row.detail)
        self.active.set(row.active_label)
        self.active.view.setHidden_(not row.active_label)
        self.meter.key = row.meter
        if row.kind == "input":
            self.control.set_enabled(row.test_enabled)
        else:
            self.control.value = row.headset
            self.control.set_enabled(row.headset_enabled)


def _driver_status():
    import audio_driver
    return audio_driver.status()


def _install_driver(**options):
    import audio_driver
    return audio_driver.install(**options)


def _end_column(control):
    """A row's control in a fixed column at its end, so every row's meter lines up."""
    column = widgets.box()
    widgets.size(column, 112)
    column.addSubview_(control)
    AppKit.NSLayoutConstraint.activateConstraints_([
        control.trailingAnchor().constraintEqualToAnchor_(column.trailingAnchor()),
        control.leadingAnchor().constraintGreaterThanOrEqualToAnchor_(column.leadingAnchor()),
        control.centerYAnchor().constraintEqualToAnchor_(column.centerYAnchor()),
        column.heightAnchor().constraintGreaterThanOrEqualToAnchor_(control.heightAnchor()),
    ])
    return column


def _dot_line(label):
    """`label` with a 6 pt dot beside its first line, as the Crossing legend marks its states.
    Returns (view, dot); the dot's colour is set with widgets.paint."""
    holder = widgets.box()
    dot = widgets.box("ink_3", radius=3)
    holder.addSubview_(dot)
    holder.addSubview_(label.view)
    font = theme.font(label.size, label.weight)
    AppKit.NSLayoutConstraint.activateConstraints_([
        dot.widthAnchor().constraintEqualToConstant_(6),
        dot.heightAnchor().constraintEqualToConstant_(6),
        dot.leadingAnchor().constraintEqualToAnchor_(holder.leadingAnchor()),
        dot.centerYAnchor().constraintEqualToAnchor_constant_(label.view.firstBaselineAnchor(), -font.xHeight() / 2),
        label.view.leadingAnchor().constraintEqualToAnchor_constant_(holder.leadingAnchor(), 14),
        label.view.topAnchor().constraintEqualToAnchor_(holder.topAnchor()),
        holder.bottomAnchor().constraintEqualToAnchor_(label.view.bottomAnchor()),
        holder.trailingAnchor().constraintGreaterThanOrEqualToAnchor_(label.view.trailingAnchor()),
    ])
    return holder, dot


class AudioPage:
    """The page's modules in `body`, kept in step with the service.

    `service`: a callable returning the AudioService, or None while there is none. `tiles`: a
    callable returning the Crossing layout (settings["layout"]). `peers_without_audio`: a callable
    returning the names of linked machines whose Beamer has no audio. `call_after` hops to the main
    thread (AppHelper.callAfter; tests pass their own)."""

    def __init__(self, body, service, tiles, own_id, own_label, peers_without_audio=lambda: (),
                 call_after=None, open_url=None, driver=None, installer=None):
        self.service_of = service
        self.tiles_of = tiles
        self.own_id = own_id
        self.own_label = own_label
        self.peers_without_audio = peers_without_audio
        self.call_after = call_after or AppHelper.callAfter
        self.open_url = open_url or (lambda url: AppKit.NSWorkspace.sharedWorkspace().openURL_(
            AppKit.NSURL.URLWithString_(url)))
        self.driver_of = driver or _driver_status
        self.installer = installer or _install_driver
        self.driver = None
        self.driver_loaded = False
        self.installing = False
        self.service = None
        self.unsubscribe = []
        self.selected = None
        self.page = None
        self.levels = {}
        self.levels_pending = False
        self.snapshot_pending = False
        self.dirty = True
        self.menus_open = 0
        self.watching = False
        self.service_wait = False
        self.observers = []
        self.observed_window = None
        self.row_views = []
        self.row_sets = {}
        self.notice_signature = None
        self.timings = None
        self.camera_snap = None
        self.camera_model = None
        self._build(body)

    # Building.

    def _module(self, body, title):
        module = widgets.Module()
        heading = widgets.Label(title, theme.TYPE["body"], 600)
        module.add(heading.view)
        widgets.add(body, module.view)
        return module, heading

    def _build(self, body):
        self.body = body
        mic, _ = self._module(body, tr("Microphone"))
        self.mic_module = mic
        self.mic_popup = ChoicePopup(self._pick_mic, self._menu, tr("Microphone your apps hear"))
        row, self.mic_caption = widgets.field_row(tr("Microphone your apps hear"), self.mic_popup.view)
        self.mic_row = row
        mic.add(row)
        self.mic_status = widgets.Label("", theme.TYPE["body"], 500, wrap=True)
        status, self.mic_dot = _dot_line(self.mic_status)
        self.mic_status_row = status
        mic.add(status)
        self.mic_meter = level_meter()
        self.mic_meter.setAccessibilityLabel_(tr("Shared microphone level"))
        mic.add(self.mic_meter)
        self.mic_hint = widgets.note(ink="ink_3")
        mic.add(self.mic_hint.view)
        mic.body.setCustomSpacing_afterView_(14, row)

        # Notices right under the microphone, where a missing driver or a denied permission is
        # seen without scrolling.
        self.notices_module = widgets.Module(spacing=0)
        widgets.add(body, self.notices_module.view)
        self.notices_host = widgets.stack(spacing=0)
        self.notices_host.setAlignment_(AppKit.NSLayoutAttributeWidth)
        self.notices_module.add(self.notices_host)
        self.install_note = widgets.note(ink="fault")
        self.install_note.view.setHidden_(True)
        self.notices_module.add(self.install_note.view)
        self.notice_buttons = []

        # The camera (dev/camera/PHASE2.md §8.3): its module under the notices strip, hidden until
        # the camera feature and a machine with a camera side are there.
        import camera_section
        self.camera = camera_section.CameraModule(body, self._pick_camera, self._pick_quality, self._camera_action,
                                                  self._menu)

        machines, _ = self._module(body, tr("Machines"))
        self.map = AudioMap.alloc().init().setup()
        self.map.on_select = self._select
        self.map.on_window = self._window_changed
        machines.add(self.map)
        machines.body.setCustomSpacing_afterView_(10, self.map)
        legend = widgets.stack(vertical=False, spacing=18)
        legend.addArrangedSubview_(_legend_item(_legend_swatch(False), tr("Sound")))
        legend.addArrangedSubview_(_legend_item(_legend_swatch(True), tr("Microphone")))
        self.camera_legend = _legend_item(_legend_swatch(True, dashes=ARROW_DASHES["camera"]), tr("Camera"))
        self.camera_legend.setHidden_(True)
        legend.addArrangedSubview_(self.camera_legend)
        legend.addArrangedSubview_(_legend_item(_legend_swatch(False, "ink_3"), tr("Waiting"), "ink_3"))
        machines.add(legend, full_width=False)
        machines.add(widgets.note(tr("Click a machine to see its devices and choose where its sound plays.")).view)

        panel, self.panel_heading = self._module(body, "")
        self.panel_module = panel
        self.output_popup = ChoicePopup(self._pick_output, self._menu, tr("Sound plays on"))
        row, self.output_caption = widgets.field_row(tr("Sound plays on"), self.output_popup.view)
        self.output_row = row
        panel.add(row)
        self.mute_note = widgets.note(ink="ink_3")
        panel.add(self.mute_note.view)
        self.output_status = widgets.Label("", theme.TYPE["body"], 500, wrap=True)
        line, self.output_dot = _dot_line(self.output_status)
        self.output_line = line
        panel.add(line)
        # A Windows machine's own low-latency switch: the model never shows it on a Mac (each
        # machine sets its own), kept here so both apps render the same panel model.
        self.vmic_exclusive = widgets.Switch(tr("Low-latency virtual microphone"),
                                             on_change=self._vmic_exclusive_changed)
        self.vmic_exclusive_note = widgets.note(ink="ink_3")
        self.vmic_exclusive_box = widgets.stack(spacing=2)
        self.vmic_exclusive_box.setAlignment_(AppKit.NSLayoutAttributeWidth)
        widgets.add(self.vmic_exclusive_box, self.vmic_exclusive.view)
        widgets.add(self.vmic_exclusive_box, self.vmic_exclusive_note.view)
        self.vmic_exclusive_box.setHidden_(True)
        panel.add(self.vmic_exclusive_box)
        # One stack of device rows per machine, kept while its devices stay, so going back to a
        # machine shows its rows at once; only the selected one is in the panel.
        self.rows_host = widgets.stack(spacing=0)
        self.rows_host.setAlignment_(AppKit.NSLayoutAttributeWidth)
        self.rows_host.setDetachesHiddenViews_(True)
        panel.add(self.rows_host)
        panel.body.setCustomSpacing_afterView_(16, line)


    def _row_set(self, panel):
        """(signature, stack, row views) of `panel`'s machine: updated in place while its devices
        stay, built again when one comes or goes."""
        rows = panel.inputs + panel.outputs
        signature = (tuple((row.kind, row.key) for row in rows), panel.inputs_empty, panel.outputs_empty)
        held = self.row_sets.get(panel.machine)
        if held is not None and held[0] == signature:
            for shown, row in zip(held[2], rows):
                shown.update(row)
            return held
        if held is not None:
            self.rows_host.removeArrangedSubview_(held[1])
            held[1].removeFromSuperview()
        host = widgets.stack(spacing=0)
        host.setAlignment_(AppKit.NSLayoutAttributeWidth)
        views = []
        self._device_rows(host, views, tr("Inputs"), panel.inputs, panel.inputs_empty)
        self._device_rows(host, views, tr("Outputs"), panel.outputs, panel.outputs_empty)
        widgets.add(self.rows_host, host)
        held = self.row_sets[panel.machine] = (signature, host, views)
        return held

    def _device_rows(self, host, views, title, rows, empty):
        widgets.add(host, self._section(title))
        if not rows:
            label = widgets.note(empty, ink="ink_3")
            holder = widgets.stack(spacing=0)
            holder.setEdgeInsets_(AppKit.NSEdgeInsets(8, 0, 12, 0))
            widgets.add(holder, label.view)
            widgets.add(host, holder)
            return
        for index, row in enumerate(rows):
            if index:
                widgets.add(host, widgets.hairline())
            widgets.add(host, self._device_row(row, views))

    def _section(self, title):
        holder = widgets.stack(spacing=0)
        holder.setEdgeInsets_(AppKit.NSEdgeInsets(14, 0, 6, 0))
        widgets.add(holder, widgets.eyebrow(title))
        return holder

    def _device_row(self, row, views):
        line = widgets.stack(vertical=False, spacing=14)
        line.setEdgeInsets_(AppKit.NSEdgeInsets(8, 0, 8, 0))
        line.heightAnchor().constraintGreaterThanOrEqualToConstant_(52).setActive_(True)
        shown = _RowView(row, line)
        words = widgets.stack(spacing=2)
        shown.name = widgets.Label(row.name, theme.TYPE["body"], 600 if row.active else 500)
        widgets.squeeze(shown.name.view)
        words.addArrangedSubview_(shown.name.view)
        detail = widgets.stack(vertical=False, spacing=8)
        detail.setDetachesHiddenViews_(True)
        shown.detail = widgets.Label(row.detail, theme.TYPE["small"], ink="ink_3")
        shown.active = widgets.Label(row.active_label, theme.TYPE["small"], 600, "signal")
        shown.active.view.setHidden_(not row.active_label)
        detail.addArrangedSubview_(shown.detail.view)
        detail.addArrangedSubview_(shown.active.view)
        words.addArrangedSubview_(detail)
        widgets.hug(words, AppKit.NSLayoutPriorityDefaultLow)
        line.addArrangedSubview_(words)
        shown.meter = level_meter(width=120)
        shown.meter.setAccessibilityLabel_(tr("{device} level").format(device=row.name))
        line.addArrangedSubview_(shown.meter)
        if row.kind == "input":
            button = widgets.action_button(tr("Test"), lambda: self._test(shown.row), scale="small")
            button.view.widthAnchor().constraintGreaterThanOrEqualToConstant_(64).setActive_(True)
            button.view.setContentCompressionResistancePriority_forOrientation_(
                AppKit.NSLayoutPriorityRequired, AppKit.NSLayoutConstraintOrientationHorizontal)
            button.view.setToolTip_(tr("Show this microphone's level for ten seconds."))
            line.addArrangedSubview_(_end_column(button.view))
            shown.control = button
        else:
            switch = widgets.Switch(tr("Headset"), on_change=lambda on: self._headset(shown.row, on))
            switch.view.setToolTip_(tr("A headset cannot be heard by a microphone, so Beamer leaves its echo alone."))
            widgets.hug(switch.view)
            widgets.hug(switch.words.view)
            line.addArrangedSubview_(_end_column(switch.view))
            shown.control = switch
        shown.update(row)
        views.append(shown)
        return line

    @property
    def row_controls(self):
        return [(shown.row, shown.control, shown.line) for shown in self.row_views]

    @property
    def meters(self):
        meters = {}
        for shown in self.row_views:
            meters.setdefault(shown.row.meter, []).append(shown.meter)
        return meters

    # Data.

    @property
    def visible(self):
        window = self.map.window()
        if window is None or not window.isVisible() or self.map.isHiddenOrHasHiddenAncestor():
            return False
        return bool(window.occlusionState() & AppKit.NSWindowOcclusionStateVisible)

    def _ensure_service(self):
        service = self.service_of()
        if service is self.service:
            return service
        for unsubscribe in self.unsubscribe:
            unsubscribe()
        self.unsubscribe = []
        self.service = service
        self.watching = False
        if service is not None:
            self.unsubscribe = [service.subscribe(self._snapshot_arrived),
                                service.subscribe_levels(self._levels_arrived)]
        return service

    def snapshot(self):
        service = self._ensure_service()
        if service is None:
            return model.empty_snapshot(self.own_id(), helper="no_helper")
        return service.snapshot()

    def camera_snapshot(self):
        """The service's camera snapshot (core.camera.model.CameraSnapshot) while the camera feature
        is on and the service has a camera part, else None."""
        service = self.service
        reader = getattr(service, "camera_snapshot", None) if service is not None and camera_on() else None
        if reader is None:
            return None
        try:
            return reader()
        except Exception:  # a camera fault must not take the Audio page down
            LOG.exception("camera: snapshot")
            return None

    def refresh(self):
        """Builds the page model and brings every part of the page up to it; cheap when nothing
        changed. Deferred while a menu of the page is open."""
        if self.menus_open:
            self.dirty = True
            return
        self.dirty = False
        started = Foundation.CFAbsoluteTimeGetCurrent() if self.timings is not None else 0
        if not self.driver_loaded:
            self._load_driver()
        audio = self.snapshot()
        camera = self.camera_snapshot()
        extras = section = None
        if camera is not None:
            from core.camera import view as camera_view
            extras = camera_view.map_extras(camera)
            section = camera_view.camera_section(camera, audio, self.own_label, "macos")
            if not section.shown:
                extras = None
        self.camera_snap = camera
        page = view.page_model(audio, view.layout_tiles(self.tiles_of()), self.selected,
                               self.own_label, tuple(self.peers_without_audio()),
                               getattr(self.driver, "state", None), camera=extras)
        if page != self.page:
            self._show(page)
        if section != self.camera_model:
            self.camera_model = section
            self.camera.show(section)
            self.camera_legend.setHidden_(section is None or not section.shown)
        if self.timings is not None:
            self.timings.append(("refresh", Foundation.CFAbsoluteTimeGetCurrent() - started))

    def _show(self, page):
        previous, self.page = self.page, page
        self.selected = page.selected
        self.mic_popup.set(page.mic_choices, page.mic_selected)
        self.mic_status.set(page.mic_line, ink="ink" if page.mic_tone == view.LIVE else page.mic_tone)
        widgets.paint(self.mic_dot, page.mic_tone)
        self.mic_hint.set(page.mic_hint)
        self.mic_meter.key = page.mic_meter
        self.mic_meter.setHidden_(page.mic_meter is None)
        self.map.show(page.tiles, page.arrows, page.selected, page.map_description)
        panel = page.panel
        self.panel_heading.set(panel.heading)
        self.output_caption.set(panel.output_label)
        self.output_popup.set(panel.output_choices, panel.output_selected, panel.output_enabled)
        self.mute_note.set(panel.mute_note)
        self.mute_note.view.setHidden_(not panel.mute_note)
        self.output_status.set(panel.output_line, ink="ink" if panel.output_tone == view.LIVE else panel.output_tone)
        widgets.paint(self.output_dot, panel.output_tone)
        self.output_line.setHidden_(not panel.output_line)
        self.vmic_exclusive_box.setHidden_(not panel.vmic_exclusive_shown)
        if panel.vmic_exclusive_shown:
            self.vmic_exclusive.value = panel.vmic_exclusive_on
            self.vmic_exclusive.words.set(panel.vmic_exclusive_label)
            self.vmic_exclusive.view.setAccessibilityLabel_(panel.vmic_exclusive_label)
            self.vmic_exclusive_note.set(panel.vmic_exclusive_note)
        shown_machines = {tile.machine for tile in page.tiles}
        for machine in [machine for machine in self.row_sets if machine not in shown_machines]:
            _signature, host, _views = self.row_sets.pop(machine)
            self.rows_host.removeArrangedSubview_(host)
            host.removeFromSuperview()
        _signature, current, self.row_views = self._row_set(panel)
        for _signature, host, _views in self.row_sets.values():
            host.setHidden_(host is not current)
        if page.notices != self.notice_signature:
            self.notice_signature = page.notices
            self._show_notices(page.notices)
        self._apply_levels(force=True)

    def _show_notices(self, notices):
        for child in list(self.notices_host.arrangedSubviews()):
            self.notices_host.removeArrangedSubview_(child)
            child.removeFromSuperview()
        self.notice_buttons = []
        self.notices_module.view.setHidden_(not notices and not self.install_note.text)
        for index, notice in enumerate(notices):
            if index:
                widgets.add(self.notices_host, widgets.hairline())
            line = widgets.stack(vertical=False, spacing=16)
            line.setEdgeInsets_(AppKit.NSEdgeInsets(10, 0, 10, 0))
            text = widgets.Label(notice.text, theme.TYPE["note"], 500 if notice.tone == view.FAULT else 400,
                                 "ink" if notice.tone in (view.FAULT, view.WAIT) else "ink_2", wrap=True)
            text.view.widthAnchor().constraintLessThanOrEqualToConstant_(theme.READING_WIDTH).setActive_(True)
            words, dot = _dot_line(text)
            widgets.paint(dot, notice.tone)
            widgets.hug(words, AppKit.NSLayoutPriorityDefaultLow)
            line.addArrangedSubview_(words)
            if notice.action:
                button = widgets.action_button(notice.action_label, lambda notice=notice: self._notice_action(notice),
                                               scale="small", style="primary" if notice.tone == view.WAIT else "plain")
                if notice.action == "install_driver" and self.installing:
                    button.set_title(tr("Installing…"))
                    button.set_enabled(False)
                line.addArrangedSubview_(widgets.hug(button.view))
                self.notice_buttons.append((notice, button))
            widgets.add(self.notices_host, line)

    def _apply_levels(self, force=False):
        self.levels_pending = False
        if not force and not self.visible:
            return
        started = Foundation.CFAbsoluteTimeGetCurrent() if self.timings is not None else 0
        levels = self.levels
        self.mic_meter.set_reading(view.meter_reading(levels, self.mic_meter.key))
        for shown in self.row_views:
            shown.meter.set_reading(view.meter_reading(levels, shown.meter.key))
        self.map.set_levels(levels)
        if self.timings is not None:
            self.timings.append(("levels", Foundation.CFAbsoluteTimeGetCurrent() - started))

    # Service callbacks, on the service's worker thread: hand over and hop.

    def _snapshot_arrived(self):
        if not self.snapshot_pending:
            self.snapshot_pending = True
            self.call_after(self._snapshot_on_main)

    def _snapshot_on_main(self):
        self.snapshot_pending = False
        if self.visible:
            self.refresh()
        else:
            self.dirty = True

    def _levels_arrived(self, levels):
        self.levels = levels
        if not self.levels_pending:
            self.levels_pending = True
            self.call_after(self._apply_levels)

    # Visibility: the service meters only while someone can see the page.

    def _window_changed(self):
        window = self.map.window()
        if window is not self.observed_window:
            centre = Foundation.NSNotificationCenter.defaultCenter()
            for observer in self.observers:
                centre.removeObserver_(observer)
            self.observers = []
            self.observed_window = window
            if window is not None:
                for name in (AppKit.NSWindowDidChangeOcclusionStateNotification, AppKit.NSWindowWillCloseNotification,
                             AppKit.NSWindowDidMiniaturizeNotification, AppKit.NSWindowDidDeminiaturizeNotification):
                    observer = _Observer.alloc().initWithCallback_(self.update_watch)
                    centre.addObserver_selector_name_object_(observer, "changed:", name, window)
                    self.observers.append(observer)
        self.update_watch()

    def update_watch(self):
        service = self._ensure_service()
        visible = self.visible
        if visible and not self.watching and not self.installing:
            # Installed or removed by hand since the page was last open.
            self.driver_loaded = False
            self.dirty = True
        if visible and self.dirty:
            self.refresh()
        if service is None and visible and not self.service_wait:
            # The app may start its audio after the window: look again while the page is open.
            self.service_wait = True
            AppHelper.callLater(2.0, self._service_wait_over)
        if service is None or visible == self.watching:
            return
        self.watching = visible
        service.watch(visible)

    def _service_wait_over(self):
        self.service_wait = False
        if self.service_of() is not None:
            self.dirty = True
        self.update_watch()

    # Actions.

    def _menu(self, is_open):
        self.menus_open = max(0, self.menus_open + (1 if is_open else -1))
        if not self.menus_open and self.dirty:
            # Out of the menu's tracking loop first.
            self.call_after(self.refresh)

    def _select(self, machine):
        self.selected = machine
        self.refresh()

    def _pick_mic(self, choice):
        if self.service is not None:
            view.apply_mic(self.service, choice)

    def _pick_camera(self, choice):
        if self.service is not None:
            from core.camera import view as camera_view
            camera_view.apply_camera(self.service, choice)

    def _pick_quality(self, size):
        if self.service is not None:
            from core.camera import view as camera_view
            camera_view.apply_quality(self.service, size)

    def _camera_action(self, action):
        """A camera row's, notice's or status line's button: the service's, else this app's own."""
        from core.camera import view as camera_view
        if self.service is not None and camera_view.apply_camera_action(self.service, action, self.camera_snap):
            return
        if action == camera_view.ACTION_PRIVACY:
            self.open_url(camera_view.PRIVACY_URL)

    def _pick_output(self, choice):
        if self.service is not None and self.page is not None:
            view.apply_output(self.service, self.page.panel.machine, choice)

    def _headset(self, row, on):
        if self.service is not None:
            view.apply_headset(self.service, row, on)

    def _vmic_exclusive_changed(self, on):
        if self.service is not None and self.page is not None:
            view.apply_vmic_exclusive(self.service, self.page.panel, on)

    def _test(self, row):
        if self.service is not None:
            view.apply_test(self.service, row)

    def _notice_action(self, notice):
        if self.service is not None and view.apply_notice(self.service, notice):
            return
        if notice.action in SETTINGS_URLS:
            self.open_url(SETTINGS_URLS[notice.action])
        elif notice.action == "install_driver":
            self.install_driver()

    # The Beamer Microphone driver (mac_app/audio_driver.py).

    def _load_driver(self):
        self.driver_loaded = True
        try:
            self.driver = self.driver_of()
        except Exception:  # an unreadable bundle must not take the page down
            LOG.exception("audio: driver status")
            self.driver = None

    def _streams_running(self):
        snap = self.service.snapshot() if self.service is not None else None
        return bool(snap and any(stream.state in view.STREAMS_RUNNING for stream in snap.streams))

    def install_driver(self):
        """Installs or updates the driver on a worker thread: it restarts coreaudiod and may wait on
        the administrator prompt. The notice said first that sound stops for about a second."""
        if self.installing:
            return
        self.installing = True
        self.install_note.set("")
        for _notice, button in self.notice_buttons:
            if _notice.action == "install_driver":
                button.set_title(tr("Installing…"))
                button.set_enabled(False)
        running = self._streams_running()
        prompt = tr("Beamer wants to install Beamer Microphone, the microphone your apps hear from your other machines.")

        def work():
            try:
                result = self.installer(streams_running=running, prompt=prompt)
            except Exception as error:  # reported on the page, never raised on a worker thread
                LOG.exception("audio: driver install")
                result = error
            self.call_after(self._installed, result)

        threading.Thread(target=work, name="Beamer-audio-driver", daemon=True).start()

    def _installed(self, result):
        self.installing = False
        outcome = getattr(result, "result", "failed")
        LOG.info("audio: driver install %s %s", outcome, getattr(result, "detail", result))
        self.install_note.set({
            "installed": "",
            "cancelled": "",
            "busy": tr("Sound started streaming before the install could begin. Try again once it stops."),
            "unavailable": tr("This build of Beamer does not carry Beamer Microphone."),
        }.get(outcome, tr("Beamer Microphone could not be installed. The log has the details.")))
        self.install_note.view.setHidden_(not self.install_note.text)
        self._load_driver()
        self.notice_signature = None
        self.page = None
        self.refresh()
