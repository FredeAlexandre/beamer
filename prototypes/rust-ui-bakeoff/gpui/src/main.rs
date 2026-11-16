// PROTOTYPE: Rust UI bake-off, GPUI slice (throwaway, never merged). See ../SPEC.md and NOTES.md.
#![cfg_attr(target_os = "windows", windows_subsystem = "windows")]

mod logfile;
mod overlay;
mod platform;
mod settings;
mod state;
mod theme;

use std::{borrow::Cow, time::{Duration, Instant}};

use futures::StreamExt;
use gpui::{
    App, AppContext, Bounds, Global, KeyBinding, QuitMode, TitlebarOptions, WindowBackgroundAppearance, WindowBounds,
    WindowHandle, WindowKind, WindowOptions, point, px, size,
};

use overlay::Overlay;
use settings::Settings;
use state::{AppState, DisplayEntry, GlobalState, Page, Phase};

pub struct Args {
    pub test_glow: bool,
    pub quit_after: Option<f64>,
    pub open_settings: bool,
}

#[derive(Default)]
pub struct Windows {
    pub settings: Option<WindowHandle<Settings>>,
    pub overlay: Option<(gpui::DisplayId, WindowHandle<Overlay>)>,
}
impl Global for Windows {}

fn parse_args() -> Args {
    let mut a = Args { test_glow: false, quit_after: None, open_settings: false };
    let mut it = std::env::args().skip(1);
    while let Some(s) = it.next() {
        match s.as_str() {
            "--test-glow" => a.test_glow = true,
            "--open-settings" => a.open_settings = true,
            "--quit-after" => a.quit_after = it.next().and_then(|v| v.parse().ok()),
            _ => {}
        }
    }
    a
}

fn main() {
    let start = Instant::now();
    logfile::init(start);
    let args = parse_args();
    logln!(
        "bakeoff-gpui start: gpui-pre 0.3.12, pid {}, args test_glow={} open_settings={} quit_after={:?}",
        std::process::id(), args.test_glow, args.open_settings, args.quit_after
    );

    gpui_platform::application().with_quit_mode(QuitMode::Explicit).run(move |cx: &mut App| {
        // Menu-bar app: no Dock icon, but it may still show windows.
        cx.set_activation_policy(gpui::ActivationPolicy::Accessory);
        cx.text_system()
            .add_fonts(vec![
                Cow::Borrowed(include_bytes!("../assets/HankenGrotesk-Variable.ttf").as_slice()),
                Cow::Borrowed(include_bytes!("../assets/B612Mono-Regular.ttf").as_slice()),
                Cow::Borrowed(include_bytes!("../assets/B612Mono-Bold.ttf").as_slice()),
            ])
            .unwrap_or_else(|e| logln!("ERROR add_fonts: {e:#}"));
        cx.bind_keys([
            KeyBinding::new("tab", settings::FocusNext, None),
            KeyBinding::new("shift-tab", settings::FocusPrev, None),
        ]);

        let displays = enumerate_displays(cx);
        let glow_on = displays.iter().find(|d| !d.is_main).or(displays.first()).map(|d| d.id);
        let state = cx.new(|_| AppState {
            page: Page::Crossing,
            resistance: 120.0,
            dont_cross_dragging: true,
            paused: false,
            shortcut_hold: false,
            tap_ms: 300.0,
            goes_to: 0,
            send_to: 0,
            displays,
            glow_on,
            phase: Phase::Idle,
            overlay_frame: "-".into(),
            click_through: "-".into(),
            last_fps: None,
            refresh_hz: 0.0,
            startup_ms: None,
            started: start,
        });
        cx.set_global(GlobalState(state.clone()));
        cx.set_global(Windows::default());

        if std::env::var_os("BAKEOFF_STATIC_GLOW").is_some() {
            state.update(cx, |s, _| s.phase = Phase::Preview { level: 1.0 }); // debug: a still, full glow
        }
        open_overlay(cx);
        cx.observe(&state, |state, cx| {
            let want = state.read(cx).glow_on;
            let have = cx.global::<Windows>().overlay.as_ref().map(|(d, _)| *d);
            if want != have {
                if std::env::var_os("BAKEOFF_STATIC_GLOW").is_some() {
            state.update(cx, |s, _| s.phase = Phase::Preview { level: 1.0 }); // debug: a still, full glow
        }
        open_overlay(cx);
            }
        })
        .detach();

        // Monitor hot-plug (bonus): GPUI reports it; re-enumerate, keep or re-pick the glow display.
        cx.observe_displays(|event, cx| {
            logln!("displays changed: {event:?}");
            let displays = enumerate_displays(cx);
            let st = cx.global::<GlobalState>().0.clone();
            st.update(cx, |s, cx| {
                if !displays.iter().any(|d| Some(d.id) == s.glow_on) {
                    s.glow_on = displays.iter().find(|d| !d.is_main).or(displays.first()).map(|d| d.id);
                }
                s.displays = displays;
                cx.notify();
            });
            platform::tray::rebuild_glow_menu(cx);
        })
        .detach();

        let tray_rx = platform::tray::create(cx);
        cx.spawn(async move |cx| {
            let mut rx = tray_rx;
            while let Some(msg) = rx.next().await {
                cx.update(|cx| platform::tray::handle(msg, cx));
            }
        })
        .detach();
        logln!("tray ready at {:.1} ms", start.elapsed().as_secs_f64() * 1000.0);

        if args.open_settings {
            open_settings(cx);
        }
        if args.test_glow {
            cx.spawn(async move |cx| {
                cx.background_executor().timer(Duration::from_secs(1)).await;
                loop {
                    cx.update(|cx| test_glow(cx));
                    cx.background_executor().timer(Duration::from_secs(3)).await;
                }
            })
            .detach();
        }
        if let Some(secs) = args.quit_after {
            cx.spawn(async move |cx| {
                cx.background_executor().timer(Duration::from_secs_f64(secs)).await;
                logln!("quit-after {secs}s: {}", platform::process_stats());
                cx.update(|cx| cx.quit());
            })
            .detach();
        }
        // Debug: exercise the live re-target path (what the tray's Glow on does) without a mouse.
        if std::env::var_os("BAKEOFF_RETARGET").is_some() {
            cx.spawn(async move |cx| {
                cx.background_executor().timer(Duration::from_millis(2500)).await;
                cx.update(|cx| {
                    let st = cx.global::<GlobalState>().0.clone();
                    st.update(cx, |s, cx| {
                        let next = s.displays.iter().find(|d| Some(d.id) != s.glow_on).map(|d| d.id);
                        logln!("debug retarget: {:?} -> {:?}", s.glow_on, next);
                        s.glow_on = next.or(s.glow_on);
                        cx.notify();
                    });
                });
            })
            .detach();
        }
        // Resource snapshots, for the idle numbers in NOTES.md.
        cx.spawn(async move |cx| {
            loop {
                cx.background_executor().timer(Duration::from_secs(5)).await;
                logln!("stats: {}", platform::process_stats());
            }
        })
        .detach();
        let _ = cx;
    });
}

fn enumerate_displays(cx: &mut App) -> Vec<DisplayEntry> {
    let primary = cx.primary_display().map(|d| d.id());
    let mut out = Vec::new();
    for d in cx.displays() {
        let meta = platform::display_meta(d.id());
        let hz = d.refresh_interval().map(|i| 1.0 / i.as_secs_f32()).unwrap_or(0.0);
        let b = d.bounds();
        let e = DisplayEntry {
            id: d.id(),
            name: meta.name,
            frame: meta.frame,
            scale: meta.scale,
            refresh_hz: hz,
            is_main: Some(d.id()) == primary,
        };
        logln!(
            "display {:?} '{}' frame {} (gpui bounds {:?}x{:?} at {:?},{:?}) scale {} refresh {:.1} Hz{}",
            e.id, e.name, e.frame, b.size.width, b.size.height, b.origin.x, b.origin.y, e.scale, hz,
            if e.is_main { " [main]" } else { "" }
        );
        out.push(e);
    }
    out
}

pub fn open_overlay(cx: &mut App) {
    let state = cx.global::<GlobalState>().0.clone();
    if let Some((_, old)) = cx.global_mut::<Windows>().overlay.take() {
        old.update(cx, |_, window, _| window.remove_window()).ok();
    }
    let Some(id) = state.read(cx).glow_on else { return };
    let Some(display) = cx.find_display(id) else {
        logln!("ERROR glow display {id:?} not found");
        return;
    };
    let bounds = display.bounds(); // display-local on macOS, global on Windows: GPUI's own convention.
    let t = Instant::now();
    // GPUI throttles inactive windows to ~30 fps by default (and low-power / thermal to 30 / 60).
    // The overlay is never activated; it measured 144 fps with the defaults too (its `active`
    // flag never flips), but the caps are off so a focus change can't halve the glow.
    // BAKEOFF_DEFAULT_FRAME_CAPS=1 keeps GPUI's defaults.
    let default_caps = if std::env::var_os("BAKEOFF_DEFAULT_FRAME_CAPS").is_some() {
        WindowOptions::default()
    } else {
        WindowOptions {
            inactive_frame_interval: None,
            low_power_frame_interval: None,
            thermal_frame_interval: None,
            ..Default::default()
        }
    };
    let handle = cx.open_window(
        WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(bounds)),
            titlebar: None,
            focus: false,
            show: true,
            kind: WindowKind::PopUp,
            is_movable: false,
            is_resizable: false,
            is_minimizable: false,
            display_id: Some(id),
            window_background: WindowBackgroundAppearance::Transparent,
            ..default_caps
        },
        |window, cx| {
            let flags = platform::configure_overlay(window, id);
            let fixup = platform::overlay_fixup(window, id);
            let st = state.clone();
            cx.spawn(async move |cx| {
                let msg = fixup();
                logln!("overlay fixup (outside GPUI's borrow): {msg}");
                if let Some(f) = msg.split("-> ").last() {
                    let f = f.to_string();
                    cx.update(|cx| st.update(cx, |s, cx| { s.overlay_frame = f; cx.notify(); }));
                }
            })
            .detach();
            let hz = window.refresh_interval(cx).map(|i| 1.0 / i.as_secs_f32()).unwrap_or(0.0);
            let wb = window.bounds();
            logln!(
                "overlay window on {:?}: gpui bounds {:?}, refresh {:.1} Hz, flags: {}",
                id, wb, hz, flags.describe
            );
            state.update(cx, |s, _| {
                s.overlay_frame = flags.frame.clone();
                s.click_through = if flags.click_through { "on (set + read back)".into() } else { "OFF".into() };
                s.refresh_hz = hz;
            });
            cx.new(|cx| Overlay::new(state.clone(), window, cx))
        },
    );
    match handle {
        Ok(h) => {
            logln!("overlay opened in {:.1} ms", t.elapsed().as_secs_f64() * 1000.0);
            cx.global_mut::<Windows>().overlay = Some((id, h));
        }
        Err(e) => logln!("ERROR open overlay: {e:#}"),
    }
}

pub fn open_settings(cx: &mut App) {
    if let Some(h) = cx.global::<Windows>().settings {
        if h.update(cx, |_, window, _| window.activate_window()).is_ok() {
            cx.activate(true);
            return;
        }
    }
    let state = cx.global::<GlobalState>().0.clone();
    // BAKEOFF_SETTINGS_H: debug, a taller window for screenshots of the whole page.
    let h = std::env::var("BAKEOFF_SETTINGS_H").ok().and_then(|v| v.parse().ok()).unwrap_or(640.0);
    let bounds = Bounds::centered(None, size(px(900.), px(h)), cx);
    let handle = cx.open_window(
        WindowOptions {
            window_bounds: Some(WindowBounds::Windowed(bounds)),
            titlebar: Some(TitlebarOptions { title: Some("Beamer".into()), appears_transparent: false, traffic_light_position: None }),
            window_min_size: Some(size(px(640.), px(540.))),
            kind: WindowKind::Normal,
            focus: true,
            show: true,
            ..Default::default()
        },
        |window, cx| cx.new(|cx| Settings::new(state, window, cx)),
    );
    match handle {
        Ok(h) => cx.global_mut::<Windows>().settings = Some(h),
        Err(e) => logln!("ERROR open settings: {e:#}"),
    }
    cx.activate(true);
    let _ = point(px(0.), px(0.));
}

pub fn test_glow(cx: &mut App) {
    let state = cx.global::<GlobalState>().0.clone();
    state.update(cx, |s, cx| {
        s.phase = Phase::Push { p0: 0.0, t0: Instant::now() };
        cx.notify();
    });
}
