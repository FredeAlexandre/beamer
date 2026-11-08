# WP8 Packaging and permissions: report

Branch `audio/wp8`. PHASE2 §10 implemented in the WP8 files only; native steps are wired at the §2.1 paths
and skipped with a `WARNING` while `native/audio/build-mac.sh` (WP3) / `build-win.cmd` (WP4) are absent.

**Done.** Mac: `build_dmg.sh` runs `build-mac.sh`, copies `build/mac/beamer-audio` to `Contents/MacOS/`, the driver
+ `install-driver.sh` to `Resources/audio/` before `sign_app`; helper signed with `entitlements-audio.plist`
(audio-input only, id `uk.co.kalkman.beamer.audio`), driver signed as a bundle before the app, its Mach-O kept out
of the nested loop; after signing the build checks the helper's entitlement and runs `install-driver.sh --check`.
`entitlements.plist` += audio-input; `setup.py` += both usage strings, `"audio_driver"` after `"windows_input"`;
`Install-Beamer-Mac.command` ad-hoc reset list += `Microphone AudioCapture`. New `audio_driver.py`
(status/offer/install: beamer-hal via `sudo -n` when present, else osascript admin prompt running the bundled
`install-driver.sh`; refuses while streams run; no screen words, the page passes a translated prompt; call off the
main thread), `native/audio/mac-driver/install-driver.sh` (beamer-hal's checks + Team ID match with the app;
`--check` mode without root), `mac_app/tests/test_audio_driver.py` (23 tests).
Windows: `build_win_app.ps1` runs `build-win.cmd`, signs the exe with `Sign-BeamerArtifact`; `Install-Beamer.ps1`
`Install-BeamerAudioHelper` stops a helper from the install dir, copies with the retry loop, runs `restore` once
(15 s cap, failure = warning), removes a stale helper when the build has none; `Beamer-Setup.iss` ships it when it
exists (ISPP `FileExists`, else `#pragma warning`) and runs `restore` post-install; `build_portable.spec` adds it to
`COLLECT` as `EXECUTABLE` so it lands beside `Beamer.exe`, not in `_internal`.

**Permission flow (needs Alex, H1; nothing triggered, no tccutil/TCC edits).** Beamer spawns the helper, so macOS
names Beamer and shows its Info.plist strings: Microphone at the first mic capture, System Audio Recording at the
first tap, once each; the helper reports `permission`. Stable "Beamer Dev" signing keeps grants across deploys;
an ad-hoc fallback build revokes them on install (script, by design). Driver: the page offers Install/Update
(`audio_driver.offer`) only with no stream, warns that audio stops ~1 s, then `install()`; dev Macs use beamer-hal.

**Acceptance**
- `dev/deploy.sh` both (9ab2fc1, lock held): pass; Beamer running on both; Mac<->PC and PC<->asusrog links up (fresh link timing lines both peers).
- `codesign --verify --strict --deep /Applications/Beamer.app`: pass ("Beamer Dev", not ad-hoc). `plutil -p`: both strings: pass. App entitlements incl. audio-input: pass.
- Helper/driver inside: not in the deployed app (no build-mac.sh yet). With a temporary stub build-mac.sh (universal C stub + prototype driver, not committed) a local build_dmg.sh: app verifies, helper shows audio-input + hardened runtime, driver verifies with `identifier "com.beamer.audio.driver"`, `--check` ok, a tampered driver breaks the app seal: pass.
- PC: deploy log shows the skip warning, Beamer installs: pass. Scratch copy on lebig with a stub build-win.cmd: helper built + reported, portable output has `beamer-audio.exe` beside `Beamer.exe`: pass; `Install-BeamerAudioHelper` (install+restore, replace running, restore fail/hang 2 s, stale removal): pass. Signed helper: not run (no Certum cert on lebig). ISCC: not run (Inno Setup not installed on lebig).
- `test_audio_driver` 23/23 pass; `core.tests.test_i18n_coverage` pass; mac_app suite (`discover -s tests`): 3679 run, the same 2 failures + 5 errors as the untouched baseline (git-archive revisions, glow_falloff import, click-focus ring, frame clock): pass
- Helper `ready` in Beamer's log: not run (needs WP5 + WP3/WP4).

**Deviations / needs**
- Operator: `win_app/build_portable.ps1` (not WP8's) rejects any top-level file not in its allowlist; add `'beamer-audio.exe'` or the portable build fails once WP4 lands.
- WP3: `build-mac.sh` is run as `bash build-mac.sh` (no args, `DEVELOPER_DIR` set) and must write `build/mac/beamer-audio` (universal) + `build/mac/BeamerAudio.driver`; no `$HOME` path may appear in the binaries (build_dmg.sh refuses the app: use `-ffile-prefix-map`, no `__FILE__` paths). Keep `mac-driver/install-driver.sh` when moving the driver in.
- WP4: `build-win.cmd` must set up MSVC itself (plain PowerShell over ssh) and exit non-zero on failure; `beamer-audio.exe restore` must default to `%LOCALAPPDATA%\Beamer`, exit 0 with nothing to restore, finish in < 15 s, and fail cleanly in an ssh (session 0) run.
- WP2: `.gitignore` for `native/audio/build/` is not on this branch yet. WP6: driver UI words + prompt text, call `install` on a worker.

Commits: 9ab2fc1 (packaging), a3e93d5 (report).
