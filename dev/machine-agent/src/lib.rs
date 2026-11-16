//! The machine agent (#47, #48, ADR 0005): serves the protocol in `PROTOCOL.md` on a loopback port and
//! posts real OS input through a [`Desktop`].

pub mod cli;
pub mod keys;
#[cfg(target_os = "macos")]
pub mod mac;
pub mod pc;
mod server;

pub use keys::Key;
pub use server::serve;

/// The protocol version `hello` reports. It goes up with any change a harness would notice.
pub const PROTOCOL: u64 = 1;

/// The port an agent listens on, on 127.0.0.1 only.
pub const PORT: u16 = 24870;

/// A point in the OS's global pointer coordinates (`PROTOCOL.md`, Coordinates).
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}

/// One display's bounds in the same coordinates.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Display {
    pub x: f64,
    pub y: f64,
    pub width: f64,
    pub height: f64,
    pub main: bool,
}

impl Display {
    pub(crate) fn contains(&self, p: Point) -> bool {
        p.x >= self.x && p.x < self.x + self.width && p.y >= self.y && p.y < self.y + self.height
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Button {
    Left,
    Right,
    Middle,
}

/// One OS input event, as the agent posts it.
#[derive(Clone, Debug, PartialEq)]
pub enum Input {
    /// The pointer moves to `to`. `delta` is the motion the event carries, which differs from
    /// `to` minus the old position when the pointer is held at an edge.
    Move {
        to: Point,
        delta: (f64, f64),
    },
    Button {
        button: Button,
        down: bool,
    },
    Key {
        key: Key,
        down: bool,
    },
    /// Wheel motion in lines; positive `dy` is the wheel turned away from the user.
    Scroll {
        dx: i32,
        dy: i32,
    },
}

/// The OS, as the agent sees it. One implementation per OS, and a fake in the tests.
pub trait Desktop: Send {
    /// The agent's name in `hello`: "mac", "pc".
    fn name(&self) -> &'static str;
    /// Whether the OS lets this process post input (on macOS, the Accessibility grant).
    fn trusted(&self) -> bool;
    fn displays(&self) -> Vec<Display>;
    fn pointer(&self) -> Point;
    /// The OS monotonic clock in nanoseconds (`PROTOCOL.md`, Time).
    fn mono_ns(&self) -> u64;
    /// Posts one event. Only called while [`Desktop::trusted`] holds.
    fn post(&mut self, input: Input) -> Result<(), String>;
    /// Whether the OS sees `key` as held down, `key` being a code from [`keys`].
    fn key_down(&self, key: &str) -> bool;
}
