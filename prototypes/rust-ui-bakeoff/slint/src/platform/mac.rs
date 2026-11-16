// PROTOTYPE: macOS escape hatches: NSWindow level/collection behaviour, NSScreen names, process start.
use objc2::MainThreadMarker;
use objc2::rc::Retained;
use objc2_app_kit::{
    NSApplication, NSScreen, NSStatusWindowLevel, NSView, NSWindow, NSWindowCollectionBehavior,
};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};
use slint::winit_030::winit;

use crate::Display;

pub const RENDERER: &str = "Skia via wgpu 30 (Metal)";

fn ns_window(w: &winit::window::Window) -> Option<Retained<NSWindow>> {
    let RawWindowHandle::AppKit(h) = w.window_handle().ok()?.as_raw() else { return None };
    let view: &NSView = unsafe { h.ns_view.cast().as_ref() };
    view.window()
}

pub fn overlay_attributes(attrs: winit::window::WindowAttributes) -> winit::window::WindowAttributes {
    use winit::platform::macos::WindowAttributesExtMacOS;
    attrs.with_has_shadow(false)
}

pub fn displays(_w: &winit::window::Window) -> Vec<Display> {
    let mtm = MainThreadMarker::new().unwrap();
    NSScreen::screens(mtm)
        .iter()
        .enumerate()
        .map(|(i, s)| {
            let f = s.frame();
            Display {
                name: s.localizedName().to_string(),
                id: format!("NSScreen[{i}]"),
                x: f.origin.x,
                y: f.origin.y,
                w: f.size.width,
                h: f.size.height,
                scale: s.backingScaleFactor(),
                refresh_hz: s.maximumFramesPerSecond() as f64,
                is_main: i == 0, // screens[0] holds the menu bar
            }
        })
        .collect()
}

/// Above the menu bar, on every Space, over full-screen apps, out of Mission Control and Cmd-`.
pub fn shape_overlay(w: &winit::window::Window) -> String {
    let Some(win) = ns_window(w) else { return "no NSWindow".into() };
    win.setLevel(NSStatusWindowLevel);
    win.setCollectionBehavior(
        NSWindowCollectionBehavior::CanJoinAllSpaces
            | NSWindowCollectionBehavior::FullScreenAuxiliary
            | NSWindowCollectionBehavior::Stationary
            | NSWindowCollectionBehavior::IgnoresCycle,
    );
    win.setHasShadow(false);
    win.setHidesOnDeactivate(false);
    report(w)
}

pub fn report(w: &winit::window::Window) -> String {
    let Some(win) = ns_window(w) else { return "no NSWindow".into() };
    format!(
        "on (ignoresMouseEvents={}), level {} (status=25), collectionBehavior {:#x} (allSpaces|fullScreenAux|stationary|ignoresCycle=0x151), opaque={}, canBecomeKey={}",
        win.ignoresMouseEvents(),
        win.level(),
        win.collectionBehavior().0,
        win.isOpaque(),
        win.canBecomeKeyWindow(),
    )
}

pub fn place_overlay(w: &winit::window::Window, d: &Display) -> String {
    let Some(win) = ns_window(w) else { return "no NSWindow".into() };
    let mtm = MainThreadMarker::new().unwrap();
    let screens = NSScreen::screens(mtm);
    let Some(screen) = screens.iter().find(|s| s.localizedName().to_string() == d.name) else {
        return "screen gone".into();
    };
    win.setFrame_display(screen.frame(), true);
    let f = win.frame();
    format!("NSWindow frame ({}, {}) {}x{} pt", f.origin.x, f.origin.y, f.size.width, f.size.height)
}

pub fn activate_app() {
    let mtm = MainThreadMarker::new().unwrap();
    #[allow(deprecated)]
    NSApplication::sharedApplication(mtm).activateIgnoringOtherApps(true);
}

pub fn process_start_time() -> Option<std::time::SystemTime> {
    let mut info: libc::proc_bsdinfo = unsafe { std::mem::zeroed() };
    let size = std::mem::size_of::<libc::proc_bsdinfo>() as i32;
    let n = unsafe {
        libc::proc_pidinfo(
            std::process::id() as i32,
            libc::PROC_PIDTBSDINFO,
            0,
            &mut info as *mut _ as *mut libc::c_void,
            size,
        )
    };
    (n == size).then(|| {
        std::time::UNIX_EPOCH
            + std::time::Duration::from_secs(info.pbi_start_tvsec)
            + std::time::Duration::from_micros(info.pbi_start_tvusec)
    })
}
