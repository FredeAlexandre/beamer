//! macOS escape hatch (objc2): display names and refresh rates from NSScreen, and the overlay
//! panel's real level, collection behaviour, style and mouse policy, read back from AppKit.

use objc2::MainThreadMarker;
use objc2_app_kit::{NSApplication, NSScreen, NSWindowStyleMask};
use objc2_foundation::{NSNumber, NSString};
use rune::NativeDisplayId;

use crate::DisplayInfo;

pub fn refine(info: &mut DisplayInfo) {
    let Some(NativeDisplayId::MacOS(id)) = info.native else { return };
    let mtm = MainThreadMarker::new().expect("main thread");
    for screen in NSScreen::screens(mtm).iter() {
        let desc = screen.deviceDescription();
        let key = NSString::from_str("NSScreenNumber");
        let Some(n) = desc.objectForKey(&key) else { continue };
        let Ok(n) = n.downcast::<NSNumber>() else { continue };
        if n.unsignedIntValue() == id {
            info.name = screen.localizedName().to_string();
            info.refresh_hz = screen.maximumFramesPerSecond() as f64;
        }
    }
}

pub fn overlay_flags(window_number: Option<u32>, _title: &str) -> (String, String) {
    let mtm = MainThreadMarker::new().expect("main thread");
    let Some(number) = window_number else { return ("unknown (no window number)".into(), "no window number".into()) };
    let app = NSApplication::sharedApplication(mtm);
    for w in app.windows().iter() {
        if w.windowNumber() as u32 != number {
            continue;
        }
        let ignores = w.ignoresMouseEvents();
        let mask = w.styleMask();
        let f = w.frame();
        let flags = format!(
            "class {}, level {}, collectionBehavior {:#x}, ignoresMouseEvents {}, nonactivatingPanel {}, borderless {}, opaque {}, canBecomeKey {}, canBecomeMain {}, frame ({}, {}) {}x{} pt",
            w.class().name().to_str().unwrap_or("?"),
            w.level(),
            w.collectionBehavior().0,
            ignores,
            mask.contains(NSWindowStyleMask::NonactivatingPanel),
            mask == NSWindowStyleMask::Borderless || !mask.contains(NSWindowStyleMask::Titled),
            w.isOpaque(),
            w.canBecomeKeyWindow(),
            w.canBecomeMainWindow(),
            f.origin.x, f.origin.y, f.size.width, f.size.height,
        );
        let ct = if ignores { "on (ignoresMouseEvents = YES, read back)" } else { "OFF (ignoresMouseEvents = NO)" };
        return (ct.into(), flags);
    }
    ("unknown (window not found)".into(), format!("window {number} not among NSApp.windows"))
}
