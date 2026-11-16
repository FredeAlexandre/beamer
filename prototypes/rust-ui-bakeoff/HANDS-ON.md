# PROTOTYPE: hands-on checklist for the Rust UI bake-off

Same slice in each toolkit (see SPEC.md); each `<toolkit>/NOTES.md` has the numbers, the escape
hatches it needed, and the developer-experience notes. This page is what to do at the machines.

## Start one

- **Mac**: `prototypes/rust-ui-bakeoff/run.sh <rune|gpui|slint> --open-settings`
  (add `--test-glow` to loop the glow every 3 s). It lives in the menu bar; Quit is in its menu.
- **PC**: double-click `Desktop\Beamer bake-off\bakeoff-<toolkit>.exe`; it lives in the tray
  (left-click opens Settings, right-click the menu).

Run one toolkit at a time per machine. Tip: the GN10 is shared, so make sure it is on the machine
you are testing before judging its frame rate.

## For each toolkit, on each machine

1. **Overlay**: Tray ▸ Test glow. Click, scroll and hover *through* the glow and through the faint
   `OVERLAY · <toolkit>` label. Switch Tray ▸ Glow on between displays: the glow moves live.
2. **Smoothness**: does the glow judder? On the 144 Hz / 240 Hz screens especially. (Log has the
   measured fps: Mac `~/Library/Logs/BeamerBakeoff/<toolkit>.log`, PC `%LOCALAPPDATA%\BeamerBakeoff\`.)
3. **Mac only**: Test glow over a full-screen app and on another Space; the overlay stays out of
   Mission Control and Cmd-Tab, and covers (or not) the menu-bar strip.
4. **PC only**: the overlay stays out of Alt-Tab and the taskbar and never steals focus while typing.
5. **Settings**: drag the Resistance slider (glow preview), Hold to push (pressure strip, crossing
   flash, glow), the switches, the segmented control, the dropdown. Pause crossing stays in sync with
   the tray both ways. Close Settings: the app keeps running; reopen it.
6. **Keyboard**: Tab / Shift-Tab with a visible focus ring, arrows on sliders, Space on switches.
7. **Appearance**: switch the system light/dark with Settings open. Fonts against
   `assets/<mac|windows>-crossing-dark.png`.
8. **Screen reader**: VoiceOver (Cmd-F5) / Narrator (Ctrl-Win-Enter) on the controls.
9. **Feel**: then open `<toolkit>/src` and read how the page, the state and the animation are
   written. Which one would you enjoy writing 60 surfaces in?

## What the agents measured (not felt)

| | Rune | GPUI | Slint |
|---|---|---|---|
| Clean release build, Mac / PC | 83 / 132 s | 69 / 159 s | 90 / 155 s |
| Binary, Mac / PC | 25.2 / 21.1 MB | 10.2 / 11.0 MB | 19.3 / 23.9 MB |
| Start → first frame, Mac / PC | 162–262 / 25–115 ms | 137–145 / 115–194 ms | 290–310 / 283 ms |
| Idle RSS, tray only, Mac / PC | 86 / 71 MB | 82 / 45 MB | 97 / 78 MB |
| Overlay fps, Mac GN10 144 Hz | 144 | 144 | 60 (GN10 maybe on the PC then) |
| Overlay fps, PC | ~205, not vsync-locked | ~240, paced to the fastest monitor | ~160, ms-rounded timer |
| Overlay via | toolkit API, no platform code | `PopUp` + objc2 / Win32 by hand | escape hatches: 97 lines Mac, 136 Win |
| Tray via | `tray-icon` (Rune has none on Mac) | `tray-icon` | Slint's own `SystemTrayIcon` |
| Widgets | built in | **none**: 760 lines hand-built | built in |
