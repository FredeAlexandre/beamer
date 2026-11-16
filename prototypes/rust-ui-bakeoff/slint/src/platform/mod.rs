// PROTOTYPE: platform escape hatches (everything Slint leaves to us).
#[cfg(target_os = "macos")]
mod mac;
#[cfg(target_os = "macos")]
pub use mac::*;
#[cfg(target_os = "windows")]
mod win;
#[cfg(target_os = "windows")]
pub use win::*;
