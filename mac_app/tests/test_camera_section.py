# SPDX-License-Identifier: GPL-3.0-only
"""The Camera module of the Mac's Audio & Camera page and the menu bar's camera items
(dev/camera/PHASE2.md §8.3-§8.5, §12 CW7), offscreen: the page shows the camera model, its controls
call the service's camera setters, the map draws the camera's badge and arrow, the menu shows the
sharing switch and the permission item, and the main thread stays under a frame.

    python -m mac_app.tests.test_camera_section render <folder>   # PNGs of the 8 states, light and dark
"""

from __future__ import annotations

import os
import statistics
import sys
import threading
import time
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import pages  # noqa: E402
import theme  # noqa: E402
from core import feature_flags  # noqa: E402
from core.audio import view as audio_view  # noqa: E402
from core.camera import view as camera_view  # noqa: E402
from core.camera.model import CameraRef  # noqa: E402
from core.tests import camera_fixtures as cf  # noqa: E402
from core.tests.camera_fixtures import MAC, PC  # noqa: E402
from mac_app.tests.design_offscreen import AppKit, settle  # noqa: E402
from mac_app.tests.test_window_geometry import offscreen_control_window  # noqa: E402


def _pages_with(audio=True, camera=True):
    import importlib
    with mock.patch.object(feature_flags, "AUDIO", audio, create=True), \
            mock.patch.object(feature_flags, "CAMERA", camera, create=True):
        entries = importlib.reload(pages).PAGES
    importlib.reload(pages)
    return entries


WITH_CAMERA = _pages_with()


def build(fixture, dark=True, width=1000, height=1100, service=None):
    """The control window on the Audio & Camera page showing `fixture` through a camera FakeService.
    The caller holds the CAMERA flag on (CameraFlag)."""
    theme.init_fonts()
    with mock.patch.object(pages, "PAGES", WITH_CAMERA), \
            mock.patch.object(pages, "KEYS", tuple(p[0] for p in WITH_CAMERA)), \
            mock.patch.object(theme, "system_dark", return_value=dark):
        control = offscreen_control_window()
        control._select_page("audio")
    service = service or cf.service_for(fixture)
    control.controller.audio = service
    page = control.audio_page
    page.tiles_of = lambda: fixture.audio.tiles
    page.peers_without_audio = lambda: fixture.audio.peers_without_audio
    page.selected = fixture.audio.selected
    page.driver_of = lambda: None
    page.installer = lambda **options: (_ for _ in ()).throw(AssertionError("no install in tests"))
    page.call_after = lambda fn, *args: fn(*args)
    control.window.setFrame_display_(((100000, 100000), (width, height)), False)
    page.refresh()
    page.levels = dict(fixture.audio.levels)
    page._apply_levels(force=True)
    for _ in range(3):
        settle(control.window)
    return control, page, service


class CameraFlag(unittest.TestCase):
    def setUp(self):
        from core import i18n
        i18n.install("en")
        patcher = mock.patch.object(feature_flags, "CAMERA", True, create=True)
        patcher.start()
        self.addCleanup(patcher.stop)


class PageTitle(unittest.TestCase):
    def test_audio_and_camera_with_the_flag_same_key_and_place(self):
        with_camera = _pages_with(camera=True)
        audio_only = _pages_with(camera=False)
        self.assertEqual([page[0] for page in with_camera], [page[0] for page in audio_only])
        index = [page[0] for page in with_camera].index("audio")
        self.assertEqual(index, 3)
        self.assertEqual(with_camera[index][1], "Audio & Camera")
        self.assertIn("the camera they see", with_camera[index][3])
        self.assertEqual(audio_only[index][1], "Audio")
        self.assertNotIn("audio", [page[0] for page in _pages_with(audio=False, camera=True)])


class CameraModuleTests(CameraFlag):
    def test_shows_the_model(self):
        control, page, _service = build(cf.streaming())
        module = page.camera
        section = module.section
        self.assertFalse(module.view.isHidden())
        self.assertEqual(module.status.text, "Microsoft Teams on lebig is using it.")
        self.assertEqual(module.detail.text, "1280x720 · 30 fps · 72 ms")
        self.assertEqual(module.popup.shown.title(), "This Mac · Default camera (MacBook Air Camera)")
        self.assertEqual(module.quality.value, "720p")
        self.assertTrue(module.quality.enabled)
        self.assertEqual(module.hint.text, section.hint)
        self.assertTrue(module.help_row.isHidden())
        self.assertTrue(module.status_button.view.isHidden())
        self.assertEqual([(row.label, button.title) for row, button in module.rows], [("Camera sharing", "Stop Sharing")])
        self.assertEqual(module.notice_buttons, [])
        self.assertFalse(page.camera_legend.isHidden())
        # The map: the badge on the Mac's tile in the accent colour, the app on lebig's, a live dotted arrow.
        parts = page.map.parts
        self.assertEqual(parts[MAC]["camera_text"].string(), "MacBook Air Camera")
        self.assertFalse(parts[MAC]["camera"].isHidden())
        self.assertEqual(parts[PC]["camera_text"].string(), "Microsoft Teams uses the camera")
        cameras = [arrow for arrow in page.map.arrows if arrow.kind == "camera"]
        self.assertEqual([(arrow.source, arrow.target, arrow.state) for arrow in cameras], [(MAC, PC, "active")])
        shown = [line for line, _head in page.map.arrow_layers if not line.isHidden()]
        self.assertIn(list(camera_view_dashes()), [list(line.lineDashPattern() or []) for line in shown])
        self.assertFalse(control.window.isVisible())

    def test_the_arrow_lanes_follow_the_tile_lines(self):
        # Streaming: the mic and the camera both run between the Mac and lebig. On the tile the camera's
        # line sits above the mic's, so the camera's arrow runs above the mic's too (same layer space).
        import Quartz
        import audio_page
        _control, page, _service = build(cf.streaming())
        parts = page.map.parts[MAC]
        self.assertLess(parts["camera_text"].frame().origin.y, parts["mic_text"].frame().origin.y)
        middles = {}
        for line, _head in page.map.arrow_layers:
            if line.isHidden():
                continue
            dashes = [float(value) for value in (line.lineDashPattern() or [])]
            kind = next(kind for kind, pattern in audio_page.ARROW_DASHES.items() if (pattern or []) == dashes)
            middles[kind] = Quartz.CGRectGetMidY(Quartz.CGPathGetBoundingBox(line.path()))
        self.assertEqual(set(middles), {"mic", "camera"})
        self.assertLess(middles["camera"], middles["mic"])
        self.assertEqual(audio_page.ARROW_LANES, ("camera", "mic", "sound"))
        # The tile's camera glyph is a video camera, which this macOS draws.
        self.assertIsNotNone(AppKit.NSImage.imageWithSystemSymbolName_accessibilityDescription_("video.fill", None))

    def test_the_module_sits_between_the_notices_and_the_machines(self):
        _control, page, _service = build(cf.idle())
        order = list(page.body.arrangedSubviews())
        self.assertLess(order.index(page.notices_module.view), order.index(page.camera.view))
        self.assertLess(order.index(page.camera.view), order.index(page.map.superview().superview()))

    def test_hidden_without_the_flag_or_a_camera_part(self):
        with mock.patch.object(feature_flags, "CAMERA", False):
            _control, page, _service = build(cf.streaming())
            self.assertTrue(page.camera.view.isHidden())
            self.assertTrue(page.camera_legend.isHidden())
            self.assertFalse(any(arrow.kind == "camera" for arrow in page.map.arrows))
        audio_only = mock.Mock(spec=["snapshot", "subscribe", "subscribe_levels", "watch"])
        fixture = cf.idle()
        audio_only.snapshot.return_value = fixture.audio.snapshot
        audio_only.subscribe.return_value = lambda: None
        audio_only.subscribe_levels.return_value = lambda: None
        _control, page, _service = build(fixture, service=audio_only)
        self.assertTrue(page.camera.view.isHidden())
        # No machine with a camera side: hidden too.
        empty = replace(fixture.camera, machines={})
        _control, page, _service = build(replace(fixture, camera=empty))
        self.assertTrue(page.camera.view.isHidden())

    def test_controls_call_the_service(self):
        _control, page, service = build(cf.idle())
        module = page.camera
        module.popup.pick(audio_view._choice_key("cam", MAC, cf.IPHONE_CAMERA.key))
        module.popup.pick(camera_view.OFF)
        module.quality._choose("1080p")
        _row, button = module.rows[0]
        button.view.performClick_(None)
        self.assertEqual(service.calls, [
            ("set_camera", (MAC, CameraRef(cf.IPHONE_CAMERA.key, cf.IPHONE_CAMERA.name))),
            ("set_camera", (None, None)),
            ("set_quality", ("1080p",)),
            ("set_camera_sharing", (False,)),
        ])

    def test_sharing_off_and_permission_and_denied(self):
        _control, page, service = build(cf.sharing_off())
        self.assertEqual(page.camera.status.text, "Camera sharing is off on this Mac.")
        _row, button = page.camera.rows[0]
        self.assertEqual(button.title, "Share the Camera")
        button.view.performClick_(None)
        self.assertEqual(service.calls[-1], ("set_camera_sharing", (True,)))

        _control, page, service = build(cf.permission_from_pc())
        notice, button = page.camera.notice_buttons[0]
        self.assertEqual((notice.code, button.title), ("camera_permission", "Allow Camera Access…"))
        button.view.performClick_(None)
        self.assertEqual(service.calls, [("ask_camera_permission", ())])

        _control, page, service = build(cf.denied())
        urls = []
        page.open_url = urls.append
        row, button = page.camera.rows[-1]
        self.assertEqual((row.label, button.title), ("Camera access", "Open Privacy Settings"))
        button.view.performClick_(None)
        self.assertEqual(urls, [camera_view.PRIVACY_URL])
        self.assertEqual(service.calls, [])

    def test_off_disables_quality_and_error_offers_retry(self):
        _control, page, service = build(cf.off())
        self.assertFalse(page.camera.quality.enabled)
        self.assertEqual(page.camera.popup.shown.title(), "Off")
        fixture = cf.idle()
        failed = replace(fixture.camera, status=cf.status("error", PC, "no_receiver"))
        _control, page, service = build(replace(fixture, camera=failed))
        self.assertFalse(page.camera.status_button.view.isHidden())
        page.camera.status_button.view.performClick_(None)
        self.assertEqual(service.calls, [("set_camera", (MAC, None))])

    def test_a_camera_change_reaches_the_page(self):
        _control, page, service = build(cf.idle())
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=True):
            service.push_camera(cf.streaming().camera)
        self.assertEqual(page.camera.status.text, "Microsoft Teams on lebig is using it.")
        # While a menu of the page is open, the module waits like the rest of the page.
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=True):
            page._menu(True)
            service.push_camera(cf.sharing_off().camera)
            self.assertEqual(page.camera.status.text, "Microsoft Teams on lebig is using it.")
            page._menu(False)
        self.assertEqual(page.camera.status.text, "Camera sharing is off on this Mac.")

    def test_pc_not_installed_notice_names_the_pc(self):
        _control, page, _service = build(cf.pc_not_installed())
        self.assertEqual([notice.text for notice in page.camera.section.notices],
                         ["lebig: Beamer Camera is not installed there. Install it in Beamer on lebig."])
        self.assertEqual(len(page.camera.notices_host.arrangedSubviews()), 2)    # hairline + notice
        self.assertEqual(page.camera.notice_buttons, [])

    def test_main_thread_stays_under_a_frame_with_camera_snapshots_at_4_hz(self):
        """A worker pushes camera snapshots at 4 Hz and levels at 20 Hz; the menu's status tick runs
        every 0.4 s; every main-thread call stays under 16 ms. 3 s by default; CAMERA_UI_SECONDS=60
        for the acceptance run."""
        seconds = float(os.environ.get("CAMERA_UI_SECONDS", "3"))
        control, page, service = build(cf.streaming())
        from Foundation import NSDate, NSRunLoop
        from PyObjCTools import AppHelper
        import kvm_bridge_app
        page.call_after = AppHelper.callAfter
        page.timings = []
        tray = FakeTray(service)
        menu_timings = []
        states = [cf.streaming().camera, cf.reduced().camera, cf.permission_from_pc().camera, cf.sharing_off().camera,
                  cf.idle().camera, cf.denied().camera]
        keys = sorted(page.page.meters)
        stop = threading.Event()

        def worker():
            tick = 0
            while not stop.is_set():
                service.push_levels({key: (-40 + 30 * abs(((tick * 0.37 + index) % 2) - 1), -20.0)
                                     for index, key in enumerate(keys)})
                if tick % 5 == 0:
                    service.push_camera(states[(tick // 5) % len(states)])
                tick += 1
                time.sleep(0.05)

        thread = threading.Thread(target=worker, daemon=True)
        end = time.monotonic() + seconds
        next_menu = time.monotonic()
        with mock.patch.object(type(page), "visible", new_callable=mock.PropertyMock, return_value=True):
            thread.start()
            try:
                while time.monotonic() < end:
                    NSRunLoop.currentRunLoop().runUntilDate_(NSDate.dateWithTimeIntervalSinceNow_(0.02))
                    if time.monotonic() >= next_menu:
                        next_menu += 0.4
                        started = time.perf_counter()
                        kvm_bridge_app.TrayApp._camera_items(tray)
                        kvm_bridge_app.TrayApp._refresh_tray_state(tray)
                        menu_timings.append((time.perf_counter() - started) * 1000)
            finally:
                stop.set()
                thread.join()
        refreshes = [duration * 1000 for kind, duration in page.timings if kind == "refresh"]
        levels = [duration * 1000 for kind, duration in page.timings if kind == "levels"]
        report = (f"{seconds:g} s: {len(refreshes)} refreshes, max {max(refreshes):.2f} ms, median "
                  f"{statistics.median(refreshes):.2f} ms; {len(levels)} level updates, max {max(levels):.2f} ms; "
                  f"{len(menu_timings)} menu ticks, max {max(menu_timings):.2f} ms")
        print("\n" + report, file=sys.stderr)
        self.assertGreater(len(refreshes), seconds * 4 * 0.6, report)
        self.assertLess(max(refreshes), 16, report)
        self.assertLess(max(levels), 16, report)
        self.assertLess(max(menu_timings), 16, report)
        self.assertFalse(control.window.isVisible())


def camera_view_dashes():
    import audio_page
    return audio_page.ARROW_DASHES["camera"]


class FakeItem:
    def __init__(self, key):
        self.title = key
        self.hidden = False


class FakeTray:
    """The parts of TrayApp the camera items use."""

    def __init__(self, service):
        import logging
        self.controller = mock.Mock()
        self.controller.audio = service
        self.controller.cfg.hide_addresses = False
        self.controller.redirecting = False
        self.controller.driver = None
        self.logger = logging.getLogger("test-camera-menu")
        self.camera_line_item = FakeItem("beamer-camera-line")
        self.camera_sharing_item = FakeItem("beamer-camera-sharing")
        self.camera_allow_item = FakeItem("beamer-camera-allow")
        self.status_item = FakeItem("status")
        self.status_item_view = mock.Mock()
        self._refusal_notices = mock.Mock()
        self._last_refusal = ""
        self._camera_line = ""
        import kvm_bridge_app
        for name in ("_camera_menu", "_camera_items", "_drive_status"):
            setattr(self, name, getattr(kvm_bridge_app.TrayApp, name).__get__(self))


class MenuTests(CameraFlag):
    def tray(self, fixture):
        return FakeTray(cf.service_for(fixture))

    def items(self, tray):
        import kvm_bridge_app
        kvm_bridge_app.TrayApp._camera_items(tray)
        return {name: (item.title, item.hidden) for name, item in
                (("line", tray.camera_line_item), ("sharing", tray.camera_sharing_item), ("allow", tray.camera_allow_item))}

    def test_sharing_item_in_both_states(self):
        self.assertEqual(self.items(self.tray(cf.idle()))["sharing"], ("Stop Sharing the Camera", False))
        self.assertEqual(self.items(self.tray(cf.sharing_off()))["sharing"], ("Share the Camera", False))

    def test_allow_only_while_needs_permission(self):
        self.assertTrue(self.items(self.tray(cf.idle()))["allow"][1])
        self.assertTrue(self.items(self.tray(cf.denied()))["allow"][1])
        self.assertEqual(self.items(self.tray(cf.permission_from_pc()))["allow"], ("Allow Camera Access…", False))

    def test_the_in_use_line_and_tooltip(self):
        import kvm_bridge_app
        tray = self.tray(cf.streaming())
        items = self.items(tray)
        self.assertEqual(items["line"], ("Camera: Microsoft Teams on lebig", False))
        kvm_bridge_app.TrayApp._refresh_tray_state(tray)
        tooltip = tray.status_item_view.button().setToolTip_.call_args[0][0]
        self.assertEqual(tooltip, "Idle\nCamera: Microsoft Teams on lebig")
        self.assertTrue(self.items(self.tray(cf.idle()))["line"][1])

    def test_items_call_the_service(self):
        import kvm_bridge_app
        tray = self.tray(cf.idle())
        kvm_bridge_app.TrayApp.toggle_camera_sharing(tray, None)         # never shown: nothing to act on
        self.items(tray)
        kvm_bridge_app.TrayApp.toggle_camera_sharing(tray, None)
        tray.controller.audio._camera = cf.sharing_off().camera
        self.items(tray)
        kvm_bridge_app.TrayApp.toggle_camera_sharing(tray, None)
        kvm_bridge_app.TrayApp.allow_camera_access(tray, None)
        self.assertEqual(tray.controller.audio.calls, [("set_camera_sharing", (False,)), ("set_camera_sharing", (True,)),
                                                       ("ask_camera_permission", ())])

    def test_sharing_item_acts_on_its_label_with_two_quick_clicks(self):
        """R4 #3: the service has not answered yet (its snapshot still says sharing on) when Alex clicks
        twice; each click does what the title then showed, and the title turns at once."""
        import kvm_bridge_app
        tray = self.tray(cf.idle())
        self.assertEqual(self.items(tray)["sharing"][0], "Stop Sharing the Camera")
        kvm_bridge_app.TrayApp.toggle_camera_sharing(tray, None)
        self.assertEqual(tray.camera_sharing_item.title, "Share the Camera")       # at once, no tick
        self.assertEqual(self.items(tray)["sharing"][0], "Share the Camera")       # a tick, stale snapshot
        kvm_bridge_app.TrayApp.toggle_camera_sharing(tray, None)
        self.assertEqual(tray.camera_sharing_item.title, "Stop Sharing the Camera")
        self.assertEqual(tray.controller.audio.calls, [("set_camera_sharing", (False,)), ("set_camera_sharing", (True,))])

    def test_stop_sharing_never_turns_sharing_on(self):
        """R4 #3: the menu showed "Stop Sharing the Camera"; sharing was turned off on the page before the
        click. The click still stops (it never inverts the state read at click time)."""
        import kvm_bridge_app
        tray = self.tray(cf.idle())
        self.items(tray)
        tray.controller.audio._camera = cf.sharing_off().camera
        kvm_bridge_app.TrayApp.toggle_camera_sharing(tray, None)
        self.assertEqual(tray.controller.audio.calls, [("set_camera_sharing", (False,))])

    def test_hidden_without_the_flag_a_service_or_a_camera(self):
        with mock.patch.object(feature_flags, "CAMERA", False):
            self.assertEqual({hidden for _title, hidden in self.items(self.tray(cf.streaming())).values()}, {True})
        tray = self.tray(cf.streaming())
        tray.controller.audio = None
        self.assertEqual({hidden for _title, hidden in self.items(tray).values()}, {True})
        pc = cf.seen_from_pc(cf.streaming())
        self.assertEqual({hidden for _title, hidden in self.items(self.tray(pc)).values()}, {True})

    def test_a_failing_service_hides_the_items(self):
        tray = self.tray(cf.idle())
        tray.controller.audio.camera_snapshot = mock.Mock(side_effect=RuntimeError("boom"))
        with self.assertLogs("test-camera-menu", "ERROR"):
            items = self.items(tray)
        self.assertTrue(items["sharing"][1])


def render(destination):
    """The 8 acceptance states (PHASE2 §12 CW7) as the Mac page shows them, light and dark."""
    from mac_app.tests.settings_layout_offscreen import render as render_view
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    written = []
    with mock.patch.object(feature_flags, "CAMERA", True, create=True):
        from core import i18n
        i18n.install("en")
        for dark in (False, True):
            for name, make in cf.FIXTURES.items():
                fixture = make()
                control, page, _service = build(fixture, dark=dark)
                document = control.pages["audio"].documentView()
                height = document.frame().size.height
                control.window.setFrame_display_(((100000, 100000), (1000, height + 100)), False)
                for _ in range(3):
                    settle(control.window)
                path = destination / f"camera-{name.replace('_', '-')}-{'dark' if dark else 'light'}.png"
                render_view(control.window.contentView(), 1, path)
                written.append(path)
    return written


if __name__ == "__main__":
    if len(sys.argv) > 2 and sys.argv[1] == "render":
        for path in render(sys.argv[2]):
            print(path)
    else:
        unittest.main()
