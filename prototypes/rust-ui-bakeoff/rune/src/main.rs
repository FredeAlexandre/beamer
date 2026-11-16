//! PROTOTYPE (throwaway, never merged): the bake-off slice in Rune. See ../SPEC.md and NOTES.md.
//! A tray / menu-bar item, the settings window's Crossing page, and a click-through edge-glow
//! overlay on one display, all on Rune's one event loop.

mod platform;

use std::f32::consts::PI;
use std::io::Write;
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

use rune::tok::Family;
use rune::{
    Align, App, AppHandle, AppSpec, Color, Cx, Display, DisplayTarget, Drag, El, Live, NativeDisplayId, OverlayHandle,
    OverlayInput, OverlayLevel, OverlayOptions, OverlayPlacement, OverlaySpec, Phase, PopupMenu, RunOptions, ScrollSpec, TextStyle,
    Tx, WindowSpec, col, el, row, text,
};

const TOOLKIT: &str = "rune";

// ---------------------------------------------------------------- logging and flags

static T0: OnceLock<Instant> = OnceLock::new();
static LOG: OnceLock<Mutex<std::fs::File>> = OnceLock::new();

fn since_start() -> f64 {
    T0.get().unwrap().elapsed().as_secs_f64()
}

macro_rules! log {
    ($($a:tt)*) => {{
        let line = format!("[{:8.3}] {}", since_start(), format!($($a)*));
        eprintln!("{line}");
        if let Some(f) = LOG.get() { let _ = writeln!(f.lock().unwrap(), "{line}"); }
    }};
}

fn log_path() -> std::path::PathBuf {
    #[cfg(target_os = "macos")]
    let dir = std::path::PathBuf::from(std::env::var("HOME").unwrap()).join("Library/Logs/BeamerBakeoff");
    #[cfg(windows)]
    let dir = std::path::PathBuf::from(std::env::var("LOCALAPPDATA").unwrap()).join("BeamerBakeoff");
    #[cfg(not(any(target_os = "macos", windows)))]
    let dir = std::env::temp_dir().join("BeamerBakeoff");
    std::fs::create_dir_all(&dir).unwrap();
    dir.join(format!("{TOOLKIT}.log"))
}

struct Flags {
    /// Extra to the spec, for measuring: `--glow-on <display index>` instead of the default.
    glow_on: Option<usize>,
    test_glow: bool,
    open_settings: bool,
    quit_after: Option<f64>,
}

fn flags() -> Flags {
    let args: Vec<String> = std::env::args().collect();
    let quit_after = args.iter().position(|a| a == "--quit-after").and_then(|i| args.get(i + 1)).and_then(|v| v.parse().ok());
    let glow_on = args.iter().position(|a| a == "--glow-on").and_then(|i| args.get(i + 1)).and_then(|v| v.parse().ok());
    Flags { glow_on, test_glow: args.iter().any(|a| a == "--test-glow"), open_settings: args.iter().any(|a| a == "--open-settings"), quit_after }
}

// ---------------------------------------------------------------- tokens (copied from tokens.py)

#[derive(Clone, Copy)]
struct Pal {
    ground: Color,
    panel: Color,
    well: Color,
    rule: Color,
    edge: Color,
    ink: Color,
    ink_2: Color,
    ink_3: Color,
    signal: Color,
    signal_dim: Color,
    flash: Color,
    off: Color,
}

fn hx(s: &str) -> Color {
    rune::color::hex(s).unwrap()
}

fn palette(dark: bool) -> Pal {
    if dark {
        Pal {
            ground: hx("#0e0f0d"), panel: hx("#161714"), well: hx("#1f201c"), rule: hx("#2a2b26"), edge: hx("#6e6f66"),
            ink: hx("#efeee6"), ink_2: hx("#b3b2a7"), ink_3: hx("#8f8e84"), signal: hx("#5fd4f4"), signal_dim: hx("#1f4a55"),
            flash: hx("#b77a00"), off: hx("#4a4b44"),
        }
    } else {
        Pal {
            ground: hx("#f1f0ea"), panel: hx("#fbfaf6"), well: hx("#e5e4dc"), rule: hx("#dcdbd3"), edge: hx("#7f7e74"),
            ink: hx("#1b1c18"), ink_2: hx("#46463f"), ink_3: hx("#66655c"), signal: hx("#006a88"), signal_dim: hx("#bfe0ea"),
            flash: hx("#b55f00"), off: hx("#aeada3"),
        }
    }
}

const UI: Family = Family::Named("Hanken Grotesk");
const MONO: Family = Family::Named("B612 Mono");
const fn ui(size: f32, weight: f32) -> TextStyle {
    TextStyle::new(size, weight).family(UI)
}
const fn mono(size: f32, weight: f32) -> TextStyle {
    TextStyle::new(size, weight).family(MONO)
}
// TYPE from tokens.py
const T_EYEBROW: TextStyle = ui(10.5, 650.0).tracking(10.5 * 0.14);
const T_SMALL: TextStyle = ui(11.5, 400.0);
const T_NOTE: TextStyle = ui(12.5, 400.0);
const T_BODY: TextStyle = ui(13.0, 400.0);
const T_BUTTON: TextStyle = ui(13.0, 600.0);
const T_TITLE: TextStyle = ui(30.0, 700.0);
const T_READOUT: TextStyle = mono(34.0, 400.0);
const T_READOUT_KEY: TextStyle = mono(9.5, 400.0).tracking(9.5 * 0.14);
const T_KEYCAP: TextStyle = ui(20.0, 500.0);
const T_FIELD_MONO: TextStyle = mono(11.5, 400.0).lh(17.0);

static FONTS: &[&[u8]] = &[
    include_bytes!("../assets/HankenGrotesk-Variable.ttf"),
    include_bytes!("../assets/B612Mono-Regular.ttf"),
    include_bytes!("../assets/B612Mono-Bold.ttf"),
];

// ---------------------------------------------------------------- the push-and-cross engine

const PUSH_RATE: f32 = 400.0; // px/s while held
const DECAY: f32 = 0.25; // s, release before crossing
const FLASH: f32 = 0.12; // s, at the crossing
const FADE: f32 = 0.30; // s, after the flash
const PREVIEW_FADE: f32 = 0.25;

#[derive(Clone, Copy, Debug)]
enum Motion {
    Idle,
    Push { t0: Instant, base: f32 },
    Release { t0: Instant, from: f32 },
    Cross { t0: Instant },
    Preview { level: f32 },
    PreviewFade { t0: Instant, from: f32 },
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
struct Sample {
    pressure: f32,
    glow: f32,
    flash: bool,
    animating: bool,
}

fn secs(a: Instant, b: Instant) -> f32 {
    b.saturating_duration_since(a).as_secs_f32()
}

impl Motion {
    /// Where the sequence is at `now`; steps the phase on (push → cross → idle) as time passes.
    fn sample(&mut self, now: Instant, res: f32) -> Sample {
        let res = res.max(1.0);
        match *self {
            Motion::Idle => Sample::default(),
            Motion::Push { t0, base } => {
                let p = base + PUSH_RATE * secs(t0, now);
                if p >= res {
                    *self = Motion::Cross { t0: t0 + Duration::from_secs_f32((res - base) / PUSH_RATE) };
                    log!("crossed at {res:.0} px");
                    return self.sample(now, res);
                }
                Sample { pressure: p, glow: p / res, flash: false, animating: true }
            }
            Motion::Release { t0, from } => {
                let k = 1.0 - secs(t0, now) / DECAY;
                if k <= 0.0 {
                    *self = Motion::Idle;
                    return Sample::default();
                }
                Sample { pressure: from * k, glow: from * k / res, flash: false, animating: true }
            }
            Motion::Cross { t0 } => {
                let t = secs(t0, now);
                if t < FLASH {
                    Sample { pressure: 0.0, glow: 1.0, flash: true, animating: true }
                } else if t < FLASH + FADE {
                    Sample { pressure: 0.0, glow: 1.0 - (t - FLASH) / FADE, flash: false, animating: true }
                } else {
                    *self = Motion::Idle;
                    Sample::default()
                }
            }
            Motion::Preview { level } => Sample { pressure: 0.0, glow: level, flash: false, animating: false },
            Motion::PreviewFade { t0, from } => {
                let k = 1.0 - secs(t0, now) / PREVIEW_FADE;
                if k <= 0.0 {
                    *self = Motion::Idle;
                    return Sample::default();
                }
                Sample { pressure: 0.0, glow: from * k, flash: false, animating: true }
            }
        }
    }
}

// ---------------------------------------------------------------- state shared across windows and threads

#[derive(Clone, Debug)]
struct DisplayInfo {
    name: String,
    native: Option<NativeDisplayId>,
    position: (i32, i32),
    size: (u32, u32),
    scale: f64,
    primary: bool,
    refresh_hz: f64,
}

impl DisplayInfo {
    fn frame(&self) -> String {
        format!("{}x{} at ({}, {}) px, scale {}", self.size.0, self.size.1, self.position.0, self.position.1, self.scale)
    }
}

struct Shared {
    motion: Motion,
    resistance: f32,
    glow_on: usize,
    displays: Vec<DisplayInfo>,
    overlay: Option<OverlayHandle<Glow>>,
    settings: Option<AppHandle<Ui>>,
    /// (fps, presents, seconds) of the overlay's last finished animation.
    last_fps: Option<(f64, u64, f64)>,
    startup_ms: Option<f64>,
    click_through: String,
    overlay_flags: String,
}

type Sh = Arc<Mutex<Shared>>;

impl Shared {
    fn sample(&mut self, now: Instant) -> Sample {
        let res = self.resistance;
        self.motion.sample(now, res)
    }
    /// Wakes both surfaces so each starts drawing; their polls keep themselves going.
    fn wake_all(&self) {
        if let Some(o) = &self.overlay {
            o.update(|_, _| {});
        }
        if let Some(a) = &self.settings {
            a.update(|_, _| {});
        }
    }
}

fn start_sequence(sh: &Sh) {
    let mut s = sh.lock().unwrap();
    s.motion = Motion::Push { t0: Instant::now(), base: 0.0 };
    log!("push-and-cross sequence starts (resistance {:.0} px)", s.resistance);
    s.wake_all();
}

fn retarget(sh: &Sh, i: usize) {
    let mut s = sh.lock().unwrap();
    if i >= s.displays.len() {
        return;
    }
    s.glow_on = i;
    let d = s.displays[i].clone();
    if let (Some(o), Some(id)) = (&s.overlay, d.native) {
        o.set_display(DisplayTarget::Native(id));
    }
    log!("overlay re-targeted to {} ({}, {:.0} Hz)", d.name, d.frame(), d.refresh_hz);
    platform::tray::set_glow(i);
    s.wake_all();
}

// ---------------------------------------------------------------- the overlay

struct Glow {
    sh: Sh,
    level: Live<f32>,
    label: Live<f32>,
    flash: bool,
    /// Start of the running animation: when, the overlay's present count then, and polls since.
    measuring: Option<(Instant, u64, u32)>,
}

fn glow_view(g: &Glow, _: &mut Cx<Glow>) -> El {
    // The dark palette's signal whatever the window shows: it draws on the desktop.
    let colour = if g.flash { hx("#b77a00") } else { hx("#5fd4f4") };
    el().key("overlay")
        .w_full()
        .h_full()
        .child(
            el().key("edge-glow")
                .w(64.0)
                .pin(None, Some(0.0), Some(0.0), Some(0.0))
                .linear_gradient(PI / 2.0, [(0.0, colour.alpha(0.0)), (0.55, colour.alpha(0.18)), (0.85, colour.alpha(0.55)), (1.0, colour)])
                .live_opacity(&g.level),
        )
        .child(
            col().key("label-box").w_full().h_full().center().child(
                text(format!("OVERLAY · {TOOLKIT}")).key("label").font(ui(48.0, 700.0)).color(Color::WHITE).live_opacity(&g.label),
            ),
        )
}

fn glow_poll(g: &mut Glow, _clock: f64, tx: &mut Tx) -> bool {
    let now = Instant::now();
    let mut sh = g.sh.lock().unwrap();
    let smp = sh.sample(now);
    g.level.set(smp.glow);
    g.label.set(if smp.glow > 0.001 { 0.08 } else { 0.0 });
    let changed = smp.flash != g.flash;
    g.flash = smp.flash;
    if changed {
        tx.instant();
    }
    let presents = sh.overlay.as_ref().map(|o| o.surface_stats().totals.presents).unwrap_or(0);
    if smp.animating {
        tx.wake_after(Duration::ZERO); // next frame, paced by the display link
        match &mut g.measuring {
            None => g.measuring = Some((now, presents, 1)),
            Some(m) => m.2 += 1,
        }
    } else if let Some((t0, p0, polls)) = g.measuring.take() {
        let dt = now.duration_since(t0).as_secs_f64();
        let n = presents.saturating_sub(p0);
        let fps = n as f64 / dt.max(1e-6);
        let d = &sh.displays[sh.glow_on];
        log!(
            "glow animation on {}: {:.3} s, {} presents, {} polls -> {:.1} fps measured (display refresh {:.0} Hz)",
            d.name, dt, n, polls, fps, d.refresh_hz
        );
        if let Some(o) = &sh.overlay {
            log!("overlay surface stats: {}", o.surface_stats().summary());
        }
        sh.last_fps = Some((fps, n, dt));
        if let Some(a) = &sh.settings {
            a.update(|_, _| {});
        }
    }
    changed
}

// ---------------------------------------------------------------- the settings window

#[derive(Clone, Copy, PartialEq)]
enum Page {
    Overview,
    Crossing,
    Keyboard,
    Design,
}

const GOES_TO: [&str; 3] = ["Last machine", "Next in order", "Next among ticked"];

struct Ui {
    sh: Sh,
    page: Page,
    resistance: f32,
    dragging_resistance: bool,
    dont_cross_dragging: bool,
    paused: bool,
    hold_trigger: bool,
    tap_ms: f32,
    goes_to: usize,
    send_to: usize,
    smp: Sample,
    flags_seen: String,
}

fn ui_poll(s: &mut Ui, _clock: f64, tx: &mut Tx) -> bool {
    let smp = s.sh.lock().unwrap().sample(Instant::now());
    if smp.animating {
        tx.wake_after(Duration::ZERO);
    }
    if smp != s.smp {
        s.smp = smp;
        tx.instant();
        return true;
    }
    false
}

fn module(p: Pal, key: &str, eyebrow: &str, body: El) -> El {
    col().key(key)
        .w_full()
        .gap(12.0)
        .px(16.0)
        .py(14.0)
        .fill(p.panel)
        .child(text(eyebrow.to_uppercase()).key("eyebrow").font(T_EYEBROW).color(p.ink_3))
        .child(body)
}

fn focusable(e: El, p: Pal) -> El {
    e.focusable(true).focus_ring(2.0, 2.0, p.signal)
}

fn switch_row(p: Pal, key: &'static str, label: &str, on: bool, flip: fn(&mut Ui)) -> El {
    let (w, h, k) = (38.0, 22.0, 16.0);
    let knob = el().key("knob").size(k, k).radius(k / 2.0).absolute(if on { w - k - 3.0 } else { 3.0 }, (h - k) / 2.0)
        .fill(if on { p.ground } else { p.ink_3 });
    let sw = el().key("switch").size(w, h).radius(h / 2.0)
        .fill(if on { p.signal } else { p.well })
        .border(1.0, if on { p.signal } else { p.edge })
        .motion(rune::tok::motion::SNAPPY)
        .child(knob)
        .a11y_hidden();
    focusable(
        row().key(key).role("switch").label(label).checked(on).w_full().h(34.0).center_y().justify_between().on_press(flip),
        p,
    )
    .child(text(label).key("label").font(T_BODY).color(p.ink).a11y_hidden())
    .child(sw)
}

/// A ruler slider: track with ticks, a signal fill and a knob. `on_set` gets the new value.
#[allow(clippy::too_many_arguments)]
fn ruler(
    p: Pal, key: &'static str, label: &'static str, value: f32, min: f32, max: f32, step: f32, enabled: bool,
    on_set: fn(&mut Ui, f32), on_grab: fn(&mut Ui, bool),
) -> El {
    let h = 34.0;
    let t = ((value - min) / (max - min)).clamp(0.0, 1.0);
    let ticks_n = 10;
    let mut ticks = row().key("ticks").w_full().h(10.0).justify_between().absolute(0.0, 22.0).a11y_hidden();
    for i in 0..=ticks_n {
        ticks = ticks.child(el().key(format!("t{i}")).w(1.0).h(if i % 5 == 0 { 10.0 } else { 5.0 }).fill(p.edge));
    }
    let ink = if enabled { p.signal } else { p.off };
    // The fill and knob are placed in fractions of the track: Rune lays out in points, so the
    // knob uses a percentage-free trick: a row whose first child grows by `t` and second by `1 - t`.
    let fill_row = row().key("fill-row").w_full().h(4.0).absolute(0.0, 9.0).radius(2.0).fill(p.well).a11y_hidden()
        .child(el().key("fill").grow(t.max(0.0001)).h(4.0).radius(2.0).fill(ink))
        .child(el().key("rest").grow((1.0 - t).max(0.0001)).h(4.0));
    let knob_row = row().key("knob-row").w_full().h(22.0).absolute(0.0, 0.0).center_y().a11y_hidden()
        .child(el().key("before").grow(t.max(0.0001)))
        .child(el().key("knob").size(4.0, 20.0).radius(2.0).fill(if enabled { p.ink } else { p.off }))
        .child(el().key("after").grow((1.0 - t).max(0.0001)));
    let mut e = el().key(key).role("slider").label(label)
        .range_value(f64::from(value), f64::from(min), f64::from(max)).range_step(f64::from(step))
        .a11y_value(format!("{value:.0}"))
        .w_full().h(h).min_w(160.0)
        .child(fill_row).child(knob_row).child(ticks)
        .disabled(!enabled);
    if enabled {
        e = focusable(e, p)
            .cursor(rune::Cursor::Pointer)
            .on_step(move |s: &mut Ui, steps, _| {
                let v = (value + steps as f32 * step).clamp(min, max);
                on_set(s, v);
            })
            .on_drag(move |s: &mut Ui, d: &mut Drag, tx| {
                let v = (min + (d.x / d.w.max(1.0)).clamp(0.0, 1.0) * (max - min)) / step;
                let v = v.round() * step;
                match d.phase {
                    Phase::Down => {
                        on_grab(s, true);
                        on_set(s, v);
                    }
                    Phase::Move => {
                        on_set(s, v);
                        tx.instant();
                    }
                    Phase::Up => on_grab(s, false),
                }
            });
    }
    e
}

fn set_resistance(s: &mut Ui, v: f32) {
    s.resistance = v;
    let mut sh = s.sh.lock().unwrap();
    sh.resistance = v;
    if s.dragging_resistance {
        sh.motion = Motion::Preview { level: v / 500.0 };
        if let Some(o) = &sh.overlay {
            o.update(|_, _| {});
        }
    }
}

fn grab_resistance(s: &mut Ui, on: bool) {
    s.dragging_resistance = on;
    let mut sh = s.sh.lock().unwrap();
    let now = Instant::now();
    if on {
        sh.motion = Motion::Preview { level: s.resistance / 500.0 };
    } else if let Motion::Preview { level } = sh.motion {
        sh.motion = Motion::PreviewFade { t0: now, from: level };
    }
    sh.wake_all();
}

fn push_strip(p: Pal, s: &Ui) -> El {
    let res = s.resistance.max(1.0);
    let t = (s.smp.pressure / 500.0).clamp(0.0, 1.0);
    let tick = (res / 500.0).clamp(0.0, 1.0);
    let flash = s.smp.flash;
    col().key("push-strip").w_full().gap(4.0)
        .role("progressbar").label("Pressure").range_value(f64::from(s.smp.pressure), 0.0, f64::from(res))
        .child(
            el().key("strip").w_full().h(14.0).radius(3.0).fill(if flash { p.flash } else { p.well }).border(1.0, p.rule)
                .child(
                    row().key("strip-fill-row").w_full().h(12.0).absolute(0.0, 0.0)
                        .child(el().key("strip-fill").grow(t.max(0.0001)).h(12.0).radius(2.0).fill(if flash { p.flash } else { p.signal }))
                        .child(el().key("strip-rest").grow((1.0 - t).max(0.0001))),
                )
                .child(
                    row().key("strip-tick-row").w_full().h(22.0).absolute(0.0, -4.0)
                        .child(el().key("tick-before").grow(tick.max(0.0001)))
                        .child(el().key("tick").w(2.0).h(22.0).fill(p.ink))
                        .child(el().key("tick-after").grow((1.0 - tick).max(0.0001))),
                )
                .a11y_hidden(),
        )
        .child(
            row().key("strip-legend").w_full().justify_between()
                .child(text(format!("PRESSURE {:>3.0} PX", s.smp.pressure)).font(T_READOUT_KEY).color(p.ink_3))
                .child(text(format!("CROSSES AT {:.0} PX", s.resistance)).font(T_READOUT_KEY).color(p.ink_3)),
        )
}

fn hold_button(p: Pal) -> El {
    let b = row().key("hold").role("button").label("Hold to push").h(32.0).px(14.0).center().radius(5.0)
        .fill(p.well).border(1.0, p.edge).hover(p.rule).press(p.signal_dim)
        .cursor(rune::Cursor::Pointer)
        .on_drag(|s: &mut Ui, d: &mut Drag, _| {
            let now = Instant::now();
            let mut sh = s.sh.lock().unwrap();
            match d.phase {
                Phase::Down => {
                    let base = sh.sample(now).pressure;
                    sh.motion = Motion::Push { t0: now, base };
                    sh.wake_all();
                }
                Phase::Move => {}
                Phase::Up => {
                    if let Motion::Push { .. } = sh.motion {
                        let from = sh.sample(now).pressure;
                        sh.motion = Motion::Release { t0: now, from };
                        sh.wake_all();
                    }
                }
            }
        })
        .child(text("Hold to push").font(T_BUTTON).color(p.ink).a11y_hidden());
    focusable(b, p)
}

fn segmented(p: Pal, hold: bool) -> El {
    let seg = |key: &'static str, label: &'static str, selected: bool, value: bool| {
        focusable(
            el().key(key).role("radio").label(label).checked(selected).h(28.0).px(12.0).center().radius(5.0)
                .fill(if selected { p.signal } else { Color::TRANSPARENT })
                .hover(if selected { Color::TRANSPARENT } else { p.rule })
                .on_press(move |s: &mut Ui| s.hold_trigger = value),
            p,
        )
        .child(text(label).font(T_BUTTON).color(if selected { p.ground } else { p.ink_2 }).a11y_hidden())
    };
    row().key("trigger").role("radiogroup").label("Trigger").focus_group().gap(2.0).padding(2.0).radius(6.0).fill(p.well).border(1.0, p.rule)
        .child(seg("double-tap", "Double-tap", !hold, false))
        .child(seg("hold-trigger", "Hold", hold, true))
}

fn keycap(p: Pal) -> El {
    row().key("keycap").role("img").label("Control key").h(44.0).px(14.0).gap(8.0).center().radius(6.0)
        .fill(p.well).border(1.0, p.edge).shadow(2.0, 0.0, p.rule)
        .child(text("⌃").font(T_KEYCAP).color(p.ink).a11y_hidden())
        .child(text("Control").font(T_KEYCAP).color(p.ink).a11y_hidden())
}

fn dropdown(p: Pal, goes_to: usize) -> El {
    focusable(
        row().key("goes-to").role("combobox").label("Shortcut goes to").a11y_value(GOES_TO[goes_to]).h(30.0).px(10.0).gap(10.0).center_y()
            .radius(4.0).fill(p.well).border(1.0, p.edge).hover(p.rule).cursor(rune::Cursor::Pointer)
            .on_press_tx(|s: &mut Ui, tx: &mut Tx| {
                let ids = ["0", "1", "2"];
                let menu = PopupMenu::picker(ids.iter().zip(GOES_TO.iter()).map(|(i, t)| (*i, *t)), ids[s.goes_to]);
                tx.popup_menu(menu, "goes-to", |s: &mut Ui, picked, _| {
                    if let Some(i) = picked.and_then(|v| v.parse().ok()) {
                        s.goes_to = i;
                    }
                });
            }),
        p,
    )
    .child(text(GOES_TO[goes_to]).font(T_BODY).color(p.ink).a11y_hidden())
    .child(text("▾").font(T_BODY).color(p.ink_3).a11y_hidden())
}

fn labelled(p: Pal, key: &'static str, label: &str, control: El) -> El {
    row().key(key).w_full().gap(16.0).center_y()
        .child(text(label).font(T_BODY).color(p.ink_2).w(170.0))
        .child(row().grow(1.0).min_w(0.0).child(control))
}

fn state_block(p: Pal, s: &Ui) -> El {
    let sh = s.sh.lock().unwrap();
    let d = &sh.displays[sh.glow_on];
    let fps = match sh.last_fps {
        Some((f, n, t)) => format!("{f:.1} fps ({n} presents in {t:.2} s)"),
        None => "no animation yet".into(),
    };
    let lines = [
        format!("resistance        {:.0} px", s.resistance),
        format!("dont_cross_drag   {}", s.dont_cross_dragging),
        format!("pause_crossing    {}", s.paused),
        format!("trigger           {}", if s.hold_trigger { "hold" } else { "double-tap" }),
        format!("time_between_taps {:.0} ms{}", s.tap_ms, if s.hold_trigger { " (disabled)" } else { "" }),
        format!("shortcut_goes_to  {}", GOES_TO[s.goes_to]),
        format!("send_input_to     {}", ["This machine", "Other machine"][s.send_to]),
        format!("pressure          {:.0} px{}", s.smp.pressure, if s.smp.flash { "  CROSSED" } else { "" }),
        format!("glow              {:.2}", s.smp.glow),
        format!("overlay_display   {}", d.name),
        format!("overlay_frame     {}", d.frame()),
        format!("click_through     {}", sh.click_through),
        format!("overlay_native    {}", sh.overlay_flags),
        format!("overlay_fps_last  {fps}"),
        format!("display_refresh   {:.0} Hz", d.refresh_hz),
        format!("startup           {}", sh.startup_ms.map(|m| format!("{m:.0} ms (process start -> first settings frame)")).unwrap_or("settings not shown yet".into())),
        format!("seen              {}", s.flags_seen),
    ];
    let mut c = col().key("state-lines").w_full().gap(0.0).padding(12.0).radius(4.0).fill(p.well);
    for (i, l) in lines.iter().enumerate() {
        c = c.child(text(l.clone()).key(format!("l{i}")).font(T_FIELD_MONO).color(p.ink_2).wrap());
    }
    c
}

fn crossing_page(p: Pal, s: &Ui) -> El {
    let resistance = col().w_full().gap(14.0)
        .child(
            row().key("res-row").w_full().gap(20.0).center_y()
                .child(row().grow(1.0).min_w(0.0).child(ruler(p, "resistance", "Resistance", s.resistance, 0.0, 500.0, 5.0, true, set_resistance, grab_resistance)))
                .child(
                    row().key("readout").gap(6.0).w(150.0).justify_end().items_end()
                        .child(text(format!("{:.0}", s.resistance)).key("readout-value").font(T_READOUT).color(p.ink))
                        .child(text("px").font(T_BODY).color(p.ink_3).mb(7.0)),
                ),
        )
        .child(text("The push, in pixels, before the pointer crosses. Dragging previews the glow on its display.").font(T_NOTE).color(p.ink_3).wrap())
        .child(push_strip(p, s))
        .child(row().gap(12.0).center_y().child(hold_button(p)).child(text("Rises at 400 px/s; crosses at the tick.").font(T_NOTE).color(p.ink_3)));

    let pointer = col().w_full().gap(4.0)
        .child(switch_row(p, "dont-cross", "Don't cross while dragging", s.dont_cross_dragging, |s| s.dont_cross_dragging = !s.dont_cross_dragging))
        .child(el().h(1.0).w_full().fill(p.rule).a11y_hidden())
        .child(switch_row(p, "pause", "Pause crossing", s.paused, |s| {
            s.paused = !s.paused;
            platform::tray::set_paused(s.paused);
            log!("pause crossing: {} (settings)", s.paused);
        }));

    let shortcut = col().w_full().gap(14.0)
        .child(labelled(p, "key-row", "Shortcut key", keycap(p)))
        .child(labelled(p, "trigger-row", "Trigger", segmented(p, s.hold_trigger)))
        .child(labelled(
            p, "taps-row", "Time between taps",
            row().w_full().gap(14.0).center_y()
                .child(row().grow(1.0).min_w(0.0).child(ruler(p, "taps", "Time between taps", s.tap_ms, 150.0, 600.0, 10.0, !s.hold_trigger, |s, v| s.tap_ms = v, |_, _| {})))
                .child(text(format!("{:.0} ms", s.tap_ms)).font(mono(13.0, 400.0)).color(if s.hold_trigger { p.off } else { p.ink }).w(64.0).align(Align::End)),
        ))
        .child(labelled(p, "goes-row", "Shortcut goes to", dropdown(p, s.goes_to)));

    col().key("crossing").w_full().gap(1.0)
        .child(
            col().key("page-head").w_full().gap(8.0).pb(24.0)
                .child(text("Crossing").font(T_TITLE).color(p.ink).role("heading").level(1))
                .child(text("How hard you push at the edge before the pointer goes to the other machine, and what the shortcut does.").font(T_BODY).color(p.ink_2).wrap()),
        )
        .child(module(p, "m-resistance", "Resistance", resistance))
        .child(module(p, "m-pointer", "This machine's pointer", pointer))
        .child(module(p, "m-shortcut", "Shortcut", shortcut))
        .child(module(p, "m-state", "State", state_block(p, s)))
}

fn placeholder(p: Pal, title: &str) -> El {
    col().w_full().gap(8.0)
        .child(text(title.to_owned()).font(T_TITLE).color(p.ink).role("heading").level(1))
        .child(text("Not in this prototype.").font(T_BODY).color(p.ink_3))
}

fn nav(p: Pal, key: &'static str, label: &'static str, page: Page, current: Page) -> El {
    let on = page == current;
    focusable(
        row().key(key).role("tab").label(label).checked(on).focus_selected(on).w_full().h(36.0).px(16.0).gap(10.0).center_y()
            .fill(if on { p.panel } else { Color::TRANSPARENT }).hover(p.well)
            .on_press(move |s: &mut Ui| s.page = page)
            .child(el().w(3.0).h(36.0).absolute(0.0, 0.0).fill(if on { p.signal } else { Color::TRANSPARENT }))
            .child(text(label).font(ui(15.0, if on { 650.0 } else { 500.0 })).color(if on { p.ink } else { p.ink_2 }).a11y_hidden()),
        p,
    )
}

fn sidebar(p: Pal, s: &Ui) -> El {
    let (here, there) = if cfg!(windows) { ("On PC", "Studio Mac") } else { ("On Mac", "Desk PC") };
    col().key("sidebar").w(172.0).h_full().fill(p.ground)
        .child(
            col().key("link").w_full().gap(3.0).px(16.0).pt(22.0).pb(20.0)
                .child(row().gap(8.0).center_y().child(el().size(8.0, 8.0).radius(4.0).fill(p.signal)).child(text(here).font(ui(19.0, 700.0)).color(p.ink)))
                .child(text(there).font(mono(12.5, 400.0)).color(p.ink_2).ml(16.0))
                .child(text("Response time: 3 ms").font(T_SMALL).color(p.ink_3).ml(16.0)),
        )
        .child(el().h(1.0).w_full().fill(p.rule))
        .child(
            col().key("nav").role("tablist").label("Pages").focus_group().w_full().pt(16.0).gap(2.0)
                .child(nav(p, "nav-overview", "Overview", Page::Overview, s.page))
                .child(nav(p, "nav-crossing", "Crossing", Page::Crossing, s.page))
                .child(nav(p, "nav-keyboard", "Keyboard", Page::Keyboard, s.page))
                .child(nav(p, "nav-design", "Design", Page::Design, s.page)),
        )
        .child(el().grow(1.0))
        .child(text(format!("Beamer bake-off · {TOOLKIT}")).key("footer").font(T_SMALL).color(p.ink_3).px(16.0).pb(14.0))
}

fn ui_view(s: &Ui, cx: &mut Cx<Ui>) -> El {
    let p = palette(cx.system_dark());
    let page = match s.page {
        Page::Crossing => crossing_page(p, s),
        Page::Overview => placeholder(p, "Overview"),
        Page::Keyboard => placeholder(p, "Keyboard"),
        Page::Design => placeholder(p, "Design"),
    };
    row().key("window").w_full().h_full().fill(p.ground)
        .child(sidebar(p, s))
        .child(el().w(1.0).h_full().fill(p.rule))
        .child(
            col().key("main").grow(1.0).min_w(0.0).h_full()
                .child(
                    col().key("page-scroll").w_full().grow(1.0).min_h(0.0).scroll(ScrollSpec { stick_bottom: false })
                        .child(col().key("page").w_full().max_w(960.0).px(32.0).pt(28.0).pb(32.0).child(page)),
                )
                .child(el().h(1.0).w_full().fill(p.rule))
                .child(
                    row().key("page-footer").w_full().h(44.0).px(24.0).center_y()
                        .child(text("Changes apply as you make them. Closing this window keeps the prototype running in the tray.").font(T_NOTE).color(p.ink_2)),
                ),
        )
}

fn on_key(_: &mut Ui, ev: &rune::KeyEvent, tx: &mut Tx) -> bool {
    match ev.key {
        rune::Key::Command('w') => tx.close_window(),
        rune::Key::Command('q') => tx.quit(),
        _ => return false,
    }
    true
}

// ---------------------------------------------------------------- startup

fn main() {
    T0.set(Instant::now()).unwrap();
    let path = log_path();
    LOG.set(Mutex::new(std::fs::File::create(&path).unwrap())).unwrap();
    let f = flags();
    log!("bakeoff-{TOOLKIT} starting; log {}; args {:?}", path.display(), std::env::args().skip(1).collect::<Vec<_>>());
    log!("rune rev 0796df5f; OS {} {}", std::env::consts::OS, std::env::consts::ARCH);

    // Rune parses its own agent-tooling flags from argv unless given its own list: give it none.
    rune::run_windows_with_args(RunOptions { accessory: true }, Vec::<String>::new(), move |displays: &[Display]| {
        log!("event loop up; {} display(s) (Rune's startup snapshot)", displays.len());
        let infos: Vec<DisplayInfo> = displays.iter().map(platform::describe).collect();
        for (i, d) in infos.iter().enumerate() {
            log!("display {i}: {:?} {}{}, refresh {:.0} Hz, native {:?}", d.name, d.frame(), if d.primary { ", main" } else { "" }, d.refresh_hz, d.native);
        }
        let glow_on = f.glow_on.filter(|&i| i < infos.len()).or_else(|| infos.iter().position(|d| !d.primary)).unwrap_or(0);
        let target = infos[glow_on].native.map(DisplayTarget::Native).unwrap_or(DisplayTarget::Primary);

        let sh: Sh = Arc::new(Mutex::new(Shared {
            motion: Motion::Idle,
            resistance: 120.0,
            glow_on,
            displays: infos.clone(),
            overlay: None,
            settings: None,
            last_fps: None,
            startup_ms: None,
            click_through: "not set yet".into(),
            overlay_flags: "not read yet".into(),
        }));

        let ui_sh = sh.clone();
        let spec = AppSpec::<Ui> {
            min_size: Some((640.0, 540.0)),
            fonts: FONTS,
            families: ("Hanken Grotesk", "B612 Mono"),
            poll: Some(ui_poll),
            on_key: Some(on_key),
            // BAKEOFF_TALL=1: a taller first window, only so an agent can screenshot the whole page.
            ..AppSpec::new("Beamer", (900.0, if std::env::var_os("BAKEOFF_TALL").is_some() { 1300.0 } else { 640.0 }), || unreachable!("init comes from App::init"), ui_view)
        };
        let frame_sh = sh.clone();
        let mut first = true;
        let mut app = App::new(spec)
            .init(move || Ui {
                sh: ui_sh,
                page: Page::Crossing,
                resistance: 120.0,
                dragging_resistance: false,
                dont_cross_dragging: true,
                paused: false,
                hold_trigger: false,
                tap_ms: 300.0,
                goes_to: 0,
                send_to: 0,
                smp: Sample::default(),
                flags_seen: "-".into(),
            })
            .keep_running()
            .chrome(rune::Chrome { full_size_content: false, ..rune::Chrome::DEFAULT })
            .tab_visits_controls(true)
            .on_close(|_, _| log!("settings window closed; still running in the tray"))
            .on_window(|w| log!("settings window created: window number {:?}", w.window_number()))
            .on_frame_drawn(move |_rt| {
                if first {
                    first = false;
                    let ms = since_start() * 1000.0;
                    log!("first settings frame drawn: {ms:.0} ms after process start");
                    let mut s = frame_sh.lock().unwrap();
                    s.startup_ms = Some(ms);
                    if let Some(a) = &s.settings {
                        a.update(|_, _| {});
                    }
                }
            });
        if !f.open_settings {
            app = app.windowless();
        }

        let mut overlay_spec = OverlaySpec::new("Beamer bake-off glow", (100.0, 100.0), glow_view);
        overlay_spec.interactive = false;
        let created_sh = sh.clone();
        let overlay = app.overlay(
            overlay_spec
                .options(OverlayOptions {
                    placement: Some(OverlayPlacement::FullScreen),
                    display: target,
                    level: OverlayLevel::Status,
                    input: OverlayInput::None,
                    prewarm: true,
                    ..Default::default()
                })
                .poll(glow_poll)
                .on_created(move |c| {
                    log!("overlay created: window number {:?}, capabilities {:?}", c.window_number, c.capabilities);
                    created_sh.lock().unwrap().click_through = "requested (OverlayInput::None)".into();
                }),
            Glow { sh: sh.clone(), level: Live::new(0.0), label: Live::new(0.0), flash: false, measuring: None },
        );
        overlay.show();
        let handle = app.handle();
        {
            let mut s = sh.lock().unwrap();
            s.overlay = Some(overlay.clone());
            s.settings = Some(handle.clone());
        }

        // Tray / menu-bar item (tray-icon: Rune has none on macOS and only a flat one on Windows).
        let names: Vec<String> = infos.iter().map(|d| d.name.clone()).collect();
        let tray_sh = sh.clone();
        let tray_app = handle.clone();
        platform::tray::install(&names, glow_on, move |cmd| match cmd {
            platform::tray::Cmd::SendTo(i) => {
                log!("tray: send input to #{i}");
                tray_app.update(move |s, _| s.send_to = i);
            }
            platform::tray::Cmd::Pause(on) => {
                log!("pause crossing: {on} (tray)");
                tray_app.update(move |s, _| s.paused = on);
            }
            platform::tray::Cmd::GlowOn(i) => retarget(&tray_sh, i),
            platform::tray::Cmd::TestGlow => start_sequence(&tray_sh),
            platform::tray::Cmd::Settings => {
                log!("tray: settings");
                tray_app.open();
            }
            platform::tray::Cmd::Quit => {
                log!("tray: quit");
                tray_app.quit();
            }
        });
        log!("tray installed: {:.0} ms after process start", since_start() * 1000.0);

        // Read back what the overlay window really got, once AppKit / Win32 made it.
        let probe_sh = sh.clone();
        let probe_app = handle.clone();
        std::thread::spawn(move || {
            std::thread::sleep(Duration::from_millis(1500));
            probe_app.update(move |s, _| {
                let number = probe_sh.lock().unwrap().overlay.as_ref().and_then(|o| o.window_number());
                let (click_through, flags) = platform::overlay_flags(number, "Beamer bake-off glow");
                log!("overlay native flags: {flags}");
                let mut sh = probe_sh.lock().unwrap();
                sh.click_through = click_through;
                sh.overlay_flags = flags.clone();
                s.flags_seen = format!("read back {:.1} s after start", since_start());
            });
        });

        if f.test_glow {
            let sh = sh.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_secs(1));
                loop {
                    start_sequence(&sh);
                    std::thread::sleep(Duration::from_secs(3));
                }
            });
        }
        if let Some(secs) = f.quit_after {
            let h = handle.clone();
            std::thread::spawn(move || {
                std::thread::sleep(Duration::from_secs_f64(secs));
                log!("--quit-after {secs} s: quitting");
                h.quit();
            });
        }
        if f.open_settings {
            log!("--open-settings: settings window opens at start");
        }
        vec![WindowSpec::from_app("settings", app)]
    });
    log!("event loop ended");
}

