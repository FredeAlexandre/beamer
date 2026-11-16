// PROTOTYPE: the platform escape hatches. Line counts of mac.rs, win.rs and tray.rs go in NOTES.md.
#[cfg(target_os = "macos")]
mod mac;
#[cfg(target_os = "macos")]
pub use mac::*;
#[cfg(target_os = "windows")]
mod win;
#[cfg(target_os = "windows")]
pub use win::*;
pub mod tray;

pub struct DisplayMeta {
    pub name: String,
    pub frame: String,
    pub scale: f64,
}

pub struct OverlayFlags {
    pub click_through: bool,
    pub frame: String,
    pub describe: String,
}
