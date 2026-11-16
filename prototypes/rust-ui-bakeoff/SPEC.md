# PROTOTYPE: Rust UI toolkit bake-off (throwaway, never merged)

Answers the wayfinder ticket **"UI prototype bake-off between the top Rust toolkits"**
(FredeAlexandre/beamer#12): *which of the shortlisted toolkits feels right once Alex has used it?*

Three toolkits build **the same slice**, so Alex compares toolkits, not designs:

| Dir | Toolkit | Why it is here (from the survey, `research/rust-ui-toolkits`) |
|---|---|---|
| `rune/` | Rune (anomalyco/rune), git only | built-in per-display click-through overlays, AccessKit, i18n; 3 weeks old, no releases, no macOS tray |
| `gpui/` | GPUI (Zed) | proven by Zed, AccessKit, `WindowKind::PopUp`; click-through and tray by hand |
| `slint/` | Slint 1.x | the mature control: tray, AccessKit, gettext; click-through only via `unstable-winit-030` |

Each is a standalone cargo project (no shared workspace, no shared crate), binary `bakeoff-<toolkit>`.

## The slice

### A. Tray (Windows) / menu-bar item (macOS)

Icon: a simple glyph (a small filled circle with a vertical bar is fine; template image on the Mac).
No Dock icon on the Mac (accessory activation policy). Menu:

- **Send input to** ▸ radio: `This Mac` / `Desk PC` on the Mac, `This PC` / `Studio Mac` on Windows (fake, state only)
- **Pause crossing** (checkbox; the same state as the switch on the settings page, kept in sync both ways)
- separator
- **Glow on** ▸ radio, one item per connected display, by its name (e.g. `Color LCD`, `GN10`); default: the first display that is not the main one, else the main one
- **Test glow** (plays the push-and-cross sequence below once)
- separator
- **Settings…** (opens or raises the settings window)
- **Quit**

Windows: left-click on the icon opens Settings, right-click opens the menu. Mac: click opens the menu.
Closing the settings window keeps the app running.

### B. Settings window

Title `Beamer`, 900×640 default, minimum 640×540. Follows the system light/dark appearance live.
Colours and type from the repo-root `tokens.py` (`PALETTE`, `PALETTE_LIGHT`, `TYPE`): copy the values,
don't import. Fonts: Hanken Grotesk (UI) and B612 Mono (readouts) from `win_app/assets/` (copy the
TTFs into the prototype's `assets/` and embed them). Visual reference: `assets/mac-crossing-dark.png`,
`assets/windows-crossing-dark.png` (same layout: sidebar left, scrolling page right, footer).

**Sidebar** (150–180 wide): a link block at the top (signal-coloured dot, `On Mac` / `On PC` big,
`Desk PC` / `Studio Mac` under it, `Response time: 3 ms` small); rows **Overview, Crossing, Keyboard,
Design** with Crossing selected; the others show only a page title and the line *Not in this
prototype.* Footer: `Beamer bake-off · <toolkit>`.

**Crossing page** (scrolls), modules on `panel` over `ground`:

1. **Resistance**
   - A ruler slider 0–500 (the push, in px, before the pointer crosses), default 120.
   - Its value as a big B612 Mono readout (`TYPE.readout`) with `px` after it.
   - A **PushStrip**: a horizontal bar showing live pressure (0 → resistance) as a fill in `signal`,
     with a tick at the resistance.
   - A **Hold to push** button: while pressed, pressure rises at 400 px/s; when it reaches the
     resistance it **crosses** (strip flashes `resistance_flash` for 120 ms, then pressure drops to 0);
     on release before crossing, pressure decays to 0 over 250 ms.
   - **While the slider is being dragged**, the overlay glow lights at intensity `value / 500` as a
     preview; it fades out over 250 ms on release.
2. **This machine's pointer**: switch *Don't cross while dragging*; switch *Pause crossing* (synced with the tray).
3. **Shortcut**: a keycap showing `⌃ Control` (display only); segmented control *Double-tap / Hold*;
   slider *Time between taps* 150–600 ms (disabled when Hold is picked); dropdown *Shortcut goes to*:
   `Last machine` / `Next in order` / `Next among ticked`.
4. **State** (the prototype surfaces its state): a B612 Mono block, live, listing every setting above,
   the current pressure, the overlay's display name and frame, click-through on/off, the overlay's
   **measured frame rate during the last animation** and the display's refresh rate, and the
   startup time (process start → first settings frame).

Keyboard: Tab moves focus through the controls with a visible focus ring, arrows move sliders, Space
toggles switches. Record honestly in NOTES.md what works.

### C. Edge-glow overlay (the hard requirement)

One overlay window on the **Glow on** display, covering the **whole display**:

- borderless, transparent, always on top, **click-through** (every click, scroll and hover passes to
  whatever is under it, including where the overlay draws), never takes focus, not in the
  taskbar / Dock / Alt-Tab / Mission Control;
- macOS: above normal windows and the menu bar (status level or higher), on **all Spaces** and over
  **full-screen apps** (`canJoinAllSpaces | fullScreenAuxiliary | stationary | ignoresCycle`), a
  non-activating panel if the toolkit allows;
- Windows: `WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`
  or whatever achieves the same with the toolkit's renderer; per-monitor DPI correct.

It draws a **glow along the display's right edge**: `signal` (#5fd4f4, the dark palette whatever the
window shows), full height, alpha falling off from the edge over ~64 logical px, overall intensity =
pressure. While anything is lit it also draws a faint centred label `OVERLAY · <toolkit>` at 8% alpha,
so Alex can check clicks pass through drawn pixels too.

**Push-and-cross sequence** (Test glow, `--test-glow`, and Hold to push all use it): intensity follows
pressure up; at the crossing it flashes to full in `resistance_flash` (#b77a00) for 120 ms, then fades
to 0 over 300 ms.

Animate at the display's frame rate (vsync, 120 Hz on the GN10 if it allows, ProMotion-aware on the
built-in screen); **measure** the frames actually presented per second during each animation and log
it. When nothing is lit, render nothing (no idle redraw loop): idle CPU should be ~0.

Re-target the overlay live when **Glow on** changes. Monitor hot-plug is a bonus, not required: note
whether the toolkit can see it.

### D. Logging and flags

Log to `~/Library/Logs/BeamerBakeoff/<toolkit>.log` (Mac) and `%LOCALAPPDATA%\BeamerBakeoff\<toolkit>.log`
(Windows): startup time, displays found (name, frame, scale, refresh), the overlay window and the flags
it got, each animation's measured fps, errors.

Flags: `--test-glow` (play the sequence 1 s after start, then every 3 s), `--quit-after <secs>`,
`--open-settings` (open the settings window at start; without it the app starts in the tray only).

## Rules

- **Throwaway.** No tests, no error handling beyond running, no abstractions. But **don't fake**: if
  something can't be made to work in reasonable time, leave it visibly unfinished and say so in
  NOTES.md. A failure is a finding.
- Platform escape hatches (`objc2*`, `windows` crate, `tray-icon`, `raw-window-handle`) live in
  `src/platform/{mac,win}.rs`. Count their lines in NOTES.md: how much the toolkit leaves to you is
  the point.
- Pin exact versions / git revs in `Cargo.toml`, commit `Cargo.lock`.
- macOS: no full Xcode on this Mac (Command Line Tools only, so no `metal` compiler). Rust is in
  `~/.cargo/bin` (not on PATH by default: `export PATH="$HOME/.cargo/bin:$PATH"`).

## NOTES.md (one per toolkit): what Alex reads next to the running app

- Versions / revs pinned; what had to be patched.
- Numbers, Mac and PC: clean release build time, release binary size, startup → first frame, idle
  RSS and CPU (tray only; settings open), overlay fps measured vs display refresh.
- Hard requirements: click-through, all Spaces / over full screen, tray: **how** each was met
  (toolkit API or escape hatch, lines of platform code) and whether verified or only built.
- What didn't work or was rough; bugs hit (with links to upstream issues if found).
- **Developer experience**, honestly: compile times, error messages, docs, examples, how the code
  reads, how state and animation are expressed, anything that was fun or painful. Alex wants to play
  with modern Rust UI; this paragraph matters as much as the numbers.
- What Alex should try by hand (the HITL checklist): things only a person can judge.

## Running

- Mac: `prototypes/rust-ui-bakeoff/run.sh <toolkit> [flags]`
- PC: `prototypes/rust-ui-bakeoff/sync-pc.sh <toolkit>` copies the sources to the PC and builds
  there; the exe lands in `C:\Users\Alex\Desktop\Beamer bake-off\`, where Alex double-clicks it.
  Over SSH a GUI can't start (session 0); to smoke-test, `sync-pc.sh <toolkit> --smoke` runs it
  through a one-shot scheduled task in Alex's desktop session with `--test-glow --quit-after 8` and
  prints its log.
