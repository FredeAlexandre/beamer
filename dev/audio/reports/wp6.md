# WP6 report: Audio page model + Mac Audio page

**Done.** `core/audio/view.py` (pure model, §9.1), `core/tests/audio_fixtures.py` (4 acceptance states seen from the
Mac, the same seen from lebig in `FIXTURES_PC`, 3 machines, `FakeService`), `test_audio_view.py` (milestone 1:
8102be9). `mac_app/audio_page.py`: `LevelMeter`; `AudioMap` (Crossing tile style on the shared layout: name, link line,
mic line, output line, level bar, recording mark; arrows sound solid / mic dashed / grey waiting; click and arrow
keys select); grouped popups (machine headers, "lebig · Wave:3" when closed); selected-machine panel (output, mute
note, status, Inputs: meter + Test, Outputs: meter + Headset); notices with actions. Registered in `pages.py`,
`kvm_bridge_app.py` (builders + `_audio_page`), `setup.py`. Driver (operator note): install/update offered only while
no stream runs, the notice says every app goes quiet for about a second, `audio_driver.install()` runs on a worker
thread. WP7 requests: extractor + coverage test now read `core/audio`, `TileView.recording_line`, PC-seen fixtures.

**Acceptance (this Mac, offscreen, nothing deployed)**
- `test_audio_view`: pass (29 tests, incl. real `AudioService`s from the cluster harness). `test_i18n_coverage`:
  pass; `beamer.pot` regenerated, 1363 strings incl. view.py; `it.po` untouched (120 soft-missing).
- Renders in `dev/audio/reports/wp6-shots/`: no peers, mic streaming to the PC, sound redirected + echo notice,
  driver missing, light and dark, plus `crossing-dark.png`; 128-colour PNGs, ~410 KB. Read and iterated (scale,
  dots, row columns, tile line priorities, arrows). Re-render: `python -m mac_app.tests.test_audio_page render <dir>`.
- Selectors call the setters (FakeService): pass (mic, output, Test, Headset, map selection, notice actions,
  driver install off the main thread, outdated update withheld while streaming).
- 60 s at 20 Hz (`AUDIO_METER_SECONDS=60`; worker pushes levels and a snapshot every 2 s; Mac loaded by other
  agents): 1137 level updates, max 3.11 ms (median 0.46); 27 snapshot refreshes, max 4.01 ms. Pass. A first click
  on another machine builds its rows in 7-10 ms (kept afterwards: 0.9 ms).
- mac_app suite (`discover -s tests -t ..`, 3800 tests incl. imported core tests): only failures that are not WP6's:
  profiles_migration/tile_resize (4, known), glow_falloff (no pytest), click_focus tab ring (fails on base cf612fd
  too), and core test_audio_helper (5, WP1) which fail only inside this run and pass alone.

**Deviations**
- The page entry exists only with `core.feature_flags.AUDIO` on (upstream with the flag off: no Audio page).
- Headset switch writes an explicit form ("headset" / "speakers"), so it shows what was set.
- §9.1 additions (backward compatible): `TileView.input_line`, `listen_line` ("Hears This Mac"), `recording_line`;
  `page_model(..., peers_without_audio=(), driver=None)`; notice `driver_outdated`; `apply_*` helpers; the mic
  status reads the planner's `params["to"]`; output selector disabled while "Here" is the only choice.
- Edited outside WP6 at the operator's request: `tools/i18n_extract.py`, `core/tests/test_i18n_coverage.py`
  (`SOURCES` += `core/audio`; `_error` of fake_helper.py listed as a diagnostic).
- Notices strip moved right under the Microphone module (accepted by the operator; PHASE2 §0/§9.2 updated).

**Needs from others**
1. Done by the operator (33bc196): `test_pages.py` expects "audio"; py2app includes `core.audio.view`.
2. WP5: the page reads the service as `controller.audio` (looked up again every 2 s while the page is open).
3. WP7: `TileView.listen_line`/`input_line`/`recording_line` and `FIXTURES_PC` are new since its merge.
4. Translator: the audio strings in `it.po`. model.py: nothing needed.

**Commits:** 8102be9 model + fixtures; c914b50 Mac page; cc44588, fae89db merges of feat/audio; 32294cf driver offer,
rows, renders; 620cfa1 catalogue core/audio, recording_line, PC fixtures; report.
