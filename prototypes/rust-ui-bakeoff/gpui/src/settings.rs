// PROTOTYPE: the settings window. GPUI has no widgets: every control below (slider, switch, segmented
// control, dropdown, button, focus ring) is built here from `div`s, `canvas` and event listeners.
use std::time::Instant;

use gpui::{
    AnyElement, App, Bounds, Context, DispatchPhase, Entity, FocusHandle, FontWeight, Hsla, InteractiveElement,
    IntoElement, KeyDownEvent, MouseButton, MouseDownEvent, MouseMoveEvent, MouseUpEvent, ParentElement, Pixels,
    Render, SharedString, StatefulInteractiveElement, Styled, Subscription, WeakEntity, Window, actions, anchored,
    canvas, deferred, div, point, px,
};

use gpui::prelude::FluentBuilder;
use crate::{
    logln,
    state::{AppState, GOES_TO, Page},
    theme::{self, Palette},
};

actions!(bakeoff, [FocusNext, FocusPrev]);

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
enum SliderKind {
    Resistance,
    Tap,
}

impl SliderKind {
    fn range(self) -> (f32, f32, f32) {
        match self {
            SliderKind::Resistance => (0.0, 500.0, 5.0),
            SliderKind::Tap => (150.0, 600.0, 10.0),
        }
    }
}

struct Focus {
    root: FocusHandle,
    nav: Vec<FocusHandle>,
    resistance: FocusHandle,
    hold: FocusHandle,
    sw_drag: FocusHandle,
    sw_pause: FocusHandle,
    segmented: FocusHandle,
    tap: FocusHandle,
    dropdown: FocusHandle,
}

pub struct Settings {
    state: Entity<AppState>,
    f: Focus,
    dragging: Option<SliderKind>,
    holding: bool,
    dropdown_open: bool,
    first_frame: bool,
    _subs: Vec<Subscription>,
}

impl Settings {
    pub fn new(state: Entity<AppState>, window: &mut Window, cx: &mut Context<Self>) -> Self {
        let mut i = 0;
        let mut stop = |cx: &mut Context<Self>| {
            i += 1;
            cx.focus_handle().tab_index(i).tab_stop(true)
        };
        let f = Focus {
            root: cx.focus_handle(),
            nav: (0..4).map(|_| stop(cx)).collect(),
            resistance: stop(cx),
            hold: stop(cx),
            sw_drag: stop(cx),
            sw_pause: stop(cx),
            segmented: stop(cx),
            tap: stop(cx),
            dropdown: stop(cx),
        };
        window.focus(&f.root, cx);
        let subs = vec![
            cx.observe(&state, |_, _, cx| cx.notify()),
            // Follow the system light/dark appearance live.
            cx.observe_window_appearance(window, |_, window, cx| {
                logln!("settings: appearance changed to {:?}", window.appearance());
                cx.notify();
            }),
        ];
        Settings { state, f, dragging: None, holding: false, dropdown_open: false, first_frame: false, _subs: subs }
    }

    fn set_slider(&mut self, kind: SliderKind, v: f32, cx: &mut Context<Self>) {
        let (min, max, step) = kind.range();
        let v = ((v / step).round() * step).clamp(min, max);
        let dragging = self.dragging == Some(SliderKind::Resistance);
        self.state.update(cx, |s, cx| {
            match kind {
                SliderKind::Resistance => {
                    s.resistance = v;
                    if dragging {
                        s.preview();
                    }
                }
                SliderKind::Tap => s.tap_ms = v,
            }
            cx.notify();
        });
    }

    fn end_drag(&mut self, cx: &mut Context<Self>) {
        if self.dragging.take() == Some(SliderKind::Resistance) {
            self.state.update(cx, |s, cx| {
                s.end_preview(Instant::now());
                cx.notify();
            });
        }
        cx.notify();
    }

    fn press_hold(&mut self, down: bool, cx: &mut Context<Self>) {
        if down == self.holding {
            return;
        }
        self.holding = down;
        self.state.update(cx, |s, cx| {
            if down { s.start_push(Instant::now()) } else { s.end_push(Instant::now()) }
            cx.notify();
        });
    }

    fn update_state(&mut self, cx: &mut Context<Self>, f: impl FnOnce(&mut AppState)) {
        self.state.update(cx, |s, cx| {
            f(s);
            cx.notify();
        });
    }
}

fn eyebrow(text: &'static str, p: &Palette) -> impl IntoElement {
    div().text_size(px(theme::EYEBROW)).text_color(p.ink_3).font_weight(FontWeight::SEMIBOLD).child(text)
}

fn module(p: &Palette) -> gpui::Div {
    div().bg(p.panel).px(px(16.)).py(px(14.)).flex().flex_col().gap(px(10.))
}

fn ring<E: Styled + InteractiveElement>(e: E, p: &Palette) -> E {
    let c = p.signal;
    e.border_2().border_color(gpui::transparent_black()).focus_visible(move |s| s.border_color(c))
}

impl Render for Settings {
    fn render(&mut self, window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let started = self.state.read(cx).started;
        if !self.first_frame {
            self.first_frame = true;
            logln!("settings: first render at {:.1} ms", started.elapsed().as_secs_f64() * 1000.0);
            let state = self.state.clone();
            window.on_next_frame(move |_, cx| {
                let ms = started.elapsed().as_secs_f64() * 1000.0;
                logln!("startup: process start -> first settings frame presented {ms:.1} ms");
                state.update(cx, |s, cx| {
                    s.startup_ms.get_or_insert(ms);
                    cx.notify();
                });
            });
        }
        let p = theme::for_appearance(window.appearance());
        let now = Instant::now();
        let sample = self.state.read(cx).sample(now);
        if sample.animating {
            window.request_animation_frame();
        }
        let page = self.state.read(cx).page;

        div()
            .id("root")
            .track_focus(&self.f.root)
            .on_action(cx.listener(|_, _: &FocusNext, window, cx| {
                window.focus_next(cx);
                logln!("key: Tab -> focus_next, focused {:?}", window.focused(cx).is_some());
            }))
            .on_action(cx.listener(|_, _: &FocusPrev, window, cx| window.focus_prev(cx)))
            .capture_key_down(cx.listener(|_, e: &KeyDownEvent, _, _| logln!("key down: {:?}", e.keystroke.key)))
            .size_full()
            .flex()
            .flex_row()
            .bg(p.ground)
            .text_color(p.ink)
            .font_family(theme::UI)
            .text_size(px(theme::BODY))
            .child(self.sidebar(&p, page, cx))
            .child(
                div()
                    .flex_1()
                    .h_full()
                    .flex()
                    .flex_col()
                    .border_l_1()
                    .border_color(p.rule)
                    .child(
                        div()
                            .id("page")
                            .flex_1()
                            .overflow_y_scroll()
                            .child(match page {
                                Page::Crossing => self.crossing(&p, sample, window, cx).into_any_element(),
                                other => div()
                                    .px(px(32.))
                                    .py(px(28.))
                                    .flex()
                                    .flex_col()
                                    .gap(px(8.))
                                    .child(div().text_size(px(30.)).font_weight(FontWeight::BOLD).child(format!("{other:?}")))
                                    .child(div().text_color(p.ink_2).child("Not in this prototype."))
                                    .into_any_element(),
                            }),
                    )
                    .child(
                        div()
                            .px(px(24.))
                            .py(px(12.))
                            .border_t_1()
                            .border_color(p.rule)
                            .text_color(p.ink_2)
                            .child("Changes apply as you make them. Closing this window keeps the bake-off running in the tray."),
                    ),
            )
    }
}

impl Settings {
    fn sidebar(&self, p: &Palette, page: Page, cx: &mut Context<Self>) -> impl IntoElement {
        #[cfg(target_os = "macos")]
        let (on, peer) = ("On Mac", "Desk PC");
        #[cfg(not(target_os = "macos"))]
        let (on, peer) = ("On PC", "Studio Mac");
        let rows = [Page::Overview, Page::Crossing, Page::Keyboard, Page::Design];
        div()
            .w(px(170.))
            .h_full()
            .flex()
            .flex_col()
            .child(
                div()
                    .px(px(16.))
                    .py(px(18.))
                    .border_b_1()
                    .border_color(p.rule)
                    .flex()
                    .flex_col()
                    .gap(px(2.))
                    .child(
                        div()
                            .flex()
                            .items_center()
                            .gap(px(8.))
                            .child(div().size(px(9.)).rounded_full().bg(p.signal))
                            .child(div().text_size(px(20.)).font_weight(FontWeight::BOLD).child(on)),
                    )
                    .child(div().pl(px(17.)).font_family(theme::MONO).text_color(p.ink_2).child(peer))
                    .child(div().pl(px(17.)).text_size(px(theme::SMALL)).text_color(p.ink_3).child("Response time: 3 ms")),
            )
            .child(div().flex().flex_col().py(px(10.)).children(rows.iter().enumerate().map(|(i, &r)| {
                let selected = r == page;
                let fh = self.f.nav[i].clone();
                ring(
                    div()
                        .id(("nav", i))
                        .track_focus(&fh)
                        .px(px(14.))
                        .py(px(8.))
                        .text_size(px(15.))
                        .cursor_pointer()
                        .relative()
                        .when(selected, |d| d.bg(p.well).text_color(p.ink).font_weight(FontWeight::SEMIBOLD))
                        .when(!selected, |d| d.text_color(p.ink_2).hover(|s| s.bg(p.panel)))
                        .on_click(cx.listener(move |this, _, _, cx| this.update_state(cx, |s| s.page = r)))
                        .on_key_down(cx.listener(move |this, e: &KeyDownEvent, _, cx| {
                            if matches!(e.keystroke.key.as_str(), "space" | "enter") {
                                this.update_state(cx, |s| s.page = r);
                            }
                        })),
                    p,
                )
                .when(selected, |d| d.child(div().absolute().left(px(-2.)).top_0().bottom_0().w(px(3.)).bg(p.signal)))
                .child(format!("{r:?}"))
            })))
            .child(div().flex_1())
            .child(
                div()
                    .px(px(16.))
                    .py(px(14.))
                    .text_size(px(theme::SMALL))
                    .text_color(p.ink_3)
                    .child("Beamer bake-off · gpui"),
            )
    }

    fn crossing(&mut self, p: &Palette, sample: crate::state::Sample, _window: &mut Window, cx: &mut Context<Self>) -> impl IntoElement {
        let s = self.state.read(cx);
        let resistance = s.resistance;
        let (dont, paused, hold_mode, tap, goes) = (s.dont_cross_dragging, s.paused, s.shortcut_hold, s.tap_ms, s.goes_to);
        let state_lines = state_block(s, sample);

        div()
            .px(px(32.))
            .py(px(28.))
            .flex()
            .flex_col()
            .gap(px(18.))
            .child(div().text_size(px(30.)).font_weight(FontWeight::BOLD).child("Crossing"))
            .child(
                div()
                    .text_color(p.ink_2)
                    .child("How hard the pointer pushes against the edge before it crosses to the other machine."),
            )
            .child(
                div()
                    .flex()
                    .flex_col()
                    .gap(px(1.))
                    // 1. Resistance
                    .child(
                        module(p)
                            .child(eyebrow("RESISTANCE", p))
                            .child(
                                div()
                                    .flex()
                                    .items_end()
                                    .gap(px(6.))
                                    .font_family(theme::MONO)
                                    .child(div().text_size(px(theme::READOUT)).child(format!("{resistance:.0}")))
                                    // items_baseline did not line the two sizes up; end + a nudge does.
                                    .child(div().text_size(px(theme::BODY)).text_color(p.ink_2).mb(px(7.)).child("px")),
                            )
                            .child(self.slider(SliderKind::Resistance, resistance, true, p, cx))
                            .child(push_strip(p, sample, resistance))
                            .child(
                                div().flex().items_center().gap(px(12.)).child(self.hold_button(p, cx)).child(
                                    div()
                                        .text_size(px(theme::NOTE))
                                        .text_color(p.ink_3)
                                        .child("Hold to build pressure at 400 px/s; it crosses at the resistance."),
                                ),
                            ),
                    )
                    // 2. This machine's pointer
                    .child(
                        module(p)
                            .child(eyebrow("THIS MACHINE'S POINTER", p))
                            .child(self.switch_row("Don't cross while dragging", dont, 0, p, cx))
                            .child(self.switch_row("Pause crossing", paused, 1, p, cx)),
                    )
                    // 3. Shortcut
                    .child(
                        module(p)
                            .child(eyebrow("SHORTCUT", p))
                            .child(
                                div().flex().items_center().gap(px(16.)).child(keycap(p)).child(self.segmented(hold_mode, p, cx)),
                            )
                            .child(
                                div()
                                    .flex()
                                    .flex_col()
                                    .gap(px(4.))
                                    .child(
                                        div()
                                            .flex()
                                            .justify_between()
                                            .text_color(if hold_mode { p.ink_3 } else { p.ink })
                                            .child("Time between taps")
                                            .child(div().font_family(theme::MONO).child(format!("{tap:.0} ms"))),
                                    )
                                    .child(self.slider(SliderKind::Tap, tap, !hold_mode, p, cx)),
                            )
                            .child(
                                div()
                                    .flex()
                                    .items_center()
                                    .gap(px(12.))
                                    .child("Shortcut goes to")
                                    .child(self.dropdown(goes, p, cx)),
                            ),
                    )
                    // 4. State
                    .child(
                        module(p).child(eyebrow("STATE", p)).child(
                            div()
                                .font_family(theme::MONO)
                                .text_size(px(theme::SMALL))
                                .text_color(p.ink_2)
                                .flex()
                                .flex_col()
                                .gap(px(2.))
                                .children(state_lines.into_iter().map(|(k, v)| {
                                    div()
                                        .flex()
                                        .gap(px(8.))
                                        .child(div().w(px(230.)).flex_none().text_color(p.ink_3).child(k))
                                        .child(div().flex_1().child(v))
                                })),
                        ),
                    ),
            )
    }

    /// A ruler slider: track with ticks, a fill and a thumb. `canvas` gives the track's bounds at
    /// paint time, where window-level mouse listeners turn x into a value (drags continue outside).
    fn slider(&self, kind: SliderKind, value: f32, enabled: bool, p: &Palette, cx: &mut Context<Self>) -> impl IntoElement {
        let (min, max, step) = kind.range();
        let frac = (value - min) / (max - min);
        let fh = match kind {
            SliderKind::Resistance => self.f.resistance.clone(),
            SliderKind::Tap => self.f.tap.clone(),
        };
        let weak: WeakEntity<Self> = cx.entity().downgrade();
        let dragging = self.dragging == Some(kind);
        let fh2 = fh.clone();
        let major = if kind == SliderKind::Resistance { 50.0 } else { 50.0 };
        let ticks = ((max - min) / (major / 5.0)) as usize;
        let (thumb, fill) = if enabled { (p.ink, p.signal) } else { (p.off, p.off) };
        let tick_colour = p.edge;

        ring(div().id(match kind { SliderKind::Resistance => "slider-res", SliderKind::Tap => "slider-tap" }), p)
            .track_focus(&fh)
            .when(!enabled, |d| d.tab_stop(false))
            .rounded(px(4.))
            .px(px(8.))
            .py(px(4.))
            .on_key_down(cx.listener(move |this, e: &KeyDownEvent, _, cx| {
                if !enabled {
                    return;
                }
                let big = if e.keystroke.modifiers.shift { 10.0 } else { 1.0 };
                let d = match e.keystroke.key.as_str() {
                    "left" | "down" => -step * big,
                    "right" | "up" => step * big,
                    _ => return,
                };
                let v = match kind {
                    SliderKind::Resistance => this.state.read(cx).resistance,
                    SliderKind::Tap => this.state.read(cx).tap_ms,
                };
                this.set_slider(kind, v + d, cx);
                cx.stop_propagation();
            }))
            .child(
                div()
                    .relative()
                    .h(px(30.))
                    .w_full()
                    // ticks
                    .children((0..=ticks).map(|i| {
                        let x = i as f32 / ticks as f32;
                        let is_major = i % 5 == 0;
                        div()
                            .absolute()
                            .left(gpui::relative(x))
                            .top(px(if is_major { 18. } else { 22. }))
                            .w(px(1.))
                            .h(px(if is_major { 10. } else { 6. }))
                            .bg(tick_colour)
                    }))
                    // rail and fill
                    .child(div().absolute().left_0().right_0().top(px(8.)).h(px(4.)).rounded(px(2.)).bg(p.well))
                    .child(div().absolute().left_0().w(gpui::relative(frac)).top(px(8.)).h(px(4.)).rounded(px(2.)).bg(fill))
                    // thumb
                    .child(
                        div()
                            .absolute()
                            .left(gpui::relative(frac))
                            .ml(px(-7.))
                            .top(px(3.))
                            .size(px(14.))
                            .rounded_full()
                            .bg(thumb)
                            .when(dragging, |d| d.border_2().border_color(p.signal)),
                    )
                    .child(
                        canvas(
                            |bounds, _, _| bounds,
                            move |_, bounds: Bounds<Pixels>, window, _| {
                                if !enabled {
                                    return;
                                }
                                let to_value = move |x: Pixels| min + ((x - bounds.left()) / bounds.size.width).clamp(0.0, 1.0) * (max - min);
                                let w = weak.clone();
                                let fh = fh2.clone();
                                window.on_mouse_event(move |e: &MouseDownEvent, phase, window, cx| {
                                    if phase == DispatchPhase::Bubble && e.button == MouseButton::Left && bounds.contains(&e.position) {
                                        fh.focus(window, cx);
                                        w.update(cx, |this, cx| {
                                            this.dragging = Some(kind);
                                            this.set_slider(kind, to_value(e.position.x), cx);
                                        })
                                        .ok();
                                    }
                                });
                                if dragging {
                                    let w = weak.clone();
                                    window.on_mouse_event(move |e: &MouseMoveEvent, phase, _, cx| {
                                        if phase == DispatchPhase::Bubble {
                                            w.update(cx, |this, cx| this.set_slider(kind, to_value(e.position.x), cx)).ok();
                                        }
                                    });
                                    let w = weak.clone();
                                    window.on_mouse_event(move |_: &MouseUpEvent, phase, _, cx| {
                                        if phase == DispatchPhase::Bubble {
                                            w.update(cx, |this, cx| this.end_drag(cx)).ok();
                                        }
                                    });
                                }
                            },
                        )
                        .absolute()
                        .size_full(),
                    ),
            )
    }

    fn hold_button(&self, p: &Palette, cx: &mut Context<Self>) -> impl IntoElement {
        let holding = self.holding;
        ring(div().id("hold"), p)
            .track_focus(&self.f.hold)
            .px(px(14.))
            .py(px(7.))
            .rounded(px(5.))
            .bg(if holding { p.signal_dim } else { p.well })
            .text_color(p.ink)
            .cursor_pointer()
            .on_mouse_down(MouseButton::Left, cx.listener(|this, _, _, cx| this.press_hold(true, cx)))
            .on_mouse_up(MouseButton::Left, cx.listener(|this, _, _, cx| this.press_hold(false, cx)))
            .on_mouse_up_out(MouseButton::Left, cx.listener(|this, _, _, cx| this.press_hold(false, cx)))
            .on_key_down(cx.listener(|this, e: &KeyDownEvent, _, cx| {
                if e.keystroke.key == "space" && !e.is_held {
                    this.press_hold(true, cx);
                }
            }))
            .on_key_up(cx.listener(|this, e: &gpui::KeyUpEvent, _, cx| {
                if e.keystroke.key == "space" {
                    this.press_hold(false, cx);
                }
            }))
            .child("Hold to push")
    }

    fn switch_row(&self, label: &'static str, on: bool, which: usize, p: &Palette, cx: &mut Context<Self>) -> impl IntoElement {
        let fh = if which == 0 { &self.f.sw_drag } else { &self.f.sw_pause };
        let toggle = move |this: &mut Self, cx: &mut Context<Self>| {
            this.update_state(cx, |s| {
                if which == 0 { s.dont_cross_dragging = !s.dont_cross_dragging } else { s.paused = !s.paused }
            })
        };
        div().flex().items_center().justify_between().child(label).child(
            ring(div().id(("switch", which)), p)
                .track_focus(fh)
                .rounded_full()
                .p(px(1.))
                .cursor_pointer()
                .on_click(cx.listener(move |this, _, _, cx| toggle(this, cx)))
                .on_key_down(cx.listener(move |this, e: &KeyDownEvent, _, cx| {
                    if e.keystroke.key == "space" {
                        toggle(this, cx);
                    }
                }))
                .child(
                    div()
                        .w(px(36.))
                        .h(px(20.))
                        .rounded_full()
                        .border_1()
                        .border_color(if on { p.signal } else { p.edge })
                        .bg(if on { p.signal } else { p.well })
                        .flex()
                        .items_center()
                        .when(on, |d| d.justify_end())
                        .px(px(2.))
                        .child(div().size(px(14.)).rounded_full().bg(if on { p.ground } else { p.ink_3 })),
                ),
        )
    }

    fn segmented(&self, hold_mode: bool, p: &Palette, cx: &mut Context<Self>) -> impl IntoElement {
        let seg = |label: &'static str, selected: bool, value: bool, p: &Palette, cx: &mut Context<Self>| {
            div()
                .id(label)
                .px(px(12.))
                .py(px(5.))
                .rounded(px(4.))
                .cursor_pointer()
                .when(selected, |d| d.bg(p.signal).text_color(p.ground))
                .when(!selected, |d| d.text_color(p.ink_2))
                .on_click(cx.listener(move |this, _, _, cx| this.update_state(cx, |s| s.shortcut_hold = value)))
                .child(label)
        };
        ring(div().id("segmented"), p)
            .track_focus(&self.f.segmented)
            .flex()
            .rounded(px(6.))
            .bg(p.well)
            .p(px(2.))
            .on_key_down(cx.listener(|this, e: &KeyDownEvent, _, cx| match e.keystroke.key.as_str() {
                "left" => this.update_state(cx, |s| s.shortcut_hold = false),
                "right" => this.update_state(cx, |s| s.shortcut_hold = true),
                "space" => this.update_state(cx, |s| s.shortcut_hold = !s.shortcut_hold),
                _ => {}
            }))
            .child(seg("Double-tap", !hold_mode, false, p, cx))
            .child(seg("Hold", hold_mode, true, p, cx))
    }

    fn dropdown(&self, goes: usize, p: &Palette, cx: &mut Context<Self>) -> impl IntoElement {
        let open = self.dropdown_open;
        let pal = *p;
        div()
            .flex()
            .flex_col()
            .child(
                ring(div().id("dropdown"), p)
                    .track_focus(&self.f.dropdown)
                    .min_w(px(200.))
                    .flex()
                    .justify_between()
                    .gap(px(12.))
                    .px(px(10.))
                    .py(px(5.))
                    .rounded(px(4.))
                    .bg(p.well)
                    .cursor_pointer()
                    .on_click(cx.listener(|this, _, _, cx| {
                        this.dropdown_open = !this.dropdown_open;
                        cx.notify();
                    }))
                    .on_key_down(cx.listener(|this, e: &KeyDownEvent, _, cx| match e.keystroke.key.as_str() {
                        "space" | "enter" => {
                            this.dropdown_open = !this.dropdown_open;
                            cx.notify();
                        }
                        "escape" => {
                            this.dropdown_open = false;
                            cx.notify();
                        }
                        "up" => this.update_state(cx, |s| s.goes_to = s.goes_to.saturating_sub(1)),
                        "down" => this.update_state(cx, |s| s.goes_to = (s.goes_to + 1).min(GOES_TO.len() - 1)),
                        _ => {}
                    }))
                    .child(GOES_TO[goes])
                    .child(div().text_color(p.ink_3).child("▾")),
            )
            .when(open, |d| {
                d.child(deferred(
                    anchored().child(
                        div()
                            .id("dropdown-list")
                            .mt(px(4.))
                            .min_w(px(200.))
                            .bg(pal.panel)
                            .border_1()
                            .border_color(pal.edge)
                            .rounded(px(4.))
                            .py(px(4.))
                            .shadow_lg()
                            .on_mouse_down_out(cx.listener(|this, _, _, cx| {
                                this.dropdown_open = false;
                                cx.notify();
                            }))
                            .children(GOES_TO.iter().enumerate().map(|(i, label)| {
                                div()
                                    .id(("goes", i))
                                    .px(px(10.))
                                    .py(px(5.))
                                    .cursor_pointer()
                                    .when(i == goes, |d| d.text_color(pal.signal))
                                    .hover(|s| s.bg(pal.well))
                                    .on_click(cx.listener(move |this, _, _, cx| {
                                        this.dropdown_open = false;
                                        this.update_state(cx, |s| s.goes_to = i);
                                    }))
                                    .child(*label)
                            })),
                    ),
                ))
            })
    }
}

fn keycap(p: &Palette) -> impl IntoElement {
    div()
        .flex()
        .items_center()
        .gap(px(8.))
        .px(px(12.))
        .py(px(6.))
        .rounded(px(6.))
        .border_1()
        .border_color(p.edge)
        .bg(p.well)
        .child(div().font_family(theme::MONO).text_size(px(theme::KEYCAP)).child("⌃"))
        .child(div().text_color(p.ink_2).child("Control"))
}

/// PushStrip: live pressure as a fill in `signal` on the slider's 0–500 scale, a tick at the
/// resistance; flashes `resistance_flash` on crossing.
fn push_strip(p: &Palette, s: crate::state::Sample, resistance: f32) -> impl IntoElement {
    let fill = if s.flash { p.resistance_flash } else { p.signal };
    let w = if s.flash { resistance } else { s.pressure } / 500.0;
    div().px(px(10.)).child(
        div()
            .relative()
            .h(px(12.))
            .w_full()
            .rounded(px(2.))
            .bg(p.well)
            .child(div().absolute().left_0().top_0().bottom_0().w(gpui::relative(w.clamp(0.0, 1.0))).rounded(px(2.)).bg(fill))
            .child(
                div()
                    .absolute()
                    .left(gpui::relative(resistance / 500.0))
                    .top(px(-3.))
                    .h(px(18.))
                    .w(px(2.))
                    .bg(p.ink),
            ),
    )
}

fn state_block(s: &AppState, sample: crate::state::Sample) -> Vec<(SharedString, SharedString)> {
    #[cfg(target_os = "macos")]
    let send = ["This Mac", "Desk PC"];
    #[cfg(not(target_os = "macos"))]
    let send = ["This PC", "Studio Mac"];
    let onoff = |b: bool| if b { "on" } else { "off" };
    let d = s.glow_display();
    let fps = match s.last_fps {
        Some((fps, n, secs)) => format!("{fps:.1} fps ({n} frames in {secs:.2} s)"),
        None => "no animation yet".into(),
    };
    vec![
        ("resistance".into(), format!("{:.0} px", s.resistance).into()),
        ("pressure".into(), format!("{:.1} px{}", sample.pressure, if sample.flash { "  CROSSED" } else { "" }).into()),
        ("glow intensity".into(), format!("{:.2}", sample.intensity).into()),
        ("don't cross while dragging".into(), onoff(s.dont_cross_dragging).into()),
        ("pause crossing".into(), onoff(s.paused).into()),
        ("shortcut".into(), format!("Control, {}", if s.shortcut_hold { "hold" } else { "double-tap" }).into()),
        ("time between taps".into(), format!("{:.0} ms{}", s.tap_ms, if s.shortcut_hold { " (unused)" } else { "" }).into()),
        ("shortcut goes to".into(), GOES_TO[s.goes_to].into()),
        ("send input to".into(), send[s.send_to].into()),
        ("overlay display".into(), d.map(|d| d.name.clone()).unwrap_or("-".into()).into()),
        ("overlay frame".into(), s.overlay_frame.clone().into()),
        ("click-through".into(), s.click_through.clone().into()),
        ("overlay fps, last animation".into(), fps.into()),
        ("display refresh".into(), format!("{:.1} Hz", s.refresh_hz).into()),
        (
            "startup".into(),
            s.startup_ms.map(|m| format!("{m:.0} ms (process start -> first settings frame)")).unwrap_or("-".into()).into(),
        ),
    ]
}

#[allow(dead_code)]
fn unused(_: AnyElement, _: Hsla, _: &App) {
    let _ = point(px(0.), px(0.));
}
