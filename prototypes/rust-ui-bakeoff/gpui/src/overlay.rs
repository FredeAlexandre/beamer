// PROTOTYPE: the click-through edge-glow overlay. Renders only when notified; while a time-driven
// animation runs it asks for the next frame, and it counts those frames to measure the frame rate.
use std::time::{Duration, Instant};

use gpui::{
    Context, Entity, IntoElement, ParentElement, Render, Styled, Window, div, linear_color_stop, linear_gradient, px,
};

use crate::{logln, platform, state::AppState, theme};

pub struct Overlay {
    state: Entity<AppState>,
    frames: u32,
    first: Option<Instant>,
    last: Option<Instant>,
    max_gap: Duration,
    probed: bool,
}

impl Overlay {
    pub fn new(state: Entity<AppState>, _window: &mut Window, cx: &mut Context<Self>) -> Self {
        cx.observe(&state, |_, _, cx| cx.notify()).detach();
        Overlay { state, frames: 0, first: None, last: None, max_gap: Duration::ZERO, probed: false }
    }
}

impl Render for Overlay {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let now = Instant::now();
        let s = self.state.read(cx).sample(now);

        if s.animating {
            if let Some(last) = self.last {
                self.max_gap = self.max_gap.max(now - last);
            }
            self.first.get_or_insert(now);
            self.last = Some(now);
            self.frames += 1;
            window.request_animation_frame();
            if s.flash && !self.probed {
                // Once per animation: does the glow reach the screen, and do clicks pass through it?
                self.probed = true;
                cx.spawn_in(window, async move |_, cx| {
                    cx.background_executor().timer(Duration::from_millis(40)).await;
                    cx.update(|window, _| logln!("probe during flash: {}", platform::probe_overlay(window))).ok();
                })
                .detach();
            }
        } else if self.frames > 0 {
            let secs = self.last.unwrap().duration_since(self.first.unwrap()).as_secs_f32();
            let fps = if secs > 0.0 { (self.frames - 1) as f32 / secs } else { 0.0 };
            let hz = window.refresh_interval(cx).map(|i| 1.0 / i.as_secs_f32()).unwrap_or(0.0);
            logln!(
                "animation: {} frames in {:.3} s = {:.1} fps (display {:.1} Hz, longest gap {:.1} ms)",
                self.frames, secs, fps, hz, self.max_gap.as_secs_f64() * 1000.0
            );
            let frames = self.frames;
            self.state.update(cx, |st, _| st.last_fps = Some((fps, frames, secs)));
            self.frames = 0;
            self.first = None;
            self.last = None;
            self.max_gap = Duration::ZERO;
            self.probed = false;
            cx.spawn_in(window, async move |_, cx| {
                cx.background_executor().timer(Duration::from_millis(60)).await;
                cx.update(|window, _| logln!("probe when idle: {}", platform::probe_overlay(window))).ok();
            })
            .detach();
        }

        let root = div().size_full().relative();
        if s.intensity <= 0.001 {
            return root; // nothing lit: an empty, fully transparent frame
        }
        let pal = theme::dark(); // the desktop effects always take the dark palette
        let colour = if s.flash { pal.resistance_flash } else { pal.signal };
        let a = s.intensity.clamp(0.0, 1.0);
        root.child(
            // ~64 logical px of falloff: a steep inner gradient over a softer, wider one.
            div()
                .absolute()
                .top_0()
                .bottom_0()
                .right_0()
                .w(px(64.))
                .bg(linear_gradient(90., linear_color_stop(colour.opacity(0.0), 0.0), linear_color_stop(colour.opacity(a * 0.55), 1.0))),
        )
        .child(
            div()
                .absolute()
                .top_0()
                .bottom_0()
                .right_0()
                .w(px(16.))
                .bg(linear_gradient(90., linear_color_stop(colour.opacity(0.0), 0.0), linear_color_stop(colour.opacity(a * 0.6), 1.0))),
        )
        .child(
            div()
                .absolute()
                .size_full()
                .flex()
                .items_center()
                .justify_center()
                .font_family(theme::MONO)
                .text_size(px(28.))
                .text_color(pal.ink.opacity(0.08))
                .child("OVERLAY · gpui"),
        )
    }
}
