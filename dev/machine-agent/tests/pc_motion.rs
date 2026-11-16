//! How the PC agent posts a move: one SendInput event, absolute where the pointer lands where the
//! move asks, relative where an edge holds it, so the push reaches Raw Input as hand motion.

use machine_agent::Point;
use machine_agent::pc::{Motion, VirtualScreen, motion};

/// Two 1920x1080 displays side by side, the primary one on the right.
const SCREEN: VirtualScreen = VirtualScreen {
    x: -1920,
    y: 0,
    width: 3840,
    height: 1080,
};

fn p(x: f64, y: f64) -> Point {
    Point { x, y }
}

#[test]
fn a_move_the_pointer_follows_is_absolute_on_the_virtual_screen() {
    // Windows maps a normalised n to the pixel floor(n * size / 65536) from the screen's origin.
    let at = |x, y| motion(p(0.0, 0.0), p(x, y), (x, y), SCREEN);
    assert_eq!(at(-1920.0, 0.0), Motion::Absolute { x: 0, y: 0 });
    assert_eq!(at(0.0, 540.0), Motion::Absolute { x: 32768, y: 32768 });
    assert_eq!(at(1919.0, 1079.0), Motion::Absolute { x: 65519, y: 65476 });
    assert_eq!(at(123.0, 145.0), Motion::Absolute { x: 34868, y: 8799 });
}

#[test]
fn a_fractional_point_lands_on_the_pixel_it_rounds_to() {
    let m = motion(p(0.0, 0.0), p(122.6, 144.5), (122.6, 144.5), SCREEN);
    assert_eq!(m, Motion::Absolute { x: 34868, y: 8799 });
}

#[test]
fn a_move_held_at_an_edge_is_relative_and_carries_the_whole_push() {
    let m = motion(p(1910.0, 500.0), p(1919.0, 500.0), (40.0, 0.0), SCREEN);
    assert_eq!(m, Motion::Relative { dx: 40, dy: 0 });
    let m = motion(p(1919.0, 500.0), p(1919.0, 520.0), (40.0, 20.0), SCREEN);
    assert_eq!(m, Motion::Relative { dx: 40, dy: 20 });
}
