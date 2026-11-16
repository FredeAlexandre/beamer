// PROTOTYPE: macOS escape hatches (objc2 + raw-window-handle). Everything GPUI does not do itself.
use gpui::{DisplayId, Window};
use objc2::{msg_send, rc::Retained, runtime::AnyObject};
use objc2_app_kit::{NSScreen, NSWindowCollectionBehavior};
use objc2_foundation::{MainThreadMarker, NSNumber, NSPoint, NSRect, NSString};
use raw_window_handle::{HasWindowHandle, RawWindowHandle};

use super::{DisplayMeta, OverlayFlags};

const NS_STATUS_WINDOW_LEVEL: isize = 25;

fn screen_for(id: DisplayId) -> Option<Retained<NSScreen>> {
    let mtm = MainThreadMarker::new()?;
    let key = NSString::from_str("NSScreenNumber");
    NSScreen::screens(mtm).iter().find(|s| unsafe {
        let num: *mut AnyObject = msg_send![&*s.deviceDescription(), objectForKey: &*key];
        !num.is_null() && (*(num as *const NSNumber)).unsignedIntValue() as u64 == u64::from(id)
    })
}

pub fn display_meta(id: DisplayId) -> DisplayMeta {
    match screen_for(id) {
        Some(s) => {
            let f = s.frame();
            DisplayMeta {
                name: s.localizedName().to_string(),
                frame: format!("{}x{} at ({},{}) pt, AppKit bottom-left", f.size.width, f.size.height, f.origin.x, f.origin.y),
                scale: s.backingScaleFactor(),
            }
        }
        None => DisplayMeta { name: format!("Display {}", u64::from(id)), frame: "?".into(), scale: 1.0 },
    }
}

fn ns_window(window: &Window) -> *mut AnyObject {
    let RawWindowHandle::AppKit(h) = HasWindowHandle::window_handle(window).unwrap().as_raw() else { unreachable!() };
    let view = h.ns_view.as_ptr() as *mut AnyObject;
    unsafe { msg_send![view, window] }
}

/// GPUI's `WindowKind::PopUp` already gives a non-activating NSPanel at NSPopUpWindowLevel (101) with
/// CanJoinAllSpaces | FullScreenAuxiliary. Added here: click-through, status level (like Beamer
/// today, so the menu bar's own menus stay above it), Stationary | IgnoresCycle, no shadow.
pub fn configure_overlay(window: &Window, _id: DisplayId) -> OverlayFlags {
    let w = ns_window(window);
    unsafe {
        let level_before: isize = msg_send![w, level];
        // BAKEOFF_NO_CLICK_THROUGH=1: control run, to show the hit-test probe does see the overlay.
        let _: () = msg_send![w, setIgnoresMouseEvents: std::env::var_os("BAKEOFF_NO_CLICK_THROUGH").is_none()];
        let _: () = msg_send![w, setLevel: NS_STATUS_WINDOW_LEVEL];
        let cb = NSWindowCollectionBehavior::CanJoinAllSpaces
            | NSWindowCollectionBehavior::FullScreenAuxiliary
            | NSWindowCollectionBehavior::Stationary
            | NSWindowCollectionBehavior::IgnoresCycle;
        let _: () = msg_send![w, setCollectionBehavior: cb];
        let _: () = msg_send![w, setHasShadow: false];
        let _: () = msg_send![w, setExcludedFromWindowsMenu: true];

        let ignores: bool = msg_send![w, ignoresMouseEvents];
        let level: isize = msg_send![w, level];
        let cb: NSWindowCollectionBehavior = msg_send![w, collectionBehavior];
        let style: usize = msg_send![w, styleMask];
        let can_key: bool = msg_send![w, canBecomeKeyWindow];
        let opaque: bool = msg_send![w, isOpaque];
        let class: *const AnyObject = msg_send![w, className];
        let class = (*(class as *const NSString)).to_string();
        let f: NSRect = msg_send![w, frame];
        let frame = format!("{}x{} at ({},{}) pt", f.size.width, f.size.height, f.origin.x, f.origin.y);
        OverlayFlags {
            click_through: ignores,
            frame: frame.clone(),
            describe: format!(
                "class {class}, ignoresMouseEvents={ignores}, level {level_before}->{level}, collectionBehavior={:#x} \
                 (allSpaces|fullScreenAux|stationary|ignoresCycle), styleMask={style:#x} (nonactivating={}), \
                 canBecomeKey={can_key}, opaque={opaque}, frame as GPUI made it {frame}",
                cb.0,
                style & (1 << 7) != 0
            ),
        }
    }
}

/// GPUI's PopUp panel has the Titled style bit, so AppKit constrains it below the menu bar (30 pt
/// down on the GN10, its bottom off-screen), and a plain setFrame is constrained again. Drop to
/// borderless + non-activating, then put it on the whole screen. Returned as a closure to run
/// outside GPUI's App borrow: AppKit calls GPUI's resize/move handlers synchronously, and inside
/// `open_window` those fail with "RefCell already borrowed".
pub fn overlay_fixup(window: &Window, id: DisplayId) -> impl FnOnce() -> String + 'static {
    let w = ns_window(window) as usize;
    move || unsafe {
        let w = w as *mut AnyObject;
        let f0: NSRect = msg_send![w, frame];
        let _: () = msg_send![w, setStyleMask: 1usize << 7];
        if let Some(screen) = screen_for(id) {
            let _: () = msg_send![w, setFrame: screen.frame(), display: true];
        }
        let f: NSRect = msg_send![w, frame];
        let style: usize = msg_send![w, styleMask];
        let cv: *mut AnyObject = msg_send![w, contentView];
        let cf: NSRect = msg_send![cv, frame];
        format!(
            "styleMask -> {style:#x} (borderless|nonactivating), contentView {}x{} at ({},{}), frame {}x{} at ({},{}) -> {}x{} at ({},{}) pt",
            cf.size.width, cf.size.height, cf.origin.x, cf.origin.y,
            f0.size.width, f0.size.height, f0.origin.x, f0.origin.y, f.size.width, f.size.height, f.origin.x, f.origin.y
        )
    }
}

/// Which window AppKit would deliver a click to at a point on the glow edge.
pub fn probe_overlay(window: &Window) -> String {
    let w = ns_window(window);
    unsafe {
        let num: isize = msg_send![w, windowNumber];
        let f: NSRect = msg_send![w, frame];
        let p = NSPoint::new(f.origin.x + f.size.width - 4.0, f.origin.y + f.size.height / 2.0);
        let cls = objc2::class!(NSWindow);
        // "the frontmost window that would be hit by a mouse-down at the specified screen location"
        let at: isize = msg_send![cls, windowNumberAtPoint: p, belowWindowWithWindowNumber: 0isize];
        let ignores: bool = msg_send![w, ignoresMouseEvents];
        let visible: bool = msg_send![w, isVisible];
        let on_active_space: bool = msg_send![w, isOnActiveSpace];
        format!(
            "overlay #{num} visible={visible} onActiveSpace={on_active_space} ignoresMouseEvents={ignores}; \
             hit test windowNumberAtPoint(right edge)=#{at} -> {}; pixel probe: not on macOS (needs Screen Recording), use screencapture",
            if at == num { "THE OVERLAY: not click-through" } else { "another window: passes through" }
        )
    }
}

pub fn process_stats() -> String {
    unsafe {
        let mut info: libc::proc_taskinfo = std::mem::zeroed();
        let size = std::mem::size_of::<libc::proc_taskinfo>() as i32;
        let n = libc::proc_pidinfo(
            std::process::id() as i32,
            libc::PROC_PIDTASKINFO,
            0,
            &mut info as *mut _ as *mut libc::c_void,
            size,
        );
        if n != size {
            return "proc_pidinfo failed".into();
        }
        // pti_total_* are Mach absolute time units; on Apple silicon timebase is 125/3 ns per tick.
        #[allow(deprecated)]
        let cpu_ns = {
            let mut tb = libc::mach_timebase_info { numer: 0, denom: 0 };
            libc::mach_timebase_info(&mut tb);
            (info.pti_total_user + info.pti_total_system) as f64 * tb.numer as f64 / tb.denom as f64
        };
        format!("rss {:.1} MB, cpu {:.3} s", info.pti_resident_size as f64 / 1048576.0, cpu_ns / 1e9)
    }
}
