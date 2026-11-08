# SPDX-License-Identifier: GPL-3.0-only
"""The Qt Audio page (dev/audio/PHASE2.md §9.2, §13 WP7), on the shared fixtures and a fake service.

Also renders the acceptance shots: `python win_app/tests/test_audio_page_win.py --render <dir>` writes
the four fixture states in light and dark, each as the window shows it, plus the Crossing page.
"""

from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
for path in (ROOT, ROOT / "win_app", TESTS):
    sys.path.insert(0, str(path))

from PySide6.QtCore import QCoreApplication, QEvent, QPointF, Qt
from PySide6.QtGui import QColor, QImage, QKeyEvent, QPainter
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication, QVBoxLayout, QWidget

import audio_page_win
import theme
import widgets
from core import i18n
from core.audio import model, view
from core.audio.model import DeviceRef, MicRoute, OutputRoute, RouteStatus
from core.tests import audio_fixtures as fx


def app():
    instance = QApplication.instance() or QApplication([])
    if not getattr(instance, "_audio_fonts", False):
        theme.init_fonts()
        instance.setFont(theme.font(theme.TYPE["body"]))
        theme.apply_stylesheet(instance)
        instance._audio_fonts = True
    return instance


def settle(application, rounds=6):
    for _ in range(rounds):
        QCoreApplication.sendPostedEvents(None, QEvent.Type.DeferredDelete)
        QCoreApplication.sendPostedEvents(None, QEvent.Type.LayoutRequest)
        application.processEvents()


def as_pc(fixture):
    """The same state seen from lebig: this PC is `own`, the Mac a linked peer. In driver_missing
    it is this PC that lacks its virtual mic (VB-CABLE), as the Mac lacks Beamer Microphone there."""
    snap = fixture.snapshot
    if fixture.name == "driver_missing":
        bare = fx.pc()
        snap = replace(snap, machines={**snap.machines, fx.PC: replace(
            bare, virtual_mic=model.VirtualMic(None, False, False), features=bare.features - {"virtual_mic"})})
    direct = frozenset({fx.MAC}) if fx.MAC in snap.machines and len(snap.machines) > 1 else frozenset()
    machines = dict(snap.machines)
    if fx.PC not in machines:
        machines[fx.PC] = fx.pc(links=())
        direct = frozenset()
    snap = replace(snap, own=fx.PC, direct=direct, machines=machines,
                   online=model.online(fx.PC, direct, machines))
    tiles = dict(fixture.tiles)
    tiles.setdefault(fx.PC, fx.TILES[fx.PC])
    return replace(fixture, snapshot=snap, tiles=tiles, own_label="This PC")


class Host(QWidget):
    """A page column as the window gives it: the page under a plain width-limited column."""

    def __init__(self, page, width=760):
        super().__init__()
        self.setProperty("vernier", "plain")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(24, 24, 24, 24)
        layout.addWidget(page)
        layout.addStretch(1)
        self.resize(width, 1200)


def make_page(fixture, service=None):
    service = service or fx.FakeService(fixture.snapshot)
    page = audio_page_win.AudioPageWidget(service, own_label=fixture.own_label, tiles=lambda: fixture.tiles,
                                          peers_without_audio=lambda: fixture.peers_without_audio)
    page.selected = fixture.selected
    page.refresh()
    page.apply_levels(fixture.levels)
    return page, service


class Geometry(unittest.TestCase):
    def test_touching_tiles_get_a_gap_and_keep_their_order(self):
        rects = audio_page_win.map_geometry({fx.PC: fx.TILES[fx.PC], fx.MAC: fx.TILES[fx.MAC]}, 700, 260)
        pc, mac = rects[fx.PC], rects[fx.MAC]
        self.assertAlmostEqual(mac[0] - (pc[0] + pc[2]), audio_page_win.MAP_GAP, delta=1)
        self.assertLess(pc[0], mac[0])
        for x, y, w, h in rects.values():
            self.assertGreaterEqual(x, 0)
            self.assertGreaterEqual(y, 0)
            self.assertLessEqual(x + w, 700)
            self.assertLessEqual(y + h, 260)

    def test_three_machines_fit_the_canvas(self):
        rects = audio_page_win.map_geometry(dict(fx.TILES), 420, 260)
        self.assertGreater(rects[fx.ROG][1], rects[fx.PC][1] + rects[fx.PC][3])
        for x, y, w, h in rects.values():
            self.assertTrue(0 <= x and x + w <= 420 and 0 <= y and y + h <= 260)

    def test_arrows_cross_the_gap(self):
        start, end = audio_page_win.arrow_line((0, 0, 100, 60), (150, 10, 100, 60))
        self.assertEqual(start[1], end[1])
        self.assertGreater(start[0], 100)
        self.assertLess(end[0], 150)
        back_start, back_end = audio_page_win.arrow_line((150, 10, 100, 60), (0, 0, 100, 60))
        self.assertGreater(back_start[0], back_end[0])

    def test_two_arrows_between_one_pair_do_not_overlap(self):
        arrows = (view.Arrow("a", "b", "mic", "active", ""), view.Arrow("b", "a", "sound", "active", ""))
        offsets = audio_page_win.arrow_offsets(arrows)
        lines = [audio_page_win.arrow_line(*rects, offset) for rects, offset in
                 zip((((0, 0, 100, 60), (150, 0, 100, 60)), ((150, 0, 100, 60), (0, 0, 100, 60))), offsets)]
        self.assertNotEqual(lines[0][0][1], lines[1][0][1])

    def test_nearest_follows_the_arrow_keys(self):
        rects = {"pc": (0, 0, 100, 60), "mac": (150, 0, 100, 60), "rog": (0, 100, 100, 60)}
        self.assertEqual(audio_page_win.nearest(rects, "pc", (1, 0)), "mac")
        self.assertEqual(audio_page_win.nearest(rects, "pc", (0, 1)), "rog")
        self.assertEqual(audio_page_win.nearest(rects, "mac", (-1, 0)), "pc")
        self.assertIsNone(audio_page_win.nearest(rects, "pc", (-1, 0)))


class Page(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = app()
        i18n.install("en")

    def tearDown(self):
        settle(self.app)

    def test_without_a_service_the_page_says_so(self):
        page = audio_page_win.AudioPageWidget(lambda: None, own_label="This PC")
        self.assertFalse(page.unavailable.isHidden())
        self.assertTrue(page.mic_module.isHidden())

    def test_every_fixture_shows_its_model(self):
        for name, make in list(fx.FIXTURES.items()) + [("three_machines", fx.three_machines)]:
            for fixture in (make(), as_pc(make())):
                with self.subTest(name, own=fixture.own_label):
                    page, _service = make_page(fixture)
                    model_page = page.page
                    self.assertEqual(page.mic_select.text(),
                                     view.choice_by_key(model_page.mic_choices, model_page.mic_selected).title)
                    self.assertEqual(page.mic_line.text(), model_page.mic_line)
                    self.assertEqual(len(page.map.tiles), len(model_page.tiles))
                    self.assertEqual(len(page.rows), len(model_page.panel.inputs) + len(model_page.panel.outputs))
                    self.assertEqual([widget.notice.code for widget in page.notices],
                                     [notice.code for notice in model_page.notices])
                    page.deleteLater()

    def test_mic_choice_calls_set_mic(self):
        fixture = fx.mic_streaming()
        page, service = make_page(fixture)
        key = next(choice.key for choice in page.mic_select.choices
                   if choice.device is not None and choice.device.key == fx.AIRPODS_MIC.key)
        self.assertTrue(page.mic_select.choose(key))
        self.assertEqual(service.calls, [("set_mic", (fx.MAC, DeviceRef(fx.AIRPODS_MIC.key, fx.AIRPODS_MIC.name)))])
        page.mic_select.choose(view.OFF)
        self.assertEqual(service.calls[-1], ("set_mic", (None, None)))
        # A header is never a pick.
        header = next(choice.key for choice in page.mic_select.choices if choice.header)
        self.assertFalse(page.mic_select.choose(header))

    def test_output_choice_calls_set_output_for_the_selected_machine(self):
        fixture = fx.mic_streaming()
        page, service = make_page(fixture)
        page.map.on_select(fx.PC)
        self.assertEqual(page.page.panel.machine, fx.PC)
        key = next(choice.key for choice in page.output_select.choices
                   if choice.machine == fx.MAC and choice.device is None and not choice.header)
        self.assertTrue(page.output_select.choose(key))
        self.assertEqual(service.calls[-1], ("set_output", (fx.PC, fx.MAC, None)))
        service.push(replace(service.snapshot(), route=replace(service.snapshot().route,
                                                                outputs={fx.PC: OutputRoute(fx.MAC, None, fx.stamp(5))})))
        settle(self.app)
        self.assertTrue(page.output_select.choose(view.HERE))
        self.assertEqual(service.calls[-1], ("set_output", (fx.PC, None, None)))

    def test_rows_call_test_and_set_form(self):
        page, service = make_page(fx.mic_streaming())
        inputs = [row for row in page.rows if row.row.kind == "input"]
        outputs = [row for row in page.rows if row.row.kind == "output"]
        inputs[0].control.click()
        self.assertEqual(service.calls[-1], ("test_input", (fx.MAC, fx.MAC_MIC.key)))
        speakers = next(row for row in outputs if row.row.key == fx.MAC_SPEAKERS.key)
        self.assertFalse(speakers.control.isChecked())
        speakers.control.click()
        self.assertEqual(service.calls[-1], ("set_form", (fx.MAC, fx.MAC_SPEAKERS.key, "headset")))

    def test_notice_actions_go_through_the_service(self):
        fixture = fx.no_peers()
        fixture = replace(fixture, snapshot=replace(fixture.snapshot, helper="failed"))
        page, service = make_page(fixture)
        notice = next(widget for widget in page.notices if widget.notice.code == "helper_failed")
        notice.action.click()
        self.assertEqual(service.calls[-1], ("retry_helper", ()))

    def test_watch_follows_visibility(self):
        page, service = make_page(fx.no_peers())
        host = Host(page)
        host.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
        host.show()
        settle(self.app)
        self.assertEqual([call for call in service.calls if call[0] == "watch"], [("watch", (True,))])
        host.hide()
        settle(self.app)
        self.assertEqual([call for call in service.calls if call[0] == "watch"][-1], ("watch", (False,)))
        host.deleteLater()

    def test_a_snapshot_from_the_worker_updates_the_page_on_the_qt_thread(self):
        fixture = fx.no_peers()
        page, service = make_page(fixture)
        seen = []
        original = page.refresh

        def refresh():
            seen.append(threading.current_thread() is threading.main_thread())
            original()

        page.refresh = refresh
        streaming = fx.mic_streaming().snapshot
        worker = threading.Thread(target=lambda: [service.push(streaming) for _ in range(50)])
        worker.start()
        worker.join()
        self.assertEqual(seen, [])  # nothing ran on the worker
        settle(self.app)
        self.assertTrue(seen and all(seen))
        self.assertLessEqual(len(seen), 3)  # coalesced
        self.assertEqual(page.mic_line.text(), view.page_model(streaming, fixture.tiles, None, "This Mac").mic_line)

    def test_meters_never_block_the_ui_thread(self):
        """Levels at 20 Hz from a worker for 1 s: each callback returns at once, whatever the UI
        thread is doing, and the meters show the last reading once it runs."""
        fixture = fx.mic_streaming()
        page, service = make_page(fixture)
        key = f"{fx.MAC}/in/{fx.MAC_MIC.key}"
        durations = []

        def feed():
            for index in range(20):
                levels = {key: (-60.0 + index * 2.5, -50.0 + index * 2.5)}
                start = time.perf_counter()
                service.push_levels(levels)
                durations.append(time.perf_counter() - start)
                time.sleep(0.05)

        worker = threading.Thread(target=feed)
        worker.start()
        # The UI thread is busy (not processing events) for most of the run.
        time.sleep(0.6)
        worker.join()
        self.assertLess(max(durations), 0.005)
        settle(self.app)
        self.assertEqual(page.mic_meter.reading, tuple(round(value, 3) for value in
                                                       view.meter_reading({key: (-12.5, -2.5)}, key)))

    def test_the_menu_is_not_rebuilt_while_open(self):
        page, service = make_page(fx.mic_streaming())
        page.mic_select._fill()
        actions = page.mic_select.menu().actions()
        labels = [action.text().strip() for action in actions if not action.isSeparator()]
        self.assertIn("MacBook Air Microphone", labels)
        self.assertIn("This Mac", labels)
        checked = [action.text().strip() for action in actions if action.isChecked()]
        self.assertEqual(len(checked), 1)

    def test_keyboard_reaches_every_control(self):
        fixture = fx.sound_redirected()
        fixture = replace(fixture, snapshot=replace(fixture.snapshot, helper="failed"))
        page, _service = make_page(fixture)
        host = Host(page)
        host.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
        host.show()
        settle(self.app)
        expected = {page.mic_select, page.map, page.output_select}
        expected |= {row.control for row in page.rows if row.control.isEnabled()}
        expected |= {notice.action for notice in page.notices if notice.action is not None}
        host.activateWindow()
        QApplication.setActiveWindow(host)
        page.mic_select.setFocus(Qt.FocusReason.TabFocusReason)
        settle(self.app)
        reached = set()
        for _ in range(40):
            focused = QApplication.focusWidget()
            reached.add(focused)
            QTest.keyClick(focused, Qt.Key.Key_Tab)
            settle(self.app, 1)
        self.assertEqual(expected - reached, set())
        # Arrow keys move between the machines on the map.
        page.map.setFocus(Qt.FocusReason.TabFocusReason)
        before = page.selected
        QTest.keyClick(page.map, Qt.Key.Key_Right if before == fx.PC else Qt.Key.Key_Left)
        settle(self.app)
        self.assertNotEqual(page.selected, before)
        host.deleteLater()

    def test_clicking_a_tile_selects_it(self):
        page, _service = make_page(fx.mic_streaming())
        host = Host(page)
        host.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
        host.show()
        settle(self.app)
        rect = page.map.tile_rect(fx.PC)
        QTest.mouseClick(page.map, Qt.MouseButton.LeftButton, pos=rect.center().toPoint())
        settle(self.app)
        self.assertEqual(page.page.panel.machine, fx.PC)
        host.deleteLater()


class InTheWindow(unittest.TestCase):
    """The page in the settings window: after Crossing in the sidebar, opened with Ctrl+<n>."""

    @classmethod
    def setUpClass(cls):
        cls.app = app()
        i18n.install("en")

    def setUp(self):
        self.window, self.folder = open_window()

    def tearDown(self):
        close_window(self.window, self.folder)

    def test_audio_follows_crossing_and_opens_with_its_shortcut(self):
        import pages_win
        keys = list(pages_win.KEYS)
        self.assertEqual(keys.index("audio"), keys.index("crossing") + 1)
        self.window._select_page("overview")
        from PySide6.QtGui import QKeySequence, QShortcut
        wanted = QKeySequence(f"Ctrl+{keys.index('audio') + 1}")
        shortcuts = [item for item in self.window.findChildren(QShortcut) if item.key() == wanted]
        self.assertEqual(len(shortcuts), 1)
        # An offscreen window is never active, so the key press itself cannot fire it here.
        shortcuts[0].activated.emit()
        settle(self.app)
        self.assertEqual(self.window._page, "audio")
        self.assertIs(self.window.stack.currentWidget().widget().findChild(audio_page_win.AudioPageWidget),
                      self.window.audio_page)

    def test_the_page_finds_the_service_when_the_app_has_one(self):
        page = self.window.audio_page
        self.window._select_page("audio")
        settle(self.app)
        self.assertIsNone(page.service)
        self.assertFalse(page.unavailable.isHidden())
        service = fx.FakeService(as_pc(fx.mic_streaming()).snapshot)
        self.window.audio_service = service
        page._find_service()
        settle(self.app)
        self.assertIs(page.service, service)
        self.assertIn(("watch", (True,)), service.calls)
        self.window._select_page("crossing")
        settle(self.app)
        self.assertEqual(service.calls[-1], ("watch", (False,)))


def open_window():
    from test_settings_layout_acceptance import SyntheticWindow, write_synthetic_settings
    scratch = ROOT / "win_app" / ".test-tmp"
    scratch.mkdir(parents=True, exist_ok=True)
    folder = tempfile.TemporaryDirectory(prefix="beamer-audio-page-", dir=scratch)
    window = SyntheticWindow(write_synthetic_settings(Path(folder.name)))
    window.full_screen_timer.stop()
    window.refresh_timer.stop()
    window.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
    window.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
    window.resize(1040, 900)
    window.show()
    window.activateWindow()
    settle(QApplication.instance())
    return window, folder


def close_window(window, folder):
    window.server.stop()
    window.close()
    window.deleteLater()
    settle(QApplication.instance())
    folder.cleanup()


# -- renders ----------------------------------------------------------------------------------------

def _set_theme(application, root, dark):
    theme.set_dark(dark)
    theme.apply_stylesheet(application)
    widgets.refresh_colours(root)


def grab(widget, scale=1.0):
    image = widget.grab().toImage()
    canvas = QImage(image.size(), QImage.Format.Format_ARGB32_Premultiplied)
    canvas.fill(QColor(theme.colour("ground")))
    painter = QPainter(canvas)
    painter.drawImage(0, 0, image)
    painter.end()
    if scale != 1.0:
        canvas = canvas.scaledToWidth(round(canvas.width() * scale), Qt.TransformationMode.SmoothTransformation)
    return canvas


def render_page_only(out: Path, names=None, views=("mac", "pc")):
    """The page alone in a column, each fixture in light and dark (fast; for iterating on looks)."""
    application = app()
    i18n.install("en")
    out.mkdir(parents=True, exist_ok=True)
    for name, make in fx.FIXTURES.items():
        if names and name not in names:
            continue
        for label, fixture in (("mac", make()), ("pc", as_pc(make()))):
            if label not in views:
                continue
            for dark in (False, True):
                page, _service = make_page(fixture)
                host = Host(page)
                host.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
                host.show()
                _set_theme(application, host, dark)
                settle(application)
                host.resize(host.width(), host.sizeHint().height())
                settle(application)
                path = out / f"{name}-{label}-{'dark' if dark else 'light'}.png"
                grab(host).save(str(path))
                print(path)
                host.deleteLater()
                settle(application)


def _window_theme(window, dark):
    theme.set_dark(dark)
    window._apply_theme()
    widgets.refresh_colours(window)


def _fit_and_grab(application, window, key, width=1040):
    """The window tall enough for the whole page, so a shot needs no scrolling."""
    window._select_page(key)
    settle(application)
    document = window.stack.currentWidget().widget()
    for _ in range(3):
        chrome = window.height() - window.stack.height()
        window.resize(width, max(600, document.sizeHint().height() + chrome))
        settle(application)
    return grab(window)


def render_window(out: Path):
    """The acceptance shots (§13 WP7): each fixture state as this PC's window shows it, light and
    dark, and the Crossing page beside them for comparison."""
    application = app()
    i18n.install("en")
    out.mkdir(parents=True, exist_ok=True)
    window, folder = open_window()
    try:
        page = window.audio_page
        for name, make in fx.FIXTURES.items():
            fixture = as_pc(make())
            page._tiles = lambda tiles=fixture.tiles: tiles
            page.selected = fixture.selected
            page.set_service(fx.FakeService(fixture.snapshot))
            page.apply_levels(fixture.levels)
            for dark in (False, True):
                _window_theme(window, dark)
                path = out / f"audio-{name}-{'dark' if dark else 'light'}.png"
                _fit_and_grab(application, window, "audio").save(str(path))
                print(path)
        for dark in (False, True):
            _window_theme(window, dark)
            path = out / f"crossing-{'dark' if dark else 'light'}.png"
            _fit_and_grab(application, window, "crossing").save(str(path))
            print(path)
    finally:
        close_window(window, folder)


if __name__ == "__main__":
    if "--render" in sys.argv:
        out = Path(sys.argv[sys.argv.index("--render") + 1])
        render_window(out)
        # The fixtures exactly as given (the Mac's view), to set beside the AppKit page's shots.
        render_page_only(out / "fixtures-as-given", views=("mac",))
        sys.exit(0)
    if "--render-page" in sys.argv:
        render_page_only(Path(sys.argv[sys.argv.index("--render-page") + 1]), sys.argv[sys.argv.index("--render-page") + 2:])
    else:
        unittest.main()
