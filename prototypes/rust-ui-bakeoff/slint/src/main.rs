// PROTOTYPE: Rust UI toolkit bake-off, Slint slice (throwaway, never merged).
// Tray + settings window (Crossing page) + click-through edge-glow overlay. See ../SPEC.md.

mod platform;

use std::cell::{Cell, RefCell};
use std::io::Write as _;
use std::rc::Rc;
use std::time::{Duration, Instant, SystemTime};

use slint::winit_030::{WinitWindowAccessor, winit};
use slint::{ComponentHandle, ModelRc, VecModel};

slint::include_modules!();

const TOOLKIT: &str = "Slint 1.19.0";
const PUSH_RATE: f32 = 400.0; // px/s
const DECAY_S: f32 = 0.25;
const FLASH_S: f32 = 0.12;
const FADE_S: f32 = 0.30;
const PREVIEW_FADE_S: f32 = 0.25;

// ---------------------------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------------------------

thread_local! {
    static LOG: RefCell<Option<std::fs::File>> = const { RefCell::new(None) };
    static T0: Cell<Option<Instant>> = const { Cell::new(None) };
    /// Set while the overlay component is being created, so the winit attributes hook can tell it apart.
    static CREATING_OVERLAY: Cell<bool> = const { Cell::new(false) };
}

fn log_path() -> std::path::PathBuf {
    #[cfg(target_os = "macos")]
    let dir = std::path::PathBuf::from(std::env::var("HOME").unwrap()).join("Library/Logs/BeamerBakeoff");
    #[cfg(target_os = "windows")]
    let dir = std::path::PathBuf::from(std::env::var("LOCALAPPDATA").unwrap()).join("BeamerBakeoff");
    #[cfg(not(any(target_os = "macos", target_os = "windows")))]
    let dir = std::env::temp_dir().join("BeamerBakeoff");
    std::fs::create_dir_all(&dir).ok();
    dir.join("slint.log")
}

fn log(msg: impl AsRef<str>) {
    let t = T0.with(|t| t.get()).map(|t| t.elapsed().as_secs_f64()).unwrap_or(0.0);
    let line = format!("[{t:8.3}s] {}", msg.as_ref());
    eprintln!("{line}");
    LOG.with(|l| {
        if let Some(f) = l.borrow_mut().as_mut() {
            let _ = writeln!(f, "{line}");
            let _ = f.flush();
        }
    });
}

// ---------------------------------------------------------------------------------------------
// Displays
// ---------------------------------------------------------------------------------------------

#[derive(Clone, Debug)]
pub struct Display {
    /// Human name: NSScreen.localizedName on the Mac, the monitor's friendly name on Windows.
    pub name: String,
    /// Platform id: CGDirectDisplayID on the Mac, `\\.\DISPLAYn` on Windows.
    pub id: String,
    /// Frame: points (Cocoa, bottom-left origin) on the Mac, physical pixels on Windows.
    pub x: f64,
    pub y: f64,
    pub w: f64,
    pub h: f64,
    pub scale: f64,
    pub refresh_hz: f64,
    pub is_main: bool,
}

// ---------------------------------------------------------------------------------------------
// The push-and-cross state machine (pure, time based; frames only sample it)
// ---------------------------------------------------------------------------------------------

#[derive(Default)]
struct Glow {
    resistance: f32,
    paused: bool,
    pressure: f32,
    /// Holding (button, Space, or the test sequence): (since, pressure at start).
    hold: Option<(Instant, f32)>,
    /// Crossed while still held: ignore until released.
    spent: bool,
    decay: Option<(Instant, f32)>,
    crossed_at: Option<Instant>,
    /// Slider preview while dragging (0..1), then its fade.
    preview: Option<f32>,
    preview_fade: Option<(Instant, f32)>,
}

struct Frame {
    intensity: f32,
    flash: bool,
    pressure: f32,
    strip_flash: bool,
    animating: bool,
    crossed: bool,
}

impl Glow {
    fn step(&mut self, now: Instant) -> Frame {
        let mut crossed = false;
        if let Some((t0, p0)) = self.hold {
            let p = p0 + PUSH_RATE * (now - t0).as_secs_f32();
            if p >= self.resistance && !self.paused {
                self.pressure = 0.0;
                self.hold = None;
                self.spent = true;
                self.crossed_at = Some(now);
                crossed = true;
            } else {
                self.pressure = p.min(self.resistance);
            }
        } else if let Some((t0, p0)) = self.decay {
            let k = (now - t0).as_secs_f32() / DECAY_S;
            if k >= 1.0 {
                self.pressure = 0.0;
                self.decay = None;
            } else {
                self.pressure = p0 * (1.0 - k);
            }
        }
        let mut intensity = self.pressure / self.resistance.max(1.0);
        let mut flash = false;
        let mut strip_flash = false;
        if let Some(t) = self.crossed_at {
            let dt = (now - t).as_secs_f32();
            if dt < FLASH_S {
                intensity = 1.0;
                flash = true;
                strip_flash = true;
            } else if dt < FLASH_S + FADE_S {
                intensity = intensity.max(1.0 - (dt - FLASH_S) / FADE_S);
                flash = true;
            } else {
                self.crossed_at = None;
            }
        }
        if let Some(v) = self.preview {
            intensity = intensity.max(v);
        } else if let Some((t0, v0)) = self.preview_fade {
            let k = (now - t0).as_secs_f32() / PREVIEW_FADE_S;
            if k >= 1.0 {
                self.preview_fade = None;
            } else {
                intensity = intensity.max(v0 * (1.0 - k));
            }
        }
        let animating = self.hold.is_some()
            || self.decay.is_some()
            || self.crossed_at.is_some()
            || self.preview_fade.is_some();
        Frame { intensity, flash, pressure: self.pressure, strip_flash, animating, crossed }
    }

    fn press(&mut self, now: Instant) {
        if self.hold.is_none() && !self.spent {
            self.step(now);
            self.decay = None;
            self.hold = Some((now, self.pressure));
        }
    }

    fn release(&mut self, now: Instant) {
        self.spent = false;
        if self.hold.is_some() {
            self.step(now);
            if self.hold.take().is_some() && self.pressure > 0.0 {
                self.decay = Some((now, self.pressure));
            }
        }
    }
}

/// Frames counted while the overlay animates; one "animation" runs from idle to idle.
#[derive(Default)]
struct FpsMeter {
    frames: u32,
    first: Option<Instant>,
    last: Option<Instant>,
}

// ---------------------------------------------------------------------------------------------
// App
// ---------------------------------------------------------------------------------------------

struct App {
    settings: SettingsWindow,
    overlay: Overlay,
    tray: Tray,
    glow: RefCell<Glow>,
    fps: RefCell<FpsMeter>,
    animating: Cell<bool>,
    displays: RefCell<Vec<Display>>,
    glow_on: Cell<usize>,
    machine: Cell<usize>,
    machines: [&'static str; 2],
    test_seq: Cell<bool>,
    settings_first_frame: Cell<bool>,
    process_start: Option<SystemTime>,
}

impl App {
    /// Sample the state machine now and push it to both windows. Returns whether to keep animating.
    fn tick(&self) -> bool {
        let now = Instant::now();
        let f = self.glow.borrow_mut().step(now);
        if f.crossed {
            log(format!("crossed at resistance {:.0} px", self.glow.borrow().resistance));
        }
        self.overlay.set_intensity(f.intensity);
        self.overlay.set_flash(f.flash);
        self.settings.set_pressure(f.pressure);
        self.settings.set_strip_flash(f.strip_flash);
        if !f.animating && self.test_seq.replace(false) {
            // The test sequence holds until it crosses, then lets go.
            self.glow.borrow_mut().release(now);
        }
        f.animating
    }

    /// Start (or continue) a frame-driven animation on the overlay.
    fn kick(&self) {
        if !self.animating.replace(true) {
            *self.fps.borrow_mut() = FpsMeter::default();
        }
        self.tick();
        self.overlay.window().request_redraw();
    }

    /// Called from the overlay's rendering notifier after every presented frame.
    fn on_overlay_frame(&self) {
        if !self.animating.get() {
            return;
        }
        let now = Instant::now();
        {
            let mut m = self.fps.borrow_mut();
            m.frames += 1;
            m.first.get_or_insert(now);
            m.last = Some(now);
        }
        if self.tick() {
            self.overlay.window().request_redraw();
        } else {
            self.animating.set(false);
            let m = std::mem::take(&mut *self.fps.borrow_mut());
            let refresh = self.displays.borrow().get(self.glow_on.get()).map(|d| d.refresh_hz).unwrap_or(0.0);
            if let (Some(a), Some(b)) = (m.first, m.last) {
                let secs = (b - a).as_secs_f64();
                let fps = if secs > 0.0 { (m.frames.saturating_sub(1)) as f64 / secs } else { 0.0 };
                log(format!(
                    "animation: {} frames in {:.0} ms = {:.1} fps measured (display {:.0} Hz)",
                    m.frames,
                    secs * 1000.0,
                    fps,
                    refresh
                ));
                self.settings.set_fps_info(
                    format!("{fps:.1} fps over {} frames (display {refresh:.0} Hz)", m.frames).into(),
                );
            }
        }
    }

    fn test_glow(&self) {
        log("test glow: push-and-cross sequence");
        let now = Instant::now();
        let mut g = self.glow.borrow_mut();
        g.spent = false;
        g.hold = None;
        g.press(now);
        drop(g);
        self.test_seq.set(true);
        self.kick();
    }

    fn open_settings(&self) {
        self.settings.show().unwrap();
        self.settings.window().with_winit_window(|w| w.focus_window());
        platform::activate_app();
    }

    fn refresh_menus(&self) {
        let machines: Vec<MenuChoice> = self
            .machines
            .iter()
            .enumerate()
            .map(|(i, m)| MenuChoice { title: (*m).into(), checked: i == self.machine.get() })
            .collect();
        // A fresh model each time: a checkable MenuItem toggles its own `checked`, which would
        // break a binding, so the rows are rebuilt instead (radio behaviour by hand).
        self.tray.set_machines(ModelRc::new(VecModel::from(machines)));
        let displays: Vec<MenuChoice> = self
            .displays
            .borrow()
            .iter()
            .enumerate()
            .map(|(i, d)| MenuChoice { title: d.name.clone().into(), checked: i == self.glow_on.get() })
            .collect();
        self.tray.set_displays(ModelRc::new(VecModel::from(displays)));
        self.settings.set_send_to(self.machines[self.machine.get()].into());
    }

    fn place_overlay(&self) {
        let displays = self.displays.borrow();
        let Some(d) = displays.get(self.glow_on.get()) else { return };
        let info = self
            .overlay
            .window()
            .with_winit_window(|w| platform::place_overlay(w, d))
            .unwrap_or_else(|| "no winit window".into());
        log(format!("overlay placed on {}: {info}", d.name));
        self.settings.set_overlay_info(
            format!("{} · {:.0}×{:.0} at ({:.0}, {:.0}) · scale {:.2}", d.name, d.w, d.h, d.x, d.y, d.scale).into(),
        );
    }
}

fn glyph(fg: [u8; 3]) -> slint::Image {
    // A filled circle with a vertical bar, 36×36 (18 pt at 2×), alpha by 4×4 supersampling.
    let n = 36u32;
    let mut buf = slint::SharedPixelBuffer::<slint::Rgba8Pixel>::new(n, n);
    for (i, px) in buf.make_mut_slice().iter_mut().enumerate() {
        let (x, y) = ((i as u32 % n) as f32, (i as u32 / n) as f32);
        let mut cov = 0.0;
        for sy in 0..4 {
            for sx in 0..4 {
                let (fx, fy) = (x + (sx as f32 + 0.5) / 4.0, y + (sy as f32 + 0.5) / 4.0);
                let in_circle = (fx - 14.0).powi(2) + (fy - 18.0).powi(2) <= 8.0f32.powi(2);
                let in_bar = (26.0..30.0).contains(&fx) && (5.0..31.0).contains(&fy);
                if in_circle || in_bar {
                    cov += 1.0 / 16.0;
                }
            }
        }
        *px = slint::Rgba8Pixel { r: fg[0], g: fg[1], b: fg[2], a: (cov * 255.0) as u8 };
    }
    slint::Image::from_rgba8(buf)
}

fn main() {
    let t0 = Instant::now();
    T0.with(|t| t.set(Some(t0)));
    let process_start = platform::process_start_time();
    LOG.with(|l| {
        *l.borrow_mut() = std::fs::OpenOptions::new().create(true).append(true).open(log_path()).ok()
    });

    let args: Vec<String> = std::env::args().collect();
    let has = |f: &str| args.iter().any(|a| a == f);
    let quit_after: Option<f64> =
        args.iter().position(|a| a == "--quit-after").and_then(|i| args.get(i + 1)).and_then(|s| s.parse().ok());

    let glow_on_arg: Option<String> =
        args.iter().position(|a| a == "--glow-on").and_then(|i| args.get(i + 1)).cloned();
    log(format!("==== bakeoff-slint start ({TOOLKIT}), args {:?}", &args[1..]));
    if let Some(ps) = process_start {
        let since = SystemTime::now().duration_since(ps).unwrap_or_default();
        log(format!("process start -> main(): {:.1} ms", since.as_secs_f64() * 1000.0));
    }

    // Backend: winit + Skia, accessory app on the Mac, and a hook that shapes the overlay window.
    #[allow(unused_mut)]
    let mut elb = winit::event_loop::EventLoop::with_user_event();
    #[cfg(target_os = "macos")]
    {
        use winit::platform::macos::{ActivationPolicy, EventLoopBuilderExtMacOS};
        elb.with_activation_policy(ActivationPolicy::Accessory)
            .with_default_menu(false)
            .with_activate_ignoring_other_apps(false);
    }
    slint::BackendSelector::new()
        .backend_name("winit".into())
        .renderer_name("skia".into())
        .with_winit_event_loop_builder(elb)
        .with_winit_window_attributes_hook(|attrs| {
            if !CREATING_OVERLAY.with(|c| c.get()) {
                return attrs;
            }
            let attrs = attrs
                .with_active(false)
                .with_decorations(false)
                .with_resizable(false)
                .with_transparent(true)
                .with_window_level(winit::window::WindowLevel::AlwaysOnTop);
            platform::overlay_attributes(attrs)
        })
        .select()
        .expect("winit + skia backend");

    CREATING_OVERLAY.with(|c| c.set(true));
    let overlay = Overlay::new().unwrap();
    CREATING_OVERLAY.with(|c| c.set(false));
    let settings = SettingsWindow::new().unwrap();
    let tray = Tray::new().unwrap();

    #[cfg(target_os = "macos")]
    let (here, peer, machines, footer) = (
        "On Mac",
        "Desk PC",
        ["This Mac", "Desk PC"],
        "Changes apply as you make them. Closing this window keeps Beamer running in the menu bar.",
    );
    #[cfg(not(target_os = "macos"))]
    let (here, peer, machines, footer) = (
        "On PC",
        "Studio Mac",
        ["This PC", "Studio Mac"],
        "Changes apply as you make them. Closing this window keeps Beamer running in the tray.",
    );
    settings.set_here(here.into());
    settings.set_peer(peer.into());
    settings.set_footer(footer.into());
    settings.set_toolkit("Slint".into());
    settings.set_renderer_info(platform::RENDERER.into());
    overlay.set_label("OVERLAY · Slint".into());
    tray.set_glyph_for_dark(glyph([255, 255, 255]));
    tray.set_glyph_for_light(glyph([0, 0, 0]));

    let app = Rc::new(App {
        glow: RefCell::new(Glow { resistance: settings.get_resistance(), ..Default::default() }),
        settings,
        overlay,
        tray,
        fps: Default::default(),
        animating: Cell::new(false),
        displays: Default::default(),
        glow_on: Cell::new(0),
        machine: Cell::new(0),
        machines,
        test_seq: Cell::new(false),
        settings_first_frame: Cell::new(false),
        process_start,
    });
    app.refresh_menus();

    // ---- settings callbacks
    let w = Rc::downgrade(&app);
    app.settings.on_resistance_edited(move |v| {
        let Some(app) = w.upgrade() else { return };
        app.glow.borrow_mut().resistance = v;
        let mut g = app.glow.borrow_mut();
        if g.preview.is_some() {
            g.preview = Some(v / 500.0);
        }
        drop(g);
        app.tick();
    });
    let w = Rc::downgrade(&app);
    app.settings.on_resistance_drag(move |dragging| {
        let Some(app) = w.upgrade() else { return };
        let now = Instant::now();
        let v = app.settings.get_resistance() / 500.0;
        {
            let mut g = app.glow.borrow_mut();
            if dragging {
                g.preview = Some(v);
                g.preview_fade = None;
            } else {
                g.preview = None;
                g.preview_fade = Some((now, v));
            }
        }
        if dragging {
            app.tick();
        } else {
            app.kick();
        }
    });
    let w = Rc::downgrade(&app);
    app.settings.on_push(move |down| {
        let Some(app) = w.upgrade() else { return };
        let now = Instant::now();
        if down {
            app.glow.borrow_mut().press(now);
        } else {
            app.glow.borrow_mut().release(now);
        }
        app.kick();
    });
    let w = Rc::downgrade(&app);
    app.settings.on_paused_edited(move |p| {
        let Some(app) = w.upgrade() else { return };
        app.glow.borrow_mut().paused = p;
        app.tray.set_paused(p);
        log(format!("pause crossing: {p} (from settings)"));
    });
    app.settings.on_settings_changed(|| {});

    // First settings frame → startup time.
    let w = Rc::downgrade(&app);
    app.settings
        .window()
        .set_rendering_notifier(move |state, _| {
            if !matches!(state, slint::RenderingState::AfterRendering) {
                return;
            }
            let Some(app) = w.upgrade() else { return };
            if app.settings_first_frame.replace(true) {
                return;
            }
            let main_ms = t0.elapsed().as_secs_f64() * 1000.0;
            let proc_ms = app
                .process_start
                .and_then(|ps| SystemTime::now().duration_since(ps).ok())
                .map(|d| d.as_secs_f64() * 1000.0);
            let text = match proc_ms {
                Some(p) => format!("{p:.0} ms process start -> first settings frame ({main_ms:.0} ms from main)"),
                None => format!("{main_ms:.0} ms main() -> first settings frame"),
            };
            log(format!("startup: {text}"));
            app.settings.set_startup_info(text.into());
        })
        .unwrap();

    // Overlay frames drive the animation.
    let w = Rc::downgrade(&app);
    app.overlay
        .window()
        .set_rendering_notifier(move |state, _| {
            if matches!(state, slint::RenderingState::AfterRendering)
                && let Some(app) = w.upgrade()
            {
                app.on_overlay_frame();
            }
        })
        .unwrap();

    // ---- tray callbacks
    let w = Rc::downgrade(&app);
    app.tray.on_open_settings(move || {
        if let Some(app) = w.upgrade() {
            log("tray: settings");
            app.open_settings();
        }
    });
    let w = Rc::downgrade(&app);
    app.tray.on_pick_machine(move |i| {
        if let Some(app) = w.upgrade() {
            app.machine.set(i as usize);
            log(format!("tray: send input to {}", app.machines[i as usize]));
            app.refresh_menus();
        }
    });
    let w = Rc::downgrade(&app);
    app.tray.on_pick_display(move |i| {
        if let Some(app) = w.upgrade() {
            app.glow_on.set(i as usize);
            log(format!("tray: glow on {}", app.displays.borrow()[i as usize].name));
            app.refresh_menus();
            app.place_overlay();
        }
    });
    let w = Rc::downgrade(&app);
    app.tray.on_paused_edited(move |p| {
        if let Some(app) = w.upgrade() {
            app.glow.borrow_mut().paused = p;
            app.settings.set_paused(p);
            log(format!("pause crossing: {p} (from tray)"));
        }
    });
    let w = Rc::downgrade(&app);
    app.tray.on_test_glow(move || {
        if let Some(app) = w.upgrade() {
            app.test_glow();
        }
    });
    app.tray.on_quit(|| {
        log("tray: quit");
        slint::quit_event_loop().unwrap();
    });

    // ---- windows
    app.overlay.show().unwrap();
    if has("--open-settings") {
        app.settings.show().unwrap();
    }

    // Once the overlay's winit window exists: list displays, pick one, shape and place the overlay.
    let w = Rc::downgrade(&app);
    slint::spawn_local(async move {
        let Some(app) = w.upgrade() else { return };
        let ww = app.overlay.window().winit_window().await.unwrap();
        let displays = platform::displays(&ww);
        for d in &displays {
            log(format!(
                "display: {:?} id {} frame ({:.0}, {:.0}) {:.0}x{:.0} scale {:.2} refresh {:.2} Hz{}",
                d.name,
                d.id,
                d.x,
                d.y,
                d.w,
                d.h,
                d.scale,
                d.refresh_hz,
                if d.is_main { " (main)" } else { "" }
            ));
        }
        // `--glow-on <name>` (debug aid) overrides the default: the first display that isn't the main one.
        let forced = glow_on_arg.as_ref().and_then(|n| displays.iter().position(|d| &d.name == n));
        let pick = forced.or_else(|| displays.iter().position(|d| !d.is_main)).unwrap_or(0);
        *app.displays.borrow_mut() = displays;
        app.glow_on.set(pick);
        app.refresh_menus();

        // Click-through: the toolkit route (winit, behind `unstable-winit-030`).
        match ww.set_cursor_hittest(false) {
            Ok(()) => log("overlay: winit set_cursor_hittest(false) ok"),
            Err(e) => log(format!("overlay: set_cursor_hittest failed: {e}")),
        }
        let flags = platform::shape_overlay(&ww);
        log(format!("overlay flags: {flags}"));
        app.settings.set_click_through(flags.into());
        drop(ww);
        app.place_overlay();
        if app.settings.window().is_visible() {
            app.settings.window().with_winit_window(|w| w.focus_window());
        }
    })
    .unwrap();

    if has("--test-glow") {
        let w = Rc::downgrade(&app);
        slint::Timer::single_shot(Duration::from_secs(1), move || {
            let Some(app) = w.upgrade() else { return };
            app.test_glow();
            let w = Rc::downgrade(&app);
            let t = slint::Timer::default();
            t.start(slint::TimerMode::Repeated, Duration::from_secs(3), move || {
                if let Some(app) = w.upgrade() {
                    app.test_glow();
                }
            });
            std::mem::forget(t);
        });
    }
    if let Some(secs) = quit_after {
        let w = Rc::downgrade(&app);
        slint::Timer::single_shot(Duration::from_secs_f64(secs), move || {
            if let Some(app) = w.upgrade() {
                // Did anything (winit, Slint) undo the overlay's flags while running?
                let r = app.overlay.window().with_winit_window(platform::report);
                log(format!("overlay flags at quit: {}", r.unwrap_or_default()));
            }
            log(format!("--quit-after {secs}: quitting"));
            slint::quit_event_loop().unwrap();
        });
    }

    log(format!("event loop starting after {:.1} ms", t0.elapsed().as_secs_f64() * 1000.0));
    slint::run_event_loop_until_quit().unwrap();
    log("event loop ended");
}
