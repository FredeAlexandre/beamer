# SPDX-License-Identifier: GPL-3.0-only
"""The Camera module's model (core/camera/view.py, dev/camera/PHASE2.md §8.2-§8.6, §10.1): every
status line of §8.4 seen from the Mac and from the PC, the selector, quality, hint, help line,
this machine's rows, notices, map extras, the Mac menu's camera items and the apply helpers."""

from __future__ import annotations

import unittest
from dataclasses import replace

from core import i18n
from core.audio import view as audio_view
from core.camera import view
from core.camera.model import CameraRef, CameraStatus, LocalVcam
from core.tests import audio_fixtures as fx
from core.tests import camera_fixtures as cf
from core.tests.camera_fixtures import MAC, PC, ROG


def mac_section(fixture):
    return view.camera_section(fixture.camera, fixture.audio.snapshot, "This Mac", "macos")


def pc_section(fixture):
    return view.camera_section(fixture.camera, fixture.audio.snapshot, "This PC", "windows")


def with_status(fixture, state, reason=None, to=PC, **params):
    status = cf.status(state, to, reason)
    status = replace(status, params={**status.params, **params})
    sinks = dict(fixture.camera.sinks)
    if to:
        sinks[to] = replace(status)
    return replace(fixture, camera=replace(fixture.camera, status=status, sinks=sinks))


class StatusLinesFromTheMac(unittest.TestCase):
    """§8.4, as this Mac (the camera machine) shows them."""

    def setUp(self):
        i18n.install("en")

    def test_the_eight_acceptance_states(self):
        expected = {
            "off": ("Off. Apps that open Beamer Camera see a 'camera off' picture.", view.QUIET),
            "idle": ("Ready. No app on lebig is using Beamer Camera.", view.PLAIN),
            "streaming": ("Microsoft Teams on lebig is using it.", view.LIVE),
            "permission_from_pc": ("Allow camera access on this Mac.", view.WAIT),
            "sharing_off": ("Camera sharing is off on this Mac.", view.QUIET),
            "pc_not_installed": ("Beamer Camera is not installed on lebig.", view.WAIT),
            "reduced": ("Microsoft Teams on lebig is using it. Reduced quality: the network is slow.", view.WAIT),
            "denied": ("This Mac does not allow Beamer to use its camera.", view.FAULT),
        }
        self.assertEqual(set(expected), set(cf.FIXTURES))
        for name, make in cf.FIXTURES.items():
            with self.subTest(name):
                section = mac_section(make())
                self.assertEqual((section.line, section.tone), expected[name])
                self.assertTrue(section.shown)
                self.assertEqual(section.state, make().camera.status.state)

    def test_detail_while_streaming_only(self):
        self.assertEqual(mac_section(cf.streaming()).detail, "1280x720 · 30 fps · 72 ms")
        self.assertEqual(mac_section(cf.reduced()).detail, "1280x720 · 15 fps · 180 ms · through the Beamer link")
        for name in ("off", "idle", "denied", "sharing_off"):
            self.assertEqual(mac_section(cf.FIXTURES[name]()).detail, "", name)

    def test_an_app_without_a_name_and_several_sinks(self):
        fixture = cf.streaming()
        camera = fixture.camera
        machines = dict(camera.machines)
        machines[PC] = cf.pc_camera(in_use=True, app=None)
        self.assertEqual(mac_section(replace(fixture, camera=replace(camera, machines=machines))).line,
                         "An app on lebig is using it.")
        three = cf.three_machines()
        machines = dict(three.camera.machines)
        machines[ROG] = cf.pc_camera(in_use=True, app="Zoom", ident=ROG)
        status = cf.status("streaming", f"{PC} {ROG}")
        sinks = {**three.camera.sinks, ROG: cf.status("streaming", ROG)}
        snap = replace(three.camera, machines=machines, status=status, sinks=sinks)
        self.assertEqual(mac_section(replace(three, camera=snap)).line,
                         "Microsoft Teams on lebig and Zoom on asusrog are using it.")

    def test_starting(self):
        fixture = with_status(cf.streaming(), "starting")
        self.assertEqual(mac_section(fixture).line, "Starting for Microsoft Teams on lebig…")
        self.assertEqual(mac_section(fixture).tone, view.LIVE)

    def test_every_other_state(self):
        base = cf.idle()
        lines = {
            ("waiting_device", None): "MacBook Air Camera is not connected to this Mac.",
            ("no_camera", None): "This Mac has no camera.",
            ("unreachable", None): "This Mac and lebig are not linked directly.",
            ("no_direct_path", None): "Video needs a direct network path between this Mac and lebig.",
            ("outdated", None): "Beamer Camera needs an update on lebig.",
            ("broken", None): "Beamer Camera is damaged on lebig.",
            ("error", "camera_gone"): "The camera stopped on this Mac: the camera is not available.",
            ("error", "mystery"): "The camera stopped on this Mac.",
        }
        for (state, reason), line in lines.items():
            with self.subTest(state):
                section = mac_section(with_status(base, state, reason))
                self.assertEqual(section.line, line)
                self.assertEqual(section.tone, view.TONES[state])

    def test_error_carries_retry(self):
        section = mac_section(with_status(cf.idle(), "error", "no_receiver"))
        self.assertEqual((section.action, section.action_label), (view.ACTION_RETRY, "Retry"))
        section = mac_section(with_status(cf.idle(), "error", "helper"))
        self.assertEqual(section.action, view.ACTION_RETRY_HELPER)
        self.assertIsNone(mac_section(cf.streaming()).action)

    def test_waiting_device_names_the_unplugged_camera(self):
        fixture = cf.idle()
        snap = replace(fixture.camera, route=cf.route(device=CameraRef("gone", "Studio Display Camera")))
        fixture = with_status(replace(fixture, camera=snap), "waiting_device", device="Studio Display Camera")
        self.assertEqual(mac_section(fixture).line, "Studio Display Camera is not connected to this Mac.")


class StatusLinesFromThePC(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_the_acceptance_states_seen_from_lebig(self):
        expected = {
            "off": "Off. Apps that open Beamer Camera see a 'camera off' picture.",
            "idle": "Ready. No app on this PC is using Beamer Camera.",
            "streaming": "Microsoft Teams on this PC is using it.",
            "permission_from_pc": "Allow camera access on MacBook Air.",
            "sharing_off": "Turned off on MacBook Air.",
            "pc_not_installed": "Beamer Camera is not installed on this PC.",
            "reduced": "Microsoft Teams on this PC is using it. Reduced quality: the network is slow.",
            "denied": "MacBook Air does not allow Beamer to use its camera.",
            "restart_needed": "Ready. No app on this PC is using Beamer Camera.",
            "outdated": "Beamer Camera needs an update on this PC.",
        }
        self.assertEqual(set(expected), set(cf.FIXTURES_PC))
        for name, make in cf.FIXTURES_PC.items():
            with self.subTest(name):
                self.assertEqual(pc_section(make()).line, expected[name])

    def test_other_states_name_the_mac(self):
        base = cf.seen_from_pc(cf.idle())
        lines = {
            ("waiting_machine", None): "MacBook Air is offline.",
            ("no_camera", None): "MacBook Air has no camera.",
            ("unreachable", None): "MacBook Air and this PC are not linked directly.",
            ("error", "decoder"): "The camera stopped on MacBook Air: the picture could not be shown.",
        }
        for (state, reason), line in lines.items():
            with self.subTest(state):
                self.assertEqual(pc_section(with_status(base, state, reason)).line, line)
        # Retry from the PC sets the same route again; a helper Retry is only for this machine's.
        self.assertEqual(pc_section(with_status(base, "error", "helper")).action, view.ACTION_RETRY)


class SelectorAndQuality(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_choices_list_cameras_under_the_machine_that_has_them(self):
        section = mac_section(cf.idle())
        labels = [(choice.label, choice.header, choice.enabled) for choice in section.choices]
        self.assertEqual(labels, [("Off", False, True), ("This Mac", True, False),
                                  ("Default camera (MacBook Air Camera)", False, True),
                                  ("MacBook Air Camera", False, True), ("iPhone Camera", False, True)])
        self.assertEqual(section.selected, audio_view._choice_key("cam", MAC, ""))
        chosen = audio_view.choice_by_key(section.choices, section.selected)
        self.assertEqual(chosen.title, "This Mac · Default camera (MacBook Air Camera)")
        # The PC has no camera: no header for it, from either side.
        pc = pc_section(cf.seen_from_pc(cf.idle()))
        self.assertEqual([choice.label for choice in pc.choices if choice.header], ["MacBook Air"])

    def test_off_and_a_named_camera(self):
        self.assertEqual(mac_section(cf.off()).selected, view.OFF)
        fixture = cf.idle()
        ref = CameraRef(cf.IPHONE_CAMERA.key, cf.IPHONE_CAMERA.name)
        fixture = replace(fixture, camera=replace(fixture.camera, route=cf.route(device=ref)))
        self.assertEqual(mac_section(fixture).selected, audio_view._choice_key("cam", MAC, cf.IPHONE_CAMERA.key))

    def test_an_unplugged_camera_stays_selected(self):
        fixture = cf.idle()
        ref = CameraRef("gone", "Studio Display Camera")
        fixture = replace(fixture, camera=replace(fixture.camera, route=cf.route(device=ref)))
        section = mac_section(fixture)
        chosen = audio_view.choice_by_key(section.choices, section.selected)
        self.assertEqual((chosen.label, chosen.enabled), ("Studio Display Camera (not connected)", False))

    def test_a_route_to_a_machine_without_a_list(self):
        fixture = cf.idle()
        machines = {PC: fixture.camera.machines[PC]}
        snap = replace(fixture.camera, own=PC, machines=machines)
        section = view.camera_section(snap, fx.seen_from(fixture.audio, PC).snapshot, "This PC", "windows")
        chosen = audio_view.choice_by_key(section.choices, section.selected)
        self.assertEqual((chosen.machine, chosen.label, chosen.enabled), (MAC, "Default camera", False))

    def test_offline_machine_cameras_are_listed_but_not_offered(self):
        fixture = cf.idle()
        audio = fx.snapshot([fx.mac(links=()), fx.pc(links=())], own=PC, direct=())
        section = view.camera_section(replace(fixture.camera, own=PC), audio, "This PC", "windows")
        self.assertFalse(any(choice.enabled for choice in section.choices if choice.machine == MAC))

    def test_quality(self):
        section = mac_section(cf.idle())
        self.assertEqual(section.quality_label, "Quality")
        self.assertEqual([choice.key for choice in section.quality_choices], ["720p", "1080p"])
        self.assertEqual(section.quality_selected, "720p")
        self.assertTrue(section.quality_enabled)
        self.assertFalse(mac_section(cf.off()).quality_enabled)
        fixture = cf.idle()
        fixture = replace(fixture, camera=replace(fixture.camera, route=cf.route(quality="1080p")))
        self.assertEqual(mac_section(fixture).quality_selected, "1080p")

    def test_hint_and_help(self):
        mac = mac_section(cf.idle())
        self.assertEqual(mac.hint, "Apps on a PC choose 'Beamer Camera'. Macs cannot show another machine's camera yet.")
        self.assertEqual((mac.help, mac.help_action), ("", None))
        pc = pc_section(cf.seen_from_pc(cf.idle()))
        self.assertTrue(pc.help.startswith("One app at a time can use Beamer Camera unless Windows allows several"))
        self.assertEqual((pc.help_action, pc.help_action_label), (view.ACTION_CAMERA_SETTINGS, "Camera settings"))
        # Only while Beamer Camera is installed.
        self.assertEqual(pc_section(cf.FIXTURES_PC["pc_not_installed"]()).help, "")

    def test_hidden_without_any_camera_side(self):
        fixture = cf.off()
        snap = replace(fixture.camera, machines={})
        self.assertFalse(mac_section(replace(fixture, camera=snap)).shown)
        two_pcs = {PC: cf.pc_camera(), ROG: replace(cf.pc_camera(ident=ROG), features=frozenset())}
        self.assertTrue(mac_section(replace(fixture, camera=replace(snap, machines=two_pcs))).shown)


class ThisMachine(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def rows(self, section):
        return [tuple(row) for row in section.this_machine]

    def test_mac_rows(self):
        section = mac_section(cf.idle())
        self.assertEqual(section.this_machine_label, "This Mac")
        self.assertEqual(self.rows(section), [
            ("Camera sharing", "On", view.PLAIN, view.ACTION_STOP_SHARING, "Stop Sharing"),
            ("Camera access", "Allowed", view.PLAIN, None, "")])
        self.assertEqual(self.rows(mac_section(cf.sharing_off()))[0],
                         ("Camera sharing", "Off", view.QUIET, view.ACTION_SHARE, "Share the Camera"))
        self.assertEqual(self.rows(mac_section(cf.denied()))[1],
                         ("Camera access", "Not allowed", view.FAULT, view.ACTION_PRIVACY, "Open Privacy Settings"))
        # Undetermined: the notice has the button while the route names this Mac, else the row.
        self.assertEqual(self.rows(mac_section(cf.permission_from_pc()))[1], ("Camera access", "Not asked yet", view.WAIT, None, ""))
        fixture = cf.off()
        machines = {**fixture.camera.machines, MAC: cf.mac_camera(permission="undetermined")}
        snap = replace(fixture.camera, machines=machines, permission="undetermined")
        self.assertEqual(self.rows(mac_section(replace(fixture, camera=snap)))[1],
                         ("Camera access", "Not asked yet", view.PLAIN, view.ACTION_ALLOW, "Allow Camera Access…"))

    def test_the_test_source_says_so(self):
        fixture = cf.idle()
        machines = {**fixture.camera.machines, MAC: cf.mac_camera(test_source="synth")}
        section = mac_section(replace(fixture, camera=replace(fixture.camera, machines=machines)))
        self.assertEqual(section.this_machine[-1].label, "Test source")
        self.assertEqual(view.map_extras(replace(fixture.camera, machines=machines)).source_line, "Test source")

    def test_pc_rows(self):
        rows = {name: self.rows(pc_section(make())) for name, make in cf.FIXTURES_PC.items()}
        self.assertEqual(rows["idle"], [("Beamer Camera", "Installed", view.PLAIN, view.ACTION_REMOVE, "Remove…")])
        # No install or removal while a video stream runs on this PC (§8.6).
        self.assertEqual(rows["streaming"], [("Beamer Camera", "Installed", view.PLAIN, None, "")])
        self.assertEqual(rows["pc_not_installed"],
                         [("Beamer Camera", "Not installed", view.WAIT, view.ACTION_INSTALL, "Install Beamer Camera…")])
        self.assertEqual(rows["outdated"],
                         [("Beamer Camera", "Needs an update", view.WAIT, view.ACTION_UPDATE, "Update Beamer Camera…")])
        self.assertEqual(rows["restart_needed"],
                         [("Beamer Camera", "Close apps that use the camera to finish updating Beamer Camera.", view.WAIT, None, "")])
        broken = cf.seen_from_pc(cf.pc_not_installed())
        machines = {**broken.camera.machines, PC: cf.pc_camera(install="broken")}
        broken = replace(broken, camera=replace(broken.camera, machines=machines, vcam_install_offer="repair"))
        self.assertEqual(self.rows(pc_section(broken)),
                         [("Beamer Camera", "Damaged", view.WAIT, view.ACTION_REPAIR, "Repair Beamer Camera…")])
        # A source that never reported its version (0) is not an update in waiting.
        idle = cf.seen_from_pc(cf.idle(), local=LocalVcam(0, 2, 2))
        self.assertEqual(self.rows(pc_section(idle))[0][1], "Installed")


class Notices(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_permission_notice_on_the_mac_only(self):
        notices = mac_section(cf.permission_from_pc()).notices
        self.assertEqual([(notice.code, notice.text, notice.tone, notice.action, notice.action_label) for notice in notices],
                         [("camera_permission", "Beamer needs camera access to share this Mac's camera.", view.WAIT,
                           view.ACTION_ALLOW, "Allow Camera Access…")])
        self.assertEqual(pc_section(cf.FIXTURES_PC["permission_from_pc"]()).notices, ())

    def test_a_pc_without_beamer_camera_is_named(self):
        notices = mac_section(cf.pc_not_installed()).notices
        self.assertEqual([notice.text for notice in notices],
                         ["lebig: Beamer Camera is not installed there. Install it in Beamer on lebig."])
        # Not while the camera is off, and not on that PC's own page (its row says it).
        fixture = cf.pc_not_installed()
        off = replace(fixture.camera, route=cf.route(None), status=CameraStatus("off"))
        self.assertEqual(mac_section(replace(fixture, camera=off)).notices, ())
        self.assertEqual(pc_section(cf.FIXTURES_PC["pc_not_installed"]()).notices, ())
        # asusrog is online through lebig: it is named too, from the Mac.
        three = cf.three_machines()
        self.assertEqual([notice.machine for notice in mac_section(three).notices], [ROG])

    def test_no_notices_in_the_ordinary_states(self):
        for name in ("off", "idle", "streaming", "sharing_off", "denied", "reduced"):
            self.assertEqual(mac_section(cf.FIXTURES[name]()).notices, (), name)


class MapExtras(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_extras(self):
        self.assertEqual(view.map_extras(cf.off().camera), view.CameraMapExtras(None, "", {}, ()))
        idle = view.map_extras(cf.idle().camera)
        self.assertEqual((idle.source, idle.source_line, dict(idle.watching), idle.arrows, idle.live),
                         (MAC, "MacBook Air Camera", {PC: "Camera ready"}, (), False))
        live = view.map_extras(cf.streaming().camera)
        self.assertEqual((dict(live.watching), live.arrows, live.live),
                         ({PC: "Microsoft Teams uses the camera"}, ((MAC, PC, "active"),), True))
        waiting = view.map_extras(cf.permission_from_pc().camera)
        self.assertEqual((waiting.arrows, waiting.live), (((MAC, PC, "waiting"),), False))
        self.assertEqual(view.map_extras(cf.reduced().camera).arrows, ((MAC, PC, "active"),))
        self.assertEqual(dict(view.map_extras(cf.pc_not_installed().camera).watching), {})

    def test_page_model_carries_the_extras(self):
        fixture = cf.streaming()
        audio = fixture.audio
        page = audio_view.page_model(audio.snapshot, audio.tiles, audio.selected, "This Mac",
                                     camera=view.map_extras(fixture.camera))
        tiles = {tile.machine: tile for tile in page.tiles}
        self.assertEqual((tiles[MAC].camera_line, tiles[MAC].has_active_camera, tiles[MAC].camera_live),
                         ("MacBook Air Camera", True, True))
        self.assertEqual(tiles[PC].watching_line, "Microsoft Teams uses the camera")
        self.assertIn("Its MacBook Air Camera is the shared camera.", tiles[MAC].description)
        cameras = [arrow for arrow in page.arrows if arrow.kind == "camera"]
        self.assertEqual([(arrow.source, arrow.target, arrow.state) for arrow in cameras], [(MAC, PC, "active")])
        self.assertEqual(cameras[0].description, "lebig sees the camera of This Mac.")
        # Without extras the page is what it always was.
        plain = audio_view.page_model(audio.snapshot, audio.tiles, audio.selected, "This Mac")
        self.assertEqual(plain, audio_view.page_model(audio.snapshot, audio.tiles, audio.selected, "This Mac", camera=None))
        self.assertFalse(any(arrow.kind == "camera" for arrow in plain.arrows))
        self.assertEqual([tile.camera_line for tile in plain.tiles], ["", ""])

    def test_arrows_only_between_tiles_on_the_map(self):
        fixture = cf.streaming()
        extras = replace(view.map_extras(fixture.camera), arrows=((MAC, "elsewhere", "active"),))
        page = audio_view.page_model(fixture.audio.snapshot, fixture.audio.tiles, None, "This Mac", camera=extras)
        self.assertFalse(any(arrow.kind == "camera" for arrow in page.arrows))


class MacMenu(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def menu(self, fixture):
        return view.menu_model(fixture.camera, fixture.audio.snapshot, "This Mac")

    def test_items(self):
        idle = self.menu(cf.idle())
        self.assertEqual((idle.shown, idle.line, idle.sharing_title, idle.sharing_on, idle.allow_shown),
                         (True, "", "Stop Sharing the Camera", True, False))
        off = self.menu(cf.sharing_off())
        self.assertEqual((off.sharing_title, off.sharing_on), ("Share the Camera", False))
        asked = self.menu(cf.permission_from_pc())
        self.assertEqual((asked.allow_shown, asked.allow_title), (True, "Allow Camera Access…"))
        self.assertEqual(self.menu(cf.streaming()).line, "Camera: Microsoft Teams on lebig")
        fixture = cf.streaming()
        machines = {**fixture.camera.machines, PC: cf.pc_camera(in_use=True)}
        self.assertEqual(self.menu(replace(fixture, camera=replace(fixture.camera, machines=machines))).line,
                         "Camera: in use on lebig")

    def test_hidden_on_a_machine_without_a_camera(self):
        fixture = cf.seen_from_pc(cf.streaming())
        self.assertFalse(view.menu_model(fixture.camera, fixture.audio.snapshot, "This PC").shown)

    def test_sharing_item_acts_on_its_label(self):
        """R4 #3: the click sends what the shown title promised; the title turns at once and does not
        come back while a stale snapshot still says the old state."""
        on, off = self.menu(cf.idle()), self.menu(cf.sharing_off())
        item = view.SharingItem()
        self.assertIsNone(item.click(0.0))                          # nothing shown yet: nothing sent
        self.assertEqual(item.show(on, 0.0), "Stop Sharing the Camera")
        # Two quick clicks while the worker has not answered (every snapshot still says "on").
        self.assertEqual(item.click(0.1), view.ACTION_STOP_SHARING)
        self.assertEqual(item.title(), "Share the Camera")
        self.assertEqual(item.show(on, 0.4), "Share the Camera")    # a status tick with the old snapshot
        self.assertEqual(item.click(0.5), view.ACTION_SHARE)        # what that label said
        self.assertEqual(item.title(), "Stop Sharing the Camera")
        # Once the snapshot agrees, or after the hold, the snapshot decides again.
        self.assertEqual(item.show(on, 0.6), "Stop Sharing the Camera")
        self.assertEqual(item.show(off, 0.7), "Share the Camera")
        # The state changed elsewhere (the page) while the menu showed "Stop Sharing": a click stops.
        item = view.SharingItem()
        item.show(on, 0.0)
        self.assertEqual(item.click(0.1), view.ACTION_STOP_SHARING)
        item = view.SharingItem()
        item.show(on, 0.0)
        item.click(0.1)
        self.assertEqual(item.show(on, 0.1 + view.SharingItem.HOLD), "Stop Sharing the Camera")

    def test_store_notices(self):
        fixture = cf.idle()
        unreadable = replace(fixture, camera=replace(fixture.camera, store_problem="unreadable",
                                                     machines={**fixture.camera.machines,
                                                               MAC: cf.mac_camera(sharing=False)}))
        codes = [notice.code for notice in mac_section(unreadable).notices]
        self.assertIn("camera_store_unreadable", codes)
        failed = replace(fixture, camera=replace(fixture.camera, store_problem="save_failed"))
        notice = [n for n in mac_section(failed).notices if n.code == "camera_store_save_failed"][0]
        self.assertEqual(notice.tone, view.FAULT)
        self.assertNotIn("camera_store_unreadable", [n.code for n in pc_section(cf.seen_from_pc(unreadable)).notices])


class Actions(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_apply_helpers_call_the_service(self):
        fixture = cf.idle()
        service = cf.service_for(fixture)
        section = mac_section(fixture)
        for choice in section.choices:
            view.apply_camera(service, choice)
        view.apply_quality(service, "1080p")
        view.apply_quality(service, "4k")
        air = CameraRef(cf.AIR_CAMERA.key, cf.AIR_CAMERA.name)
        iphone = CameraRef(cf.IPHONE_CAMERA.key, cf.IPHONE_CAMERA.name)
        self.assertEqual(service.calls, [
            ("set_camera", (None, None)), ("set_camera", (MAC, None)), ("set_camera", (MAC, air)),
            ("set_camera", (MAC, iphone)), ("set_quality", ("1080p",))])

    def test_actions(self):
        fixture = cf.idle()
        service = cf.service_for(fixture)
        for action in view.SERVICE_ACTIONS:
            self.assertTrue(view.apply_camera_action(service, action, fixture.camera), action)
        for action in view.APP_ACTIONS + (None, "unknown"):
            self.assertFalse(view.apply_camera_action(service, action, fixture.camera), action)
        self.assertEqual(service.calls, [("set_camera_sharing", (True,)), ("set_camera_sharing", (False,)),
                                         ("ask_camera_permission", ()), ("set_camera", (MAC, None)),
                                         ("retry_helper", ())])
        notice = mac_section(cf.permission_from_pc()).notices[0]
        self.assertTrue(view.apply_camera_notice(service, notice))
        self.assertEqual(service.calls[-1], ("ask_camera_permission", ()))
        row = mac_section(cf.sharing_off()).this_machine[0]
        self.assertTrue(view.apply_row(service, row))
        self.assertEqual(service.calls[-1], ("set_camera_sharing", (True,)))


class Languages(unittest.TestCase):
    def tearDown(self):
        i18n.install("en")

    def test_every_state_builds_in_every_shipped_language(self):
        # Strings reach the catalogue through test_i18n_coverage (core/camera is scanned); here every
        # fixture's section, map extras and menu build in each language without a format error.
        for language in ("en", "it"):
            i18n.install(language)
            for make in list(cf.FIXTURES.values()) + list(cf.FIXTURES_PC.values()):
                fixture = make()
                mac_section(fixture)
                pc_section(fixture)
                view.map_extras(fixture.camera)
                view.menu_model(fixture.camera, fixture.audio.snapshot, "This Mac")


if __name__ == "__main__":
    unittest.main()
