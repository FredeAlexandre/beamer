//! `sans-io.clippy.toml`, the `clippy.toml` every sans-I/O crate carries (rules 1, 2 and 5 of #16,
//! ADR 0004): `cargo clippy -D warnings` rejects a clock read, a thread, a socket or a file in
//! such a crate.

mod common;

use common::Fixture;

/// One use of each banned family, each in its own function so every one is reported.
const BANNED: &str = r#"
use std::time::{Duration, Instant, SystemTime};

pub fn clock() -> Instant { Instant::now() }
pub fn wall_clock() -> SystemTime { SystemTime::now() }
pub fn age(since: Instant) -> Duration { since.elapsed() }

pub fn thread() { std::thread::spawn(|| ()); }
pub fn nap() { std::thread::sleep(Duration::from_millis(1)); }
thread_local! { static SLOT: u8 = const { 0 }; }

pub fn socket() -> std::io::Result<std::net::UdpSocket> { std::net::UdpSocket::bind("0.0.0.0:0") }
pub fn lookup() -> std::io::Result<()> {
    use std::net::ToSocketAddrs;
    "localhost:1".to_socket_addrs().map(|_| ())
}

pub fn file() -> std::io::Result<String> { std::fs::read_to_string("store.json") }
pub fn write() -> std::io::Result<()> { std::fs::write("store.json", "{}") }
pub fn exists(path: &std::path::Path) -> bool { path.exists() }
"#;

/// What a domain crate does: values in, values out. Instants and socket addresses are values;
/// only reading the clock or opening the socket is I/O.
const CLEAN: &str = r#"
use std::net::SocketAddr;
use std::time::{Duration, Instant};

pub fn next_deadline(now: Instant, rtt: Duration) -> Instant { now + rtt * 2 }
pub fn same_peer(a: SocketAddr, b: SocketAddr) -> bool { a == b }
"#;

#[test]
fn clippy_rejects_clocks_threads_sockets_and_files_in_a_sans_io_crate() {
    let ws = Fixture::new("sans_io_clippy_bites");
    ws.sans_io("beamer-crossing", "")
        .file("crates/beamer-crossing/src/lib.rs", BANNED);
    ws.clippy().fails_naming(&[
        "disallowed method `std::time::Instant::now`",
        "disallowed method `std::time::SystemTime::now`",
        "disallowed method `std::time::Instant::elapsed`",
        "disallowed method `std::thread::spawn`",
        "disallowed method `std::thread::sleep`",
        "disallowed macro `std::thread_local`",
        "disallowed type `std::net::UdpSocket`",
        "disallowed method `std::net::ToSocketAddrs::to_socket_addrs`",
        "disallowed method `std::fs::read_to_string`",
        "disallowed method `std::fs::write`",
        "disallowed method `std::path::Path::exists`",
    ]);
}

#[test]
fn clippy_passes_a_sans_io_crate_that_takes_time_as_an_argument() {
    let ws = Fixture::new("sans_io_clippy_clean");
    ws.sans_io("beamer-link", "")
        .file("crates/beamer-link/src/lib.rs", CLEAN);
    let run = ws.clippy();
    run.passes();
    // A path clippy can't resolve is only a warning about the config, not a failed build.
    assert!(
        !run.text.contains("warning"),
        "clippy warned:\n{}",
        run.text
    );
}
