// PROTOTYPE: the one shared state (settings, pressure, overlay facts) and the push-and-cross clock.
use std::time::{Duration, Instant};

use gpui::{DisplayId, Global, Entity};

pub const PUSH_RATE: f32 = 400.0; // px of pressure per second while held
pub const RELEASE: Duration = Duration::from_millis(250);
pub const FLASH: Duration = Duration::from_millis(120);
pub const FADE: Duration = Duration::from_millis(300);
pub const PREVIEW_FADE: Duration = Duration::from_millis(250);

/// The animation is a pure function of time from the phase's start, so every window
/// (overlay, settings strip) samples the same clock at its own frame rate.
#[derive(Clone, Copy, Debug)]
pub enum Phase {
    Idle,
    Push { p0: f32, t0: Instant },
    Release { p0: f32, t0: Instant },
    Cross { t0: Instant },
    Preview { level: f32 },
    PreviewFade { level: f32, t0: Instant },
}

#[derive(Clone, Copy, Debug, Default)]
pub struct Sample {
    pub pressure: f32,
    pub intensity: f32,
    /// Strip and glow show `resistance_flash`.
    pub flash: bool,
    /// Time-driven: the windows must keep asking for frames.
    pub animating: bool,
}

impl Phase {
    /// Folds a push that has reached the resistance into a crossing, and finished phases into Idle.
    pub fn resolve(self, now: Instant, r: f32) -> Phase {
        match self {
            Phase::Push { p0, t0 } => {
                let need = ((r - p0).max(0.0)) / PUSH_RATE;
                let tc = t0 + Duration::from_secs_f32(need);
                if now >= tc { Phase::Cross { t0: tc }.resolve(now, r) } else { self }
            }
            Phase::Release { t0, .. } if now >= t0 + RELEASE => Phase::Idle,
            Phase::Cross { t0 } if now >= t0 + FLASH + FADE => Phase::Idle,
            Phase::PreviewFade { t0, .. } if now >= t0 + PREVIEW_FADE => Phase::Idle,
            other => other,
        }
    }

    pub fn sample(self, now: Instant, r: f32) -> Sample {
        let r_safe = r.max(1.0);
        match self.resolve(now, r) {
            Phase::Idle => Sample::default(),
            Phase::Push { p0, t0 } => {
                let p = (p0 + PUSH_RATE * now.duration_since(t0).as_secs_f32()).min(r);
                Sample { pressure: p, intensity: p / r_safe, flash: false, animating: true }
            }
            Phase::Release { p0, t0 } => {
                let k = 1.0 - now.duration_since(t0).as_secs_f32() / RELEASE.as_secs_f32();
                let p = p0 * k.clamp(0.0, 1.0);
                Sample { pressure: p, intensity: p / r_safe, flash: false, animating: true }
            }
            Phase::Cross { t0 } => {
                let dt = now.duration_since(t0);
                if dt < FLASH {
                    Sample { pressure: r, intensity: 1.0, flash: true, animating: true }
                } else {
                    let k = 1.0 - (dt - FLASH).as_secs_f32() / FADE.as_secs_f32();
                    Sample { pressure: 0.0, intensity: k.clamp(0.0, 1.0), flash: false, animating: true }
                }
            }
            Phase::Preview { level } => Sample { pressure: 0.0, intensity: level, flash: false, animating: false },
            Phase::PreviewFade { level, t0 } => {
                let k = 1.0 - now.duration_since(t0).as_secs_f32() / PREVIEW_FADE.as_secs_f32();
                Sample { pressure: 0.0, intensity: level * k.clamp(0.0, 1.0), flash: false, animating: true }
            }
        }
    }
}

#[derive(Clone, Debug)]
pub struct DisplayEntry {
    pub id: DisplayId,
    pub name: String,
    pub frame: String,
    pub scale: f64,
    pub refresh_hz: f32,
    pub is_main: bool,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Page { Overview, Crossing, Keyboard, Design }

pub struct AppState {
    pub page: Page,
    pub resistance: f32,
    pub dont_cross_dragging: bool,
    pub paused: bool,
    pub shortcut_hold: bool,
    pub tap_ms: f32,
    pub goes_to: usize,
    pub send_to: usize,
    pub displays: Vec<DisplayEntry>,
    pub glow_on: Option<DisplayId>,
    pub phase: Phase,
    // Facts the overlay reports back, shown in the State block.
    pub overlay_frame: String,
    pub click_through: String,
    pub last_fps: Option<(f32, u32, f32)>, // fps, frames, seconds
    pub refresh_hz: f32,
    pub startup_ms: Option<f64>,
    pub started: Instant,
}

pub const GOES_TO: [&str; 3] = ["Last machine", "Next in order", "Next among ticked"];

impl AppState {
    pub fn sample(&self, now: Instant) -> Sample {
        self.phase.sample(now, self.resistance)
    }

    pub fn start_push(&mut self, now: Instant) {
        let p0 = self.sample(now).pressure;
        self.phase = Phase::Push { p0: if matches!(self.phase.resolve(now, self.resistance), Phase::Release { .. }) { p0 } else { 0.0 }, t0: now };
    }

    pub fn end_push(&mut self, now: Instant) {
        if let Phase::Push { .. } = self.phase.resolve(now, self.resistance) {
            let p0 = self.sample(now).pressure;
            self.phase = Phase::Release { p0, t0: now };
        }
    }

    pub fn preview(&mut self) {
        self.phase = Phase::Preview { level: self.resistance / 500.0 };
    }

    pub fn end_preview(&mut self, now: Instant) {
        if let Phase::Preview { level } = self.phase {
            self.phase = Phase::PreviewFade { level, t0: now };
        }
    }

    pub fn glow_display(&self) -> Option<&DisplayEntry> {
        self.displays.iter().find(|d| Some(d.id) == self.glow_on)
    }
}

pub struct GlobalState(pub Entity<AppState>);
impl Global for GlobalState {}
