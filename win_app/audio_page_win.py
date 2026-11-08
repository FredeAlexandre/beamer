# SPDX-License-Identifier: GPL-3.0-only
"""The Audio page of the Qt app (dev/audio/PHASE2.md §9.2), for Windows and Linux.

Everything it shows comes from core/audio/view.py's page model, shared with the Mac page; this file
only lays it out and draws it in the Crossing page's style:

- `LevelMeter`: a thin horizontal bar, RMS fill and peak tick, -60..0 dBFS;
- `AudioMap`: the machines on the Crossing layout's tiles, read only, with the mic and sound routes as
  arrows (sound solid, mic dashed, grey while waiting); click or arrow keys select a machine;
- `MenuSelect`: a button that opens a menu of `view.Choice`s, grouped under machine headers;
- `AudioPageWidget`: the Microphone module, the notices right under it (both apps), Machines and
  the selected machine.

Threads (§9.3): the service calls `subscribe`/`subscribe_levels` callbacks on its worker thread. They
only store what arrived and post one queued signal (coalesced); the page reads the snapshot, builds the
model and updates its widgets on the Qt thread. Levels touch the meters only. Nothing here waits on the
service: its setters enqueue.
"""

from __future__ import annotations

import math
import sys
from typing import Callable, Optional

from PySide6.QtCore import QObject, QPointF, QRectF, QSize, Qt, QTimer, QUrl, Signal
from PySide6.QtGui import (QAccessible, QAccessibleAnnouncementEvent, QColor, QDesktopServices, QFontMetrics,
                           QPainter, QPainterPath, QPen, QPolygonF)
from PySide6.QtWidgets import (QFrame, QGridLayout, QHBoxLayout, QLabel, QMenu, QPushButton, QSizePolicy,
                               QVBoxLayout, QWidget)

import theme
import tokens
import widgets
from core.audio import view
from core.i18n import N_, tr

# The map: the Crossing canvas's height and margin; tiles drawn up to 1.15x the layout's units, so
# four lines fit a 100-unit tile; GAP pixels between neighbouring tiles leave room for the arrows.
MAP_HEIGHT = 260
MAP_MARGIN = 12.0
MAP_GAP = 52.0
MAX_SCALE = 1.15
ARROW_SPREAD = 12.0
METER_WIDTH = 120
CONTROL_WIDTH = 116

# A model tone (a palette ink) as a label role of the stylesheet.
NOTE_ROLES = {view.QUIET: "note-quiet", view.PLAIN: "note", view.LIVE: "note-live", view.WAIT: "note-amber",
              view.FAULT: "note-fault"}

LEGEND = (("sound", N_("Sound")), ("mic", N_("Microphone")), ("waiting", N_("Waiting")))
UNAVAILABLE = N_("Audio is not running in this Beamer yet. Microphones and speakers show here once it is.")
MAP_HINT = N_("Click a machine, or use the arrow keys, to see its devices and choose where its sound plays.")
# The app's own notice actions that this platform can carry out.
OWN_ACTIONS = {"win32": {"open_mic_settings": "ms-settings:privacy-microphone"}}


def _colour(name: str, alpha: float = 1.0) -> QColor:
    colour = QColor(theme.colour(name))
    colour.setAlphaF(max(0.0, min(1.0, alpha)))
    return colour


def note_role(tone: str) -> str:
    return NOTE_ROLES.get(tone, "note")


# -- geometry (pure: tuples in, tuples out) ---------------------------------------------------------

def map_geometry(frames: dict, width: float, height: float, gap: float = MAP_GAP, margin: float = MAP_MARGIN,
                 largest: float = MAX_SCALE) -> dict:
    """Machine -> (x, y, w, h) in pixels: the layout's tiles scaled to fit and centred, with `gap`
    pixels added between tiles that sit side by side (or one above the other), so a touching
    pair of screens on the Crossing map stays in place here with room for an arrow between."""
    if not frames:
        return {}
    eps = 0.5
    left = min(frame[0] for frame in frames.values())
    top = min(frame[1] for frame in frames.values())
    right = max(frame[0] + frame[2] for frame in frames.values())
    bottom = max(frame[1] + frame[3] for frame in frames.values())
    columns = {key: sum(1 for other, o in frames.items() if other != key and o[0] + o[2] <= frame[0] + eps)
               for key, frame in frames.items()}
    rows = {key: sum(1 for other, o in frames.items() if other != key and o[1] + o[3] <= frame[1] + eps)
            for key, frame in frames.items()}
    room_w = max(1.0, width - 2 * margin - gap * max(columns.values()))
    room_h = max(1.0, height - 2 * margin - gap * max(rows.values()))
    scale = min(room_w / max(1.0, right - left), room_h / max(1.0, bottom - top), largest)
    placed = {key: ((frame[0] - left) * scale + columns[key] * gap, (frame[1] - top) * scale + rows[key] * gap,
                    frame[2] * scale, frame[3] * scale) for key, frame in frames.items()}
    extent_w = max(x + w for x, _y, w, _h in placed.values())
    extent_h = max(y + h for _x, y, _w, h in placed.values())
    dx, dy = (width - extent_w) / 2, (height - extent_h) / 2
    return {key: (round(x + dx), round(y + dy), round(w), round(h)) for key, (x, y, w, h) in placed.items()}


def _boundary(rect, towards) -> tuple:
    """Where the line from `rect`'s centre towards the point `towards` leaves it."""
    x, y, w, h = rect
    cx, cy = x + w / 2, y + h / 2
    dx, dy = towards[0] - cx, towards[1] - cy
    if dx == 0 and dy == 0:
        return cx, cy
    t = min((w / 2) / abs(dx) if dx else math.inf, (h / 2) / abs(dy) if dy else math.inf)
    return cx + dx * t, cy + dy * t


def arrow_line(source, target, offset: float = 0.0, inset: float = 6.0) -> tuple:
    """((x1, y1), (x2, y2)) from tile `source` to tile `target` (pixel rects): straight across the
    gap between facing sides when the tiles share a row or a column, else from border to border
    along the line between their centres. `offset` moves it sideways, for a pair with two arrows."""
    sx, sy, sw, sh = source
    tx, ty, tw, th = target
    over_y = (max(sy, ty), min(sy + sh, ty + th))
    over_x = (max(sx, tx), min(sx + sw, tx + tw))
    if (tx >= sx + sw or sx >= tx + tw) and over_y[1] - over_y[0] > 2 * abs(offset):
        y = (over_y[0] + over_y[1]) / 2 + offset
        if tx >= sx + sw:
            return (sx + sw + inset, y), (tx - inset, y)
        return (sx - inset, y), (tx + tw + inset, y)
    if (ty >= sy + sh or sy >= ty + th) and over_x[1] - over_x[0] > 2 * abs(offset):
        x = (over_x[0] + over_x[1]) / 2 + offset
        if ty >= sy + sh:
            return (x, sy + sh + inset), (x, ty - inset)
        return (x, sy - inset), (x, ty + th + inset)
    start = _boundary(source, (tx + tw / 2, ty + th / 2))
    end = _boundary(target, (sx + sw / 2, sy + sh / 2))
    length = math.hypot(end[0] - start[0], end[1] - start[1]) or 1.0
    ux, uy = (end[0] - start[0]) / length, (end[1] - start[1]) / length
    # The normal of the pair, not of this arrow: the same offset lands on the same side both ways.
    flip = -1.0 if ux < 0 or (ux == 0 and uy < 0) else 1.0
    nx, ny = -uy * offset * flip, ux * offset * flip
    return ((start[0] + ux * inset + nx, start[1] + uy * inset + ny),
            (end[0] - ux * inset + nx, end[1] - uy * inset + ny))


def arrow_offsets(arrows) -> list:
    """The sideways offset of each arrow: several between one pair of machines sit side by side,
    whichever way each points (arrow_line takes offsets on a fixed axis, not the arrow's own)."""
    pairs = {}
    for index, arrow in enumerate(arrows):
        pairs.setdefault(frozenset((arrow.source, arrow.target)), []).append(index)
    offsets = [0.0] * len(arrows)
    for indexes in pairs.values():
        for rank, index in enumerate(indexes):
            offsets[index] = (rank - (len(indexes) - 1) / 2) * ARROW_SPREAD
    return offsets


def nearest(rects: dict, current: str, direction: tuple) -> Optional[str]:
    """The tile beyond `current` in `direction` ((-1, 0) is left), the closest by centre, where a
    tile straight along that line wins over one off to the side."""
    if current not in rects:
        return None
    x, y, w, h = rects[current]
    cx, cy = x + w / 2, y + h / 2
    best, best_score = None, math.inf
    for key, (ox, oy, ow, oh) in rects.items():
        if key == current:
            continue
        dx, dy = ox + ow / 2 - cx, oy + oh / 2 - cy
        along = dx * direction[0] + dy * direction[1]
        if along <= 1:
            continue
        across = abs(dx * direction[1] - dy * direction[0])
        score = along + 2 * across
        if score < best_score:
            best, best_score = key, score
    return best


# -- painting helpers -------------------------------------------------------------------------------

def paint_mic_glyph(painter: QPainter, x: float, y: float, colour: QColor, size: float = 12.0) -> None:
    """A microphone, `size` tall, top left at (x, y)."""
    unit = size / 12.0
    pen = QPen(colour, 1.3)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    painter.save()
    painter.setPen(pen)
    painter.setBrush(colour)
    painter.drawRoundedRect(QRectF(x + 3.5 * unit, y, 4 * unit, 7 * unit), 2 * unit, 2 * unit)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    cup = QPainterPath(QPointF(x + 1.5 * unit, y + 5 * unit))
    cup.cubicTo(QPointF(x + 1.5 * unit, y + 10 * unit), QPointF(x + 9.5 * unit, y + 10 * unit),
                QPointF(x + 9.5 * unit, y + 5 * unit))
    painter.drawPath(cup)
    painter.drawLine(QPointF(x + 5.5 * unit, y + 9.3 * unit), QPointF(x + 5.5 * unit, y + 12 * unit))
    painter.restore()


def paint_arrow(painter: QPainter, start, end, colour: QColor, dashed: bool) -> None:
    pen = QPen(colour, 2.0)
    pen.setCapStyle(Qt.PenCapStyle.FlatCap)
    if dashed:
        pen.setDashPattern([2.2, 1.8])
    head = 8.0
    length = math.hypot(end[0] - start[0], end[1] - start[1])
    if length < 4:
        return
    ux, uy = (end[0] - start[0]) / length, (end[1] - start[1]) / length
    shaft_end = QPointF(end[0] - ux * head * 0.8, end[1] - uy * head * 0.8)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawLine(QPointF(*start), shaft_end)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(colour)
    tip = QPointF(*end)
    painter.drawPolygon(QPolygonF([
        tip,
        QPointF(end[0] - ux * head - uy * head * 0.5, end[1] - uy * head + ux * head * 0.5),
        QPointF(end[0] - ux * head + uy * head * 0.5, end[1] - uy * head - ux * head * 0.5),
    ]))


def paint_meter(painter: QPainter, rect: QRectF, reading: Optional[tuple]) -> None:
    """The level bar's look, shared by LevelMeter and the map's tiles: a `well` track, the RMS in
    `signal`, the peak as a tick (`amber` within 3 dB of full scale)."""
    radius = min(rect.height() / 2, 2.0)
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(_colour("rule"))
    painter.drawRoundedRect(rect, radius, radius)
    if reading is None:
        return
    rms, peak = reading
    if rms > 0:
        fill = QRectF(rect.left(), rect.top(), max(rect.height(), rect.width() * rms), rect.height())
        painter.setBrush(_colour("signal"))
        painter.drawRoundedRect(fill, radius, radius)
    if peak > 0:
        x = rect.left() + max(1.0, min(rect.width() - 1.0, rect.width() * peak))
        painter.setBrush(_colour("amber" if peak >= 0.95 else "ink_2"))
        painter.drawRect(QRectF(x - 1.0, rect.top() - 1.0, 2.0, rect.height() + 2.0))


class LevelMeter(QWidget):
    """A horizontal level bar (§9.2): RMS fill and peak tick on -60..0 dBFS. Shows an empty track
    while the levels map has nothing for its key."""

    def __init__(self, key: Optional[str] = None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.key = key
        self.reading = None
        self.setFixedHeight(10)
        self.setMinimumWidth(48)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self.setAccessibleName(tr("Level"))
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)

    def set_reading(self, reading: Optional[tuple]) -> None:
        if reading is not None:
            reading = (round(reading[0], 3), round(reading[1], 3))
        if reading != self.reading:
            self.reading = reading
            self.update()

    def sizeHint(self) -> QSize:
        return QSize(METER_WIDTH, 10)

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        paint_meter(painter, QRectF(0, 3, self.width(), 4), self.reading)


def _spoken(tile: view.TileView) -> str:
    # The model's description already ends with the tile's recording_line while an app records.
    return tile.description


def paint_speaker_glyph(painter: QPainter, x: float, y: float, colour: QColor, size: float = 12.0) -> None:
    """A loudspeaker with one sound wave, `size` tall, top left at (x, y)."""
    unit = size / 12.0
    painter.save()
    painter.setPen(Qt.PenStyle.NoPen)
    painter.setBrush(colour)
    painter.drawPolygon(QPolygonF([QPointF(x, y + 4 * unit), QPointF(x + 3 * unit, y + 4 * unit),
                                   QPointF(x + 7 * unit, y + 0.5 * unit), QPointF(x + 7 * unit, y + 11.5 * unit),
                                   QPointF(x + 3 * unit, y + 8 * unit), QPointF(x, y + 8 * unit)]))
    pen = QPen(colour, 1.3)
    pen.setCapStyle(Qt.PenCapStyle.RoundCap)
    painter.setPen(pen)
    painter.setBrush(Qt.BrushStyle.NoBrush)
    painter.drawArc(QRectF(x + 5 * unit, y + 3 * unit, 6 * unit, 6 * unit), -60 * 16, 120 * 16)
    painter.restore()


def tile_mic_text(tile: view.TileView) -> tuple:
    """(text, ink, important) of a tile's microphone line, as on the Mac page: the shared mic on its
    machine, else who hears it, else the machine's own default mic (quiet)."""
    if tile.mic_line:
        return tile.mic_line, "ink", True
    if tile.listen_line:
        return tile.listen_line, view.LIVE, True
    return tile.input_line, view.QUIET, False


def linked_without_audio(app) -> tuple:
    """The names of the machines linked to `app` (kvm_bridge_win's window) whose Beamer announces no
    `audio`, from the capabilities of their links as audio_win.PcAudio holds them. Empty when this
    Beamer runs without audio itself (it then does not track the links' capabilities)."""
    audio = getattr(app, "_audio", None)
    peers = getattr(audio, "peers", None)
    if peers is None:
        return ()
    names = []
    for peer, caps in sorted(peers.peers().items()):
        if caps is None or "audio" in caps:
            continue
        name = audio.host.peer_name(peer)
        names.append(name if name and name != peer else "")
    return tuple(names)


class AudioMap(QWidget):
    """The machines, drawn as on the Crossing map: one tile per machine on the shared layout (own
    tile on `well`, the others on `panel`), name, platform line with the link dot, the shared mic,
    where the sound plays, and a thin level bar; arrows for the routes. Read only: a click or the
    arrow keys select a machine (`on_select`), Home and End the first and last."""

    def __init__(self, on_select: Callable[[str], None] | None = None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.on_select = on_select or (lambda _machine: None)
        self.tiles = ()
        self.arrows = ()
        self.selected = None
        self.mic_tone = view.PLAIN
        self.levels = {}
        self._hover = None
        self._rects = {}
        self._keyboard_focus = False
        self.setFixedHeight(MAP_HEIGHT)
        self.setFocusPolicy(Qt.FocusPolicy.StrongFocus)
        self.setMouseTracking(True)
        self.setAccessibleName(tr("Machines"))
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)

    def set_page(self, page: view.AudioPage) -> None:
        seen = (page.tiles, page.arrows, page.selected, page.mic_tone)
        if seen == (self.tiles, self.arrows, self.selected, self.mic_tone):
            return
        self.tiles, self.arrows, self.selected, self.mic_tone = seen
        self._layout()
        self.setAccessibleDescription(page.map_description)
        self.update()

    def set_levels(self, levels) -> None:
        keys = {tile.meter for tile in self.tiles if tile.meter}
        readings = {key: view.meter_reading(levels, key) for key in keys}
        if readings != {key: view.meter_reading(self.levels, key) for key in keys}:
            self.levels = dict(levels)
            self.update()
        else:
            self.levels = dict(levels)

    def tile_rect(self, machine: str) -> Optional[QRectF]:
        rect = self._rects.get(machine)
        return QRectF(*rect) if rect is not None else None

    def _layout(self) -> None:
        self._rects = map_geometry({tile.machine: tile.frame for tile in self.tiles}, self.width(), self.height())

    def resizeEvent(self, event) -> None:
        self._layout()
        super().resizeEvent(event)

    def _tile_at(self, point) -> Optional[str]:
        for tile in reversed(self.tiles):
            rect = self.tile_rect(tile.machine)
            if rect is not None and rect.contains(point):
                return tile.machine
        return None

    def _choose(self, machine: Optional[str], announce: bool = False) -> None:
        if machine is None or machine == self.selected:
            return
        self.on_select(machine)
        if announce:
            tile = next((tile for tile in self.tiles if tile.machine == machine), None)
            if tile is not None:
                QAccessible.updateAccessibility(QAccessibleAnnouncementEvent(self, _spoken(tile)))

    def mousePressEvent(self, event) -> None:
        if event.button() == Qt.MouseButton.LeftButton:
            self.setFocus(Qt.FocusReason.MouseFocusReason)
            self._keyboard_focus = False
            self._choose(self._tile_at(event.position()))
            event.accept()
            return
        super().mousePressEvent(event)

    def mouseMoveEvent(self, event) -> None:
        hover = self._tile_at(event.position())
        if hover != self._hover:
            self._hover = hover
            self.setCursor(Qt.CursorShape.PointingHandCursor if hover else Qt.CursorShape.ArrowCursor)
            self.update()
        text = self.tooltip_at(event.position())
        if text != self.toolTip():
            self.setToolTip(text)
        super().mouseMoveEvent(event)

    def tooltip_at(self, point) -> str:
        """Over a tile's recording mark, its recording_line; elsewhere on a tile, its description."""
        tile = next((tile for tile in self.tiles if tile.machine == self._tile_at(point)), None)
        if tile is None:
            return ""
        rect = self.tile_rect(tile.machine)
        if tile.recording_line and QRectF(rect.right() - 26, rect.top(), 26, 26).contains(point):
            return tile.recording_line
        return _spoken(tile)

    def leaveEvent(self, event) -> None:
        if self._hover is not None:
            self._hover = None
            self.update()
        super().leaveEvent(event)

    def keyPressEvent(self, event) -> None:
        self._keyboard_focus = True
        moves = {Qt.Key.Key_Left: (-1, 0), Qt.Key.Key_Right: (1, 0), Qt.Key.Key_Up: (0, -1), Qt.Key.Key_Down: (0, 1)}
        order = [tile.machine for tile in self.tiles]
        if event.key() in moves and not event.modifiers() & ~Qt.KeyboardModifier.KeypadModifier:
            target = nearest(self._rects, self.selected, moves[event.key()])
            if target is None and order and self.selected in order:
                # Nothing that way: step through the reading order instead, so every tile is reached.
                step = -1 if event.key() in (Qt.Key.Key_Left, Qt.Key.Key_Up) else 1
                index = order.index(self.selected) + step
                target = order[index] if 0 <= index < len(order) else None
            self._choose(target, announce=True)
            self.update()
            event.accept()
            return
        if event.key() in (Qt.Key.Key_Home, Qt.Key.Key_End) and order:
            self._choose(order[0] if event.key() == Qt.Key.Key_Home else order[-1], announce=True)
            self.update()
            event.accept()
            return
        super().keyPressEvent(event)

    def focusInEvent(self, event) -> None:
        self._keyboard_focus = event.reason() != Qt.FocusReason.MouseFocusReason
        self.update()
        super().focusInEvent(event)

    def focusOutEvent(self, event) -> None:
        self.update()
        super().focusOutEvent(event)

    def paintEvent(self, _event) -> None:
        if not self._rects and self.tiles:
            self._layout()
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        painter.setPen(QPen(_colour("edge"), 1.0))
        painter.setBrush(_colour("ground"))
        painter.drawRoundedRect(QRectF(0.5, 0.5, self.width() - 1, self.height() - 1), 4, 4)
        for tile in self.tiles:
            rect = self.tile_rect(tile.machine)
            if rect is not None:
                self._paint_tile(painter, tile, rect)
        offsets = arrow_offsets(self.arrows)
        for arrow, offset in zip(self.arrows, offsets):
            source, target = self._rects.get(arrow.source), self._rects.get(arrow.target)
            if source is None or target is None:
                continue
            start, end = arrow_line(source, target, offset)
            colour = _colour("signal") if arrow.state == "active" else _colour("ink_3")
            paint_arrow(painter, start, end, colour, dashed=arrow.kind == "mic")
        if self.hasFocus() and self._keyboard_focus and self.selected in self._rects:
            painter.setPen(QPen(_colour("ink_2"), 2.0))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(self.tile_rect(self.selected).adjusted(-3, -3, 3, 3), 3, 3)
        painter.end()

    def _paint_tile(self, painter: QPainter, tile: view.TileView, rect: QRectF) -> None:
        radius = tokens.RADIUS["button"]
        painter.setOpacity(1.0 if tile.online else 0.55)
        line = "ink_2" if tile.selected else "edge"
        painter.setPen(QPen(_colour(line), 2.0 if tile.selected else 1.0))
        painter.setBrush(_colour("well" if tile.own else "panel"))
        painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius)
        if self._hover == tile.machine and not tile.selected:
            painter.setPen(QPen(_colour("ink_3"), 1.0))
            painter.setBrush(Qt.BrushStyle.NoBrush)
            painter.drawRoundedRect(rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius)
        tall = rect.height()
        # Name, centred as on the Crossing map.
        painter.setPen(_colour("ink"))
        painter.setFont(theme.font(10 if tall < 32 else tokens.TYPE["body"], 650))
        title = QRectF(rect.left() + 8, rect.top() + 7, max(1.0, rect.width() - 16), 22)
        if tile.recording:
            title.adjust(10, 0, -10, 0)
        painter.drawText(title, Qt.AlignmentFlag.AlignCenter,
                         painter.fontMetrics().elidedText(tile.title, Qt.TextElideMode.ElideRight,
                                                          max(1, round(title.width()))))
        if tile.recording:
            # An app records this machine's virtual mic: an on-air dot in the corner.
            centre = QPointF(rect.right() - 13, rect.top() + 13)
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(_colour("signal", 0.25))
            painter.drawEllipse(centre, 6, 6)
            painter.setBrush(_colour("signal"))
            painter.drawEllipse(centre, 3, 3)
        # The microphone and the output in two lines over the level bar, as on the Mac page; short of
        # room the shared mic (or who hears it) beats the platform line, which beats the own default mic.
        mic_text, mic_ink, important = tile_mic_text(tile)
        out_y = rect.bottom() - 33
        mic_y = out_y - 19
        show_mic = bool(mic_text) and (mic_y >= rect.top() + 48 or (important and mic_y >= rect.top() + 30))
        if tall >= 56 and not (show_mic and mic_y < rect.top() + 48):
            painter.setPen(Qt.PenStyle.NoPen)
            painter.setBrush(_colour("signal" if tile.online else "ink_3"))
            painter.drawEllipse(QPointF(rect.left() + 15, rect.top() + 39), 3, 3)
            painter.setPen(_colour("ink_2"))
            painter.setFont(theme.font(tokens.TYPE["note"]))
            detail = QRectF(rect.left() + 24, rect.top() + 31, max(1.0, rect.width() - 32), 16)
            painter.drawText(detail, Qt.AlignmentFlag.AlignBottom | Qt.AlignmentFlag.AlignLeft,
                             painter.fontMetrics().elidedText(tile.subtitle, Qt.TextElideMode.ElideRight,
                                                              max(1, round(detail.width()))))
        room = max(1, round(rect.width() - 28 - 10))
        if show_mic:
            glyph = view.LIVE if tile.has_active_mic or tile.listen_line else view.QUIET
            if not tile.online:
                glyph = mic_ink = view.QUIET
            paint_mic_glyph(painter, rect.left() + 10, mic_y + 2, _colour(glyph), 11.0)
            painter.setPen(_colour(mic_ink))
            painter.setFont(theme.font(tokens.TYPE["note"], 600 if tile.has_active_mic else 400))
            painter.drawText(QRectF(rect.left() + 28, mic_y, room, 16), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                             painter.fontMetrics().elidedText(mic_text, Qt.TextElideMode.ElideRight, room))
        if tall >= 64:
            tone = tile.output_tone
            paint_speaker_glyph(painter, rect.left() + 9, out_y + 2, _colour(tone if tone != view.PLAIN else view.QUIET), 12.0)
            painter.setPen(_colour(tone))
            painter.setFont(theme.font(tokens.TYPE["note"]))
            painter.drawText(QRectF(rect.left() + 28, out_y, room, 16), Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignLeft,
                             painter.fontMetrics().elidedText(tile.output_line, Qt.TextElideMode.ElideRight, room))
        if tile.meter and tall >= 56:
            paint_meter(painter, QRectF(rect.left() + 12, rect.bottom() - 10, rect.width() - 24, 3),
                        view.meter_reading(self.levels, tile.meter))
        painter.setOpacity(1.0)


class MenuSelect(widgets.ElidingButton):
    """A selector over `view.Choice`s: shows the chosen one's `title`, opens a menu with the choices
    grouped under their machine headers, and calls `on_choose(choice)` for an enabled pick. The menu
    is filled when it opens, so a snapshot arriving meanwhile never rebuilds it under the pointer."""

    CHEVRON = 10

    def __init__(self, on_choose: Callable[[view.Choice], None], parent: QWidget | None = None) -> None:
        super().__init__("", parent)
        self.on_choose = on_choose
        self.choices = ()
        self.selected = None
        self.setStyleSheet("QPushButton { text-align: left; padding-right: 30px; }")
        self.setMinimumWidth(320)
        self.setMaximumWidth(440)
        self.setSizePolicy(QSizePolicy.Policy.Expanding, QSizePolicy.Policy.Fixed)
        self._menu = QMenu(self)
        self._menu.aboutToShow.connect(self._fill)
        self.setMenu(self._menu)

    def set_choices(self, choices, selected: str, enabled: bool = True) -> None:
        self.choices, self.selected = tuple(choices), selected
        chosen = view.choice_by_key(self.choices, selected)
        text = chosen.title if chosen is not None else ""
        if text != self.text():
            self.setText(text)
        self.setEnabled(enabled and any(choice.enabled and not choice.header for choice in self.choices))

    def menu_open(self) -> bool:
        return self._menu.isVisible()

    def choose(self, key: str) -> bool:
        """Picks the choice `key` as a click in the menu would; False when it is not pickable."""
        choice = view.choice_by_key(self.choices, key)
        if choice is None or not choice.enabled:
            return False
        if key != self.selected:
            self.on_choose(choice)
        return True

    def _fill(self) -> None:
        self._menu.clear()
        grouped = False
        for choice in self.choices:
            if choice.header:
                if self._menu.actions():
                    self._menu.addSeparator()
                action = self._menu.addAction(choice.label)
                action.setEnabled(False)
                font = action.font()
                font.setWeight(font.Weight.DemiBold)
                action.setFont(font)
                grouped = True
                continue
            action = self._menu.addAction(("   " if grouped else "") + choice.label)
            action.setCheckable(True)
            action.setChecked(choice.key == self.selected)
            action.setEnabled(choice.enabled)
            action.triggered.connect(lambda _checked=False, key=choice.key: self.choose(key))

    def paintEvent(self, event) -> None:
        super().paintEvent(event)
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        pen = QPen(_colour("ink_2" if self.isEnabled() else "ink_3"), 1.5)
        pen.setCapStyle(Qt.PenCapStyle.RoundCap)
        pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
        painter.setPen(pen)
        x, y = self.width() - 18, self.height() / 2 - 2
        chevron = QPainterPath(QPointF(x - 4, y))
        chevron.lineTo(QPointF(x, y + 4))
        chevron.lineTo(QPointF(x + 4, y))
        painter.drawPath(chevron)


class DeviceRow(QWidget):
    """One input or output of the selected machine: name, detail and state, its level, and Test
    (inputs) or the Headset switch (outputs)."""

    def __init__(self, row: view.DeviceRow, on_test, on_headset, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setProperty("vernier", "plain")
        self.row = row
        grid = QGridLayout(self)
        grid.setContentsMargins(0, 8, 0, 8)
        grid.setHorizontalSpacing(16)
        grid.setVerticalSpacing(2)
        self.name = widgets.ElidingLabel(row.name)
        self.name.setProperty("vernier", "tile-name")
        self.name.setMinimumWidth(40)
        self.name.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        self.detail = widgets.ElidingLabel("")
        self.detail.setProperty("vernier", "note")
        self.detail.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        grid.addWidget(self.name, 0, 0)
        grid.addWidget(self.detail, 1, 0)
        self.meter = LevelMeter(row.meter)
        self.meter.setFixedWidth(METER_WIDTH)
        grid.addWidget(self.meter, 0, 1, 2, 1, Qt.AlignmentFlag.AlignVCenter)
        if row.kind == "input":
            self.control = QPushButton(tr("Test"))
            self.control.setProperty("vernier", "small")
            self.control.clicked.connect(lambda: on_test(self.row))
        else:
            self.control = widgets.Switch(tr("Headset"))
            self.control.setSizePolicy(QSizePolicy.Policy.Fixed, QSizePolicy.Policy.Preferred)
            self.control.toggled.connect(lambda on: on_headset(self.row, on))
        grid.addWidget(self.control, 0, 2, 2, 1, Qt.AlignmentFlag.AlignVCenter | Qt.AlignmentFlag.AlignRight)
        grid.setColumnStretch(0, 1)
        # One control column for Test and Headset, so every row's meter lines up.
        grid.setColumnMinimumWidth(2, CONTROL_WIDTH)
        self.update_row(row)

    def update_row(self, row: view.DeviceRow) -> None:
        self.row = row
        if self.name.text() != row.name:
            self.name.setText(row.name)
        detail = " · ".join(part for part in (row.detail, row.active_label) if part)
        if self.detail.text() != detail:
            self.detail.setText(detail)
        widgets.set_role(self.detail, "note-live" if row.active else "note")
        self.meter.key = row.meter
        if row.kind == "input":
            self.control.setEnabled(row.test_enabled)
            self.control.setAccessibleName(tr("Test {device}").format(device=row.name))
        else:
            self.control.blockSignals(True)
            self.control.setChecked(row.headset)
            self.control.blockSignals(False)
            self.control.setEnabled(row.headset_enabled)
            self.control.setAccessibleName(tr("{device} is a headset").format(device=row.name))


class NoticeView(QFrame):
    """One notice: a tone dot, its sentence and its action, if this app can carry it out."""

    def __init__(self, notice: view.Notice, on_action, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.notice = notice
        self.setProperty("vernier", "banner")
        line = QHBoxLayout(self)
        line.setContentsMargins(12, 10, 12, 10)
        line.setSpacing(10)
        dot_column = QVBoxLayout()
        dot_column.setContentsMargins(0, 5, 0, 0)
        dot_column.addWidget(widgets.Led(notice.tone))
        dot_column.addStretch(1)
        line.addLayout(dot_column)
        self.text = widgets.label(notice.text, note_role(notice.tone), wrap=True)
        self.text.setSizePolicy(QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Preferred)
        line.addWidget(self.text, 1)
        self.action = None
        if notice.action and notice.action_label:
            self.action = QPushButton(notice.action_label)
            self.action.setProperty("vernier", "small")
            self.action.clicked.connect(lambda: on_action(self.notice))
            line.addWidget(self.action, 0, Qt.AlignmentFlag.AlignVCenter)


class _Relay(QObject):
    """Signals posted from the service's worker thread, delivered on the Qt thread."""
    snapshot = Signal()
    levels = Signal()


def _clear(layout) -> None:
    while layout.count():
        item = layout.takeAt(0)
        widget = item.widget()
        if widget is not None:
            widget.hide()
            widget.deleteLater()


class AudioPageWidget(QWidget):
    """The Audio page's modules. `service` is the AudioService (§8.1), or a callable returning it or
    None while the app has none; `tiles()` the Crossing layout as view.page_model takes it;
    `peers_without_audio()` the names of linked machines whose Beamer has no audio; `driver()` the
    Mac driver's state for view.page_model (None here: the PC's virtual mic is VB-CABLE, which the
    snapshot reports). Without a service or `peers_without_audio`, the page reads its window's
    (kvm_bridge_win's `audio` and, through `_audio`, the links' capabilities)."""

    def __init__(self, service=None, *, own_label: str, tiles: Callable[[], dict] = dict,
                 peers_without_audio: Optional[Callable[[], tuple]] = None,
                 driver: Callable[[], Optional[str]] = lambda: None, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.setProperty("vernier", "plain")
        self.own_label = own_label
        self._tiles = tiles
        self._peers_without_audio = peers_without_audio or (lambda: linked_without_audio(self.window()))
        self._driver = driver
        self._lookup = service if callable(service) and not hasattr(service, "snapshot") else None
        self.service = None
        self.page = None
        self.selected = None
        self.levels = {}
        self._unsubscribe = []
        self._snapshot_pending = False
        self._levels_pending = False
        self._latest_levels = {}
        self._dirty = False
        self._watching = False
        self._row_keys = None
        self._notice_keys = None
        self.rows = []
        self.notices = []
        self._relay = _Relay(self)
        self._relay.snapshot.connect(self._snapshot_arrived, Qt.ConnectionType.QueuedConnection)
        self._relay.levels.connect(self._levels_arrived, Qt.ConnectionType.QueuedConnection)
        self._build()
        # Looks for the service while the page shows and the app has none yet.
        self._find_timer = QTimer(self)
        self._find_timer.setInterval(1000)
        self._find_timer.timeout.connect(self._find_service)
        if self._lookup is None and service is not None:
            self.set_service(service)
        else:
            self._show_unavailable()
        self.destroyed.connect(lambda *_: self._drop_subscriptions())

    # -- building -----------------------------------------------------------------------------------

    def _build(self) -> None:
        outer = QVBoxLayout(self)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(20)

        self.unavailable = widgets.Module(tr("Audio"))
        self.unavailable.body.addWidget(widgets.label(tr(UNAVAILABLE), "note", wrap=True))
        outer.addWidget(self.unavailable)

        self.mic_module = widgets.Module(tr("Microphone"))
        control = QWidget()
        control.setProperty("vernier", "plain")
        column = QVBoxLayout(control)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(8)
        self.mic_select = MenuSelect(self._mic_chosen)
        column.addWidget(self.mic_select)
        self.mic_meter = LevelMeter()
        self.mic_meter.setMaximumWidth(440)
        column.addWidget(self.mic_meter)
        self.mic_label = widgets.label("", "key")
        self.mic_line = widgets.label("", "note", wrap=True)
        self.mic_hint = widgets.label("", "note-quiet", wrap=True)
        self.mic_row = widgets.FormRow(self.mic_label, control, (self.mic_line, self.mic_hint))
        self.mic_module.body.addWidget(self.mic_row)
        outer.addWidget(self.mic_module)

        # The notices right under the Microphone module, so a missing driver or an echo is seen
        # without scrolling (the same place on the Mac page).
        self.notices_box = QVBoxLayout()
        self.notices_box.setSpacing(8)
        outer.addLayout(self.notices_box)

        self.map_module = widgets.Module(tr("Machines"))
        self.map = AudioMap(self._machine_chosen)
        self.map_module.body.addWidget(self.map)
        legend = QWidget()
        legend.setProperty("vernier", "plain")
        line = QHBoxLayout(legend)
        line.setContentsMargins(0, 0, 0, 0)
        line.setSpacing(6)
        for kind, text in LEGEND:
            line.addWidget(_LegendMark(kind))
            line.addWidget(widgets.label(tr(text), "note"))
            line.addSpacing(10)
        line.addStretch(1)
        self.map_module.body.addWidget(legend)
        self.map_module.body.addWidget(widgets.label(tr(MAP_HINT), "note-quiet", wrap=True))
        outer.addWidget(self.map_module)

        self.panel_module = widgets.Module("")
        self.output_label = widgets.label("", "key")
        output = QWidget()
        output.setProperty("vernier", "plain")
        column = QVBoxLayout(output)
        column.setContentsMargins(0, 0, 0, 0)
        column.setSpacing(8)
        self.output_select = MenuSelect(self._output_chosen)
        column.addWidget(self.output_select)
        self.mute_note = widgets.label("", "note-quiet", wrap=True)
        self.output_line = widgets.label("", "note", wrap=True)
        self.output_row = widgets.FormRow(self.output_label, output, (self.output_line, self.mute_note))
        self.panel_module.body.addWidget(self.output_row)
        self.inputs_heading = widgets.Eyebrow(tr("Microphones"))
        self.panel_module.body.addSpacing(4)
        self.panel_module.body.addWidget(self.inputs_heading)
        self.inputs_empty = widgets.label("", "note-quiet", wrap=True)
        self.panel_module.body.addWidget(self.inputs_empty)
        self.inputs_box = QVBoxLayout()
        self.inputs_box.setSpacing(0)
        self.panel_module.body.addLayout(self.inputs_box)
        self.outputs_heading = widgets.Eyebrow(tr("Outputs"))
        self.panel_module.body.addSpacing(4)
        self.panel_module.body.addWidget(self.outputs_heading)
        self.outputs_empty = widgets.label("", "note-quiet", wrap=True)
        self.panel_module.body.addWidget(self.outputs_empty)
        self.outputs_box = QVBoxLayout()
        self.outputs_box.setSpacing(0)
        self.panel_module.body.addLayout(self.outputs_box)
        outer.addWidget(self.panel_module)


    # -- the service --------------------------------------------------------------------------------

    def set_service(self, service) -> None:
        if service is self.service:
            return
        if self._watching and self.service is not None:
            self.service.watch(False)
        self._watching = False
        self._drop_subscriptions()
        self.service = service
        if service is None:
            self._show_unavailable()
            return
        self._find_timer.stop()
        self._unsubscribe = [service.subscribe(self._on_snapshot), service.subscribe_levels(self._on_levels)]
        self.refresh()
        self._sync_watch()

    def _find_service(self) -> None:
        if self.service is None and self._lookup is not None:
            found = self._lookup()
            if found is None and self.window() is not self:
                # The app's own service, as kvm_bridge_win holds it.
                found = getattr(self.window(), "audio", None)
            if found is not None:
                self.set_service(found)

    def _drop_subscriptions(self) -> None:
        for undo in self._unsubscribe:
            try:
                undo()
            except Exception:  # noqa: BLE001 - a service already stopped
                pass
        self._unsubscribe = []

    # Worker thread: store, post one signal, return.
    def _on_snapshot(self) -> None:
        if not self._snapshot_pending:
            self._snapshot_pending = True
            try:
                self._relay.snapshot.emit()
            except RuntimeError:  # the page is gone
                pass

    def _on_levels(self, levels) -> None:
        self._latest_levels = levels
        if not self._levels_pending:
            self._levels_pending = True
            try:
                self._relay.levels.emit()
            except RuntimeError:
                pass

    # Qt thread.
    def _snapshot_arrived(self) -> None:
        self._snapshot_pending = False
        self.refresh()

    def _levels_arrived(self) -> None:
        self._levels_pending = False
        self.apply_levels(self._latest_levels)

    def showEvent(self, event) -> None:
        super().showEvent(event)
        if self.service is None and self._lookup is not None:
            self._find_service()
            if self.service is None:
                self._find_timer.start()
        elif self.service is not None:
            self.refresh()
        self._sync_watch()

    def hideEvent(self, event) -> None:
        super().hideEvent(event)
        self._find_timer.stop()
        self._sync_watch()

    def _sync_watch(self) -> None:
        visible = self.isVisible()
        if self.service is not None and visible != self._watching:
            self._watching = visible
            self.service.watch(visible)

    # -- showing the model --------------------------------------------------------------------------

    def _show_unavailable(self) -> None:
        self.unavailable.show()
        for module in (self.mic_module, self.map_module, self.panel_module):
            module.hide()
        _clear(self.notices_box)
        self.notices, self._notice_keys = [], None

    def refresh(self) -> None:
        """Rebuilds the model from the service's snapshot and updates the widgets in place. While a
        selector's menu is open it waits for the menu to close."""
        if self.service is None:
            return
        if self.mic_select.menu_open() or self.output_select.menu_open():
            if not self._dirty:
                self._dirty = True
                for select in (self.mic_select, self.output_select):
                    select.menu().aboutToHide.connect(self._menu_closed, Qt.ConnectionType.QueuedConnection)
            return
        try:
            tiles = self._tiles() or {}
        except Exception:  # noqa: BLE001 - a layout being rewritten; the next refresh has it
            tiles = {}
        try:
            without_audio = tuple(self._peers_without_audio() or ())
        except Exception:  # noqa: BLE001 - links changing under us; the next refresh has them
            without_audio = ()
        page = view.page_model(self.service.snapshot(), tiles, self.selected, self.own_label,
                               without_audio, self._driver())
        self.show_page(page)

    def _menu_closed(self) -> None:
        for select in (self.mic_select, self.output_select):
            try:
                select.menu().aboutToHide.disconnect(self._menu_closed)
            except (RuntimeError, TypeError):
                pass
        if self._dirty:
            self._dirty = False
            self.refresh()

    def show_page(self, page: view.AudioPage) -> None:
        self.page = page
        self.selected = page.selected
        self.unavailable.hide()
        for module in (self.mic_module, self.map_module, self.panel_module):
            module.show()
        self._text(self.mic_label, page.mic_label)
        self.mic_select.set_choices(page.mic_choices, page.mic_selected)
        self.mic_select.setAccessibleName(page.mic_label)
        self.mic_meter.key = page.mic_meter
        self.mic_meter.setVisible(page.mic_meter is not None)
        self._text(self.mic_line, page.mic_line)
        widgets.set_role(self.mic_line, note_role(page.mic_tone))
        self._text(self.mic_hint, page.mic_hint)
        self.map.set_page(page)
        panel = page.panel
        self.panel_module.eyebrow.setText(panel.heading)
        self._text(self.output_label, panel.output_label)
        self.output_select.set_choices(panel.output_choices, panel.output_selected, panel.output_enabled)
        self.output_select.setAccessibleName(panel.output_label)
        self._text(self.output_line, panel.output_line)
        self.output_line.setVisible(bool(panel.output_line))
        widgets.set_role(self.output_line, note_role(panel.output_tone))
        self._text(self.mute_note, panel.mute_note)
        self.mute_note.setVisible(bool(panel.mute_note))
        self._show_rows(panel)
        self._show_notices(page.notices)
        self.apply_levels(self.levels)

    @staticmethod
    def _text(label: QLabel, text: str) -> None:
        if label.text() != text:
            label.setText(text)

    def _show_rows(self, panel: view.MachinePanel) -> None:
        keys = tuple((row.machine, row.kind, row.key) for row in panel.inputs + panel.outputs)
        if keys != self._row_keys:
            self._row_keys = keys
            _clear(self.inputs_box)
            _clear(self.outputs_box)
            self.rows = []
            for box, rows in ((self.inputs_box, panel.inputs), (self.outputs_box, panel.outputs)):
                for index, row in enumerate(rows):
                    if index:
                        box.addWidget(widgets.rule(False))
                    widget = DeviceRow(row, self._test, self._headset)
                    box.addWidget(widget)
                    self.rows.append(widget)
        else:
            for widget, row in zip(self.rows, panel.inputs + panel.outputs):
                widget.update_row(row)
        self._text(self.inputs_empty, panel.inputs_empty)
        self.inputs_empty.setVisible(bool(panel.inputs_empty))
        self._text(self.outputs_empty, panel.outputs_empty)
        self.outputs_empty.setVisible(bool(panel.outputs_empty))

    def _show_notices(self, notices) -> None:
        own = OWN_ACTIONS.get(sys.platform, {})
        shown = tuple(notice if notice.action in (None, "retry_helper", "send_sound") or notice.action in own
                      else _without_action(notice) for notice in notices)
        keys = tuple((notice.code, notice.text, notice.tone, notice.action, notice.action_label) for notice in shown)
        if keys == self._notice_keys:
            for widget, notice in zip(self.notices, shown):
                widget.notice = notice
            return
        self._notice_keys = keys
        _clear(self.notices_box)
        self.notices = []
        for notice in shown:
            widget = NoticeView(notice, self._notice_action)
            self.notices_box.addWidget(widget)
            self.notices.append(widget)

    def apply_levels(self, levels) -> None:
        """The levels map (§8.1 `subscribe_levels`) on the meters only."""
        self.levels = levels
        self.mic_meter.set_reading(view.meter_reading(levels, self.mic_meter.key))
        for row in self.rows:
            row.meter.set_reading(view.meter_reading(levels, row.meter.key))
        self.map.set_levels(levels)

    # -- actions ------------------------------------------------------------------------------------

    def _machine_chosen(self, machine: str) -> None:
        self.selected = machine
        self.refresh()

    def _mic_chosen(self, choice: view.Choice) -> None:
        if self.service is not None:
            view.apply_mic(self.service, choice)

    def _output_chosen(self, choice: view.Choice) -> None:
        if self.service is not None and self.page is not None:
            view.apply_output(self.service, self.page.panel.machine, choice)

    def _test(self, row: view.DeviceRow) -> None:
        if self.service is not None:
            view.apply_test(self.service, row)

    def _headset(self, row: view.DeviceRow, on: bool) -> None:
        if self.service is not None:
            view.apply_headset(self.service, row, on)

    def _notice_action(self, notice: view.Notice) -> None:
        if self.service is None or view.apply_notice(self.service, notice):
            return
        target = OWN_ACTIONS.get(sys.platform, {}).get(notice.action)
        if target:
            QDesktopServices.openUrl(QUrl(target))


def _without_action(notice: view.Notice) -> view.Notice:
    from dataclasses import replace
    return replace(notice, action=None, action_label="")


class _LegendMark(QWidget):
    """A short sample of an arrow for the map's legend."""

    def __init__(self, kind: str, parent: QWidget | None = None) -> None:
        super().__init__(parent)
        self.kind = kind
        self.setFixedSize(26, 12)

    def paintEvent(self, _event) -> None:
        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)
        colour = _colour("ink_3" if self.kind == "waiting" else "signal")
        paint_arrow(painter, (1, 6), (25, 6), colour, dashed=self.kind == "mic")
