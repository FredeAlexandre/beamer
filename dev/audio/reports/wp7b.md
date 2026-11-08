# WP7b report: Qt Audio page follow-up (branch pc/wp7b, PC lebig)

## Done (win_app/audio_page_win.py, its test, wp7-shots)
1. Tiles draw the mic line as the Mac does (`mic_line`, else `listen_line` "Hears MacBook Air" in signal, else
   `input_line` in ink_3; on short tiles the important one wins over the platform line) and the output line,
   each with a glyph. The recording mark's tooltip is `recording_line`; the spoken text is `description` (which
   already holds that sentence; the local string is gone, `beamer.pot` loses one source ref). `page_model` gets
   `driver()` (None on the PC; a Mac-only install notice would show without its button) and peers without
   audio from `linked_without_audio(window)`: `window._audio.peers.peers()` caps + `host.peer_name`. That is
   unavailable (empty) when this Beamer runs without audio (`_audio` None): it then tracks no link caps.
2. Test uses `fx.FIXTURES_PC`; `as_pc()` removed. Renders now show `vbcable_missing`, not driver_missing.
3. Notices sit right under the Microphone module.
4. "AUDIO": `kvm_bridge_win._page_shell` (l.938) upper-cases every page title except Crossing (upstream 1.5.2).
   Overview, Keyboard, Design, Connection and Audio all show upper case; Audio already matches. No change.
5. Renders (window, offscreen, PC's view) in `wp7-shots/`: 4 states light+dark, Crossing, `fixtures-as-given/`.
   Compared with wp6-shots.

## Tests
- `test_audio_page_win`: 27 pass (+8: tile lines, recording tooltip, notice order, driver, peers, window links).
- `win_app` suite: 4069 tests, 1 failure + 6 errors = exactly the 7 known ones. Root `test_audio_*`: 135 OK
  (2 skipped); `test_i18n_coverage` OK.

## Needs (outside my files)
- kvm_bridge_win `_audio_page` reads `self.audio_service`, which nothing sets (the app holds `self.audio`). The
  page falls back to `window().audio`/`._audio`, but the lambda should read `self.audio` and pass
  `peers_without_audio=lambda: audio_page_win.linked_without_audio(self)`.
- Mac page: notices still at the bottom. Repo: these files are stored CRLF since WP7 (unchanged here).
- No venv in the worktree: I ran the main clone's `win_app\.venv` interpreter (read only).

Commits: 2265b6c page + test + pot · 485e634 renders · report commits.
