//! Key names: W3C UI Events `code` values, which name physical key positions, so a request types
//! the same key on the Mac's layout and on the PC's French AZERTY one.

/// A key the protocol knows, with its native code on each OS.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Key {
    /// The W3C `code` name, such as "KeyA" or "ShiftLeft".
    pub code: &'static str,
    /// The macOS virtual key code (`kVK_*` in HIToolbox's Events.h), which is positional.
    pub mac: u16,
}

impl Key {
    /// The key named `code`, if the protocol knows it.
    pub fn named(code: &str) -> Option<Key> {
        KEYS.iter().find(|k| k.code == code).copied()
    }
}

const fn k(code: &'static str, mac: u16) -> Key {
    Key { code, mac }
}

/// Every key the protocol knows.
pub const KEYS: &[Key] = &[
    k("KeyA", 0x00),
    k("KeyB", 0x0B),
    k("KeyC", 0x08),
    k("KeyD", 0x02),
    k("KeyE", 0x0E),
    k("KeyF", 0x03),
    k("KeyG", 0x05),
    k("KeyH", 0x04),
    k("KeyI", 0x22),
    k("KeyJ", 0x26),
    k("KeyK", 0x28),
    k("KeyL", 0x25),
    k("KeyM", 0x2E),
    k("KeyN", 0x2D),
    k("KeyO", 0x1F),
    k("KeyP", 0x23),
    k("KeyQ", 0x0C),
    k("KeyR", 0x0F),
    k("KeyS", 0x01),
    k("KeyT", 0x11),
    k("KeyU", 0x20),
    k("KeyV", 0x09),
    k("KeyW", 0x0D),
    k("KeyX", 0x07),
    k("KeyY", 0x10),
    k("KeyZ", 0x06),
    k("Digit0", 0x1D),
    k("Digit1", 0x12),
    k("Digit2", 0x13),
    k("Digit3", 0x14),
    k("Digit4", 0x15),
    k("Digit5", 0x17),
    k("Digit6", 0x16),
    k("Digit7", 0x1A),
    k("Digit8", 0x1C),
    k("Digit9", 0x19),
    k("Minus", 0x1B),
    k("Equal", 0x18),
    k("BracketLeft", 0x21),
    k("BracketRight", 0x1E),
    k("Backslash", 0x2A),
    k("Semicolon", 0x29),
    k("Quote", 0x27),
    k("Backquote", 0x32),
    k("Comma", 0x2B),
    k("Period", 0x2F),
    k("Slash", 0x2C),
    k("IntlBackslash", 0x0A),
    k("Enter", 0x24),
    k("Escape", 0x35),
    k("Backspace", 0x33),
    k("Delete", 0x75),
    k("Tab", 0x30),
    k("Space", 0x31),
    k("CapsLock", 0x39),
    k("ShiftLeft", 0x38),
    k("ShiftRight", 0x3C),
    k("ControlLeft", 0x3B),
    k("ControlRight", 0x3E),
    k("AltLeft", 0x3A),
    k("AltRight", 0x3D),
    k("MetaLeft", 0x37),
    k("MetaRight", 0x36),
    k("ArrowLeft", 0x7B),
    k("ArrowRight", 0x7C),
    k("ArrowDown", 0x7D),
    k("ArrowUp", 0x7E),
    k("Home", 0x73),
    k("End", 0x77),
    k("PageUp", 0x74),
    k("PageDown", 0x79),
    k("F1", 0x7A),
    k("F2", 0x78),
    k("F3", 0x63),
    k("F4", 0x76),
    k("F5", 0x60),
    k("F6", 0x61),
    k("F7", 0x62),
    k("F8", 0x64),
    k("F9", 0x65),
    k("F10", 0x6D),
    k("F11", 0x67),
    k("F12", 0x6F),
    k("F13", 0x69),
    k("F14", 0x6B),
    k("F15", 0x71),
    k("F16", 0x6A),
    k("F17", 0x40),
    k("F18", 0x4F),
    k("F19", 0x50),
    k("F20", 0x5A),
];
