# Rune: bake-off notes (PROTOTYPE, throwaway)

The slice from `../SPEC.md` in [Rune](https://github.com/anomalyco/rune). Binary `bakeoff-rune`.
Run: `../run.sh rune [--test-glow] [--open-settings] [--quit-after N]` on the Mac,
`../sync-pc.sh rune [--smoke]` for the PC. One extra flag, for measuring only:
`--glow-on <display index>` overrides the default overlay display.

"Verified" below means an agent saw it in the log, a native read-back or a screenshot. Nothing here
was checked by a person yet. That is the HITL list at the end.

## Versions and patches

- `rune` git rev `0796df5fa269b85d4fc04786bbd37f7349eb6606` (`dev`, 2026-11-16), with
  `default-features = false, features = ["accessibility", "metal-backend"]`. The default features also
  include embedded Inter, SVG, Ogg audio and D-Bus, which the slice doesn't need.
- `[patch.crates-io]` at the same rev: `winit`, `parley_engine`, `wgpu-hal`, `accesskit_windows`
  **and `accesskit_macos`**. That's five crates, not the four the survey listed: `accesskit_macos`
  was added since. Rune's README says to copy its root `[patch]` table on every bump.
- `tray-icon =0.26.1` (muda 0.21.2), `objc2 =0.6.5`, `objc2-app-kit`/`objc2-foundation =0.3.2`,
  `windows-sys =0.61.2`. These match the versions already in Rune's tree, so nothing is duplicated.
  `Cargo.lock` is committed (457 packages).
- Toolchain: Rust 1.100 on both machines. Rune pins 1.99.1 for itself, and 1.100 built it without
  complaint.
- Nothing in Rune had to be patched or worked around to build, on either OS.

## Numbers

| | Mac (M-series Air, Command Line Tools) | PC (Windows, MSVC) |
|---|---|---|
| Clean release build (deps already fetched) | 83 s (440 s CPU) | 132 s |
| Rebuild after touching `main.rs` (release) | 19 s | ~35 s (sync-pc rebuilds) |
| Release binary | 25.2 MB, 20.9 MB stripped | 21.1 MB |
| Startup → tray installed | 12–178 ms | 9–18 ms |
| Startup → first settings frame (`--open-settings`) | 162–262 ms (262 on the first cold run) | 25–115 ms (115 on the first run after a build) |
| Idle RSS, tray only | 86 MB RSS / 47 MB phys_footprint | 71 MB working set / 79–82 MB private |
| Idle RSS, settings open | 100 MB RSS / 62 MB phys_footprint | 90 MB working set / 119–121 MB private |
| Idle CPU, tray only (10 s sample, after 8 s) | 0.03 s CPU / 10 s (0.3 %) | 0.000 s (below the 15.6 ms tick) |
| Idle CPU, settings open | 0.06 s CPU / 10 s (0.6 %) | 0.000 s |
| Overlay fps vs refresh, GN10 | **143–144 fps @ 144 Hz** (131–140 while the settings window animates too) | **205 fps @ 240 Hz** |
| Overlay fps vs refresh, other display | **59.5–60 fps @ 60 Hz** (built-in; this Air has no ProMotion) | **205–206 fps @ 144 Hz** (MSI G24C6) |

How the numbers were taken:

- **fps** is the overlay's own present counter (`OverlayHandle::surface_stats().totals.presents`),
  differenced over each push-and-cross sequence (0.72 s) and divided by its wall time. It counts
  frames presented, not frames requested. The log has both (presents and polls).
- **Startup** runs from `main()`'s first line, not from process creation, to the first
  `App::on_frame_drawn`.
- **Idle**: Mac `ps -o cputime,rss` plus `footprint`. PC: `Win32_Process` user plus kernel time and
  `Get-Process` memory, sampled from SSH while the app ran in the desktop session.

## Hard requirements: how each was met

### Overlay (Rune API, no escape hatch to make it work)

`App::overlay(OverlaySpec::new(..).options(OverlayOptions { placement: FullScreen, display:
DisplayTarget::Native(id), level: Status, input: OverlayInput::None, prewarm: true }))`. The README
and the survey under-sell this. `overlay.rs` has more than the README's "Edge overlays" section:
display targeting, full-screen and rect placement, input regions, levels, `set_display` to re-target
live, and `surface_stats()` for frame counting. That is the whole hard requirement as API.

- **macOS**, verified by reading the panel back through AppKit (`platform/mac.rs`):
  class `RuneOverlayPanel`, level 25, collectionBehavior `0x40151` = canJoinAllSpaces | stationary |
  ignoresCycle | fullScreenAuxiliary (+ canJoinAllApplications), `ignoresMouseEvents = YES`,
  NonactivatingPanel, `canBecomeKey/Main = NO`, frame = the whole GN10 (2560×1440 pt). A screenshot
  strip of the GN10's right edge showed the cyan glow over other apps' windows. All Spaces, over a
  full-screen app, no Dock / Mission Control: **set and read back, not verified by a person**.
- **Windows**, read back with `GetWindowLongPtrW` (`platform/win.rs`): exstyle `0x82c01b8` =
  LAYERED, TRANSPARENT, TOOLWINDOW, NOACTIVATE, TOPMOST, **plus APPWINDOW** (winit's default, which
  Rune leaves set; Rune asks winit for `skip_taskbar` instead). The rect covers the whole target
  monitor at 1:1 scale. Click-through is **set and read back**. Whether it shows in the taskbar or in
  Alt-Tab despite APPWINDOW, and whether it draws at all on the PC (upstream only clippy-checks
  this path), needs a person. The smoke runs presented 145–149 frames per sequence with no failed
  acquisitions, so the swap chain works.
- **Click-through on drawn pixels** (the 8 % `OVERLAY · rune` label): the flags say yes. Only a
  person can confirm it.
- **Re-target live** (`Glow on`): `OverlayHandle::set_display`. On the Mac a forced re-target from
  GN10 to the built-in display moved the overlay, and its fps followed (144 → 60). Through the tray
  it is only built.
- **Idle**: the overlay asks for frames only while a sequence runs (`Tx::wake_after(ZERO)` from its
  `poll`). Afterwards it is quiet (0 presents/s in Rune's stats, ~0 CPU).
- **Hot-plug**: Rune's display list is a startup snapshot (`run_windows` factory). Not handled.
- Platform code for the overlay: **0 lines to make it work**. 48 (mac) + 81 (win) lines exist only to
  read flags back and to get display names and refresh rates (below).

### Tray / menu-bar item (escape hatch: tray-icon, 116 lines in `platform/tray.rs`)

Rune's own `rune::tray` is Windows and Linux only ("macOS isn't supported yet"), with a flat menu
(actions and separators: no submenus, no check items). The slice needs both, so tray-icon (muda)
runs on both OSes:

- It is created inside Rune's `run_windows` factory, which runs on the main thread once the event loop
  is up. Menu items live in a `thread_local`, so Rune handlers can set the Pause check. Tray events
  reach Rune state through `AppHandle::update` / `open` / `quit`, which are `Send + Sync`.
  This fits together cleanly.
- muda has no radio items: radio groups are `CheckMenuItem`s unchecked by hand.
- Accessory activation (no Dock icon) is `RunOptions { accessory: true }`.
- Windows left-click → Settings, right-click → menu: `with_menu_on_left_click(false)` and a click
  handler.
- **Built and running on both, not verified by a person**: nobody has clicked the menu yet.

### Display info Rune doesn't give (escape hatch)

`rune::Display` has a name, frame, scale and primary flag, but no refresh rate. On Windows its name
is `\\.\DISPLAY1`. `platform/mac.rs` reads `NSScreen.localizedName` and `maximumFramesPerSecond`.
`platform/win.rs` reads `EnumDisplaySettingsW` and the `QueryDisplayConfig` friendly name
(`GN10`, `MSI G24C6`). Total platform code: **mac 48, win 81, tray 116, shared glue 26** (non-blank,
non-comment lines).

## Settings window

- Rune's own `el()/row()/col()` views, hand-built in the Settings example's style (`ruler`,
  `switch_row`, `segmented`, `keycap`, the PushStrip). rune-kit wasn't used: its components follow the
  opencode look, not Beamer's tokens.
- Hanken Grotesk and B612 Mono are embedded through `AppSpec::fonts` and named with
  `Family::Named`. "Shortcut goes to" is a native popup menu (`tx.popup_menu`, NSMenu /
  TrackPopupMenuEx).
- A screenshot of the Mac window (dark) matched the reference layout: sidebar link block, nav, page
  modules on `panel`, footer. Two first-run fixes: the default `Chrome` draws content under the
  traffic lights (`full_size_content: false` gives the titled `Beamer` window), and the `px` unit
  needed `items_end`. The PC window was not seen.
- Light/dark: the view reads `cx.system_dark()` on every build, so it should follow the system
  live. **Not tested.**
- **Keyboard**: checked headlessly with a throwaway `Runtime` probe (removed afterwards).
  - Tab cycles nav → Resistance slider → Hold → both switches → segmented → taps slider →
    dropdown, with `focus_visible` on each. Needs `App::tab_visits_controls(true)`: by default
    Rune follows macOS and Tab visits only text fields and lists.
  - Right arrow steps a slider by its step (120 → 125). Space toggles a switch (and the tray check
    follows).
  - **Not possible: holding Space to push.** Space runs `on_press` once. Hold-to-push is
    pointer-only.
  - The focus ring was not looked at on a real screen.
- **Accessibility**: roles and labels are set (slider with `range_value`, switch with `checked`,
  combobox, radiogroup, tabs, heading). AccessKit is on. VoiceOver and Narrator were not tried.

## What was rough or surprising

- **Windows pacing**: the overlay presents ~205 fps on **both** the 144 Hz and the 240 Hz monitor.
  It is neither locked to its own display's refresh nor to the 240 Hz primary. Rune's stats say
  `pacing: idle` and 0 unpaced redraws, and its docs only promise display-link pacing on macOS
  ("a wake due sooner than a frame waits about a frame"). Over 144 Hz that wastes frames; under
  240 Hz it can judder. On the Mac it is exact (display link, 144/144 and 60/60).
- **Two animating surfaces on one main thread**: with the settings window drawing the PushStrip at the
  same time, the Mac overlay dropped to 131–140 fps on 144 Hz. Everything renders on the main thread
  (README: "not a dedicated render thread").
- `run_windows` ends the process inside the event loop (Rune's fast exit). Code after
  `run_windows_with_args` never runs: "event loop ended" is never logged.
- Rune parses its own agent-tooling flags (`--snap`, `--tree`, …) from argv and rejects unknown
  ones unless handed an explicit list. Here it gets `[]`, so the app owns `--test-glow` and friends.
  That also means the headless `--snap` / `--tree` tooling isn't reachable from this binary as
  written.
- The docs disagree with the code in places, though here the code is better than the docs. The
  README says per-monitor overlays are "unsupported" and that all-Spaces / NSPanel / status items
  are "not implemented here". `OverlayOptions` supports per-display, full-screen, all-Spaces panels.
  The survey's "needs objc2 for level and Spaces" turned out wrong. Read `src/overlay/options.rs`,
  not just the README.
- No upstream issues filed (the project takes issues, not PRs). None of the above blocked the slice.

## Developer experience

Rune was the fastest of the "risky" picks to get right. The first full build of ~700 lines of view
code compiled with two warnings and ran correctly the first time: settings window, overlay on the
GN10 at 144 fps, tray. The API reads like SwiftUI written as Rust builders:
`row().key("hold").role("button").on_drag(|s: &mut Ui, d, tx| ..)`. Handlers get `&mut State`, the
view re-runs, and Rune diffs and springs the changes. Motion is free: switch knobs and the segmented
fill spring without any code. That is lovely until you want something *not* to animate. Values
driven every frame then need `tx.instant()`, or a `Live<f32>` binding (`.live_opacity(&level)`),
which skips the view entirely. The overlay glow uses `Live`, which suits exactly this kind of value.

State across windows is the part that needs thought. Each window and overlay has its own state type
and runtime. Shared data goes in an `Arc<Mutex<..>>`, and you wake the other surface by hand
(`handle.update(|_, _| {})`). `poll` hooks are plain `fn` pointers, so the timing context lives in
state. It works, but it is plumbing you write yourself.

Layout is flexbox (Taffy) with no percentages in the builder API that I found. Placing a knob at
`t` of a track was done with two `grow(t)` / `grow(1 - t)` spacers, which feels like 2012 CSS. A
`Drag` gives local `x` and `w`, so the hit maths is easy.

Docs are long, dense, agent-written, and occasionally out of date (above). The source is the real
reference, and it is well commented. Compile errors were ordinary rustc ones; nothing
macro-generated. A release rebuild takes 19 s after an edit on the Mac. The headless `Runtime`
(press keys, read `elements()` with focus flags, step frames) made a keyboard check possible
without a person. That is unusual and very useful for agent work.

The worry is not quality but provenance and churn. The repo is ~3½ weeks old, has thousands of
agent commits and hundreds of branches, has no releases, and needs five `[patch]` entries that
must move in lockstep with each bump.

## HITL checklist for Alex

On the Mac (`../run.sh rune --open-settings`), then on the PC (`Beamer bake-off\bakeoff-rune.exe`):

1. The menu-bar / tray icon is there, and no Dock icon (Mac). Windows: left-click opens Settings,
   right-click opens the menu. Mac: a click opens the menu.
2. **Test glow**: a cyan glow on the right edge of the non-main display, an amber flash, then a fade.
   Does it look smooth? (Mac 144 fps; PC ~205 fps, not vsync-locked: look for judder on the 240 Hz
   GN10.)
3. During the glow, click, scroll and hover through it, **including on the faint `OVERLAY · rune`
   label** in the middle of the screen. Does everything reach the window below?
4. Mac: put an app in full screen and switch Spaces, then Test glow. Is the glow above the app and
   the menu bar? Is it absent from Mission Control and Cmd-Tab? PC: is the overlay absent from the
   taskbar and Alt-Tab? (Its window has WS_EX_APPWINDOW set alongside WS_EX_TOOLWINDOW.)
5. **Glow on** ▸ the other display, then Test glow. The glow moves.
6. Drag the Resistance slider. The glow previews at value/500 and fades on release.
   **Hold to push** fills the strip, flashes amber at the tick and crosses.
7. Toggle **Pause crossing** in the tray and on the page. They stay in sync.
8. Switch system appearance with the window open. Does it follow live?
9. Tab through the page: is the focus ring visible and does the order make sense? Use the arrows on
   the sliders and Space on the switches. Then try VoiceOver / Narrator on the switches and sliders.
10. Close the settings window. The app keeps running. **Settings…** brings it back with the same
    state. **Quit** exits.
11. Text and colours against `assets/*-crossing-dark.png` on both OSes: Hanken Grotesk / B612 Mono
    rendering, especially on Windows.
