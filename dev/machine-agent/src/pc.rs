//! The PC agent (#48): how a protocol move becomes one Windows input event, and, on Windows, the
//! desktop that posts it ([`PcDesktop`]).
//!
//! Beamer's Windows capture measures a push against an edge from Raw Input's relative counts,
//! which an absolute move does not carry. So a move the pointer follows is posted absolute, which
//! lands it on the exact pixel, and a move an edge holds is posted relative, carrying the whole
//! push. Windows applies the pointer speed and acceleration settings to a relative move, so one
//! that starts away from the edge may stop short of it: a harness puts the pointer at the edge
//! with `to` before it pushes.

use crate::Point;

#[cfg(windows)]
mod win;
#[cfg(windows)]
pub use win::PcDesktop;

/// The bounding rectangle of every display, in physical pixels: the space SendInput's absolute
/// coordinates are normalised over.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct VirtualScreen {
    pub x: i32,
    pub y: i32,
    pub width: i32,
    pub height: i32,
}

/// The pointer motion of one SendInput event.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Motion {
    /// To a point, normalised to 0..=65535 over the [`VirtualScreen`].
    Absolute { x: i32, y: i32 },
    /// By counts, as a mouse reports them.
    Relative { dx: i32, dy: i32 },
}

/// The event for a move from `from` that leaves the pointer at `to` and carries `delta`
/// (`Input::Move`): absolute when `to` is where `delta` leads, relative when an edge holds it.
pub fn motion(from: Point, to: Point, delta: (f64, f64), screen: VirtualScreen) -> Motion {
    let follows = (from.x + delta.0 - to.x).abs() < 0.5 && (from.y + delta.1 - to.y).abs() < 0.5;
    if follows {
        Motion::Absolute {
            x: normalised(to.x, screen.x, screen.width),
            y: normalised(to.y, screen.y, screen.height),
        }
    } else {
        Motion::Relative {
            dx: delta.0.round() as i32,
            dy: delta.1.round() as i32,
        }
    }
}

/// The normalised coordinate Windows maps back to the pixel `at` rounds to. Windows takes a
/// normalised `n` to pixel `floor(n * size / 65536)`, so the smallest `n` that reaches a pixel is
/// the ceiling of its inverse.
fn normalised(at: f64, origin: i32, size: i32) -> i32 {
    let size = i64::from(size.max(1));
    let pixel = (at.round() as i64 - i64::from(origin)).clamp(0, size - 1);
    ((pixel * 65536 + size - 1) / size) as i32
}
