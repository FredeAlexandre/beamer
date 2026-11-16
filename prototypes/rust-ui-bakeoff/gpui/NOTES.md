# GPUI slice: notes

Throwaway prototype for the Rust UI bake-off (see `../SPEC.md`). Binary `bakeoff-gpui`.
Run on the Mac with `../run.sh gpui [flags]`. Build on the PC with `../sync-pc.sh gpui`.
Logs: `~/Library/Logs/BeamerBakeoff/gpui.log` and `%LOCALAPPDATA%\BeamerBakeoff\gpui.log`.

## Status at a glance

"Verified" below means checked by a program (log read-back, hit-test probe, screen pixel, or a
screenshot looked at). Nothing was clicked, dragged or typed by a person.

| SPEC section | Mac | PC |
|---|---|---|
| A. Tray / menu bar | The icon shows in the menu bar (screenshot). The menu was not opened. | Created without error (`Shell_NotifyIcon` through `tray-icon`). Not seen, not clicked. |
| B. Settings window | Renders (screenshot). Startup is measured. Input is not verified. | Renders (screenshot). Input is not verified: key injection into the session failed even for Notepad. |
| C. Overlay | **Works.** Click-through is set, read back and passes the hit test (with a control run). The glow is visible (screenshot). 144 fps on the 144 Hz GN10, 60 fps on the 60 Hz built-in screen. The glow does not cover the menu-bar strip. | **Works.** `WS_EX_LAYERED \| WS_EX_TRANSPARENT` coexist with GPUI's DirectComposition swapchain. The glow pixel reaches the screen and `WindowFromPoint` passes through it (with a control run). Frames come at about 240/s even on the 144 Hz monitor. |
| C. Re-target / hot-plug | Re-targets live (built-in ↔ GN10, through a debug env var). Hot-plug is wired to `cx.observe_displays` but not exercised. | The same: re-target exercised (MSI ↔ GN10), hot-plug not exercised. |
| D. Logging and flags | Works. | Works. |

## Versions pinned

- **GPUI: `gpui-pre =0.3.12` and `gpui-pre-platform =0.3.12`** (crates.io, published 2026-11-09).
  These are weekly snapshots of Zed's `crates/gpui*`, published by huacnlee, the gpui-kit author.
  - Features: `font-kit` and `runtime_shaders`. The Mac has no `metal` compiler, so shaders compile
    at runtime. That worked first time.
  - **Why not a git dependency on zed-industries/zed:**
    - Cargo would have to fetch the whole Zed repository on both machines.
    - Zed pins toolchain 1.98.1.
    - Zed's `[patch.crates-io]` entries (async-task waker fix, calloop, …) do not apply to a
      downstream user of a git dependency anyway.
    - `gpui-pre` is the same source a week later. It resolves from crates.io, and it is the exact
      pin gpui-kit / gpui-component 0.10 use (`gpui-pre =0.3.12`), so their widgets could be added
      later without changing GPUI.
  - Zed `main` was at `12c1fc90` on 2026-11-16, where `gpui` is still `0.2.2` in its own Cargo.toml.
- `tray-icon =0.26.1` (muda 0.21.2), `raw-window-handle =0.6.2`, `futures =0.3.34`, `log =0.4.35`.
  - Mac: `objc2 =0.6.5`, `objc2-app-kit =0.3.2`, `objc2-foundation =0.3.2`, `libc =0.2.191`.
  - PC: `windows =0.62.2`.
- Rust 1.100.0 on both machines. `Cargo.lock` is committed.
- **Nothing in GPUI was patched.** Every workaround lives in `src/platform/`.

## Numbers

| | Mac (MacBook Air, Apple silicon) | PC (Windows 11, AMD GPU) |
|---|---|---|
| Clean release build | **69 s** (all dependencies, empty target dir) | **159 s** (first build there, dependencies included) |
| Rebuild after editing the app | 1.2–4 s | about 3 s |
| Release binary | **10.2 MB** (7.9 MB stripped; includes 0.4 MB of fonts) | **11.0 MB** |
| Process start → first settings frame presented | 137–145 ms warm, 404–439 ms on the first launch after a build | 115–194 ms |
| Process start → tray ready | 90–380 ms | 100–180 ms |
| Idle, tray only (RSS, CPU) | 81.5 MB, 0.002 s CPU in 31 s (about 0.01 %) | 44.7 MB working set, 0 s in 30 s, then one 47 ms tick |
| Idle, settings open | 98.5 MB, 0.02 s in 16 s untouched (about 0.1 %; more while the pointer moves over it) | 53.5 MB, 0.09 s in 30 s (about 0.3 %) |
| Overlay fps measured vs display refresh | GN10 144 Hz: **144.0 fps** (103–104 frames in 0.71 s, longest gap 7–15 ms). Built-in 60 Hz: **60.0 fps** | MSI G24C6 144 Hz: **238–240 fps**. GN10 240 Hz: **238 fps** (longest gap 5–9 ms) |

How the numbers were measured:

- **Fps.** The overlay counts its own `render` calls while an animation runs; each call asks for
  the next frame. On the Mac that means one frame per display-link tick.
- **PC fps.** GPUI paces every window from one `DwmFlush` thread. DWM composes at the fastest
  monitor's rate (the 240 Hz GN10), so the overlay on the 144 Hz MSI draws about 240 frames a second
  and roughly 40 % of them are never shown. Frames *rendered* there are not frames *presented*.
- **Idle CPU.** In-app samples every 5 s (`stats:` lines in the log). Windows CPU time has 15.6 ms
  granularity.
- **Startup.** From `main()` to GPUI's after-frame callback for the first settings frame.

## Hard requirements: how each was met

Platform code is `src/platform/`, counting lines that are not comments or blank:

- `mac.rs`: 130 lines of objc2
  - overlay flags and frame fix: about 60
  - display names and scale: 20
  - hit-test probe: 20
  - stats: 20
- `win.rs`: 186 lines of the windows crate
  - overlay flags and frame fix: about 70
  - monitor friendly names through DisplayConfig: 45
  - probes: 40
  - stats: 10
- `tray.rs`: 175 lines of tray-icon / muda, shared by both platforms.

GPUI itself gave:

- the windows
- `WindowKind::PopUp` (a non-activating NSPanel at level 101 with `CanJoinAllSpaces |
  FullScreenAuxiliary` on the Mac; `WS_EX_TOOLWINDOW | WS_EX_TOPMOST` on Windows)
- a transparent background
- `display_id`
- the display list with refresh rates
- `observe_displays` (hot-plug)
- `observe_window_appearance`
- a raw window handle

### Click-through

Click-through is done by hand on both platforms, as expected.

- **Mac.** `setIgnoresMouseEvents:YES` on the NSWindow (from the raw NSView), read back as `true`.
  `+[NSWindow windowNumberAtPoint:belowWindowWithWindowNumber:]` ("the window that would be hit by a
  mouse-down") at the glow edge returns the window underneath. **Control:**
  `BAKEOFF_NO_CLICK_THROUGH=1` skips the call, and the same probe then returns the overlay. Status:
  set, read back, hit test verified. A person has not clicked through it.
- **PC.** `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE` are added to GPUI's
  `TOOLWINDOW | TOPMOST | NOREDIRECTIONBITMAP`, plus `SetLayeredWindowAttributes(alpha 255)`.
  - **The open question from the survey is answered: they coexist with the DirectComposition
    swapchain.** In the desktop session, a `BitBlt(CAPTUREBLT)` of the edge pixel reads `#8c5d00`
    during the flash and `#000000` when idle, so the glow reaches the screen. `WindowFromPoint`
    there returns the Chrome or desktop window underneath.
  - **Control:** with `BAKEOFF_NO_CLICK_THROUGH=1`, `WindowFromPoint` returns the overlay (class
    `Zed::Window`). Status: set and probe-verified. A person has not clicked through it.

### Overlay geometry

GPUI's `PopUp` lands in the wrong place on both platforms, so both needed fixing.

- **Mac.** GPUI's panel has the `Titled` style bit, so AppKit constrains it below the menu bar:
  30 pt down on the GN10, with its bottom 30 pt off-screen. A plain `setFrame` is constrained again.
  - Fix: set the style mask to borderless + non-activating, then `setFrame` to the `NSScreen` frame.
  - This **must run outside GPUI's App borrow.** AppKit calls GPUI's resize and move handlers
    synchronously, and inside `open_window` GPUI logs `RefCell already borrowed` and drops the
    events. It runs from a spawned task instead.
- **PC.** The overlapped window GPUI makes (style 0, frame hidden in `WM_NCCALCSIZE`, placed with
  border offsets) came out 1936×1088 at (2552,353) for a 1920×1080 monitor at (2560,357). Its client
  area sat 4 px too high and missed the bottom 4 rows.
  - Fix: `WS_POPUP` (keep `WS_VISIBLE`, or GPUI's vsync loop runs unthrottled at 5,000 fps; seen
    once) and `SetWindowPos` to the monitor rect, also from a spawned task.
  - Per-monitor-v2 DPI awareness is on (GPUI's manifest). Both PC monitors are at 100 %, so mixed DPI
    was not exercised.

### All Spaces and full screen (Mac)

- Collection behaviour is set to `CanJoinAllSpaces | FullScreenAuxiliary | Stationary |
  IgnoresCycle` (read back `0x151`), level `NSStatusWindowLevel` (25), no shadow, excluded from the
  Windows menu. The app is `ActivationPolicy::Accessory`, so it has no Dock icon.
- Set and read back only. Behaviour over a full-screen app and in Mission Control is **not
  verified**.
- **Finding:** in screenshots on this macOS, the glow stops at the bottom of the menu bar. That held
  at levels 25, 101, 1000 and 2147483628, while the window frame demonstrably covers the strip. Ask
  a person whether the real screen shows the same.

### Tray

- Made with `tray-icon`. The icon is an RGBA glyph drawn at runtime, a template image on the Mac.
  The menu has two submenus of check items used as radios (muda has no radio items, and it flips a
  check item by itself on click, so the code re-syncs them).
- Events arrive through `tray-icon`'s global handlers on the main thread, from the AppKit or Win32
  loop GPUI is already running. A futures channel forwards them into GPUI's foreground executor.
  **No extra pumping was needed on either OS.**
- Tray state stays in sync both ways: the tray observes the shared state entity.
- Left-click on Windows opens Settings (`with_menu_on_left_click(false)`).
- The *Glow on* submenu is rebuilt when displays change.

## What didn't work or was rough

- **No widgets in GPUI.** Slider, switch, segmented control, dropdown, button and focus ring are all
  hand-built from `div`, `canvas`, `deferred(anchored())` and listeners: 760 lines in
  `settings.rs`. gpui-component has them all at the same pin.
  - A slider drag that continues outside the track needs window-level `window.on_mouse_event` inside
    a `canvas` paint callback. `on_mouse_move` only fires while hovered.
- **Variable fonts on the Mac.** Hanken Grotesk is a variable font. The macOS text system
  (font-kit / CoreText) renders only its default instance, so every `font_weight(BOLD)` comes out
  regular on the Mac. DirectWrite on the PC renders the weights (compare the screenshots). The fix
  would be static instances of the font.
- **Styling gaps.**
  - No letter-spacing, so the tracked eyebrows are not tracked.
  - No per-side border colours (`border_l_color` doesn't exist).
  - `items_baseline` did not line up the 34 px readout with its 13 px unit.
- **Platform inconsistencies GPUI does not hide:**
  - `DisplayId` is a CGDirectDisplayID on the Mac and an HMONITOR on Windows.
  - `PlatformDisplay::bounds()` is display-local (origin 0,0) on the Mac and global on Windows.
  - There is no display name and no scale factor, so names come from `NSScreen.localizedName` and
    DisplayConfig, and scale from `backingScaleFactor` / `GetDpiForMonitor`.
- **Re-entrancy.** Any native call that makes the OS call back into GPUI synchronously (resize,
  move) fails while you are inside a GPUI callback. The workaround is a spawned task, which runs
  outside the App borrow.
- **Windows over-rendering.** One global vsync thread at the fastest monitor's rate, as described
  under Numbers.
- **Frame throttling defaults.** `WindowOptions` throttles inactive windows to about 30 fps by
  default. The overlay was not throttled in practice (144 fps with the defaults:
  `BAKEOFF_DEFAULT_FRAME_CAPS=1`), but the caps are turned off for it anyway.
- `Window::window_handle()` is GPUI's own `AnyWindowHandle` and shadows the raw-window-handle trait
  method. Call `HasWindowHandle::window_handle(window)`.

Not found upstream as issues within the time box; no links.

## Developer experience

The good:

- Compile times are the pleasant surprise. Zed's crates built from nothing in 69 s on the Mac
  (159 s on the PC), and an edit to the app rebuilds in 1–4 s.
- The first Windows build compiled with zero errors from code written only against the macOS
  build, and everything ran first time except the two geometry bugs.
- Error messages are plain rustc. The one trap was `.when()` needing `gpui::prelude::FluentBuilder`
  in scope.
- The code reads like Tailwind in Rust (`div().flex().px(px(16.)).bg(p.panel)`). It is pleasant
  once you know the method names. They match Tailwind's, so guessing mostly works.
- State is the nicest part:
  - one `Entity<AppState>` that the overlay, the settings window and the tray each `observe`;
  - `cx.notify()` re-renders whoever watches;
  - animation is a pure function of time (`Phase::sample(now)`) that each window samples at its own
    frame rate, asking for more with `window.request_animation_frame()`.
- Idle really is idle: nothing renders unless notified.

The hard:

- The docs are thin: the README, a handful of short guides and about 45 examples. For window kinds, display
  coordinates, focus, frame pacing and what `PopUp` does on each OS, the answer was always to read
  GPUI's platform source. It is well commented, and reading it is how the geometry bugs were found.
- Because there are no widgets, the Crossing page is mostly control-building, not product UI.
  Without gpui-component, expect to write and own a widget kit.
- The hand-built controls have no accessibility labels. GPUI has AccessKit, but nothing here uses
  it.
- Fun: yes, once past the widget gap. Painful: platform geometry and the borrow re-entrancy, both
  invisible until you look at real pixels and logs.

## Debug switches (prototype only)

- `BAKEOFF_NO_CLICK_THROUGH=1`: the control run for the probes.
- `BAKEOFF_STATIC_GLOW=1`: a still, full glow, for screenshots.
- `BAKEOFF_RETARGET=1`: switch *Glow on* to the next display after 2.5 s.
- `BAKEOFF_DEFAULT_FRAME_CAPS=1`: keep GPUI's frame caps on the overlay.
- `BAKEOFF_SETTINGS_H=<px>`: settings window height.

The log also records every key that reaches the settings window (`key down:`).

## HITL checklist for Alex

1. **Mac menu bar.** Click the icon (a circle and a bar), then try each item:
   - *Send input to* radios;
   - *Pause crossing* (check it flips the switch in Settings, and the switch flips the menu);
   - *Glow on* GN10 ↔ Built-in (the glow moves);
   - *Test glow*;
   - *Settings…* (opens, or raises an open window);
   - *Quit*.
2. **PC tray.** Left-click opens Settings, right-click opens the menu. Try the same items there.
3. **Close the settings window** on each machine: the app must keep running.
4. **Click, scroll and hover through the glow**, on the bright edge *and* on the faint centred
   `OVERLAY · gpui` label, on both machines. Use Test glow in a loop with `--test-glow`.
5. **Drag the Resistance slider.** The glow previews at value/500 and fades over 250 ms on release.
   Then **Hold to push**: the strip fills, crosses, flashes amber and drops. Release early: the
   pressure decays. Judge the feel.
6. **Keyboard.**
   - Tab and Shift-Tab: is the focus ring visible?
   - Arrows on the sliders (Shift steps ×10).
   - Space on the switches and on Hold to push (hold it).
   - Space or Enter on the dropdown, Up/Down to pick, Escape to close.
   - Left/Right on Double-tap / Hold.
   - The log shows which keys arrived.
7. **Mac Spaces and full screen.** Make an app full screen on the GN10 and run Test glow. Switch
   Spaces. Open Mission Control: the overlay must not appear. **Check whether the glow covers the
   menu-bar strip** at the top of the edge (in screenshots it does not).
8. **Light/dark.** Switch the system appearance with Settings open: the page should follow live.
9. **Smoothness.** 144 Hz GN10 on the Mac. On the PC, the 144 Hz MSI renders at 240 fps: does it
   judder? Compare with the glow on the 240 Hz GN10.
10. **Looks.** Compare against `assets/mac-crossing-dark.png`. On the Mac the bold weights are
    missing (variable font). Check the dropdown popup and the page scroll.
11. **Hot-plug (bonus).** Unplug and replug a monitor. The log should show `displays changed` and
    `tray: Glow on menu rebuilt`. The glow should stay on, or move to, a connected display.
