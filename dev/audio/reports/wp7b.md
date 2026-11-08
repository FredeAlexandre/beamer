# WP7b report: Qt Audio page follow-up (branch pc/wp7b, PC lebig)

## Done (win_app/audio_page_win.py, its test, wp7-shots)
1. Model fields: tiles draw the mic line as the Mac page does (`mic_line`, else `listen_line` "Hears MacBook Air" in
   signal, else `input_line` = own default mic in ink_3; short tiles keep the important one over the platform line)
   and the output line, each with a glyph, left-aligned over the level bar. The recording mark's tooltip is
   `recording_line`; the spoken text is the model's `description` (it already holds that sentence; the local
   RECORDING string is gone, so `beamer.pot` loses one source ref). `page_model(..., peers_without_audio, driver)`:
   `driver()` defaults to None (PC: VB-CABLE comes from the snapshot); a Mac-only `driver_outdated`/`install_driver`
   notice would show without its button here. Peers without audio: `linked_without_audio(window)` reads
   `window._audio.peers.peers()` (audio_win.LinkPeers caps) + `host.peer_name`; unavailable (empty) when this Beamer
   runs without audio (`_audio` None, e.g. Linux or no helper) - it then tracks no link caps. Peers known only by id
   are passed as "" (the model drops them).
2. Test uses `fx.FIXTURES_PC`; local `as_pc()` removed. Renders now include `vbcable_missing` (not driver_missing).
3. Notices sit right under the Microphone module.
4. Title "AUDIO": not the page. `kvm_bridge_win._page_shell` (l.938) upper-cases every page title except Crossing
   (`title if key == "crossing" else title.upper()`, upstream 1.5.2). In the real window Overview, Keyboard, Design,
   Connection and Audio all show upper case, so Audio already matches; only Crossing differs. No change.
5. Renders (window, offscreen, PC's view): `wp7-shots/audio-{no_peers,mic_streaming,sound_redirected,
   vbcable_missing}-{light,dark}.png`, `crossing-*.png`, `fixtures-as-given/*-mac-*.png`. Reviewed against wp6-shots.

## Tests
- `test_audio_page_win`: 27 pass (+8: tile lines, recording tooltip, notice placement, driver arg, peers named,
  `linked_without_audio`, page reading the window's `_audio`/`audio`).
- `win_app` full suite: 4069 tests, 1 failure + 6 errors = exactly the 7 known (dev/README).
- Root: `core/tests/test_audio_*` 135 OK (2 skipped); `test_i18n_coverage` OK.

## Needs (outside my files)
- kvm_bridge_win `_audio_page`: looks up `self.audio_service`, which nothing sets (the app holds `self.audio`).
  The page now falls back to `window().audio` and `window()._audio`, but the lambda should read `self.audio`
  and pass `peers_without_audio=lambda: audio_page_win.linked_without_audio(self)`.
- Mac page: notices still at the bottom (task 3 was "decided for both apps").
- Repo: `audio_page_win.py`, its test and `beamer.pot` are stored with CRLF since WP7 (unchanged here).
- Note: the worktree has no venv; I ran the main clone's `win_app\.venv` interpreter (read only).

## Commits
2265b6c page + test + pot · 485e634 renders · next: this report
