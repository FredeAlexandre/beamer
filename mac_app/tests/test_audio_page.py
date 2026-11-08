# SPDX-License-Identifier: GPL-3.0-only
"""The Mac Audio page (dev/audio/PHASE2.md §9.2), offscreen: it shows the page model, its controls
call the service setters, meters stay cheap on the main thread, and it meters only while visible.

    python -m mac_app.tests.test_audio_page render <folder>   # PNGs of the fixture states, light and dark
"""

from __future__ import annotations

import os
import statistics
import sys
import threading
import time
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import pages  # noqa: E402
import theme  # noqa: E402
from core.audio import view  # noqa: E402
from core.audio.model import DeviceRef, RouteStatus  # noqa: E402
from core.tests import audio_fixtures as fx  # noqa: E402
from mac_app.tests.design_offscreen import AppKit, settle  # noqa: E402
from mac_app.tests.test_window_geometry import offscreen_control_window  # noqa: E402



def _pages_with_audio():
    import importlib
    from core import feature_flags
    with mock.patch.object(feature_flags, "AUDIO", True, create=True):
        entries = importlib.reload(pages).PAGES
    importlib.reload(pages)
    return entries


WITH_AUDIO = _pages_with_audio()


def build(fixture, dark=True, width=1000, height=900):
    """The control window with the Audio page selected, showing `fixture` through a FakeService."""
    theme.init_fonts()
    with mock.patch.object(pages, "PAGES", WITH_AUDIO), mock.patch.object(pages, "KEYS", tuple(p[0] for p in WITH_AUDIO)), \
            mock.patch.object(theme, "system_dark", return_value=dark):
        control = offscreen_control_window()
        control._select_page("audio")
    service = fx.FakeService(fixture.snapshot)
    control.controller.audio = service
    page = control.audio_page
    page.tiles_of = lambda: fixture.tiles
    page.peers_without_audio = lambda: fixture.peers_without_audio
    page.selected = fixture.selected
    page.call_after = lambda fn, *args: fn(*args)
    control.window.setFrame_display_(((100000, 100000), (width, height)), False)
    page.refresh()
    page.levels = dict(fixture.levels)
    page._apply_levels(force=True)
    for _ in range(3):
        settle(control.window)
    return control, page, service


def walk(view_):
    yield view_
    for child in view_.subviews():
        yield from walk(child)


class AudioPageTests(unittest.TestCase):
    def setUp(self):
        from core import i18n
        i18n.install("en")

    def test_the_page_follows_crossing_when_audio_is_on(self):
        import importlib
        from core import feature_flags
        with mock.patch.object(feature_flags, "AUDIO", True, create=True):
            importlib.reload(pages)
            try:
                self.assertEqual(pages.KEYS[:4], ("overview", "permissions", "crossing", "audio"))
                self.assertEqual(pages.PAGES[3][2], "speaker.wave.2")
            finally:
                pass
        importlib.reload(pages)

    def test_shows_the_model(self):
        control, page, _service = build(fx.sound_redirected())
        model = page.page
        self.assertEqual(page.mic_status.text, model.mic_line)
        self.assertEqual(page.mic_popup.shown.title(), "This Mac · Default (MacBook Air Microphone)")
        self.assertEqual(page.panel_heading.text, "lebig")
        self.assertEqual(page.output_caption.text, "Sound from lebig plays on")
        self.assertEqual(page.output_popup.shown.title(), "This Mac · Default (MacBook Air Speakers)")
        self.assertFalse(page.mute_note.view.isHidden())
        self.assertEqual([tile.machine for tile in page.map.tiles], [fx.PC, fx.MAC])
        self.assertEqual(len([line for line, _head in page.map.arrow_layers if not line.isHidden()]), 2)
        self.assertEqual([notice.code for notice, _button in page.notice_buttons], [])
        self.assertFalse(page.notices_module.view.isHidden())
        self.assertEqual([row.name for row, _control, _view in page.row_controls],
                         ["Microphone (Wave:3)", "Microphone (Logitech G733)", "Speakers (Realtek(R) Audio)",
                          "Speakers (Logitech G733)", "Speakers (DualSense Wireless Controller)"])
        self.assertEqual(page.map.parts[fx.PC]["output"].string(), "Sound → This Mac")
        # Meters take the fixture's levels.
        self.assertIsNotNone(page.mic_meter.reading)
        self.assertGreater(page.map.parts[fx.MAC]["bar"].frame().size.width, 0)
        self.assertFalse(control.window.isVisible())

    def test_menus_group_choices_by_machine(self):
        _control, page, _service = build(fx.mic_streaming())
        menu = page.mic_popup.popup.menu()
        titles = [(menu.itemAtIndex_(i).title(), menu.itemAtIndex_(i).isEnabled(), menu.itemAtIndex_(i).indentationLevel())
                  for i in range(menu.numberOfItems()) if not menu.itemAtIndex_(i).isSeparatorItem()]
        self.assertEqual(titles[0], ("Off", True, 0))
        self.assertEqual(titles[1][:2], ("This Mac", False))
        self.assertEqual(titles[2], ("Default (MacBook Air Microphone)", True, 1))
        self.assertIn(("lebig", False), [title[:2] for title in titles])

    def test_controls_call_the_service_setters(self):
        _control, page, service = build(fx.mic_streaming())
        wave = view._choice_key("mic", fx.PC, fx.WAVE.key)
        page.mic_popup.pick(wave)
        page.mic_popup.pick(view.OFF)
        realtek = view._choice_key("out", fx.PC, fx.REALTEK.key)
        page.output_popup.pick(realtek)
        test_button = next(control for row, control, _view in page.row_controls if row.kind == "input")
        test_button.view.performClick_(None)
        headset = next(control for row, control, _view in page.row_controls if row.key == fx.MAC_SPEAKERS.key)
        headset._toggle()
        self.assertEqual(service.calls, [
            ("set_mic", (fx.PC, DeviceRef(fx.WAVE.key, fx.WAVE.name))),
            ("set_mic", (None, None)),
            ("set_output", (fx.MAC, fx.PC, DeviceRef(fx.REALTEK.key, fx.REALTEK.name))),
            ("test_input", (fx.MAC, fx.MAC_MIC.key)),
            ("set_form", (fx.MAC, fx.MAC_SPEAKERS.key, "headset")),
        ])

    def test_selecting_a_machine_on_the_map(self):
        _control, page, service = build(fx.mic_streaming())
        self.assertEqual(page.page.panel.machine, fx.MAC)
        page.map.select(fx.PC)
        self.assertEqual(page.page.panel.machine, fx.PC)
        self.assertEqual(page.panel_heading.text, "lebig")
        page.output_popup.pick(view._choice_key("out", fx.MAC, ""))
        self.assertEqual(service.calls, [("set_output", (fx.PC, fx.MAC, None))])
        # Arrow keys walk the tiles in reading order.
        event = mock.Mock()
        event.charactersIgnoringModifiers.return_value = AppKit.NSRightArrowFunctionKey
        page.map.keyDown_(event)
        self.assertEqual(page.page.panel.machine, fx.MAC)

    def test_notice_actions(self):
        _control, page, service = build(fx.driver_missing())
        opened = []
        page.install_driver = lambda: opened.append("install")
        notice, button = page.notice_buttons[0]
        self.assertEqual(notice.code, "driver_missing")
        button.view.performClick_(None)
        self.assertEqual(opened, ["install"])
        failed = fx.no_peers()
        _control, page, service = build(fx.Fixture("failed", failed.snapshot.__class__(
            **{**failed.snapshot.__dict__, "helper": "failed", "permission": {"mic": "denied"}}), failed.tiles, None))
        urls = []
        page.open_url = urls.append
        for notice, button in page.notice_buttons:
            button.view.performClick_(None)
        self.assertEqual(service.calls, [("retry_helper", ())])
        self.assertEqual(urls, ["x-apple.systempreferences:com.apple.preference.security?Privacy_Microphone"])

    def test_snapshots_wait_while_a_menu_is_open(self):
        _control, page, service = build(fx.mic_streaming())
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=True):
            page._menu(True)
            service.push(fx.sound_redirected().snapshot)
            self.assertEqual(page.page.tiles[0].output_line, "Sound plays here")
            self.assertTrue(page.dirty)
            page._menu(False)
            self.assertEqual(page.page.tiles[0].output_line, "Sound → This Mac")
        # Hidden, a snapshot only marks the page for its next showing.
        page.dirty = False
        service.push(fx.mic_streaming().snapshot)
        self.assertTrue(page.dirty)
        self.assertEqual(page.page.tiles[0].output_line, "Sound → This Mac")

    def test_meters_only_while_visible(self):
        _control, page, service = build(fx.mic_streaming())
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=True):
            page.update_watch()
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=False):
            page.update_watch()
            page.update_watch()
        self.assertEqual([call for call in service.calls if call[0] == "watch"], [("watch", (True,)), ("watch", (False,))])

    def test_main_thread_stays_under_a_frame_with_meters_at_20_hz(self):
        """A worker pushes levels at 20 Hz and a new snapshot every 2 s; every main-thread call the
        page makes stays under 16 ms. 3 s by default; AUDIO_METER_SECONDS=60 for the acceptance run."""
        seconds = float(os.environ.get("AUDIO_METER_SECONDS", "3"))
        control, page, service = build(fx.sound_redirected())
        from Foundation import NSDate, NSRunLoop
        from PyObjCTools import AppHelper
        page.call_after = AppHelper.callAfter
        page.timings = []
        states = [fx.sound_redirected().snapshot, fx.mic_streaming().snapshot]
        keys = sorted(page.page.meters)
        stop = threading.Event()

        def worker():
            tick = 0
            while not stop.is_set():
                phase = tick * 0.37
                service.push_levels({key: (-40 + 30 * abs(((phase + index) % 2) - 1), -20 + 18 * abs(((phase + index) % 2) - 1))
                                     for index, key in enumerate(keys)})
                if tick % 40 == 39:
                    service.push(states[(tick // 40) % 2])
                tick += 1
                time.sleep(0.05)

        thread = threading.Thread(target=worker, daemon=True)
        end = time.monotonic() + seconds
        # The window stays offscreen; the page is told it is on view, as it is while metering.
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=True):
            thread.start()
            try:
                while time.monotonic() < end:
                    NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.02))
            finally:
                stop.set()
                thread.join()
        levels = [duration * 1000 for kind, duration in page.timings if kind == "levels"]
        refreshes = [duration * 1000 for kind, duration in page.timings if kind == "refresh"]
        report = (f"{seconds:g} s: {len(levels)} level updates, max {max(levels):.2f} ms, median "
                  f"{statistics.median(levels):.2f} ms; {len(refreshes)} snapshot refreshes, max "
                  f"{max(refreshes) if refreshes else 0:.2f} ms")
        print("\n" + report, file=sys.stderr)
        self.assertGreater(len(levels), seconds * 20 * 0.7)
        self.assertLess(max(levels), 16, report)
        if refreshes:
            self.assertLess(max(refreshes), 16, report)
        self.assertFalse(control.window.isVisible())


def render(destination):
    """The fixture states in light and dark, and the Crossing page for comparison."""
    from mac_app.tests.settings_layout_offscreen import render as render_view
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    written = []
    for dark in (False, True):
        for name, make in fx.FIXTURES.items():
            fixture = make()
            control, page, _service = build(fixture, dark=dark)
            document = control.pages["audio"].documentView()
            height = document.frame().size.height
            control.window.setFrame_display_(((100000, 100000), (1000, height + 100)), False)
            for _ in range(3):
                settle(control.window)
            path = destination / f"audio-{name.replace('_', '-')}-{'dark' if dark else 'light'}.png"
            render_view(control.window.contentView(), 1, path)
            written.append(path)
    # The Crossing page as the same window draws it (a PC on the left, dark), to compare the maps.
    from mac_app.tests import render_crossing_ledger as crossing
    path = destination / "crossing-dark.png"
    with mock.patch.object(crossing, "render", lambda page, _density, _path: render_view(
            page.window().contentView(), 1, path)):
        crossing.built_state(2, destination)
    written.append(path)
    return written


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "render":
        for path in render(sys.argv[2]):
            print(path)
    else:
        unittest.main()
