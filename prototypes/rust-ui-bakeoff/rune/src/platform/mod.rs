//! Escape hatches: what Rune leaves to the app. Display names and refresh rates (Rune's `Display`
//! has neither a friendly name on Windows nor a refresh rate), reading back the overlay window's
//! native flags, and the tray (tray-icon).

#[cfg(target_os = "macos")]
mod mac;
#[cfg(target_os = "macos")]
use mac as native;
#[cfg(windows)]
mod win;
#[cfg(windows)]
use win as native;

pub mod tray;

use crate::DisplayInfo;

pub fn describe(d: &rune::Display) -> DisplayInfo {
    let mut info = DisplayInfo {
        name: d.name.clone().unwrap_or_else(|| "display".into()),
        native: d.native_id(),
        position: d.position,
        size: d.size,
        scale: d.scale,
        primary: d.primary,
        refresh_hz: 0.0,
    };
    native::refine(&mut info);
    info
}

/// (click-through as the OS reports it, every overlay flag worth logging)
pub fn overlay_flags(window_number: Option<u32>, title: &str) -> (String, String) {
    native::overlay_flags(window_number, title)
}
