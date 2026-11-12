# SPDX-License-Identifier: GPL-3.0-only
"""The Qt Camera module of the Audio & Camera page (dev/camera/PHASE2.md §8.3-§8.6, §9.2, §12 CW8), on
the camera fixtures as lebig sees them (core/tests/camera_fixtures.py FIXTURES_PC) and a fake service.

Also renders the acceptance shots: `python win_app/tests/test_camera_section_win.py --render <dir>`
writes each state as this PC's window shows it, light and dark (`camera-<state>-<theme>.png`).
"""

from __future__ import annotations

import importlib
import os
import sys
import threading
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
for path in (ROOT, ROOT / "win_app", TESTS):
    sys.path.insert(0, str(path))

from PySide6.QtCore import QThread, Qt
from PySide6.QtTest import QTest
from PySide6.QtWidgets import QApplication

import audio_page_win
import camera_section_win
import vcam_install
from core import feature_flags, i18n
from core.audio import view
from core.camera import view as camera_view
from core.camera.model import CameraStatus
from core.tests import camera_fixtures as cf
from test_audio_page_win import Host, app, close_window, grab, open_window, settle

PC_STATES = ("off", "idle", "streaming", "permission_from_pc", "sharing_off", "pc_not_installed", "reduced", "denied",
             "restart_needed", "outdated")


class FakeInstaller:
    """win_app/vcam_install.py's API: records each call and keeps its `on_done`."""

    def __init__(self):
        self.calls = []

    def install(self, on_done, helper=None):
        self.calls.append(("install", on_done))

    def remove(self, on_done, helper=None):
        self.calls.append(("remove", on_done))

    def finish(self, result):
        """Calls the last step's on_done from a background thread, as vcam_install does."""
        on_done = self.calls[-1][1]
        thread = threading.Thread(target=on_done, args=(result,))
        thread.start()
        thread.join()


def camera_on(test):
    """The camera feature flag on and this app on Windows, for one test."""
    for patcher in (mock.patch.object(feature_flags, "CAMERA", True, create=True),
                    mock.patch.object(camera_section_win, "camera_platform", lambda: "windows")):
        patcher.start()
        test.addCleanup(patcher.stop)


def make_page(fixture, service=None, installer=None, opened=None, confirm=True):
    service = service or cf.service_for(fixture)
    audio = fixture.audio
    page = audio_page_win.AudioPageWidget(service, own_label=audio.own_label, tiles=lambda: audio.tiles,
                                          peers_without_audio=lambda: audio.peers_without_audio)
    module = page.camera
    module._installer = installer or FakeInstaller()
    module.open_url = (opened.append if opened is not None else (lambda _url: None))
    module.confirm_remove = (lambda: confirm)
    page.selected = audio.selected
    page.refresh()
    return page, service


def pc(name):
    return cf.FIXTURES_PC[name]()


def expected_section(fixture):
    return camera_view.camera_section(fixture.camera, fixture.audio.snapshot, fixture.audio.own_label, "windows")


class SectionFor(unittest.TestCase):
    def setUp(self):
        i18n.install("en")
        self.fixture = pc("idle")
        self.service = cf.service_for(self.fixture)

    def test_nothing_without_the_flag(self):
        with mock.patch.object(feature_flags, "CAMERA", False, create=True), \
                mock.patch.object(camera_section_win, "camera_platform", lambda: "windows"):
            state = camera_section_win.section_for(self.service, self.fixture.audio.snapshot, "This PC")
        self.assertIs(state, camera_section_win.NO_CAMERA)

    def test_nothing_on_linux(self):
        with mock.patch.object(feature_flags, "CAMERA", True, create=True), \
                mock.patch.object(camera_section_win.sys, "platform", "linux"):
            self.assertIsNone(camera_section_win.camera_platform())
            state = camera_section_win.section_for(self.service, self.fixture.audio.snapshot, "This PC")
        self.assertIs(state, camera_section_win.NO_CAMERA)

    def test_windows_is_windows(self):
        with mock.patch.object(camera_section_win.sys, "platform", "win32"):
            self.assertEqual(camera_section_win.camera_platform(), "windows")

    def test_nothing_without_a_camera_part_or_when_it_fails(self):
        camera_on(self)
        bare = audio_page_win  # any object without camera_snapshot
        self.assertIs(camera_section_win.section_for(bare, self.fixture.audio.snapshot, "This PC"),
                      camera_section_win.NO_CAMERA)
        broken = cf.service_for(self.fixture)
        broken.camera_snapshot = mock.Mock(side_effect=RuntimeError("worker gone"))
        with self.assertLogs(camera_section_win.__name__, "ERROR"):
            state = camera_section_win.section_for(broken, self.fixture.audio.snapshot, "This PC")
        self.assertIs(state, camera_section_win.NO_CAMERA)

    def test_the_section_and_extras_of_the_shared_model(self):
        camera_on(self)
        state = camera_section_win.section_for(self.service, self.fixture.audio.snapshot, "This PC")
        self.assertEqual(state.section, expected_section(self.fixture))
        self.assertEqual(state.extras, camera_view.map_extras(self.fixture.camera))
        self.assertIs(state.snapshot, self.fixture.camera)


class Geometry(unittest.TestCase):
    def test_lanes_run_camera_mic_sound_only_with_a_camera_arrow(self):
        # The tile's lines read camera, mic, sound from the top: the lanes follow them.
        arrows = (view.Arrow("a", "b", "sound", "active", ""), view.Arrow("a", "b", "camera", "active", ""),
                  view.Arrow("b", "a", "mic", "active", ""))
        self.assertEqual(camera_section_win.lane_order(arrows, [0, 1, 2]), [1, 2, 0])
        offsets = audio_page_win.arrow_offsets(arrows)
        self.assertLess(offsets[1], offsets[2])
        self.assertLess(offsets[2], offsets[0])
        audio_only = arrows[:1] + arrows[2:]
        self.assertEqual(camera_section_win.lane_order(audio_only, [0, 1]), [0, 1])

    def test_tile_lines_keep_the_audio_rule_without_a_camera(self):
        tile = type("T", (), {"camera_line": "", "watching_line": ""})()
        self.assertEqual(camera_section_win.tile_lines(tile, False, "Mic", True, 0, 73), {"mic": 73})
        self.assertEqual(camera_section_win.tile_lines(tile, False, "Mic", False, 0, 40), {})
        self.assertEqual(camera_section_win.tile_lines(tile, False, "Mic", True, 0, 40), {"mic": 40})

    def test_the_camera_line_sits_above_the_mic_and_wins_when_important(self):
        named = type("T", (), {"camera_line": "MacBook Air Camera", "watching_line": ""})()
        self.assertEqual(camera_section_win.tile_lines(named, False, "Mic", True, 0, 73), {"mic": 73, "camera": 54})
        # Short of room: the shared camera (important) beats the machine's own default mic.
        self.assertEqual(camera_section_win.tile_lines(named, False, "Own mic", False, 0, 40), {"camera": 40})
        ready = type("T", (), {"camera_line": "", "watching_line": "Camera ready"})()
        self.assertEqual(camera_section_win.tile_lines(ready, False, "Hears", True, 0, 40), {"mic": 40})


class Module(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = app()
        i18n.install("en")

    def setUp(self):
        camera_on(self)

    def tearDown(self):
        settle(self.app)

    def test_every_pc_state_shows_its_model(self):
        self.assertEqual(tuple(cf.FIXTURES_PC), PC_STATES)
        for name in PC_STATES:
            fixture = pc(name)
            with self.subTest(name):
                page, _service = make_page(fixture)
                module, section = page.camera, expected_section(fixture)
                self.assertFalse(module.isHidden())
                self.assertEqual(module.section, section)
                self.assertEqual(module.select.text(), view.choice_by_key(section.choices, section.selected).title)
                self.assertEqual(module.line.text(), section.line)
                self.assertEqual(module.line.property("vernier"), audio_page_win.note_role(section.tone))
                self.assertEqual(module.detail.text(), section.detail)
                self.assertEqual(module.detail.isHidden(), not section.detail)
                self.assertEqual(module.hint.text(), section.hint)
                self.assertEqual(module.quality.value, section.quality_selected)
                self.assertEqual(all(b.isEnabled() for b in module.quality._buttons.values()), section.quality_enabled)
                rows = [view_ for view_ in module.row_views if not view_.isHidden()]
                self.assertEqual([view_.value.text() for view_ in rows], [row.value for row in section.this_machine])
                self.assertEqual([view_.button.text() for view_ in rows if not view_.button.isHidden()],
                                 [row.action_label for row in section.this_machine if row.action])
                self.assertEqual(rows[0].heading.text(), "This PC")
                self.assertEqual(module.help_row.isHidden(), not section.help)
                self.assertEqual([widget.notice.code for widget in module.notices],
                                 [notice.code for notice in section.notices])
                page.deleteLater()

    def test_the_pc_rows_say_install_state_and_restart(self):
        rows = {name: expected_section(pc(name)).this_machine[0] for name in PC_STATES}
        self.assertEqual(rows["idle"].action, camera_view.ACTION_REMOVE)
        self.assertIsNone(rows["streaming"].action)          # a stream runs on this PC: no Remove
        self.assertEqual(rows["pc_not_installed"].action, camera_view.ACTION_INSTALL)
        self.assertEqual(rows["outdated"].action, camera_view.ACTION_UPDATE)
        self.assertEqual(rows["restart_needed"].value, "Close apps that use the camera to finish updating Beamer Camera.")
        page, _service = make_page(pc("restart_needed"))
        self.assertEqual(page.camera.row_views[0].value.text(), "Close apps that use the camera to finish updating Beamer Camera.")
        self.assertTrue(page.camera.row_views[0].button.isHidden())

    def test_the_module_sits_under_the_notices_and_above_the_map(self):
        page, _service = make_page(pc("idle"))
        outer = page.layout()
        order = [outer.itemAt(index) for index in range(outer.count())]
        notices = next(index for index, item in enumerate(order) if item.layout() is page.notices_box)
        self.assertIs(order[notices + 1].widget(), page.camera)
        self.assertIs(order[notices + 2].widget(), page.map_module)

    def test_hidden_without_the_flag_on_linux_and_without_a_camera_side(self):
        fixture = pc("streaming")
        with mock.patch.object(feature_flags, "CAMERA", False, create=True):
            page, _service = make_page(fixture)
        self.assertTrue(page.camera.isHidden())
        self.assertFalse(any(arrow.kind == "camera" for arrow in page.map.arrows))
        self.assertTrue(page.camera_legend.isHidden())
        with mock.patch.object(camera_section_win, "camera_platform", lambda: None):
            page, _service = make_page(fixture)
        self.assertTrue(page.camera.isHidden())
        self.assertEqual(page.page, view.page_model(fixture.audio.snapshot, fixture.audio.tiles, fixture.audio.selected,
                                                    fixture.audio.own_label, fixture.audio.peers_without_audio))
        # No machine with a camera or a Beamer Camera (e.g. two PCs): the model hides the module.
        bare = replace(fixture, camera=replace(fixture.camera, machines={}))
        page, _service = make_page(bare)
        self.assertTrue(page.camera.isHidden())

    def test_the_map_carries_the_camera(self):
        page, _service = make_page(pc("streaming"))
        self.assertFalse(page.camera_legend.isHidden())
        camera_arrows = [arrow for arrow in page.map.arrows if arrow.kind == "camera"]
        self.assertEqual([(arrow.source, arrow.target, arrow.state) for arrow in camera_arrows], [(cf.MAC, cf.PC, "active")])
        tiles = {tile.machine: tile for tile in page.map.tiles}
        self.assertEqual(tiles[cf.MAC].camera_line, "MacBook Air Camera")
        self.assertTrue(tiles[cf.MAC].camera_live)
        self.assertEqual(tiles[cf.PC].watching_line, "Microsoft Teams uses the camera")
        self.assertTrue(page.map._watched(cf.PC))
        host = Host(page)
        host.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
        host.show()
        settle(self.app)
        page.map.grab()  # paints the camera lines and the dotted arrow without error
        host.deleteLater()

    def test_camera_choice_and_quality_call_the_service(self):
        page, service = make_page(pc("idle"))
        module = page.camera
        self.assertTrue(module.select.choose("off"))
        self.assertEqual(service.calls[-1], ("set_camera", (None, None)))
        iphone = next(choice for choice in module.select.choices if choice.device == cf.CameraRef(
            cf.IPHONE_CAMERA.key, cf.IPHONE_CAMERA.name))
        self.assertTrue(module.select.choose(iphone.key))
        self.assertEqual(service.calls[-1], ("set_camera", (cf.MAC, iphone.device)))
        header = next(choice for choice in module.select.choices if choice.header)
        self.assertFalse(module.select.choose(header.key))
        module.quality._buttons["1080p"].click()
        self.assertEqual(service.calls[-1], ("set_quality", ("1080p",)))

    def test_quality_is_disabled_while_off(self):
        page, _service = make_page(pc("off"))
        self.assertFalse(any(button.isEnabled() for button in page.camera.quality._buttons.values()))

    def test_install_runs_vcam_install_and_relays_its_result_to_the_qt_thread(self):
        installer = FakeInstaller()
        page, service = make_page(pc("pc_not_installed"), installer=installer)
        module = page.camera
        threads = []
        module.done_hook = lambda kind, result: threads.append((kind, result, QThread.currentThread()))
        button = module.row_views[0].button
        self.assertEqual(button.text(), "Install Beamer Camera…")
        button.click()
        self.assertEqual([call[0] for call in installer.calls], ["install"])
        self.assertFalse(button.isEnabled())
        self.assertEqual(module.row_views[0].note.text(), "Waiting for Windows to install Beamer Camera…")
        # UAC refused: the result comes from vcam_install's thread, the row says so on the Qt thread.
        installer.finish(vcam_install.Result(False, "cancelled"))
        self.assertEqual(threads, [])                      # nothing ran on the background thread
        settle(self.app)
        self.assertEqual(len(threads), 1)
        self.assertIs(threads[0][2], self.app.thread())
        self.assertEqual(module.row_views[0].note.text(), "Installation was cancelled.")
        self.assertFalse(module.row_views[0].note.isHidden())
        self.assertTrue(button.isEnabled())
        self.assertNotIn(("refresh_camera_install", ()), service.calls)
        # A snapshot that changes nothing keeps the note.
        page.refresh()
        self.assertEqual(module.row_views[0].note.text(), "Installation was cancelled.")
        # Accepted: the helper re-reads the install state.
        button.click()
        installer.finish(vcam_install.Result(True, "ok"))
        settle(self.app)
        self.assertIn(("refresh_camera_install", ()), service.calls)
        self.assertTrue(module.row_views[0].note.isHidden())
        self.assertIs(threads[-1][2], self.app.thread())

    def test_a_new_install_state_clears_the_note(self):
        installer = FakeInstaller()
        page, service = make_page(pc("pc_not_installed"), installer=installer)
        page.camera.row_views[0].button.click()
        installer.finish(vcam_install.Result(False, "failed"))
        settle(self.app)
        self.assertIn("could not be installed", page.camera.row_views[0].note.text())
        service.push_camera(pc("idle").camera)
        settle(self.app)
        self.assertEqual(page.camera.row_views[0].value.text(), "Installed")
        self.assertTrue(page.camera.row_views[0].note.isHidden())

    def test_update_and_repair_use_the_same_call(self):
        for name, label in (("outdated", "Update Beamer Camera…"),):
            installer = FakeInstaller()
            page, _service = make_page(pc(name), installer=installer)
            self.assertEqual(page.camera.row_views[0].button.text(), label)
            page.camera.row_views[0].button.click()
            self.assertEqual([call[0] for call in installer.calls], ["install"])
        installer = FakeInstaller()
        page, _service = make_page(pc("idle"), installer=installer)
        page.camera.act(camera_view.ACTION_REPAIR)
        self.assertEqual([call[0] for call in installer.calls], ["install"])
        installer.finish(vcam_install.Result(False, "no_helper"))
        settle(self.app)

    def test_remove_asks_first_then_runs_vcam_install_remove(self):
        installer = FakeInstaller()
        page, service = make_page(pc("idle"), installer=installer, confirm=False)
        button = page.camera.row_views[0].button
        self.assertEqual(button.text(), "Remove…")
        button.click()
        self.assertEqual(installer.calls, [])
        page.camera.confirm_remove = lambda: True
        button.click()
        self.assertEqual([call[0] for call in installer.calls], ["remove"])
        installer.finish(vcam_install.Result(False, "cancelled"))
        settle(self.app)
        self.assertEqual(page.camera.row_views[0].note.text(), "Removal was cancelled.")
        button.click()
        installer.finish(vcam_install.Result(True, "ok"))
        settle(self.app)
        self.assertIn(("refresh_camera_install", ()), service.calls)

    def test_an_installer_that_raises_does_not_leave_the_row_waiting(self):
        installer = mock.Mock()
        installer.install.side_effect = OSError("no shell")
        page, _service = make_page(pc("pc_not_installed"), installer=installer)
        with self.assertLogs(camera_section_win.__name__, "ERROR"):
            page.camera.row_views[0].button.click()
        self.assertIsNone(page.camera.running)
        self.assertTrue(page.camera.row_views[0].button.isEnabled())
        self.assertIn("could not be installed", page.camera.row_views[0].note.text())

    def test_the_help_line_opens_camera_settings(self):
        opened = []
        page, service = make_page(pc("idle"), opened=opened)
        module = page.camera
        self.assertFalse(module.help_row.isHidden())
        self.assertTrue(module.help.text().startswith("One app at a time can use Beamer Camera"))
        self.assertEqual(module.help_button.text(), "Camera settings")
        module.help_button.click()
        self.assertEqual(opened, ["ms-settings:camera"])
        self.assertEqual([call for call in service.calls if call[0] != "watch"], [])
        # Not installed: no help line.
        page, _service = make_page(pc("pc_not_installed"))
        self.assertTrue(page.camera.help_row.isHidden())

    def test_retry_on_an_error_sets_the_route_again(self):
        fixture = pc("idle")
        fixture = replace(fixture, camera=replace(fixture.camera, status=CameraStatus("error", "no_receiver",
                                                                                      {"machine": cf.MAC, "to": cf.PC})))
        page, service = make_page(fixture)
        module = page.camera
        self.assertFalse(module.line_button.isHidden())
        self.assertEqual(module.line_button.text(), "Retry")
        self.assertEqual(module.line.property("vernier"), "note-fault")
        module.line_button.click()
        self.assertEqual(service.calls[-1], ("set_camera", (cf.MAC, None)))

    def test_a_camera_change_from_the_worker_updates_the_module_on_the_qt_thread(self):
        fixture = pc("idle")
        page, service = make_page(fixture)
        streaming = pc("streaming")
        thread = threading.Thread(target=service.push_camera, args=(streaming.camera,))
        thread.start()
        thread.join()
        wanted = expected_section(replace(streaming, audio=fixture.audio)).line
        self.assertNotEqual(page.camera.line.text(), wanted)   # nothing touched the widgets off the Qt thread
        settle(self.app)
        self.assertEqual(page.camera.line.text(), wanted)

    def test_the_camera_menu_defers_a_refresh(self):
        page, service = make_page(pc("idle"))
        with mock.patch.object(page.camera.select, "menu_open", lambda: True):
            service.push_camera(pc("off").camera)
            settle(self.app)
            self.assertEqual(page.camera.section, expected_section(pc("idle")))
        page._menu_closed()
        self.assertEqual(page.camera.section.state, "off")

    def test_keyboard_reaches_every_control(self):
        for name in ("idle", "pc_not_installed"):
            fixture = pc(name)
            fixture = replace(fixture, camera=replace(fixture.camera, status=CameraStatus(
                "error", "helper", {"machine": cf.MAC})))
            with self.subTest(name):
                page, _service = make_page(fixture)
                host = Host(page)
                host.setAttribute(Qt.WidgetAttribute.WA_DontShowOnScreen, True)
                host.show()
                settle(self.app)
                expected = set(page.camera.controls())
                self.assertIn(page.camera.select, expected)
                self.assertIn(page.camera.line_button, expected)
                self.assertIn(page.camera.row_views[0].button, expected)
                if name == "idle":
                    self.assertIn(page.camera.help_button, expected)
                host.activateWindow()
                QApplication.setActiveWindow(host)
                page.mic_select.setFocus(Qt.FocusReason.TabFocusReason)
                settle(self.app)
                reached = set()
                for _ in range(60):
                    focused = QApplication.focusWidget()
                    reached.add(focused)
                    QTest.keyClick(focused, Qt.Key.Key_Tab)
                    settle(self.app, 1)
                # Tab enters the Quality choice at its checked cell (Qt's exclusive group); the arrow
                # keys reach the other cell, as in every widgets.Choice of the app.
                cells = page.camera.quality._buttons
                unchecked = {button for button in cells.values() if not button.isChecked()}
                self.assertEqual(expected - reached, unchecked)
                checked = next(button for button in cells.values() if button.isChecked())
                checked.setFocus(Qt.FocusReason.TabFocusReason)
                QTest.keyClick(checked, Qt.Key.Key_Right)
                settle(self.app, 1)
                self.assertIn(QApplication.focusWidget(), unchecked)
                host.deleteLater()


class InTheWindow(unittest.TestCase):
    """The renamed page in the settings window: same key, after Crossing, opened with Ctrl+3."""

    @classmethod
    def setUpClass(cls):
        cls.app = app()
        i18n.install("en")

    def setUp(self):
        import pages_win
        camera_on(self)
        with mock.patch.object(sys, "platform", "win32"):
            importlib.reload(pages_win)
        self.addCleanup(importlib.reload, pages_win)
        self.pages_win = pages_win
        self.window, self.folder = open_window()

    def tearDown(self):
        close_window(self.window, self.folder)

    def test_audio_and_camera_opens_with_ctrl_3(self):
        from PySide6.QtGui import QKeySequence, QShortcut
        keys = list(self.pages_win.KEYS)
        self.assertEqual(keys.index("audio"), keys.index("crossing") + 1)
        self.assertEqual(keys.index("audio") + 1, 3)
        page = dict((key, name) for key, name, _purpose in self.pages_win.PAGES)
        self.assertEqual(page["audio"], "Audio & Camera")
        self.window._select_page("overview")
        shortcuts = [item for item in self.window.findChildren(QShortcut) if item.key() == QKeySequence("Ctrl+3")]
        self.assertEqual(len(shortcuts), 1)
        shortcuts[0].activated.emit()  # an offscreen window is never active, so no real key press
        settle(self.app)
        self.assertEqual(self.window._page, "audio")
        document = self.window.stack.currentWidget().widget()
        headings = [label.text() for label in document.findChildren(type(self.window.footer_note))]
        self.assertIn("AUDIO & CAMERA", headings)
        self.assertIs(document.findChild(audio_page_win.AudioPageWidget), self.window.audio_page)
        self.assertIsInstance(self.window.audio_page.camera, camera_section_win.CameraModule)

    def test_the_page_keeps_its_name_without_the_camera(self):
        with mock.patch.object(feature_flags, "CAMERA", False, create=True):
            importlib.reload(self.pages_win)
        self.assertEqual(dict((key, name) for key, name, _ in self.pages_win.PAGES)["audio"], "Audio")
        with mock.patch.object(sys, "platform", "linux"):
            importlib.reload(self.pages_win)
        self.assertEqual(dict((key, name) for key, name, _ in self.pages_win.PAGES)["audio"], "Audio")


# -- renders ----------------------------------------------------------------------------------------

def render_window(out: Path, names=None):
    """The acceptance shots (§12 CW8): each FIXTURES_PC state as this PC's window shows it, light and
    dark, with the camera feature on."""
    import pages_win
    from test_audio_page_win import _fit_and_grab, _window_theme
    feature_flags.CAMERA = True
    camera_section_win.camera_platform = lambda: "windows"
    real_platform = sys.platform
    sys.platform = "win32"
    try:
        importlib.reload(pages_win)
    finally:
        sys.platform = real_platform
    application = app()
    i18n.install("en")
    out.mkdir(parents=True, exist_ok=True)
    window, folder = open_window()
    try:
        page = window.audio_page
        page.camera.open_url = lambda _url: None
        for name in PC_STATES:
            if names and name not in names:
                continue
            fixture = pc(name)
            audio = fixture.audio
            page._tiles = lambda tiles=audio.tiles: tiles
            page.selected = audio.selected
            page.set_service(cf.service_for(fixture))
            page.apply_levels(audio.levels)
            shots = [name]
            if name == "pc_not_installed":
                shots.append("install_cancelled")
            for shot in shots:
                if shot == "install_cancelled":
                    # The UAC prompt refused (§8.6): the row's note until the install state changes.
                    page.camera._installer = FakeInstaller()
                    page.camera.act(camera_view.ACTION_INSTALL)
                    page.camera._finished("install", vcam_install.Result(False, "cancelled"))
                for dark in (False, True):
                    _window_theme(window, dark)
                    path = out / f"camera-{shot.replace('_', '-')}-{'dark' if dark else 'light'}.png"
                    _fit_and_grab(application, window, "audio").save(str(path))
                    print(path)
            page.set_service(None)
    finally:
        close_window(window, folder)


if __name__ == "__main__":
    if "--render" in sys.argv:
        at = sys.argv.index("--render")
        render_window(Path(sys.argv[at + 1]), sys.argv[at + 2:])
        sys.exit(0)
    unittest.main()
