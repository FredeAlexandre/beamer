# SPDX-License-Identifier: GPL-3.0-only
"""core.camera.texts: the vcam placeholder texts for every state (PHASE2 §7.5, §8.4, §10.1)."""

import os
import unittest
from dataclasses import replace
from unittest import mock

from core import i18n
from core.camera import model, texts
from core.camera.model import (Camera, CameraEntry, CameraRef, CameraRoute, CameraStatus, MachineCamera,
                               QualityEntry, Stamp)

MAC = "AA" * 16
PC = "BB" * 16
GHOST = "CC" * 16


def names(machine):
    """A MediaContext.machine_name stand-in: own, a held machine, the fallback."""
    return {PC: "lebig", MAC: "alexandres-macbook-air"}.get(machine, f"Machine · {machine[-6:]}")


def mac_list(test_source=None, capture=model.IDLE_CAPTURE):
    return MachineCamera(id=MAC, platform="macos", rev=1, status="ok", features=frozenset({"camera_capture"}),
                         cameras=(Camera("cam-1", "MacBook Air Camera", "builtin", (), True),), preferred="cam-1",
                         permission="granted", sharing=True, capture=capture,
                         virtual_camera=model.NO_VIRTUAL_CAMERA, test_source=test_source)


def snap(machine=MAC, device=None, status=CameraStatus("idle"), test_source=None, sinks=None,
         capture=model.IDLE_CAPTURE):
    route = CameraRoute(CameraEntry(machine, device, Stamp(1, MAC)), QualityEntry("720p", Stamp(1, MAC)), {})
    return replace(model.empty_snapshot(PC), route=route, status=status, sinks=sinks or {},
                   machines={MAC: mac_list(test_source, capture)})


class PlaceholderTextTests(unittest.TestCase):
    def setUp(self):
        i18n.install("en")
        self.addCleanup(i18n.install, "en")

    def test_every_state_has_a_title_and_a_line(self):
        result = texts.placeholder_texts(snap(), names)
        self.assertEqual(set(result), set(model.PLACEHOLDER_STATES))
        for state, entry in result.items():
            self.assertEqual(set(entry), {"title", "line"}, state)
            self.assertTrue(entry["title"], state)
            self.assertTrue(entry["line"], state)

    def test_wording_with_a_named_camera(self):
        result = texts.placeholder_texts(snap(device=CameraRef("cam-1", "MacBook Air Camera")), names)
        self.assertEqual(result["off"], {"title": "Beamer Camera is off", "line": "Choose a camera in Beamer"})
        self.assertEqual(result["waiting"], {"title": "Waiting for alexandres-macbook-air",
                                             "line": "MacBook Air Camera"})
        self.assertEqual(result["starting"], {"title": "Starting MacBook Air Camera",
                                              "line": "on alexandres-macbook-air"})
        self.assertEqual(result["waiting_permission"], {"title": "Waiting for camera access",
                                                        "line": "Allow it in Beamer on alexandres-macbook-air"})
        self.assertEqual(result["sharing_off"], {"title": "Camera turned off on alexandres-macbook-air",
                                                 "line": "Turn it on in Beamer on alexandres-macbook-air"})
        self.assertEqual(result["denied"], {"title": "alexandres-macbook-air has not allowed the camera",
                                            "line": "Allow it in Beamer on alexandres-macbook-air"})
        self.assertEqual(result["reconnecting"], {"title": "Reconnecting to alexandres-macbook-air",
                                                  "line": "MacBook Air Camera"})
        self.assertEqual(result["error"]["title"], "Beamer Camera stopped")

    def test_default_camera_when_the_route_follows_the_preferred_one(self):
        result = texts.placeholder_texts(snap(device=None), names)
        self.assertEqual(result["waiting"]["line"], "Default camera")
        self.assertEqual(result["reconnecting"]["line"], "Default camera")

    def test_test_source_replaces_the_camera_name(self):
        result = texts.placeholder_texts(snap(device=CameraRef("cam-1", "MacBook Air Camera"),
                                              test_source="synth"), names)
        self.assertEqual(result["waiting"]["line"], "Test source")
        self.assertEqual(result["reconnecting"]["line"], "Test source")
        self.assertEqual(result["starting"]["title"], "Starting Test source")

    def test_names_through_the_callable(self):
        own = texts.placeholder_texts(snap(machine=PC), names)            # own machine id
        self.assertEqual(own["waiting"]["title"], "Waiting for lebig")
        unknown = texts.placeholder_texts(snap(machine=GHOST), names)    # never held: the fallback
        self.assertEqual(unknown["waiting"]["title"], f"Waiting for Machine · {GHOST[-6:]}")
        off = texts.placeholder_texts(snap(machine=None), names)
        self.assertEqual(off["waiting"]["title"], "Waiting for another machine")

    def test_error_reason(self):
        failed = texts.placeholder_texts(snap(status=CameraStatus("error", "no_receiver", {})), names)
        self.assertEqual(failed["error"]["line"], "The picture did not reach this PC")
        sink = texts.placeholder_texts(snap(sinks={PC: CameraStatus("error", "decoder")}), names)
        self.assertEqual(sink["error"]["line"], "This PC could not show the picture")
        capture = texts.placeholder_texts(snap(capture=model.Capture("failed", False, (), "permission")), names)
        self.assertEqual(capture["error"]["line"], "Camera access is not allowed")
        generic = texts.placeholder_texts(snap(status=CameraStatus("error", "weird")), names)
        self.assertEqual(generic["error"]["line"], "Open Beamer for details")
        self.assertEqual(texts.placeholder_texts(snap(), names)["error"]["line"], "Open Beamer for details")

    def test_every_string_goes_through_tr(self):
        with mock.patch.dict(os.environ, {"BEAMER_LANGUAGE": i18n.PSEUDO}):
            i18n.install("auto")
        machine_names = {"alexandres-macbook-air", "MacBook Air Camera"}
        for current in (snap(device=CameraRef("cam-1", "MacBook Air Camera")), snap(test_source="synth"),
                        snap(status=CameraStatus("error", "helper"))):
            for state, entry in texts.placeholder_texts(current, names).items():
                for part, text in entry.items():
                    if text in machine_names:      # a bare name is never translated
                        continue
                    self.assertTrue(text.startswith("⟦") and text.endswith("⟧"), f"{state}.{part}: {text!r}")
        # Names stay as typed inside a template (the pseudo locale would accent them otherwise).
        denied = texts.placeholder_texts(snap(), names)["denied"]["title"]
        self.assertIn("alexandres-macbook-air", denied)
        for code in texts.REASONS:
            self.assertTrue(texts.reason_text(code).startswith("⟦"), code)


if __name__ == "__main__":
    unittest.main()
