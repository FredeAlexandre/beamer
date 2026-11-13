# Rust UI toolkits for Beamer

Research for [#5](https://github.com/FredeAlexandre/beamer/issues/5), part of the Rust port map
([#1](https://github.com/FredeAlexandre/beamer/issues/1)). Surveyed 2026-11-13 from repositories,
source files, changelogs, docs.rs and crates.io. Every claim links to the file or page it came from.
Star counts, push dates and release dates come from the GitHub and crates.io APIs on that day.

## Answer

No candidate fails the hard requirements outright. Two crates act as a universal escape hatch for
every toolkit that runs a native event loop on the main thread:

- [`winit`](#the-escape-hatch-every-toolkit-shares)'s `set_cursor_hittest(false)` gives a
  click-through window.
- [`tray-icon`](https://github.com/tauri-apps/tray-icon) gives a tray or menu-bar icon.

So the toolkits were separated on the scored criteria and on how much of the overlay work they do
natively.

**Shortlist for the prototype bake-off:**

1. **Rune** ([anomalyco/rune](https://github.com/anomalyco/rune)). It exists, and it is a UI
   toolkit: "A native GPU-rendered Rust UI framework for macOS, Windows, Linux and the web". It is
   the only candidate with first-class, per-display, click-through overlay windows, plus AccessKit,
   an i18n crate and headless agent tooling. It is also three weeks old and pre-release.
2. **GPUI** (Zed). It is battle-tested by shipping Zed on all three platforms and has AccessKit.
   Its `PopUp` window kind is already a non-activating, all-Spaces panel. Click-through and the tray
   need the escape hatch, it has no i18n, and its current version is not on crates.io.
3. **Slint**, the control. It meets the most must-haves out of the box: a built-in tray on macOS and
   Windows, multi-window, transparency, always-on-top, AccessKit, gettext `.po` translations like
   today's, and stable 1.x releases. Click-through is only reachable through an `unstable-` winit
   accessor. It is the safe fallback if both modern toolkits fail the bake-off.

**Freya** is the runner-up, should Alex prefer pure-Rust components to Slint's DSL. **Iced, egui,
Dioxus Native/Blitz, Makepad, Xilem, Floem, Vizia, Ribir, Kas and the webview stacks** are not
shortlisted. [The reasons are below](#not-shortlisted-and-why).

## What Beamer's UI needs (from today's code)

These requirements are read from the current apps:

- **Overlays.** Today these are the edge glow, crossing effects, cursor overlay, identify-screens
  labels and the notch island.
  - On the Mac, `mac_app/effects_overlay.py` opens one borderless `NSPanel` per display (because
    "a window spanning two displays shows on only one of them while displays have separate
    Spaces"). Each panel is `NonactivatingPanel`, at `NSStatusWindowLevel`, with
    `setIgnoresMouseEvents_(True)`, and joins all Spaces with `CanJoinAllSpaces |
    FullScreenAuxiliary | IgnoresCycle | Stationary`. Frames are driven by a display-refresh
    `FrameClock`, and `mac_app/frame_clock.py` caps previews at 60 on ProMotion panels.
  - `mac_app/notch_island.py` animates a shape over the notch with Core Animation
    `CASpringAnimation`.
  - On Windows, `win_app/cursor_overlay.py` and `win_app/edge_glow.py` are frameless Qt tool windows
    with `WindowTransparentForInput | WindowDoesNotAcceptFocus | WA_TranslucentBackground`. They
    re-assert `HWND_TOPMOST` with `SetWindowPos`.
  - All overlay painting is CPU-side today: Core Graphics replays of `core.effects` Pens, and
    QPainter.
- **Tray / menu bar.** The Mac uses `rumps` plus a custom `NSStatusItem` click handler
  (`mac_app/kvm_bridge_app.py`). Windows uses `QSystemTrayIcon` with a `QMenu` that has
  **submenus** (`win_app/kvm_bridge_win.py`). The icon is a **glyph drawn at runtime** from the
  drive state, with a status badge (`win_app/tray_glyph.py`), so the tray must accept RGBA icons
  that change.
- **Several windows**: the main window, settings pages and design previews, alongside the overlays.
- **Accessibility**: 113 `setAccessibility*` calls in `mac_app` and 78
  `setAccessibleName/Description` calls in `win_app`. Screen-reader labels are already a habit.
- **i18n**: gettext catalogues (`core/i18n.py`, `locales/beamer.pot`, `locales/it.po`), with
  British English as the source.
- **Licence**: Beamer is GPL-3.0. Every candidate licence below is compatible.

## The escape hatch every toolkit shares

Every winit-based toolkit (Rune, Slint, Iced, egui, Freya, Dioxus Native, Xilem, Vizia) can reach
the underlying `winit::Window`:

- `set_cursor_hittest(false)` "passes [events] through the window such that any other window behind
  it receives them". It is unsupported only on iOS, Android, Web and Orbital
  ([winit 0.30.13 `src/window.rs`](https://github.com/rust-windowing/winit/blob/v0.30.13/src/window.rs)).
- On macOS it calls `setIgnoresMouseEvents(!hittest)`
  ([`window_delegate.rs`](https://github.com/rust-windowing/winit/blob/v0.30.13/src/platform_impl/macos/window_delegate.rs)).
- On Windows it adds `WS_EX_TRANSPARENT | WS_EX_LAYERED`
  ([`window_state.rs`](https://github.com/rust-windowing/winit/blob/v0.30.13/src/platform_impl/windows/window_state.rs)).

winit's `WindowLevel::AlwaysOnTop` maps to `kCGFloatingWindowLevel` on macOS. That is **below** the
`NSStatusWindowLevel` (25) Beamer uses, and winit sets no all-Spaces or full-screen-auxiliary
behaviour. **Any toolkit except Rune therefore needs a few lines of `objc2` on the raw `NSWindow`**
(level, collection behaviour) to match today's Mac overlays. They reach it through
`raw-window-handle`.

For the tray, [`tray-icon`](https://github.com/tauri-apps/tray-icon) (0.26.1, 2026-10-07; menus
from `muda` 0.21.2) supports Windows, macOS and Linux. It needs "an event loop … running on the
thread" on Windows and the main thread on macOS, and documents forwarding events through winit's
`EventLoopProxy` ([README](https://github.com/tauri-apps/tray-icon/blob/dev/README.md)). Every
candidate runs such a loop.

That means the overlays could even be a toolkit-independent layer of raw per-display panels or
HWNDs drawn with wgpu. The criterion "can sit next to raw platform windows rendered with wgpu"
matters for exactly this reason.

## Hard requirements

| Toolkit | Transparent, click-through, topmost overlay on any monitor | Tray / menu bar with menu | Several windows |
|---|---|---|---|
| **Rune** | **Native.** See the table notes. | Native on **Windows and Linux**. On macOS, `tray.rs` says "macOS isn't supported yet", so the Mac needs `tray-icon` or `objc2` `NSStatusItem` ([tray.rs](https://github.com/anomalyco/rune/blob/dev/crates/rune/src/tray.rs)). The menu is flat: `Action` and `Separator` only, no submenus. | Native: `run_windows`, `App::window`, `App::overlay`, with different state types per window. |
| **GPUI** | **Partial.** See the table notes. | None in GPUI. Use `tray-icon`. | Native (Zed opens many windows). |
| **Slint** | **Partial.** The `Window` element has `always-on-top`, `no-frame` and `background`, but "no property exists" for click-through ([Window docs](https://docs.slint.dev/latest/docs/slint/reference/window/window/)). Click-through needs `WinitWindowAccessor::with_winit_window`, behind the `unstable-winit-030` feature ([docs.rs](https://docs.slint.dev/latest/docs/rust/slint/winit_030/trait.WinitWindowAccessor)). Transparent-window fixes for Skia and FemtoVG-WGPU on Direct3D are in the unreleased 1.19 changelog. | **Native**: the `SystemTrayIcon` element (added in 1.17.0, 2026-06-24) uses AppKit `NSStatusItem` on macOS, `Shell_NotifyIconW` on Windows and `ksni` on Linux ([system_tray.rs](https://github.com/slint-ui/slint/blob/master/internal/core/items/system_tray.rs), [example](https://github.com/slint-ui/slint/tree/master/examples/system-tray)). | Native. |
| **Freya** | Transparency is native (`with_transparency`, plus a `feature_transparent_window` example). Click-through and level go through the `with_window_attributes` / `with_window_handle` winit hooks ([config.rs](https://github.com/marc2332/freya/blob/main/crates/freya-winit/src/config.rs)). | **Native** behind the `tray` feature ([Cargo.toml](https://github.com/marc2332/freya/blob/main/crates/freya/Cargo.toml)). | Native (`feature_multi_window`, `feature_multi_monitor` examples). |
| **Iced** | **Native**: `window::Settings { transparent, level }`, `window::set_level`, `window::enable_mouse_passthrough` ([runtime/src/window.rs](https://github.com/iced-rs/iced/blob/master/runtime/src/window.rs)). Raw handle through `window::run`. | None. Use `tray-icon`. | Native: `Daemon` and multi-window since 0.12/0.13 ([CHANGELOG](https://github.com/iced-rs/iced/blob/master/CHANGELOG.md)). |
| **egui / eframe** | **Native**: `ViewportBuilder::with_transparent`, `with_window_level`, `with_mouse_passthrough` ([viewport.rs](https://github.com/emilk/egui/blob/main/crates/egui/src/viewport.rs)). | None. Use `tray-icon`. | Native (viewports). |
| **Dioxus Native / Blitz** | Through winit `WindowAttributes` in `blitz-shell`'s `WindowConfig::with_attributes` ([window.rs](https://github.com/DioxusLabs/blitz/blob/main/packages/blitz-shell/src/window.rs)). | None. `muda` is for menus. Use `tray-icon`. | `blitz-shell` keeps a `HashMap<WindowId, View>`. |
| **Makepad** | Own platform layer, not winit. The AppKit backend has transparency, `setLevel` and `CanJoinAllSpaces`. Click-through (`setIgnoresMouseEvents`) appears only as a transient state before the first frame ([appkit/window.rs](https://github.com/makepad/makepad/blob/main/platform/hal/src/backend/apple/appkit/window.rs)). **No public raw-window-handle access was found**, so the escape hatch is not clean. | Native (a `examples/tray` example on macOS and Windows). | Native. |
| **Xilem / Masonry** | winit underneath. | None. | "Initial support for using Masonry with multiple windows" ([v0.4.0 notes](https://github.com/linebender/xilem/releases/tag/v0.4.0)). |
| **Tauri (webview)** | `set_ignore_cursor_events` and `set_always_on_top` on `WebviewWindow` ([docs.rs, tauri 2.12.5](https://docs.rs/tauri/latest/tauri/webview/struct.WebviewWindow.html)). | Native. | Native. |

Table notes:

- **Rune's overlays.** `WindowOptions::overlay(display)` gives a "transparent, borderless, topmost,
  click-through presentation at that display's physical origin and size". `WindowHandle` toggles
  click-through and topmost at runtime and lends the raw window handle. `App::overlay` puts an edge
  HUD in a non-activating `NSPanel` "at status level (25) on every Space incl. full-screen ones" on
  macOS, and in a `WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED` topmost tool window on
  Windows. Sources: [crates/rune/README.md](https://github.com/anomalyco/rune/blob/dev/crates/rune/README.md),
  "Multiple windows and overlays" and "Edge overlays". There is a per-monitor overlay example with a
  shared remote cursor: [examples/overlay](https://github.com/anomalyco/rune/tree/dev/examples/overlay).
- **GPUI's overlays.** `WindowBackgroundAppearance::Transparent`, `display_id` and
  `WindowKind::PopUp` ([platform.rs](https://github.com/zed-industries/zed/blob/main/crates/gpui/src/platform.rs)).
  On macOS, `PopUp` is a `NonactivatingPanel` at `NSPopUpWindowLevel`, with `CanJoinAllSpaces |
  FullScreenAuxiliary`
  ([gpui_macos/src/window.rs](https://github.com/zed-industries/zed/blob/main/crates/gpui_macos/src/window.rs)).
  On Windows it is `WS_EX_TOOLWINDOW | WS_EX_TOPMOST`
  ([gpui_windows/src/window.rs](https://github.com/zed-industries/zed/blob/main/crates/gpui_windows/src/window.rs)).
  **There is no click-through option.** `PlatformWindow: HasWindowHandle`, so it is set by hand.
  GPUI does not use winit.

## Scores

Legend: ✅ good · ➖ partial or self-reported · ❌ missing.

| | Rune | GPUI | Slint | Freya | Iced | egui | Dioxus Native | Makepad |
|---|---|---|---|---|---|---|---|---|
| **Screen readers** | ✅ AccessKit (default `accessibility` feature): UIA, NSAccessibility, AT-SPI | ✅ AccessKit (`accesskit_macos`, `accesskit_windows`), with a guide in `_accessibility.rs` | ✅ AccessKit via `accesskit_winit` | ✅ AccessKit (default) | ❌ none ([#552](https://github.com/iced-rs/iced/issues/552) open) | ✅ AccessKit (eframe default) | ✅ AccessKit | ➖ own `libs/a11y` layer, not AccessKit |
| **i18n** | ✅ `rune-i18n`: JSON catalogues checked at build time, ICU4X plurals, OS locale negotiation | ❌ none in GPUI | ✅ `@tr()`, `slint-tr-extractor` → `.pot`/`.po`, gettext or bundled ([guide](https://github.com/slint-ui/slint/blob/master/docs/astro/src/content/docs/guide/development/translations.mdx)) | ✅ `freya-i18n` (Fluent) | ❌ | ❌ | ❌ | ❌ not found |
| **Maturity** | ❌ created 2026-10-22; **no releases** ("Rune is pre-release. Pin a rev of `dev`"); 2,317 commits; 7.0k★ | ➖ ships in Zed (v1.28.1, 2026-11-11); crates.io `gpui` 0.2.2 is from 2025-10-22, and the current split (`gpui` + `gpui_platform`) is not published: `gpui_platform` is 404 on crates.io. A third-party snapshot `gpui-pre` 0.3.12 (owner `huacnlee`, the gpui-kit author) fills the gap | ✅ 1.x; 1.15 → 1.18.1 between Feb and Sep 2026; 24.6k★ | ✅ 0.5.2 (2026-11-11), monthly RCs; 3.4k★ | ➖ last release 0.14.0 (2025-12-07); master active; 32k★ | ✅ 0.37.0 (2026-10-15), about monthly; 32k★ | ❌ blitz is "beta"; Dioxus 0.8 is alpha | ➖ crates.io 1.0.0 (2025-05-13); `main` has moved far from it |
| **Docs** | ✅ a 2,976-line crate README, examples, playground; written for agents | ➖ README plus two guides; gpui-kit adds 75+ documented components | ✅ full docs site, LSP, live preview | ✅ docs.rs, book-style README | ✅ book plus docs.rs | ✅ | ➖ | ➖ |
| **Licence** | MIT | Apache-2.0 (the crate; Zed itself is mixed) | GPL-3.0 / royalty-free 2.0 (attribution) / commercial. **GPL-3.0 fits Beamer** ([LICENSE.md](https://github.com/slint-ui/slint/blob/master/LICENSE.md)) | MIT | MIT | MIT/Apache-2.0 | MIT/Apache-2.0 | MIT |
| **Binary size** | ➖ self-reported 9.3 MB stripped for `examples/settings`, 1.2 MB of it fonts | not published | not published for desktop | not published (bundles Skia) | not published | not published | not published | not published |
| **Next to raw wgpu windows** | ✅ wgpu 30 inside (a custom Metal backend by default on macOS, `RUNE_GPU_BACKEND=wgpu` to opt out); compositor hook; `NativeSurface` imports IOSurface and D3D12 shared textures; lends raw window handles | ➖ own Metal (macOS) and DirectX/DirectComposition (Windows) renderers; wgpu only on Linux/web (`gpui_wgpu`). Raw windows next to it are fine, but shared textures don't go through wgpu | ✅ `unstable-wgpu-30`: render into wgpu and import wgpu textures (1.18 notes) | ➖ Skia | ✅ wgpu-native, `Shader` widget | ✅ `egui-wgpu` paint callbacks | ➖ Vello on wgpu; custom wgpu paint not verified | ❌ own Metal/D3D11 HAL |
| **Fun / modern** | High: `fn view(&State) -> El` builders, springs, headless PNG/tree rendering, live Rust editing | High: Tailwind-like `div().flex()…`, entity model, the reference "web-to-native" toolkit | Medium: declarative `.slint` DSL plus Rust; strong tooling | High: React-hooks-style components in Rust | Medium: Elm architecture | Medium: immediate mode | High for web people: RSX/HTML/CSS | Medium-high: live DSL, but a sprawling scope |

Sources for the scores:

- **Rune**: [crates/rune/README.md](https://github.com/anomalyco/rune/blob/dev/crates/rune/README.md)
  ("Accessibility", "Measured", "Depending on Rune"),
  [Cargo.toml](https://github.com/anomalyco/rune/blob/dev/crates/rune/Cargo.toml),
  [rune-i18n](https://github.com/anomalyco/rune/blob/dev/crates/rune-i18n/src/lib.rs),
  [CHANGELOG](https://github.com/anomalyco/rune/blob/dev/CHANGELOG.md).
- **GPUI**: [README](https://github.com/zed-industries/zed/blob/main/crates/gpui/README.md),
  [Cargo.toml](https://github.com/zed-industries/zed/blob/main/crates/gpui/Cargo.toml),
  [`_accessibility.rs`](https://github.com/zed-industries/zed/blob/main/crates/gpui/src/_accessibility.rs),
  crates.io [`gpui`](https://crates.io/crates/gpui) and [`gpui-pre`](https://crates.io/crates/gpui-pre),
  [gpui-kit Cargo.toml](https://github.com/longbridge/gpui-kit/blob/main/Cargo.toml) (pins
  `gpui-pre =0.3.12`).
- **Slint**: [CHANGELOG](https://github.com/slint-ui/slint/blob/master/CHANGELOG.md) and the
  [winit backend Cargo.toml](https://github.com/slint-ui/slint/blob/master/internal/backends/winit/Cargo.toml).
- **Freya**: [README](https://github.com/marc2332/freya/blob/main/README.md) and
  [Cargo.toml](https://github.com/marc2332/freya/blob/main/crates/freya/Cargo.toml).
- **egui**: [eframe Cargo.toml](https://github.com/emilk/egui/blob/main/crates/eframe/Cargo.toml).
- **Dioxus Native / Blitz**: [blitz README](https://github.com/DioxusLabs/blitz/blob/main/README.md)
  and [dioxus native Cargo.toml](https://github.com/DioxusLabs/dioxus/blob/main/packages/native/Cargo.toml).
- **Makepad**: [repository tree](https://github.com/makepad/makepad) (`libs/a11y`,
  `platform/hal/src/backend/windows/win32/a11y.rs`).

## Candidate notes

### Rune: confirmed real, and unusually close to Beamer's needs

[anomalyco/rune](https://github.com/anomalyco/rune) is MIT, by Anomaly (the opencode people;
contributors include `thdxr`).

- **What it is.** It describes itself as "a native GPU-rendered Rust UI framework for macOS,
  Windows, Linux and the web, with its own renderer, text, layout and springs". It is built on winit
  0.30 and wgpu 30, with Taffy layout and AccessKit.
- **Age and activity.** The repository was created on **2026-10-22**, has 2,317 commits on `dev`
  and 6,976 stars. Its top committers are `kitlangton` (1,782) and `Hona` (401).
- **How it is developed.** "We don't take pull requests. Have your agent open an issue … our agents
  usually turn it around fast." It is clearly built largely by agents.

What fits Beamer:

- **Per-display click-through overlays** and a status-level, all-Spaces `NSPanel` HUD. This is the
  same `NSPanel` recipe as `effects_overlay.py`.
- A **remote-cursor overlay example**.
- Runtime `set_click_through`.
- Several windows with different state types.
- AccessKit everywhere.
- A compile-time-checked i18n crate.
- `Live<T>` values that animate without re-running views. This suits pressure-driven glows.
- `Recorder`/`Player` audio and video.
- Headless rendering to PNG and accessibility-tree dumps "so an agent can render to PNG, dump the
  element tree and drive it with scripted input". This lines up with the map's tests-first,
  many-agent build.

What is risky:

- **No releases at all.** Apps pin a `dev` rev **and** must `[patch.crates-io]` four crates
  (`winit`, `parley_engine`, `wgpu-hal`, `accesskit_windows`) from the same rev.
- **Windows overlays are not run-tested upstream.** For `App::overlay`, "Windows builds are checked
  with `cargo xwin clippy` only".
- `App::overlay` lists "per-monitor placement" as unsupported. Per-monitor overlays use
  `WindowOptions::overlay(display)` instead, which goes through winit, so on the Mac it needs the
  same `objc2` level and Spaces fix as everyone else.
  - "Monitor enumeration is a startup snapshot", but Beamer needs hot-plug.
  - "Non-activating macOS `NSPanel`s, all-Spaces/fullscreen-space policy and status items … are not
    implemented here."
- **No macOS tray.**
- **The docs contradict each other.** The "Differences from PLAN.md" section still says "No
  AccessKit", while the Accessibility section and `Cargo.toml` show AccessKit wired in.

**Name clash:** the crate called `rune` on crates.io is
[rune-rs/rune](https://crates.io/crates/rune), an embeddable scripting language. It is unrelated.
Anomaly's Rune is git-only.

### GPUI

The [README](https://github.com/zed-industries/zed/blob/main/crates/gpui/README.md) says it "is
still pre-1.0. There will often be breaking changes between versions". It uses Metal on macOS and
Win32, DirectWrite and DirectX on Windows; there is no winit. AccessKit is integrated on both
platforms.

The packaging is awkward. The README's `gpui_platform = "*"` does not resolve on crates.io today
(404), and crates.io `gpui` is a year old. Realistically it is a git dependency on Zed, or the
`gpui-pre` snapshot that [gpui-kit](https://github.com/longbridge/gpui-kit) (20.6k★, 0.9.1 on
2026-11-09, 75+ components, AccessKit) pins.

For overlays, `WindowKind::PopUp` plus `Transparent` plus `display_id` covers most of the Mac recipe
(non-activating panel, all Spaces, full-screen auxiliary). The rest is done by hand:

- `setIgnoresMouseEvents` on macOS.
- `WS_EX_LAYERED | WS_EX_TRANSPARENT` on Windows. Whether that coexists with GPUI's
  DirectComposition swapchain (`WS_EX_NOREDIRECTIONBITMAP`) is **unverified** and is a bake-off
  item.

### Slint

The most complete on paper, and the one whose i18n matches today's gettext/`.po` workflow. 1.x
releases come about every two months (1.15.0 on 2026-02-04, then 1.16, 1.17, and 1.18.1 on
2026-09-21). The tray covers macOS and Windows natively. The weak point is that click-through needs
the `unstable-winit-030` accessor, which "may change". UI is written in the `.slint` DSL, with Rust
behind it. That is less "play with modern Rust UI" than the others, but it is the lowest-risk
choice.

### Freya

Strong on every row:

- tray, multi-window, transparency and multi-monitor examples
- Fluent i18n
- AccessKit
- frequent releases: 0.5.0 to 0.5.2 in November 2026

The concerns:

- Bus factor: `marc2332` has 2,524 commits, and the next human contributor has 19.
- Skia rather than wgpu.

It has not used Dioxus since 0.4.

## Not shortlisted, and why

None of these fails the three hard requirements once the winit and `tray-icon` escape hatch is
counted, except where noted. Each is dropped for the requirement named.

| Toolkit | Killed by |
|---|---|
| **Iced** | **Accessibility**: no screen-reader support ([#552](https://github.com/iced-rs/iced/issues/552) open). Also no i18n, and no release in 11 months (0.14.0 on 2025-12-07). Its overlay support is otherwise excellent. |
| **egui** | **Fun/modern and i18n**. Immediate mode suits tools more than a polished consumer app, and there is no i18n. It would make a good overlay layer, but it is not a toolkit to play with for the main UI. |
| **Dioxus Native / Blitz** | **Maturity**: Blitz is "beta … still many bugs and missing features", and Dioxus 0.8 is alpha (0.8.0-alpha.3, 2026-10-16). Worth revisiting in a year. |
| **Makepad** | **Escape hatch and maturity**. There is no winit and no public raw-window-handle access was found, so click-through overlays depend on Makepad's own HAL. Its crates.io 1.0.0 dates from May 2025, while `main` has moved on to a new script DSL, a window manager and a Wayland server. Accessibility is its own layer, not AccessKit. |
| **Xilem / Masonry** | **Maturity**: "an experimental Rust architecture"; 0.4.0 (2025-10-29) has only "initial" multi-window support. |
| **Floem** | **Maturity**: last release 0.2.0 (2024-11-15); last push 2026-09-24. |
| **Ribir** | **Maturity**: `0.4.0-alpha.65` (2026-04-21) is the last activity; it has stalled for seven months. |
| **Vizia, Kas** | **Maturity and ecosystem**: small (2.4k★ and 1.0k★), no tray. Vizia has AccessKit and Fluent, but too little use to bet a big-bang rewrite on. |
| **Tauri / Dioxus Desktop (webviews)** | **The point of the exercise**. They meet every hard requirement (tray, `set_ignore_cursor_events`, always-on-top, many windows) and are mature, but the UI would be HTML/TS in a system webview, not a Rust UI. One transparent webview per display at display rate is also a heavy way to draw a glow (not measured). |
| Newer, smaller projects (zgui, Akar, XenGui, Kael, a GPUI fork) | **Maturity**: single-author, months old, found only through aggregator search. Not investigated further. |

## What the bake-off has to settle

The same three-window prototype in each shortlisted toolkit, on both machines:

1. **Overlays.** One click-through, transparent overlay per display, animating at the display's rate
   (120 Hz on ProMotion).
   - On the Mac: above a full-screen app, on every Space, at status level, never taking focus.
   - On Windows: above the taskbar and over a full-screen window.
   - It must survive a display being plugged in. Rune's monitor list is a startup snapshot.
2. **Tray.** A tray or menu-bar icon redrawn at runtime (the drive-state glyph), with a menu that has
   a submenu. Rune's own tray is flat and Windows-only.
3. **Screen readers.** VoiceOver and Narrator read a settings window: labels, a toggle and a slider.
4. **i18n.** A `.po` catalogue (or its equivalent) switches language.
5. **wgpu.** A raw wgpu window or texture sits beside the toolkit's windows: the "overlay layer"
   option.
6. **Measurements**: stripped release binary size, idle CPU with overlays hidden, and the frame time
   of the glow.
7. **Click-through on Windows.** GPUI and DirectComposition with `WS_EX_LAYERED |
   WS_EX_TRANSPARENT`, and Rune's Windows overlays, which upstream only checks with clippy.

## Not verified

- **Binary sizes**: none measured here. Rune's 9.3 MB is self-reported for its own example.
- **Rune on Windows**: overlay behaviour has not been run by anyone that I could find.
- **GPUI click-through** on a DirectComposition window.
- **Makepad raw handles**: whether Makepad exposes them anywhere. None was found in the tree
  listing, and no code search was done on its main branch.
- **Slint tray submenus**: whether Slint's `SystemTrayIcon` menu supports them.
- **Dioxus Native custom wgpu painting**.
- **Performance**: no performance claim from any project's docs was reproduced.
