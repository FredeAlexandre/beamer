# SPDX-License-Identifier: GPL-3.0-only
"""The Audio page model (dev/audio/PHASE2.md §9.1): what each state shows, and what each action calls."""

from __future__ import annotations

import os
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from core import i18n
from core.audio import model, view
from core.audio.model import DeviceRef, MicRoute, OutputRoute, RouteStatus, StreamView
from core.tests import audio_fixtures as fx
from core.tests import test_i18n_coverage as coverage

VIEW = ROOT / "core" / "audio" / "view.py"


def build(fixture, **changes):
    fixture = replace(fixture, **changes) if changes else fixture
    return view.page_model(fixture.snapshot, fixture.tiles, fixture.selected, fixture.own_label,
                           fixture.peers_without_audio)


def codes(page):
    return [notice.code for notice in page.notices]


def strings(value):
    """Every user-visible string in a page model."""
    if isinstance(value, str):
        return [value]
    if isinstance(value, (tuple, list)):
        return [text for item in value for text in strings(item)]
    if hasattr(value, "__dataclass_fields__"):
        skip = {"key", "machine", "target", "source", "meter", "mic_meter", "selected", "mic_selected",
                "output_selected", "code", "action", "platform", "form", "kind", "state", "output_tone",
                "mic_tone", "tone", "frame", "device"}
        return [text for name in value.__dataclass_fields__ if name not in skip
                for text in strings(getattr(value, name))]
    return []


class FixturePages(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_every_fixture_builds_a_page(self):
        for name, make in list(fx.FIXTURES.items()) + [("three_machines", fx.three_machines)]:
            with self.subTest(name):
                page = build(make())
                self.assertTrue(page.tiles)
                self.assertIn(page.selected, {tile.machine for tile in page.tiles})
                self.assertIn(page.mic_selected, {choice.key for choice in page.mic_choices})
                self.assertIn(page.panel.output_selected, {choice.key for choice in page.panel.output_choices})
                self.assertEqual(len({choice.key for choice in page.mic_choices}), len(page.mic_choices))

    def test_no_peers(self):
        page = build(fx.no_peers())
        self.assertEqual([tile.machine for tile in page.tiles], [fx.MAC])
        tile = page.tiles[0]
        self.assertEqual((tile.title, tile.subtitle), ("This Mac", "MacBook Air · macOS"))
        self.assertTrue(tile.own and tile.online and tile.selected)
        self.assertEqual(tile.output_line, "Sound plays here")
        self.assertEqual(page.mic_selected, view.OFF)
        self.assertEqual(page.mic_tone, view.QUIET)
        self.assertIn("No microphone is shared", page.mic_line)
        self.assertEqual(page.arrows, ())
        # Only "Here" and nothing to send the sound to.
        self.assertEqual([choice.key for choice in page.panel.output_choices], [view.HERE])
        self.assertEqual(page.panel.output_line, "Plays on MacBook Air Speakers.")
        self.assertEqual(page.notices, ())
        self.assertEqual([row.name for row in page.panel.inputs], ["MacBook Air Microphone", "AirPods Pro"])
        self.assertEqual(page.panel.inputs[0].detail, "Default · Built-in")
        self.assertEqual([row.headset for row in page.panel.outputs], [False, True])

    def test_mic_streaming_to_the_pc(self):
        page = build(fx.mic_streaming())
        self.assertEqual(page.mic_line, "MacBook Air Microphone on This Mac is live for lebig.")
        self.assertEqual(page.mic_tone, view.LIVE)
        self.assertEqual(page.mic_meter, f"{fx.MAC}/in/{fx.MAC_MIC.key}")
        chosen = view.choice_by_key(page.mic_choices, page.mic_selected)
        self.assertEqual((chosen.machine, chosen.device), (fx.MAC, None))
        self.assertEqual(chosen.title, "This Mac · Default (MacBook Air Microphone)")
        # This machine's group comes first, after Off.
        self.assertEqual([choice.label for choice in page.mic_choices if choice.header], ["This Mac", "lebig"])
        tiles = {tile.machine: tile for tile in page.tiles}
        self.assertTrue(tiles[fx.MAC].has_active_mic)
        self.assertEqual(tiles[fx.MAC].mic_line, "MacBook Air Microphone")
        self.assertEqual(tiles[fx.MAC].input_line, "")
        self.assertEqual(tiles[fx.PC].input_line, "Microphone (Wave:3)")
        self.assertEqual(tiles[fx.PC].mic_line, "")
        self.assertEqual(tiles[fx.PC].listen_line, "Hears This Mac")
        self.assertEqual(tiles[fx.MAC].listen_line, "")
        self.assertTrue(tiles[fx.PC].recording)
        self.assertEqual(tiles[fx.PC].subtitle, "Windows · Linked")
        self.assertEqual([(a.source, a.target, a.kind, a.state) for a in page.arrows], [(fx.MAC, fx.PC, "mic", "active")])
        # Reading order: lebig sits left of the Mac.
        self.assertEqual([tile.machine for tile in page.tiles], [fx.PC, fx.MAC])
        self.assertEqual(codes(page), ["echo_aec", "pick_virtual_mic"])
        self.assertIn("Beamer cancels the echo of This Mac's speakers", page.notices[0].text)
        active = [row for row in page.panel.inputs if row.active]
        self.assertEqual([row.name for row in active], ["MacBook Air Microphone"])
        self.assertEqual(active[0].active_label, "Active microphone")

    def test_sound_redirected_with_echo(self):
        page = build(fx.sound_redirected())
        self.assertEqual(page.selected, fx.PC)
        panel = page.panel
        self.assertEqual(panel.heading, "lebig")
        self.assertEqual(panel.output_label, "Sound from lebig plays on")
        self.assertEqual(panel.mute_note, "Muted on lebig while it plays elsewhere.")
        self.assertEqual(panel.output_line, "Playing on MacBook Air Speakers on This Mac.")
        self.assertEqual(panel.output_tone, view.LIVE)
        chosen = view.choice_by_key(panel.output_choices, panel.output_selected)
        self.assertEqual((chosen.machine, chosen.device), (fx.MAC, None))
        self.assertEqual(panel.output_choices[0].label, "Here, on Speakers (Realtek(R) Audio)")
        tiles = {tile.machine: tile for tile in page.tiles}
        self.assertEqual(tiles[fx.PC].output_line, "Sound → This Mac")
        self.assertEqual(tiles[fx.PC].output_tone, view.LIVE)
        self.assertTrue(tiles[fx.PC].selected)
        kinds = {(a.source, a.target, a.kind, a.state) for a in page.arrows}
        self.assertEqual(kinds, {(fx.MAC, fx.PC, "mic", "active"), (fx.PC, fx.MAC, "sound", "active")})
        self.assertIn("echo_aec", codes(page))
        # lebig's speakers are muted while its sound plays on the Mac: no echo there.
        self.assertNotIn("echo_cross", codes(page))
        # The Mac's speakers play lebig's sound.
        mac_page = build(fx.sound_redirected(), selected=fx.MAC)
        speakers = next(row for row in mac_page.panel.outputs if row.key == fx.MAC_SPEAKERS.key)
        self.assertTrue(speakers.active)
        self.assertEqual(speakers.active_label, "Plays lebig")

    def test_driver_missing(self):
        page = build(fx.driver_missing())
        self.assertEqual(codes(page)[0], "driver_missing")
        notice = page.notices[0]
        self.assertEqual((notice.action, notice.action_label), ("install_driver", "Install Beamer Microphone"))
        self.assertFalse(view.apply_notice(fx.FakeService(fx.driver_missing().snapshot), notice))
        self.assertNotIn("pick_virtual_mic", codes(page))
        self.assertEqual(page.mic_line, "Microphone (Wave:3) on lebig is ready. It turns on when an app starts recording.")
        chosen = view.choice_by_key(page.mic_choices, page.mic_selected)
        self.assertEqual((chosen.machine, chosen.device), (fx.PC, DeviceRef(fx.WAVE.key, fx.WAVE.name)))
        self.assertTrue({tile.machine: tile for tile in page.tiles}[fx.PC].has_active_mic)

    def test_three_machines(self):
        page = build(fx.three_machines())
        tiles = {tile.machine: tile for tile in page.tiles}
        self.assertTrue(tiles[fx.ROG].online)
        self.assertEqual(tiles[fx.MAC].output_line, "Sound → lebig, waiting")
        self.assertEqual(tiles[fx.MAC].output_tone, view.WAIT)
        self.assertEqual(page.panel.output_line, "Headphones (Arctis 7) is not connected to lebig. "
                                                 "This Mac plays its sound itself until it is back.")
        chosen = view.choice_by_key(page.panel.output_choices, page.panel.output_selected)
        self.assertFalse(chosen.enabled)
        self.assertEqual(chosen.label, "Headphones (Arctis 7) (not connected)")
        # The unplugged device sits in lebig's group, which appears once.
        self.assertEqual([c.label for c in page.panel.output_choices if c.header], ["lebig"])
        # asusrog is not linked to the Mac: its sound cannot go there, and the Mac's cannot go to it.
        rog_page = build(fx.three_machines(), selected=fx.ROG)
        self.assertEqual([c.label for c in rog_page.panel.output_choices if c.header], ["lebig"])
        self.assertEqual({(a.source, a.target, a.kind, a.state) for a in page.arrows},
                         {(fx.ROG, fx.PC, "mic", "active"), (fx.MAC, fx.PC, "sound", "waiting")})
        self.assertEqual([tile.machine for tile in page.tiles], [fx.PC, fx.MAC, fx.ROG])


class States(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_unplugged_mic_stays_selected(self):
        fixture = fx.driver_missing()
        snap = replace(fixture.snapshot,
                       route=replace(fixture.snapshot.route, mic=MicRoute(fx.PC, DeviceRef("{usb-gone}", "Yeti"), fx.stamp(9))),
                       mic_status=RouteStatus("waiting_device"))
        page = build(fixture, snapshot=snap)
        chosen = view.choice_by_key(page.mic_choices, page.mic_selected)
        self.assertEqual(chosen.label, "Yeti (not connected)")
        self.assertFalse(chosen.enabled)
        self.assertEqual(page.mic_line, "Yeti is not connected to lebig. Beamer uses it again as soon as it is back.")
        self.assertEqual(page.mic_tone, view.WAIT)
        self.assertIsNone(page.mic_meter)

    def test_a_device_found_by_name_after_a_port_change(self):
        fixture = fx.driver_missing()
        snap = replace(fixture.snapshot, route=replace(
            fixture.snapshot.route, mic=MicRoute(fx.PC, DeviceRef("{old-port}", fx.WAVE.name), fx.stamp(9))))
        page = build(fixture, snapshot=snap)
        chosen = view.choice_by_key(page.mic_choices, page.mic_selected)
        self.assertEqual(chosen.device.key, fx.WAVE.key)
        self.assertTrue(chosen.enabled)

    def test_offline_machine_choices_are_disabled(self):
        fixture = fx.mic_streaming()
        snap = replace(fixture.snapshot, direct=frozenset(), online=frozenset({fx.MAC}),
                       mic_status=RouteStatus("off"), route=model.empty_route())
        page = build(fixture, snapshot=snap)
        pc_choices = [c for c in page.mic_choices if c.machine == fx.PC and not c.header]
        self.assertTrue(pc_choices and not any(c.enabled for c in pc_choices))
        tile = {tile.machine: tile for tile in page.tiles}[fx.PC]
        self.assertFalse(tile.online)
        self.assertEqual(tile.subtitle, "Windows · Offline")
        self.assertEqual(tile.output_tone, view.QUIET)

    def test_mic_waiting_and_failing(self):
        base = fx.mic_streaming()
        cases = [
            (RouteStatus("waiting_machine"), "This Mac is offline. Its microphone comes back with it."),
            (RouteStatus("unreachable", None, {"targets": fx.ROG}), "Machine · " + fx.ROG[-6:] + " cannot hear This Mac: "
                                                                    "they are not linked to it directly."),
            (RouteStatus("no_virtual_mic", None, {"targets": fx.PC}), "lebig cannot hear This Mac: no virtual microphone there."),
            (RouteStatus("error", "device_busy"), "MacBook Air Microphone on This Mac could not start: another app holds the device."),
            (RouteStatus("error", "something_new"), "MacBook Air Microphone on This Mac could not start."),
            (RouteStatus("ready"), "MacBook Air Microphone on This Mac is ready. It turns on when an app starts recording."),
        ]
        for status, line in cases:
            with self.subTest(status.state):
                page = build(base, snapshot=replace(base.snapshot, mic_status=status, streams=()))
                self.assertEqual(page.mic_line, line)
                self.assertEqual(page.mic_tone, view.TONES[status.state])

    def test_targets_from_streams_and_the_local_loop(self):
        base = fx.mic_streaming()
        snap = replace(base.snapshot, mic_status=RouteStatus("streaming"),
                       machines={**base.snapshot.machines, fx.MAC: fx.mac(links=(fx.PC,), in_use=True)})
        page = build(base, snapshot=snap)
        self.assertEqual(page.mic_line, "MacBook Air Microphone on This Mac is live for lebig and This Mac.")
        # No arrow from a machine to itself.
        self.assertEqual([(a.source, a.target) for a in page.arrows], [(fx.MAC, fx.PC)])

    def test_echo_cross_offers_to_send_the_sound(self):
        base = fx.mic_streaming()
        speakers_pc = fx.pc(in_use=True, outputs=(replace(fx.REALTEK, default=True), replace(fx.G733, default=False)))
        snap = replace(base.snapshot, machines={**base.snapshot.machines, fx.PC: speakers_pc})
        page = build(base, snapshot=snap)
        notice = next(n for n in page.notices if n.code == "echo_cross")
        self.assertEqual(notice.text, "If lebig's speakers can be heard by MacBook Air Microphone, people in a call on "
                                      "lebig hear themselves. Use a headset, or send lebig's sound to This Mac, where "
                                      "Beamer cancels it.")
        self.assertEqual((notice.action, notice.action_label), ("send_sound", "Send lebig's sound to This Mac"))
        service = fx.FakeService(snap)
        self.assertTrue(view.apply_notice(service, notice))
        self.assertEqual(service.calls, [("set_output", (fx.PC, fx.MAC, None))])

    def test_headset_override_silences_the_echo_notices(self):
        base = fx.mic_streaming()
        headset_mac = fx.mac(links=(fx.PC,), outputs=(replace(fx.MAC_SPEAKERS, form="headset"), fx.AIRPODS))
        snap = replace(base.snapshot, machines={**base.snapshot.machines, fx.MAC: headset_mac})
        self.assertNotIn("echo_aec", codes(build(base, snapshot=snap)))

    def test_conflict(self):
        base = fx.sound_redirected()
        outputs = {fx.PC: OutputRoute(fx.MAC, None, fx.stamp(20)), fx.MAC: OutputRoute(fx.PC, None, fx.stamp(21))}
        snap = replace(base.snapshot, route=replace(base.snapshot.route, outputs=outputs),
                       output_status={fx.PC: RouteStatus("streaming"), fx.MAC: RouteStatus("conflict")})
        page = build(base, snapshot=snap, selected=fx.MAC)
        notice = next(n for n in page.notices if n.code == "conflict")
        self.assertEqual(notice.tone, view.FAULT)
        self.assertEqual(notice.text, "This Mac's sound cannot play on Speakers (Realtek(R) Audio) on lebig: lebig sends "
                                      "its own sound elsewhere, which mutes that output.")
        self.assertEqual(page.panel.output_tone, view.FAULT)
        tile = {tile.machine: tile for tile in page.tiles}[fx.MAC]
        self.assertEqual(tile.output_line, "Sound → lebig, blocked")

    def test_helper_failed_and_permissions(self):
        base = fx.no_peers()
        snap = replace(base.snapshot, helper="failed", permission={"mic": "denied", "system_audio": "denied"})
        page = build(base, snapshot=snap)
        self.assertEqual(codes(page), ["helper_failed", "permission_mic", "permission_system"])
        service = fx.FakeService(snap)
        self.assertTrue(view.apply_notice(service, page.notices[0]))
        self.assertEqual(service.calls, [("retry_helper", ())])
        self.assertEqual(page.notices[1].action, "open_mic_settings")
        self.assertIn("System Settings", page.notices[1].text)
        self.assertEqual(page.notices[2].action, "open_system_audio_settings")
        suspect = build(base, snapshot=replace(base.snapshot, permission={"mic": "suspect"}))
        self.assertIn("only silence", suspect.notices[0].text)

    def test_windows_notices(self):
        base = fx.mic_streaming()
        no_cable = replace(fx.pc(links=(fx.MAC,)), virtual_mic=model.VirtualMic(None, False, False))
        snap = replace(base.snapshot, own=fx.PC, direct=frozenset({fx.MAC}),
                       machines={**base.snapshot.machines, fx.PC: no_cable},
                       permission={"mic": "denied"})
        page = view.page_model(snap, base.tiles, None, "This PC")
        self.assertEqual(codes(page)[:2], ["vbcable_missing", "permission_mic"])
        self.assertIn("VB-CABLE (vb-audio.com)", page.notices[0].text)
        self.assertIn("Settings, Privacy & security", page.notices[1].text)
        self.assertEqual({tile.machine: tile for tile in page.tiles}[fx.PC].title, "This PC")
        with_cable = view.page_model(replace(snap, machines={**snap.machines, fx.PC: fx.pc()}, permission={}),
                                     base.tiles, None, "This PC")
        self.assertIn("choose CABLE Output", next(n for n in with_cable.notices if n.code == "pick_virtual_mic").text)

    def test_peers_without_audio(self):
        page = build(fx.no_peers(), peers_without_audio=("Fedora laptop",))
        self.assertEqual(page.notices[-1].text, "Fedora laptop runs a Beamer without audio. Update Beamer there to use "
                                                "its microphones and speakers.")
        page = build(fx.no_peers(), peers_without_audio=("Fedora laptop", "Studio Mac"))
        self.assertTrue(page.notices[-1].text.startswith("Fedora laptop and Studio Mac run a Beamer without audio."))

    def test_machines_without_a_tile_go_in_a_row_below(self):
        page = build(fx.three_machines(), tiles={fx.MAC: (0, 0, 160, 100)})
        frames = {tile.machine: tile.frame for tile in page.tiles}
        self.assertEqual(frames[fx.MAC], (0.0, 0.0, 160.0, 100.0))
        self.assertEqual(frames[fx.PC][1], frames[fx.ROG][1])
        self.assertGreater(frames[fx.PC][1], 100)
        self.assertEqual(frames[fx.ROG][0] - frames[fx.PC][0], 184.0)
        page = build(fx.three_machines(), tiles={})
        self.assertEqual(len({tile.frame for tile in page.tiles}), 3)
        # The layout's own dicts work as well.
        page = build(fx.no_peers(), tiles={fx.MAC: {"x": 5, "y": 6, "w": 7, "h": 8, "displays": []}})
        self.assertEqual(page.tiles[0].frame, (5.0, 6.0, 7.0, 8.0))

    def test_own_machine_before_its_helper_lists_devices(self):
        snap = model.empty_snapshot(fx.MAC)
        page = view.page_model(snap, {}, None, "This Mac")
        self.assertEqual(page.tiles[0].subtitle, "Audio starting…")
        self.assertEqual(page.panel.inputs, ())
        self.assertEqual(page.panel.inputs_empty, "This Mac has not listed its devices yet.")
        self.assertEqual([c.key for c in page.mic_choices], [view.OFF])

    def test_meters(self):
        page = build(fx.sound_redirected())
        self.assertIn(f"{fx.MAC}/in/{fx.MAC_MIC.key}", page.meters)
        self.assertIn(f"{fx.PC}/out/{fx.REALTEK.key}", page.meters)
        self.assertEqual(view.level_fraction(0), 1.0)
        self.assertEqual(view.level_fraction(-60), 0.0)
        self.assertEqual(view.level_fraction(-100), 0.0)
        self.assertEqual(view.level_fraction(-30), 0.5)
        self.assertEqual(view.meter_reading({"k": (-30.0, -6.0)}, "k"), (0.5, 0.9))
        self.assertIsNone(view.meter_reading({}, "k"))
        self.assertIsNone(view.meter_reading({}, None))
        self.assertEqual(view.meter_key("M", "input", "K"), "M/in/K")
        self.assertEqual(view.meter_key("M", "output", "K"), "M/out/K")

    def test_layout_tiles(self):
        self.assertEqual(view.layout_tiles({"a": {"x": 1, "y": 2, "w": 3, "h": 4}, "b": {"x": 1}, "c": None,
                                            "d": (1, 2, 3, 4), "e": (1, 2)}),
                         {"a": (1.0, 2.0, 3.0, 4.0), "d": (1.0, 2.0, 3.0, 4.0)})


class Actions(unittest.TestCase):
    def setUp(self):
        i18n.install("en")

    def test_selectors_call_the_service_setters(self):
        fixture = fx.mic_streaming()
        service = fx.FakeService(fixture.snapshot)
        page = build(fixture)
        view.apply_mic(service, view.choice_by_key(page.mic_choices, view.OFF))
        wave = next(c for c in page.mic_choices if c.device is not None and c.device.key == fx.WAVE.key)
        view.apply_mic(service, wave)
        default_pc = next(c for c in page.mic_choices if c.machine == fx.PC and c.device is None and not c.header)
        view.apply_mic(service, default_pc)
        realtek = next(c for c in page.panel.output_choices if c.device is not None and c.device.key == fx.REALTEK.key)
        view.apply_output(service, fx.MAC, realtek)
        view.apply_output(service, fx.MAC, view.choice_by_key(page.panel.output_choices, view.HERE))
        view.apply_headset(service, page.panel.outputs[0], True)
        view.apply_headset(service, page.panel.outputs[1], False)
        view.apply_test(service, page.panel.inputs[1])
        self.assertEqual(service.calls, [
            ("set_mic", (None, None)),
            ("set_mic", (fx.PC, DeviceRef(fx.WAVE.key, fx.WAVE.name))),
            ("set_mic", (fx.PC, None)),
            ("set_output", (fx.MAC, fx.PC, DeviceRef(fx.REALTEK.key, fx.REALTEK.name))),
            ("set_output", (fx.MAC, None, None)),
            ("set_form", (fx.MAC, fx.MAC_SPEAKERS.key, "headset")),
            ("set_form", (fx.MAC, fx.AIRPODS.key, "speakers")),
            ("test_input", (fx.MAC, fx.AIRPODS_MIC.key)),
        ])

    def test_headers_do_nothing(self):
        fixture = fx.mic_streaming()
        service = fx.FakeService(fixture.snapshot)
        page = build(fixture)
        header = next(c for c in page.mic_choices if c.header)
        view.apply_mic(service, header)
        view.apply_output(service, fx.MAC, next(c for c in page.panel.output_choices if c.header))
        self.assertEqual(service.calls, [])
        self.assertIsNone(view.choice_by_key(page.mic_choices, header.key))

    def test_keys_are_stable_across_snapshots(self):
        one, two = build(fx.mic_streaming()), build(fx.sound_redirected())
        self.assertEqual([c.key for c in one.mic_choices], [c.key for c in two.mic_choices])


class Words(unittest.TestCase):
    def test_view_strings_pass_the_i18n_checks(self):
        self.assertEqual(coverage.hard_coded(VIEW), [])
        self.assertEqual(coverage.unreadable_tr(VIEW), [])
        self.assertEqual(coverage.unmarked_choice_names(VIEW), [])
        self.assertEqual(coverage.import_time_tr(VIEW), [])

    def test_every_shown_string_is_translated(self):
        with mock.patch.dict(os.environ, {"BEAMER_LANGUAGE": i18n.PSEUDO}):
            i18n.install("auto")
        try:
            fixtures = list(fx.FIXTURES.values()) + [fx.three_machines]
            for make in fixtures:
                fixture = make()
                page = build(fixture, own_label=i18n.tr("This Mac"), peers_without_audio=("Fedora",))
                names = {machine.name for machine in fixture.snapshot.machines.values()}
                devices = {device.name for machine in fixture.snapshot.machines.values()
                           for device in machine.inputs + machine.outputs} | {"Headphones (Arctis 7)"}
                for text in strings(page):
                    if not text or text in names or text in devices:
                        continue
                    with self.subTest(text=text):
                        self.assertIn("⟦", text)
        finally:
            i18n.install("en")


if __name__ == "__main__":
    unittest.main()
