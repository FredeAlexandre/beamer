// PROTOTYPE: values copied from the repo-root tokens.py (PALETTE, PALETTE_LIGHT, TYPE). Not imported.
use gpui::{Hsla, WindowAppearance, rgb};

pub const UI: &str = "Hanken Grotesk";
pub const MONO: &str = "B612 Mono";

#[derive(Clone, Copy)]
pub struct Palette {
    pub ground: Hsla,
    pub panel: Hsla,
    pub well: Hsla,
    pub rule: Hsla,
    pub edge: Hsla,
    pub ink: Hsla,
    pub ink_2: Hsla,
    pub ink_3: Hsla,
    pub signal: Hsla,
    pub signal_dim: Hsla,
    pub resistance_flash: Hsla,
    pub off: Hsla,
}

fn c(x: u32) -> Hsla {
    rgb(x).into()
}

pub fn dark() -> Palette {
    Palette {
        ground: c(0x0e0f0d),
        panel: c(0x161714),
        well: c(0x1f201c),
        rule: c(0x2a2b26),
        edge: c(0x6e6f66),
        ink: c(0xefeee6),
        ink_2: c(0xb3b2a7),
        ink_3: c(0x8f8e84),
        signal: c(0x5fd4f4),
        signal_dim: c(0x1f4a55),
        resistance_flash: c(0xb77a00),
        off: c(0x4a4b44),
    }
}

pub fn light() -> Palette {
    Palette {
        ground: c(0xf1f0ea),
        panel: c(0xfbfaf6),
        well: c(0xe5e4dc),
        rule: c(0xdcdbd3),
        edge: c(0x7f7e74),
        ink: c(0x1b1c18),
        ink_2: c(0x46463f),
        ink_3: c(0x66655c),
        signal: c(0x006a88),
        signal_dim: c(0xbfe0ea),
        resistance_flash: c(0xb55f00),
        off: c(0xaeada3),
    }
}

pub fn for_appearance(a: WindowAppearance) -> Palette {
    match a {
        WindowAppearance::Dark | WindowAppearance::VibrantDark => dark(),
        _ => light(),
    }
}

// TYPE (points on the Mac, logical px on Windows; GPUI uses logical px for both).
pub const EYEBROW: f32 = 10.5;
pub const SMALL: f32 = 11.5;
pub const NOTE: f32 = 12.5;
pub const BODY: f32 = 13.0;
pub const READOUT: f32 = 34.0;
pub const KEYCAP: f32 = 20.0;
