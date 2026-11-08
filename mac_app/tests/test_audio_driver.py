# SPDX-License-Identifier: GPL-3.0-only
"""The Beamer Microphone driver: version order, state, and which install path runs (PHASE2 §10.1).

Nothing here installs anything or shows a prompt: every command goes to a recording runner.
"""

import os
import plistlib
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import audio_driver


REPO = Path(__file__).resolve().parents[2]
INSTALL_SCRIPT = REPO / "native" / "audio" / "mac-driver" / "install-driver.sh"


def make_bundle(folder: Path, version="3", bundle_id=audio_driver.BUNDLE_ID, name=audio_driver.BUNDLE_NAME) -> Path:
    bundle = folder / name
    (bundle / "Contents" / "MacOS").mkdir(parents=True)
    with open(bundle / "Contents" / "Info.plist", "wb") as handle:
        plistlib.dump({"CFBundleIdentifier": bundle_id, "CFBundleVersion": version,
                       "CFBundleExecutable": "BeamerAudio", "CFBundlePackageType": "BNDL"}, handle)
    return bundle


class Runner:
    def __init__(self, returncode=0, stdout="", stderr=""):
        self.calls = []
        self.result = subprocess.CompletedProcess([], returncode, stdout, stderr)

    def __call__(self, command, **kwargs):
        self.calls.append((command, kwargs))
        return self.result


class VersionTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(audio_driver.parse_version("12"), (12,))
        self.assertEqual(audio_driver.parse_version(" 1.2.3 "), (1, 2, 3))
        self.assertEqual(audio_driver.parse_version("1.2.0"), (1, 2))
        for text in (None, "", "1.x", "1..2", "-1", "1.2b"):
            self.assertIsNone(audio_driver.parse_version(text), text)

    def test_compare_is_numeric_not_textual(self):
        self.assertEqual(audio_driver.compare_versions("9", "10"), -1)
        self.assertEqual(audio_driver.compare_versions("10", "9"), 1)
        self.assertEqual(audio_driver.compare_versions("1.10", "1.9"), 1)
        self.assertEqual(audio_driver.compare_versions("2", "2.0"), 0)
        self.assertEqual(audio_driver.compare_versions("2.0.1", "2"), 1)
        self.assertIsNone(audio_driver.compare_versions("2", "beta"))
        self.assertIsNone(audio_driver.compare_versions(None, "2"))


class StatusTests(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.folder)
        (self.folder / "app").mkdir()
        (self.folder / "hal").mkdir()
        self.installed = self.folder / "hal" / audio_driver.BUNDLE_NAME

    def status(self):
        return audio_driver.status(installed=self.installed, folder=self.folder / "app")

    def test_no_bundle_offers_nothing(self):
        driver = self.status()
        self.assertEqual(driver.state, audio_driver.NO_BUNDLE)
        self.assertIsNone(audio_driver.offer(driver, streams_running=False))

    def test_missing_offers_install(self):
        make_bundle(self.folder / "app", "3")
        driver = self.status()
        self.assertEqual(driver, audio_driver.DriverStatus(audio_driver.MISSING, "3", None))
        self.assertEqual(audio_driver.offer(driver, streams_running=False), "install")

    def test_older_installed_offers_update(self):
        make_bundle(self.folder / "app", "10")
        make_bundle(self.folder / "hal", "9")
        driver = self.status()
        self.assertEqual(driver, audio_driver.DriverStatus(audio_driver.OUTDATED, "10", "9"))
        self.assertEqual(audio_driver.offer(driver, streams_running=False), "update")

    def test_same_or_newer_installed_offers_nothing(self):
        make_bundle(self.folder / "app", "3")
        make_bundle(self.folder / "hal", "3")
        self.assertEqual(self.status().state, audio_driver.CURRENT)
        shutil.rmtree(self.installed)
        make_bundle(self.folder / "hal", "4")
        driver = self.status()
        self.assertEqual(driver.state, audio_driver.NEWER)
        self.assertIsNone(audio_driver.offer(driver, streams_running=False))

    def test_foreign_or_unreadable_install_is_replaced(self):
        make_bundle(self.folder / "app", "3")
        make_bundle(self.folder / "hal", "3", bundle_id="com.example.other")
        self.assertEqual(self.status().state, audio_driver.OUTDATED)
        (self.installed / "Contents" / "Info.plist").write_bytes(b"not a plist")
        self.assertEqual(self.status().state, audio_driver.OUTDATED)

    def test_a_running_stream_blocks_the_offer(self):
        make_bundle(self.folder / "app", "3")
        self.assertIsNone(audio_driver.offer(self.status(), streams_running=True))

    def test_frozen_app_looks_in_its_resources(self):
        with mock.patch.object(sys, "frozen", "macosx_app", create=True), \
                mock.patch.dict(os.environ, {"RESOURCEPATH": "/Applications/Beamer.app/Contents/Resources"}):
            self.assertEqual(audio_driver.audio_dir(), Path("/Applications/Beamer.app/Contents/Resources/audio"))

    def test_source_run_looks_in_the_native_build(self):
        with mock.patch.object(sys, "frozen", False, create=True):
            self.assertEqual(audio_driver.audio_dir(), REPO / "native" / "audio" / "build" / "mac")


class InstallPathTests(unittest.TestCase):
    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.folder)
        self.audio = self.folder / "Beamer's \"odd\" app" / "audio"
        self.audio.mkdir(parents=True)
        make_bundle(self.audio)
        self.script = self.audio / audio_driver.INSTALL_SCRIPT
        self.script.write_text("#!/bin/bash\n")
        self.hal = self.folder / "beamer-hal"

    def make_hal(self):
        self.hal.write_text("#!/bin/bash\n")
        self.hal.chmod(0o755)

    def test_beamer_hal_is_chosen_when_it_exists(self):
        self.assertEqual(audio_driver.install_method(self.hal), audio_driver.METHOD_OSASCRIPT)
        self.hal.write_text("not executable")
        self.assertEqual(audio_driver.install_method(self.hal), audio_driver.METHOD_OSASCRIPT)
        self.make_hal()
        self.assertEqual(audio_driver.install_method(self.hal), audio_driver.METHOD_HAL)

    def test_dev_mac_installs_through_beamer_hal_without_a_prompt(self):
        self.make_hal()
        runner = Runner(stdout="installed\ncoreaudiod restarted\n")
        result = audio_driver.install(streams_running=False, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result, audio_driver.InstallResult(audio_driver.INSTALLED_OK, audio_driver.METHOD_HAL,
                                                            "installed\ncoreaudiod restarted"))
        self.assertTrue(result.ok)
        [(command, kwargs)] = runner.calls
        self.assertEqual(command, ["/usr/bin/sudo", "-n", str(self.hal), "install"])
        self.assertEqual(kwargs["timeout"], audio_driver.HAL_TIMEOUT)

    def test_shipped_mac_runs_the_bundled_script_with_administrator_privileges(self):
        runner = Runner()
        result = audio_driver.install(streams_running=False, prompt='Beamer wants to install "Beamer Microphone".',
                                      folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.INSTALLED_OK)
        self.assertEqual(result.method, audio_driver.METHOD_OSASCRIPT)
        [(command, kwargs)] = runner.calls
        self.assertEqual(command[:2], ["/usr/bin/osascript", "-e"])
        escaped = str(self.script).replace('"', '\\"')
        self.assertEqual(command[2], 'do shell script "/bin/bash " & quoted form of "' + escaped + '"'
                         ' with administrator privileges with prompt "Beamer wants to install \\"Beamer Microphone\\"."')
        self.assertEqual(kwargs["timeout"], audio_driver.PROMPT_TIMEOUT)

    @unittest.skipUnless(sys.platform == "darwin", "osacompile is macOS")
    def test_the_applescript_compiles(self):
        command = audio_driver.install_command(audio_driver.METHOD_OSASCRIPT, self.script, 'a "quoted" prompt')
        out = self.folder / "check.scpt"
        subprocess.run(["/usr/bin/osacompile", "-o", str(out), "-e", command[2]], check=True, capture_output=True)

    def test_cancelled_prompt(self):
        runner = Runner(returncode=1, stderr="execution error: User canceled. (-128)\n")
        result = audio_driver.install(streams_running=False, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.CANCELLED)

    def test_failure_carries_the_output(self):
        runner = Runner(returncode=1, stderr="install-driver: signature check failed\n")
        result = audio_driver.install(streams_running=False, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.FAILED)
        self.assertIn("signature check failed", result.detail)

    def test_runner_error_is_a_failure_not_an_exception(self):
        def runner(command, **kwargs):
            raise subprocess.TimeoutExpired(command, 600)
        result = audio_driver.install(streams_running=False, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.FAILED)

    def test_never_while_a_stream_runs(self):
        self.make_hal()
        runner = Runner()
        result = audio_driver.install(streams_running=True, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.BUSY)
        self.assertEqual(runner.calls, [])

    def test_nothing_to_install_without_the_bundle_or_script(self):
        runner = Runner()
        self.script.unlink()
        result = audio_driver.install(streams_running=False, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.UNAVAILABLE)
        self.script.write_text("#!/bin/bash\n")
        shutil.rmtree(self.audio / audio_driver.BUNDLE_NAME)
        result = audio_driver.install(streams_running=False, folder=self.audio, beamer_hal=self.hal, runner=runner)
        self.assertEqual(result.result, audio_driver.UNAVAILABLE)
        self.assertEqual(runner.calls, [])


@unittest.skipUnless(sys.platform == "darwin" and INSTALL_SCRIPT.is_file(), "codesign and the script are macOS")
class InstallScriptCheckTests(unittest.TestCase):
    """install-driver.sh --check: the verification it makes as root, run without root."""

    def setUp(self):
        self.folder = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.folder)
        shutil.copy2(INSTALL_SCRIPT, self.folder / "install-driver.sh")

    def check(self):
        return subprocess.run(["/bin/bash", str(self.folder / "install-driver.sh"), "--check"],
                              capture_output=True, text=True)

    def signed_bundle(self, bundle_id=audio_driver.BUNDLE_ID):
        bundle = make_bundle(self.folder, "7", bundle_id)
        shutil.copyfile("/usr/bin/true", bundle / "Contents" / "MacOS" / "BeamerAudio")
        subprocess.run(["codesign", "--force", "--sign", "-", str(bundle)], check=True, capture_output=True)
        return bundle

    def test_a_signed_bundle_passes(self):
        self.signed_bundle()
        result = self.check()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "ok 7")

    def test_a_foreign_bundle_is_refused(self):
        self.signed_bundle("com.example.other")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unexpected bundle identifier", result.stderr)

    def test_a_tampered_bundle_is_refused(self):
        bundle = self.signed_bundle()
        with open(bundle / "Contents" / "MacOS" / "BeamerAudio", "ab") as handle:
            handle.write(b"\0")
        result = self.check()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("signature check failed", result.stderr)

    @unittest.skipIf(os.geteuid() == 0, "as root it would really install")
    def test_install_needs_root(self):
        self.signed_bundle()
        result = subprocess.run(["/bin/bash", str(self.folder / "install-driver.sh")], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("needs root", result.stderr)


if __name__ == "__main__":
    unittest.main()
