# Slint slice: notes (PROTOTYPE, throwaway)

Run: `prototypes/rust-ui-bakeoff/run.sh slint [--open-settings] [--test-glow] [--quit-after N]`.
PC: `sync-pc.sh slint`, then double-click `bakeoff-slint.exe` in `Desktop\Beamer bake-off`.
There is one extra debug flag, `--glow-on "<display name>"`, which overrides the default Glow on display.

Logs: `~/Library/Logs/BeamerBakeoff/slint.log`, `%LOCALAPPDATA%\BeamerBakeoff\slint.log`.

## Versions

- `slint =1.19.0`, released 2026-11-16, the day of this test. Features: `backend-winit`,
  `renderer-skia`, `accessibility`, `system-tray`, `unstable-winit-030` and `unstable-wgpu-30`.
  `slint-build =1.19.0` uses `.slint` edition 1.19.
- winit 0.30.13 (Slint's), `raw-window-handle =0.6.2`. Mac: `objc2 =0.6.5`,
  `objc2-app-kit =0.3.2`, `objc2-foundation =0.3.2`, `libc =0.2.191`. Windows: `windows =0.62.2`.
  These are the versions Slint already pulls in, so nothing is built twice.
- Nothing is patched. `Cargo.lock` is in the directory.

### Renderer: Skia, chosen deliberately

In 1.19, Skia on macOS and Windows draws through wgpu 30, to Metal on the Mac and Direct3D 12 on
Windows. Skia was picked over the alternatives for a full-screen transparent overlay that animates
at display rate:

- **Software renderer:** fills a 2560×1440 RGBA buffer on the CPU every frame, then copies it.
  That is the wrong cost model at 120–240 Hz.
- **FemtoVG (OpenGL, Slint's macOS default):** OpenGL is deprecated on macOS, and on Windows it
  presents through WGL and the redirection bitmap, which is where transparent-window bugs live.
- **Skia:** native presentation (CAMetalLayer / DXGI). The 1.19 changelog explicitly fixes
  "transparent windows with Skia … on Direct3D".

The cost is build time and size: 90 s clean on the Mac, and a 19–24 MB binary.

## Numbers

| | Mac (MacBook Air M5, 10 cores) | PC (Windows, AMD GPU) |
|---|---|---|
| Clean release build | 90 s | 155 s |
| Incremental (one `.slint` or `.rs` touched) | 4 s | 9 s |
| Release binary (stripped) | 19.3 MB | 23.9 MB |
| Startup, process start → first settings frame | 290–310 ms warm (3 runs); 564 ms on the first run of a fresh binary | 283 ms warm; 1033 ms on the first run after a build |
| …of which main() → first frame | 200–300 ms | 209 ms warm, 829 ms cold |
| Idle, tray only | RSS 97 MB; 0.01 s CPU over 20 s | WS 78 MB (private 114 MB); 0.000 s CPU over 20 s |
| Idle, settings open | RSS 106 MB; 0.00 s CPU over 20 s | WS 122 MB (private 187 MB); 0.000 s CPU over 20 s |
| Overlay fps (test glow), measured vs refresh | **60–62 fps on the GN10 (144 Hz)**; 61.8 on the built-in display (60 Hz) | **160 fps on the MSI G24C6 (144 Hz)** |

Idle CPU was read from `ps` / `Get-Process` at 10 s and 30 s. There is no idle redraw: both overlay
windows stay shown, but nothing is drawn until the glow lights.

**How fps is measured.** The app counts the overlay's `AfterRendering` callbacks from the first
frame of an animation to the last. Rust drives each animation itself: every frame samples the
time-based state machine, sets properties, then calls `request_redraw()`, and Slint's frame
throttle paces the loop.

- **Mac:** Slint paces with the NSView `CADisplayLink`.
- **Windows:** a timer at the monitor's refresh, rounded down to whole milliseconds (144 Hz → 6 ms
  → about 166 Hz). Presentation evidently doesn't block at vsync, so on the PC the count is frames
  rendered, which can be more than frames shown.

## Hard requirements: how each was met

### Click-through

- **Toolkit route:** `WinitWindowAccessor` → `winit::Window::set_cursor_hittest(false)`, which needs
  `unstable-winit-030`.
- **Mac:** this sets `ignoresMouseEvents`, read back as true.
- **Windows:** winit adds `WS_EX_LAYERED | WS_EX_TRANSPARENT`, read back from `GWL_EXSTYLE`.
- **Status:** set, logged, and the drawing checked by sampling pixel colours (below). **Not verified by a
  person** clicking through.

### Overlay shape on the Mac

These are objc2 calls on the NSWindow, reached through the raw window handle
(`platform/mac.rs`):

- `setLevel(NSStatusWindowLevel)`;
- collection behaviour `CanJoinAllSpaces | FullScreenAuxiliary | Stationary | IgnoresCycle`, read
  back as 0x151;
- no shadow, and `setHidesOnDeactivate(false)`;
- the frame set to the NSScreen frame.

The app is an accessory app (no Dock icon), set through the winit `EventLoopBuilder` that Slint
accepts. It cannot be a non-activating NSPanel: winit creates its own NSWindow subclass, and
`canBecomeKey` reads true.

All Spaces and over full-screen apps: **built, not verified.**

### Overlay shape on Windows

Raw `HWND` work in `platform/win.rs`:

- `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST` is added on top of what winit set;
- the window is placed on the monitor rectangle with `SetWindowPos(HWND_TOPMOST, …)`;
- `with_skip_taskbar(true)` is set through the global winit attributes hook.

**Bug hit:** a few seconds in, winit rewrote the extended style from its own flags and dropped
`TOOLWINDOW` and `NOACTIVATE` (the log at quit showed `0x000c0138`). That would have put the overlay
back into Alt-Tab. The fix is a `SetWindowSubclass` that adds the bits back on every
`WM_STYLECHANGING`. The log at quit now shows `0x080c01b8`, all five bits.

Per-monitor DPI comes from winit (PerMonitorV2); both PC monitors run at scale 1.0, so the
mixed-DPI case is **not exercised**.

### Glow drawn and transparent (checked by sampling colours, not by eye)

- **PC:** a scheduled-task script sampled the screen colour at the MSI's right edge every 30 ms. It
  read black, then cyan rising to (30, 66, 76), then amber (60, 40, 0) for about 120 ms, then a fade
  to black. Two points away from the edge kept their colours (for example 2, 2, 3) through the
  flash, so the rest of the overlay is transparent. Only numbers were kept, no image.
- **Mac:** a sample of the built-in display's right edge moved from (19, 19, 20) to a cyan tint
  during the glow, while the middle of the screen stayed unchanged.
- The GN10 read pure black from the Mac side the whole time. It was probably on the PC's input, as
  a shared monitor.

### Tray

This is Slint's own `SystemTrayIcon` element (1.17+), not `tray-icon`:

- AppKit `NSStatusItem` on the Mac, `Shell_NotifyIconW` on Windows;
- a `Menu` with submenus, checkable items and separators, all in `.slint` (`Tray` in `ui/app.slint`);
- **zero platform lines.**

Windows: left-click fires `clicked` (opens Settings) and right-click opens the menu, as the
spec asks. Mac: a click opens the menu.

Gaps:

- **No template-image support.** The Mac backend converts the image to a PNG NSImage and never calls
  `setTemplate`. The icon is therefore picked by `Palette.color-scheme`, which is the app's
  appearance, not the menu bar's. A wallpaper-tinted menu bar can get the wrong one.
- **No radio items.** A checkable `MenuItem` flips its own `checked` when activated, and that
  breaks any binding on it. Radio behaviour is done by rebuilding the row model from Rust after
  each pick.

No icon-creation error was logged (Slint logs one if the item fails). Whether the icon is
**visible, and how the menu behaves: not verified** (it needs a person).

### Platform code

Counted without blank lines and comments:

- `platform/mac.rs`: 97 lines. Overlay level and behaviour, NSScreen names (winit calls every Mac
  display "Monitor #<model number>"), app activation, process start time.
- `platform/win.rs`: 136 lines. Extended styles plus the subclass, monitor friendly names via
  `QueryDisplayConfig` (winit only gives `\\.\DISPLAYn`), placement, process start time.
- About 20 lines in `main.rs` use the `unstable-winit-030` accessor: the attributes hook, the
  event-loop builder and `set_cursor_hittest`.

Hot-plug: Slint and winit 0.30 report no monitor-change event; it would need
`NSApplicationDidChangeScreenParametersNotification` or `WM_DISPLAYCHANGE` by hand. Not done.

## What didn't work, or was rough

1. **The rendering notifier is silently dead without `unstable-wgpu-30`.** With Skia over wgpu,
   `set_rendering_notifier` returns `Ok`, but the surface's `with_graphics_api` is compiled out
   unless that feature is on, so the callback never runs. The first startup and fps
   measurements came out empty because of this. Found by reading
   `i-slint-renderer-skia/wgpu_30_surface.rs`. It is worth an upstream issue.
2. **60 Hz on the 144 Hz GN10 on the Mac.** Slint's macOS frame throttle is an NSView
   `CADisplayLink`, and its `request_screen_frame_rate` is a no-op on macOS (it is implemented only
   on iOS). A temporary probe showed that a raw NSView display link, and even
   `NSScreen(GN10).displayLink`, also ticked at 60 Hz in this process, with or without
   `preferredFrameRateRange`. Two explanations remain open: the GN10 was showing the PC at the time,
   or this is macOS behaviour for an unbundled process. **Compare with the other two prototypes'
   GN10 numbers before blaming Slint.**
3. **Rendering more frames than the PC monitor shows.** On Windows, Slint's timer throttle rounds the
   interval down to whole milliseconds and presentation doesn't block, so animations render at about
   160 fps on 144 Hz. That wastes power, and with tearing-free composition it brings no benefit.
4. **winit owns the window's styles** and rewrites them (the Windows bug above). Slint also re-applies
   `always-on-top` as a winit level whenever it changes. It doesn't change here, so the
   `NSStatusWindowLevel` set by hand survives.
5. **The window attributes hook is global**, one for every window. A thread-local "creating the
   overlay" flag around `Overlay::new()` tells the overlay apart.
6. **Showing a window on the Mac always calls `makeKeyAndOrderFront`** (winit `set_visible`), so the
   overlay can become the key window of the inactive app at launch.
7. **Globals are per component.** The tray, the settings window and the overlay each get their own
   copy, so "Pause crossing" is synced by hand in Rust (two callbacks). The state block is still
   one declarative string binding.

## Developer experience

- **Compile times are fine.** 90 s for the first clean build with Skia and wgpu, then about 4 s per
  change on the Mac, whether the change is in `.slint` or Rust.
- **The DSL compiler's errors are excellent.** It caught a `parent` used inside a component
  definition with a precise span, and that was the only DSL error the whole slice hit. After it, the
  Rust and the Windows code compiled first time on both machines.
- **Docs.** For the tray, the real reference was the doc comments in `builtin_elements.rs` and
  `examples/system-tray`. The website is good for the language but thin on `unstable-*` corners.
- **The `.slint` DSL reads well for this UI.** Custom controls with focus rings, keyboard handling and
  AccessKit roles (ruler slider, switch, segmented control, hold button) took about 250 lines, and
  the whole settings page is one readable file. Bindings make the live State block trivial.
  `changed pressed => …` and `animate x { … }` are pleasant. Fonts embed with one
  `import "….ttf"` line, and the variable Hanken Grotesk weights work.
- **The Rust side is plain:** generated `set_*`/`on_*` per property and callback, `Rc` and `Weak`
  for app state, `slint::Timer`, `spawn_local` to await the winit window.
- **Frame-driven animation fights the grain.** Slint wants declarative `animate`. Driving each frame
  from Rust (pressure comes from real input in Beamer) needs a rendering notifier plus a
  `request_redraw()` loop. It works and is about 30 lines, but it needed the hidden feature from
  item 1 above.
- **Is it fun?** Less "modern Rust UI" than writing views in Rust: two languages, generated code you
  don't see, and the overlay, which is the interesting part, lives entirely in escape hatches.
  But nothing fought back for long, and the result looked close to the reference screenshot on the
  first run.

## HITL checklist for Alex

1. **Mac menu-bar item.** Is it visible, and legible in a light and a dark menu bar (it is not a
   template image)? Do the "Send input to" and "Glow on" checkmarks move? Does "Pause crossing"
   follow the switch both ways?
2. **PC tray.** Left-click opens Settings and right-click opens the menu. Closing Settings keeps the
   app running, and Quit quits.
3. **Click-through, both machines.** While the glow and the faint `OVERLAY · Slint` label are lit
   (`--test-glow`), click, scroll and hover things under them.
4. **Mac Spaces and full screen.** Does the glow show over a full-screen app and on another Space?
   Is it absent from Mission Control, Cmd-Tab and the Dock?
5. **PC focus.** The overlay should never be in Alt-Tab or the taskbar. While typing in Notepad,
   `--test-glow` must not steal focus.
6. **Feel.** Hold to push (the 400 px/s rise, the amber flash, the 250 ms decay on release); the
   slider preview glow while dragging and its 250 ms fade; "Glow on" re-targeting live.
7. **Keyboard.** Tab order and the focus ring, arrows (Shift for ×10) on both sliders, Space on the
   switches and the Hold button, the ComboBox by keyboard.
8. **Appearance.** Switch the system between light and dark with the window open: it should follow
   live.
9. **GN10 refresh on the Mac.** With the GN10 on the Mac's input, run
   `run.sh slint --test-glow --quit-after 8` and read the `animation:` lines in the log: 60 or 144
   fps?
10. **Screen readers.** VoiceOver and Narrator on the sliders, switches and segmented control
    (AccessKit).
