# SPDX-License-Identifier: GPL-3.0-only
"""vcam_install (camera PHASE2 §9.2) with its OS seams mocked, on any platform; the real seams, the
installers' camera hunks and Install-Maintenance.ps1's camera step on Windows."""

import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.append(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))

import vcam_install
from vcam_install import Result


WIN_APP = Path(__file__).parents[1]
SID = "S-1-5-21-1004336348-1177238915-682003330-1001"
OTHER_SID = "S-1-5-21-1004336348-1177238915-682003330-1002"
DLL_BYTES = b"MZ camera source"
DLL_SHA256 = hashlib.sha256(DLL_BYTES).hexdigest()


class _Run:
    """Runs one install/remove with the seams replaced, waits for on_done, records everything."""

    def __init__(self, test, *, sid=SID, elevated=0, unelevated=0, windows=True, exe_exists=True, dll=DLL_BYTES):
        self.calls = []
        self.results = []
        self.threads = []
        self.done = threading.Event()
        directory = tempfile.mkdtemp(prefix="beamer-vcam-")
        test.addCleanup(shutil.rmtree, directory, True)
        self.exe = Path(directory) / "beamer-audio.exe"
        if exe_exists:
            self.exe.write_bytes(b"MZ")
        if dll is not None:
            (Path(directory) / "camera").mkdir()
            (Path(directory) / "camera" / vcam_install.DLL_NAME).write_bytes(dll)

        def shell(exe, parameters):
            self.calls.append(("elevated", Path(exe), parameters))
            value = elevated(parameters) if callable(elevated) else elevated
            if isinstance(value, BaseException):
                raise value
            return value

        def run(exe, arguments):
            self.calls.append(("unelevated", Path(exe), list(arguments)))
            value = unelevated(arguments) if callable(unelevated) else unelevated
            if isinstance(value, BaseException):
                raise value
            return value

        def sid_lookup():
            self.calls.append(("sid",))
            return sid

        for name, value in (("_shell_execute_elevated", shell), ("_run_unelevated", run),
                            ("_current_user_sid", sid_lookup), ("_is_windows", lambda: windows)):
            patcher = mock.patch.object(vcam_install, name, value)
            patcher.start()
            test.addCleanup(patcher.stop)

    def on_done(self, result):
        self.threads.append(threading.current_thread())
        self.results.append(result)
        self.done.set()

    def go(self, test, action, helper="default"):
        helper = self.exe if helper == "default" else helper
        action(self.on_done, helper=helper)
        test.assertTrue(self.done.wait(5), "on_done was never called")
        test.assertEqual(len(self.results), 1)
        return self.results[0]


class ResultAndApiTests(unittest.TestCase):
    def test_result_is_frozen_with_ok_and_code(self):
        result = Result(True, "ok")
        self.assertEqual((result.ok, result.code), (True, "ok"))
        with self.assertRaises(Exception):
            result.ok = False
        self.assertEqual(Result(False, "cancelled"), Result(False, "cancelled"))

    def test_both_calls_return_at_once_and_call_on_done_once_on_another_thread(self):
        for action in (vcam_install.install, vcam_install.remove):
            release = threading.Event()
            run = _Run(self, elevated=lambda _: release.wait(5) and 0)
            action(run.on_done, helper=run.exe)
            self.assertFalse(run.done.is_set(), "returned only after the work was done")
            release.set()
            self.assertTrue(run.done.wait(5))
            self.assertEqual(run.results, [Result(True, "ok")])
            self.assertIsNot(run.threads[0], threading.main_thread())


class InstallTests(unittest.TestCase):
    def test_verb_line_runs_the_helper_elevated_with_this_users_sid(self):
        run = _Run(self)
        self.assertEqual(run.go(self, vcam_install.install), Result(True, "ok"))
        self.assertEqual(run.calls, [("sid",), ("elevated", run.exe.resolve(),
                                                f"vcam-install --owner-sid {SID} --dll-sha256 {DLL_SHA256}")])

    def test_the_dll_hash_is_the_one_the_helper_will_install(self):
        """R4 #6: the hash of the DLL the helper's lookup rule picks (camera folder, then beside it),
        read before the prompt; no DLL, no prompt."""
        run = _Run(self, dll=None)
        (run.exe.parent / vcam_install.DLL_NAME).write_bytes(b"beside")
        run.go(self, vcam_install.install)
        self.assertTrue(run.calls[-1][2].endswith("--dll-sha256 " + hashlib.sha256(b"beside").hexdigest()))
        missing = _Run(self, dll=None)
        self.assertEqual(missing.go(self, vcam_install.install), Result(False, "failed"))
        self.assertEqual(missing.calls, [("sid",)])

    def test_install_update_and_repair_are_one_command_whatever_the_versions(self):
        # The helper picks the DLL by the §9.2 lookup rule and installs its own kDllVersion, so the page
        # sends the same line for missing, outdated and broken installs; no --dll, no version.
        lines = set()
        for _ in range(3):
            run = _Run(self)
            run.go(self, vcam_install.install)
            lines.add(run.calls[-1][2])
        self.assertEqual(lines, {f"vcam-install --owner-sid {SID} --dll-sha256 {DLL_SHA256}"})
        self.assertNotIn("--dll ", lines.pop())

    def test_uac_refused_is_cancelled(self):
        refused = OSError(1223, "The operation was cancelled by the user.")
        run = _Run(self, elevated=refused)
        self.assertEqual(run.go(self, vcam_install.install), Result(False, "cancelled"))

    def test_pywin32_style_error_1223_is_cancelled(self):
        error = OSError("cancelled")
        error.winerror = 1223
        run = _Run(self, elevated=error)
        self.assertEqual(run.go(self, vcam_install.install), Result(False, "cancelled"))

    def test_other_launch_errors_fail(self):
        run = _Run(self, elevated=OSError(5, "Access is denied."))
        self.assertEqual(run.go(self, vcam_install.install), Result(False, "failed"))

    def test_exit_codes_map_to_the_contract_codes(self):
        for code, expected in ((0, Result(True, "ok")), (2, Result(False, "bad_args")),
                               (3, Result(False, "not_elevated")), (4, Result(False, "failed")),
                               (1, Result(False, "failed")), (-1073741819, Result(False, "failed"))):
            with self.subTest(code=code):
                run = _Run(self, elevated=code)
                self.assertEqual(run.go(self, vcam_install.install), expected)

    def test_no_helper_when_the_exe_is_missing_or_not_windows(self):
        run = _Run(self, exe_exists=False)
        self.assertEqual(run.go(self, vcam_install.install), Result(False, "no_helper"))
        self.assertEqual(run.calls, [])
        run = _Run(self, windows=False)
        self.assertEqual(run.go(self, vcam_install.remove), Result(False, "no_helper"))
        self.assertEqual(run.calls, [])

    def test_default_helper_is_the_one_beamer_runs(self):
        run = _Run(self)
        with mock.patch("core.audio.helper.locate", return_value=[str(run.exe)]) as locate:
            self.assertEqual(run.go(self, vcam_install.install, helper=None), Result(True, "ok"))
        locate.assert_called_once_with()  # never the dev switch's helper (it may be the null helper)
        self.assertEqual(run.calls[-1][1], run.exe.resolve())
        run = _Run(self)
        with mock.patch("core.audio.helper.locate", return_value=None):
            self.assertEqual(run.go(self, vcam_install.install, helper=None), Result(False, "no_helper"))

    def test_an_unreadable_or_odd_sid_never_reaches_the_command_line(self):
        for sid in ("", None, 'S-1-5-21-1" & calc', "Alex"):
            with self.subTest(sid=sid):
                run = _Run(self, sid=sid)
                self.assertEqual(run.go(self, vcam_install.install), Result(False, "failed"))
                self.assertNotIn("elevated", [call[0] for call in run.calls])

    def test_an_unexpected_exception_still_calls_on_done_once_with_failed(self):
        run = _Run(self, elevated=RuntimeError("boom"))
        self.assertEqual(run.go(self, vcam_install.install), Result(False, "failed"))

    def test_an_on_done_that_raises_is_not_called_twice(self):
        calls = []
        done = threading.Event()

        def on_done(result):
            calls.append(result)
            done.set()
            raise ValueError("ui gone")

        run = _Run(self)
        vcam_install.install(on_done, helper=run.exe)
        self.assertTrue(done.wait(5))
        for thread in threading.enumerate():
            if thread.name.startswith("beamer-vcam"):
                thread.join(5)
        self.assertEqual(calls, [Result(True, "ok")])


class RemoveTests(unittest.TestCase):
    def test_remove_runs_vcam_remove_unelevated_then_vcam_uninstall_elevated(self):
        run = _Run(self)
        self.assertEqual(run.go(self, vcam_install.remove), Result(True, "ok"))
        exe = run.exe.resolve()
        self.assertEqual(run.calls, [("unelevated", exe, ["vcam-remove"]), ("elevated", exe, "vcam-uninstall")])

    def test_a_failed_vcam_remove_still_unregisters(self):
        for outcome in (4, 2, OSError(2, "gone"), subprocess.TimeoutExpired("beamer-audio.exe", 30)):
            with self.subTest(outcome=outcome):
                run = _Run(self, unelevated=outcome)
                self.assertEqual(run.go(self, vcam_install.remove), Result(True, "ok"))
                self.assertEqual([call[0] for call in run.calls], ["unelevated", "elevated"])

    def test_remove_cancelled_and_failed(self):
        run = _Run(self, elevated=OSError(1223, "cancelled"))
        self.assertEqual(run.go(self, vcam_install.remove), Result(False, "cancelled"))
        run = _Run(self, elevated=3)
        self.assertEqual(run.go(self, vcam_install.remove), Result(False, "not_elevated"))


@unittest.skipUnless(sys.platform == "win32", "the Windows seams need Windows")
class WindowsSeamTests(unittest.TestCase):
    def test_current_user_sid_is_this_accounts(self):
        sid = vcam_install._current_user_sid()
        self.assertRegex(sid, r"^S-1-\d+(-\d+)+$")
        out = subprocess.run(["whoami", "/user", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
        self.assertIn(sid, out)

    def test_unelevated_run_returns_the_exit_code(self):
        cmd = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
        self.assertEqual(vcam_install._run_unelevated(cmd, ["/c", "exit", "4"]), 4)

    def test_elevated_launch_returns_the_exit_code(self):
        import ctypes
        if not ctypes.windll.shell32.IsUserAnAdmin():
            self.skipTest("would show a UAC prompt: only run from an elevated process (ssh)")
        cmd = Path(os.environ["SystemRoot"]) / "System32" / "cmd.exe"
        self.assertEqual(vcam_install._shell_execute_elevated(cmd, "/c exit 3"), 3)


class PackagingContractTests(unittest.TestCase):
    """The camera hunks of the installers (camera PHASE2 §9.2), read as text."""

    def read(self, name):
        return (WIN_APP / name).read_text(encoding="utf-8")

    def test_inno_ships_the_dll_in_camera_and_removes_the_camera_before_maintenance(self):
        setup = self.read("Beamer-Setup.iss")
        self.assertIn(r'DestDir: "{app}\camera"', setup)
        self.assertIn(r"native\audio\build\win\camera\BeamerCameraSource.dll", setup)
        uninstall = setup[setup.index("[UninstallRun]"):setup.index("[Code]")]
        entries = [line for line in uninstall.splitlines() if line.startswith("Filename:")]
        self.assertEqual(len(entries), 2)
        self.assertIn(r'"{app}\beamer-audio.exe"; Parameters: "vcam-remove"', entries[0])
        self.assertIn("skipifdoesntexist", entries[0])
        self.assertIn("Invoke-InstallMaintenance.ps1", entries[1])

    def test_script_install_copies_the_dll_and_has_no_uninstall_path(self):
        install = self.read("Install-Beamer.ps1")
        self.assertIn(r"native\audio\build\win\camera\BeamerCameraSource.dll", install)
        self.assertIn("Install-BeamerCameraDll -Source $CameraDllSource -InstallDir $InstallDir", install)
        self.assertNotIn("vcam-", re.sub(r"#.*", "", install))
        self.assertNotIn("param(", install.split("function")[0])

    def test_portable_build_puts_the_dll_in_camera_at_the_top_of_the_folder(self):
        spec = self.read("build_portable.spec")
        self.assertIn('("camera\\\\BeamerCameraSource.dll", str(camera_dll), "EXECUTABLE")', spec)
        self.assertIn("audio_toc + camera_toc", spec)
        self.assertIn("'camera'", self.read("build_portable.ps1"))

    def test_maintenance_removes_the_camera_only_on_remove_and_clears_stale_versions_otherwise(self):
        source = self.read("Install-Maintenance.ps1")
        main = source[source.index("if ($MyInvocation.InvocationName -ne '.')"):]
        remove, other = main.split("} else {", 1)
        self.assertIn("Remove-BeamerCamera -UserSid $UserSid\n", remove)
        self.assertNotIn("StaleVersions", remove)
        self.assertIn("Remove-BeamerCameraStaleVersions", other.split("exit 0")[0])
        self.assertIn("{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}", source)
        self.assertIn("Stop-Service -Name 'FrameServer'", source)


@unittest.skipUnless(sys.platform == "win32", "PowerShell behaviour is checked on Windows")
class ScriptInstallBehaviourTests(unittest.TestCase):
    """Install-Beamer.ps1's camera function, lifted out of the script by the PowerShell parser (the script
    itself builds and installs Beamer, so it is never run here)."""

    def test_the_dll_lands_in_camera_and_a_build_without_it_removes_it(self):
        root = Path(tempfile.mkdtemp(prefix="beamer-camera-install-"))
        self.addCleanup(shutil.rmtree, root, True)
        script = str(WIN_APP / "Install-Beamer.ps1").replace("'", "''")
        probe = root / "probe.ps1"
        probe.write_text(rf"""$ErrorActionPreference = 'Stop'
$ast = [Management.Automation.Language.Parser]::ParseFile('{script}', [ref]$null, [ref]$null)
foreach ($name in 'Copy-WithRetry', 'Install-BeamerCameraDll') {{
    $function = $ast.Find({{ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }}, $true)
    if (-not $function) {{ throw "no $name in Install-Beamer.ps1" }}
    . ([scriptblock]::Create($function.Extent.Text))
}}
$install = '{root}\Beamer'
New-Item -ItemType Directory -Path $install -Force | Out-Null
Set-Content -Path '{root}\BeamerCameraSource.dll' -Value 'dll v2'
Install-BeamerCameraDll -Source '{root}\BeamerCameraSource.dll' -InstallDir $install | Out-Null
if ((Get-Content "$install\camera\BeamerCameraSource.dll") -ne 'dll v2') {{ throw 'not installed in camera\' }}
Install-BeamerCameraDll -Source '{root}\missing.dll' -InstallDir $install | Out-Null
if (Test-Path "$install\camera") {{ throw 'a build without the DLL kept the installed one' }}
""", encoding="utf-8-sig")
        result = subprocess.run([shutil.which("powershell") or "pwsh", "-NoProfile", "-NonInteractive",
                                 "-ExecutionPolicy", "Bypass", "-File", str(probe)],
                                capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


@unittest.skipUnless(sys.platform == "win32", "PowerShell behaviour is checked on Windows")
class MaintenanceCameraBehaviourTests(unittest.TestCase):
    """Install-Maintenance.ps1's camera step against a fake install: temp folders and an HKCU key stand in
    for %ProgramFiles%, %ProgramData% and HKLM; Stop-Service is a stub that releases the held file."""

    def setUp(self):
        self.root = Path(tempfile.mkdtemp(prefix="beamer-camera-maint-"))
        self.addCleanup(shutil.rmtree, self.root, True)
        self.key = rf"HKCU:\Software\BeamerCameraTest{os.getpid()}"
        self.addCleanup(subprocess.run, [self.powershell(), "-NoProfile", "-Command",
                                         f"Remove-Item -LiteralPath '{self.key}' -Recurse -Force -ErrorAction SilentlyContinue"])

    def powershell(self):
        return shutil.which("powershell") or "pwsh"

    def run_probe(self, probe):
        path = str(WIN_APP / "Install-Maintenance.ps1").replace("'", "''")
        programs, data = self.root / "Programs" / "Beamer" / "Camera", self.root / "Data" / "Beamer" / "Camera"
        source = f"""$ErrorActionPreference = 'Stop'
. '{path}'
$script:stopped = 0
$script:held = $null
function Stop-Service {{ param($Name, [switch]$Force, $ErrorAction)
    if ($Name -ne 'FrameServer') {{ throw "stopped $Name" }}
    $script:stopped++
    if ($script:held) {{ $script:held.Dispose(); $script:held = $null }}
}}
$paths = @{{
    ClsidKey = '{self.key}\\Classes\\CLSID\\{{19FCB9D1-E91E-45D0-B865-75D8F6D98A91}}'
    CameraKey = '{self.key}\\Beamer\\Camera'
    ProgramFolder = '{programs}'
    DataFolder = '{data}'
}}
New-Item -Path "$($paths.ClsidKey)\\InprocServer32" -Force | Out-Null
New-Item -Path $paths.CameraKey -Force | Out-Null
foreach ($version in 1, 2) {{
    New-Item -ItemType Directory -Path (Join-Path $paths.ProgramFolder $version) -Force | Out-Null
    Set-Content -Path (Join-Path $paths.ProgramFolder "$version\\BeamerCameraSource.dll") -Value 'dll'
}}
New-ItemProperty -Path $paths.CameraKey -Name DllPath -Value (Join-Path $paths.ProgramFolder '2\BeamerCameraSource.dll') | Out-Null
New-Item -ItemType Directory -Path $paths.DataFolder -Force | Out-Null
Set-Content -Path (Join-Path $paths.DataFolder 'source.log') -Value 'log'
""" + probe
        script = self.root / "probe.ps1"
        script.write_text(source, encoding="utf-8-sig")
        result = subprocess.run([self.powershell(), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass",
                                 "-File", str(script)], capture_output=True, text=True, timeout=60)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_remove_deletes_keys_and_folders_without_stopping_frame_server(self):
        self.run_probe(r"""
Remove-BeamerCamera -Paths $paths
foreach ($item in $paths.Values) { if (Test-Path -LiteralPath $item) { throw "left $item" } }
foreach ($parent in @((Split-Path -Parent $paths.ProgramFolder), (Split-Path -Parent $paths.DataFolder), (Split-Path -Parent $paths.CameraKey))) {
    if (Test-Path -LiteralPath $parent) { throw "left the empty $parent" }
}
if ($script:stopped -ne 0) { throw 'stopped FrameServer with nothing locked' }
""")

    def test_remove_stops_frame_server_only_when_the_dll_is_locked(self):
        self.run_probe(r"""
$dll = Join-Path $paths.ProgramFolder '2\BeamerCameraSource.dll'
$script:held = [IO.File]::Open($dll, 'Open', 'Read', 'None')
Remove-BeamerCamera -Paths $paths
if ($script:stopped -ne 1) { throw "FrameServer stopped $($script:stopped) times" }
foreach ($item in $paths.Values) { if (Test-Path -LiteralPath $item) { throw "left $item" } }
""")

    def test_remove_keeps_a_beamer_parent_that_is_not_empty(self):
        self.run_probe(r"""
$other = Join-Path (Split-Path -Parent $paths.ProgramFolder) 'Other'
New-Item -ItemType Directory -Path $other -Force | Out-Null
Remove-BeamerCamera -Paths $paths
if (Test-Path -LiteralPath $paths.ProgramFolder) { throw 'left the camera folder' }
if (-not (Test-Path -LiteralPath $other)) { throw 'deleted a sibling' }
""")

    def test_remove_on_a_machine_without_the_camera_is_a_no_op(self):
        self.run_probe(r"""
Remove-BeamerCamera -Paths $paths
Remove-BeamerCamera -Paths $paths
if ($script:stopped -ne 0) { throw 'stopped FrameServer' }
""")

    # Elevated Remove-Item -Recurse in Windows PowerShell 5.1 follows junctions; Users may create folders in
    # %ProgramData%. Junctions need no privilege, so these run unelevated too.
    VICTIM = r"""
$victim = Join-Path (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $paths.DataFolder))) 'Victim'
New-Item -ItemType Directory -Path (Join-Path $victim 'sub') -Force | Out-Null
Set-Content -Path (Join-Path $victim 'keep.txt') -Value 'victim'
Set-Content -Path (Join-Path $victim 'sub\keep.txt') -Value 'victim'
function Test-Entry([string]$Path) { try { [IO.File]::GetAttributes($Path) | Out-Null; $true } catch { $false } }
function Assert-Victim {
    foreach ($file in 'keep.txt', 'sub\keep.txt') {
        if (-not (Test-Path -LiteralPath (Join-Path $victim $file))) { throw "deleted $file through a junction" }
    }
}
"""

    def test_remove_never_deletes_through_a_junction_inside_the_camera_folder(self):
        self.run_probe(self.VICTIM + r"""
New-Item -ItemType Junction -Path (Join-Path $paths.DataFolder 'link') -Target $victim | Out-Null
New-Item -ItemType Junction -Path (Join-Path $paths.ProgramFolder '2\link') -Target $victim | Out-Null
Remove-BeamerCamera -Paths $paths
Assert-Victim
foreach ($item in $paths.Values) { if (Test-Path -LiteralPath $item) { throw "left $item" } }
""")

    def test_remove_deletes_a_camera_folder_that_is_a_junction_as_a_link(self):
        self.run_probe(self.VICTIM + r"""
Remove-Item -LiteralPath $paths.DataFolder -Recurse -Force
New-Item -ItemType Junction -Path $paths.DataFolder -Target $victim | Out-Null
Remove-BeamerCamera -Paths $paths
Assert-Victim
if (Test-Entry $paths.DataFolder) { throw 'left the junction' }
""")

    def test_remove_leaves_a_camera_folder_whose_parent_is_a_junction(self):
        self.run_probe(self.VICTIM + r"""
New-Item -ItemType Directory -Path (Join-Path $victim 'Camera') -Force | Out-Null
Set-Content -Path (Join-Path $victim 'Camera\keep.txt') -Value 'victim'
$beamer = Split-Path -Parent $paths.DataFolder
Remove-Item -LiteralPath $beamer -Recurse -Force
New-Item -ItemType Junction -Path $beamer -Target $victim | Out-Null
Remove-BeamerCamera -Paths $paths 3>$null
Assert-Victim
if (-not (Test-Path -LiteralPath (Join-Path $victim 'Camera\keep.txt'))) { throw 'deleted through the parent junction' }
""")

    def test_stale_versions_never_delete_through_a_junction(self):
        self.run_probe(self.VICTIM + r"""
New-Item -ItemType Junction -Path (Join-Path $paths.ProgramFolder '1\link') -Target $victim | Out-Null
New-Item -ItemType Junction -Path (Join-Path $paths.ProgramFolder '0') -Target $victim | Out-Null
Remove-BeamerCameraStaleVersions -Paths $paths
Assert-Victim
foreach ($stale in '0', '1') { if (Test-Entry (Join-Path $paths.ProgramFolder $stale)) { throw "kept $stale" } }
""")

    def test_remove_keeps_a_camera_another_account_owns(self):
        """R4 #7: OwnerSid names another account: its camera stays, keys, folders and Frame Server."""
        self.run_probe(rf"""
New-ItemProperty -Path $paths.CameraKey -Name OwnerSid -Value '{OTHER_SID}' | Out-Null
Remove-BeamerCamera -Paths $paths -UserSid '{SID}' 3>$null
foreach ($item in $paths.Values) {{ if (-not (Test-Path -LiteralPath $item)) {{ throw "removed $item" }} }}
if ($script:stopped -ne 0) {{ throw 'stopped FrameServer' }}
Remove-BeamerCamera -Paths $paths 3>$null
foreach ($item in $paths.Values) {{ if (-not (Test-Path -LiteralPath $item)) {{ throw "removed $item without a user" }} }}
""")

    def test_remove_by_the_owner_removes_everything(self):
        self.run_probe(rf"""
New-ItemProperty -Path $paths.CameraKey -Name OwnerSid -Value '{SID.lower()}' | Out-Null
Remove-BeamerCamera -Paths $paths -UserSid '{SID}'
foreach ($item in $paths.Values) {{ if (Test-Path -LiteralPath $item) {{ throw "left $item" }} }}
""")

    def test_remove_with_no_owner_recorded_removes_everything(self):
        self.run_probe(rf"""
Remove-BeamerCamera -Paths $paths -UserSid '{SID}'
foreach ($item in $paths.Values) {{ if (Test-Path -LiteralPath $item) {{ throw "left $item" }} }}
""")

    def test_stale_versions_go_the_registered_one_stays_and_a_locked_one_waits(self):
        self.run_probe(r"""
New-Item -ItemType Directory -Path (Join-Path $paths.ProgramFolder '0') -Force | Out-Null
Set-Content -Path (Join-Path $paths.ProgramFolder '0\BeamerCameraSource.dll') -Value 'dll'
$script:held = [IO.File]::Open((Join-Path $paths.ProgramFolder '0\BeamerCameraSource.dll'), 'Open', 'Read', 'None')
Remove-BeamerCameraStaleVersions -Paths $paths 3>$null
if (Test-Path -LiteralPath (Join-Path $paths.ProgramFolder '1')) { throw 'kept stale version 1' }
if (-not (Test-Path -LiteralPath (Join-Path $paths.ProgramFolder '2\BeamerCameraSource.dll'))) { throw 'deleted the registered version' }
if (-not (Test-Path -LiteralPath (Join-Path $paths.ProgramFolder '0\BeamerCameraSource.dll'))) { throw 'deleted a file in use' }
if ($script:stopped -ne 0) { throw 'install maintenance stopped FrameServer' }
$script:held.Dispose(); $script:held = $null
Remove-BeamerCameraStaleVersions -Paths $paths
if (Test-Path -LiteralPath (Join-Path $paths.ProgramFolder '0')) { throw 'the next run did not retry' }
""")

    def test_stale_versions_do_nothing_without_a_registration(self):
        self.run_probe(r"""
Remove-Item -LiteralPath $paths.CameraKey -Recurse -Force
Remove-BeamerCameraStaleVersions -Paths $paths
foreach ($version in 1, 2) { if (-not (Test-Path -LiteralPath (Join-Path $paths.ProgramFolder $version))) { throw "deleted $version" } }
""")


if __name__ == "__main__":
    unittest.main()
