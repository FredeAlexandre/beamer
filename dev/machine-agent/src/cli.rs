//! The command line of both binaries: serves the machine-agent protocol, or sends requests to an
//! agent.
//!
//! ```text
//! machine-agent [serve] [--port N] [--idle SECONDS] [--log FILE] [--ask-for-access]
//! machine-agent send [--host H] [--port N] '<request>' ...
//! ```
//!
//! `serve` is what the Mac agent's `.app` runs when `open` starts it, and what the PC agent's
//! scheduled task runs, through `machine-agent-windowless.exe` (README.md). `--log` appends what
//! it says to FILE instead of stderr, for a process that has none. With `--ask-for-access`, an
//! agent macOS does not trust yet asks to be listed under Accessibility, which shows the prompt.
//! `send` sends each request in turn on one connection, prints each reply line, and exits 1 at
//! the first reply that is not ok. A request without an "id" gets its position as one.

use std::fs::OpenOptions;
use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::process::ExitCode;
use std::time::Duration;

use crate::{Desktop, PORT};

/// How long an agent waits without a request before it quits, so none is left posting input.
const IDLE_SECONDS: u64 = 15 * 60;

const USAGE: &str = "usage: machine-agent [serve] [--port N] [--idle SECONDS] [--log FILE] [--ask-for-access]\n       \
                     machine-agent send [--host H] [--port N] '<request>' ...";

/// Runs the command line `std::env::args()` holds.
pub fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let result = match args.first().map(String::as_str) {
        Some("send") => send(&args[1..]),
        Some("serve") => serve(&args[1..]),
        Some("-h" | "--help") => {
            println!("{USAGE}");
            Ok(())
        }
        _ => serve(&args),
    };
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("machine-agent: {message}");
            ExitCode::FAILURE
        }
    }
}

/// The value after `--name` in `args`, and `args` without both.
fn option(args: &[String], name: &str) -> Result<(Option<String>, Vec<String>), String> {
    let mut rest = Vec::new();
    let mut value = None;
    let mut it = args.iter();
    while let Some(arg) = it.next() {
        if arg == name {
            value = Some(it.next().ok_or(format!("{name} needs a value"))?.clone());
        } else {
            rest.push(arg.clone());
        }
    }
    Ok((value, rest))
}

fn number<T: std::str::FromStr>(
    value: Option<String>,
    name: &str,
    default: T,
) -> Result<T, String> {
    value.map_or(Ok(default), |v| {
        v.parse()
            .map_err(|_| format!("{name} {v:?} is not a number"))
    })
}

fn serve(args: &[String]) -> Result<(), String> {
    let (log, rest) = option(args, "--log")?;
    let mut log: Box<dyn Write> = match log {
        Some(path) => Box::new(
            OpenOptions::new()
                .create(true)
                .append(true)
                .open(&path)
                .map_err(|e| format!("cannot open the log {path:?}: {e}"))?,
        ),
        None => Box::new(std::io::stderr()),
    };
    let result = serve_logged(&rest, &mut log);
    if let Err(message) = &result {
        let _ = writeln!(log, "machine-agent: {message}");
    }
    result
}

fn serve_logged(args: &[String], log: &mut dyn Write) -> Result<(), String> {
    let (port, rest) = option(args, "--port")?;
    let (idle, mut rest) = option(&rest, "--idle")?;
    let ask = rest.iter().any(|a| a == "--ask-for-access");
    rest.retain(|a| a != "--ask-for-access");
    if let Some(extra) = rest.first() {
        return Err(format!("unexpected {extra:?}\n{USAGE}"));
    }
    let port = number(port, "--port", PORT)?;
    let idle = Duration::from_secs(number(idle, "--idle", IDLE_SECONDS)?);
    let listener = TcpListener::bind(("127.0.0.1", port))
        .map_err(|e| format!("cannot listen on 127.0.0.1:{port}: {e}"))?;
    let desktop = desktop()?;
    let trusted = desktop.lock().unwrap().trusted();
    if ask && !trusted {
        ask_for_access();
    }
    let _ = writeln!(
        log,
        "machine-agent: serving protocol {} on 127.0.0.1:{port}, trusted: {trusted}",
        crate::PROTOCOL
    );
    let _ = log.flush();
    crate::serve(listener, desktop, idle).map_err(|e| e.to_string())?;
    let _ = writeln!(log, "machine-agent: stopped");
    Ok(())
}

#[cfg(target_os = "macos")]
fn ask_for_access() {
    crate::mac::ask_for_access();
}

#[cfg(not(target_os = "macos"))]
fn ask_for_access() {}

#[cfg(target_os = "macos")]
fn desktop() -> Result<std::sync::Arc<std::sync::Mutex<crate::mac::MacDesktop>>, String> {
    Ok(std::sync::Arc::new(std::sync::Mutex::new(
        crate::mac::MacDesktop::new(),
    )))
}

#[cfg(windows)]
fn desktop() -> Result<std::sync::Arc<std::sync::Mutex<crate::pc::PcDesktop>>, String> {
    Ok(std::sync::Arc::new(std::sync::Mutex::new(
        crate::pc::PcDesktop::new(),
    )))
}

#[cfg(not(any(target_os = "macos", windows)))]
fn desktop() -> Result<std::sync::Arc<std::sync::Mutex<NoDesktop>>, String> {
    Err("this OS has no machine agent: only the Mac and the PC have one".into())
}

#[cfg(not(any(target_os = "macos", windows)))]
enum NoDesktop {}

#[cfg(not(any(target_os = "macos", windows)))]
impl crate::Desktop for NoDesktop {
    fn name(&self) -> &'static str {
        match *self {}
    }
    fn trusted(&self) -> bool {
        match *self {}
    }
    fn displays(&self) -> Vec<crate::Display> {
        match *self {}
    }
    fn pointer(&self) -> crate::Point {
        match *self {}
    }
    fn mono_ns(&self) -> u64 {
        match *self {}
    }
    fn post(&mut self, _: crate::Input) -> Result<(), String> {
        match *self {}
    }
    fn key_down(&self, _: &str) -> bool {
        match *self {}
    }
}

fn send(args: &[String]) -> Result<(), String> {
    let (host, rest) = option(args, "--host")?;
    let (port, requests) = option(&rest, "--port")?;
    let host = host.unwrap_or_else(|| "127.0.0.1".into());
    let port = number(port, "--port", PORT)?;
    if requests.is_empty() {
        return Err(format!("nothing to send\n{USAGE}"));
    }
    let stream = TcpStream::connect((host.as_str(), port))
        .map_err(|e| format!("no agent on {host}:{port}: {e}"))?;
    stream
        .set_read_timeout(Some(Duration::from_secs(10)))
        .map_err(|e| e.to_string())?;
    let mut writer = stream.try_clone().map_err(|e| e.to_string())?;
    let mut reader = BufReader::new(stream);
    for (n, request) in requests.iter().enumerate() {
        let mut request: serde_json::Value =
            serde_json::from_str(request).map_err(|e| format!("{request:?} is not JSON: {e}"))?;
        if let Some(object) = request.as_object_mut() {
            object.entry("id").or_insert((n + 1).into());
        }
        writer
            .write_all(format!("{request}\n").as_bytes())
            .map_err(|e| format!("the agent closed the connection: {e}"))?;
        let mut reply = String::new();
        reader
            .read_line(&mut reply)
            .map_err(|e| format!("no reply: {e}"))?;
        if reply.is_empty() {
            return Err("the agent closed the connection".into());
        }
        // A closed stdout (`| head`) ends the run quietly instead of panicking.
        std::io::stdout()
            .write_all(reply.as_bytes())
            .map_err(|e| format!("cannot print the reply: {e}"))?;
        let reply: serde_json::Value =
            serde_json::from_str(&reply).map_err(|e| format!("the reply is not JSON: {e}"))?;
        if reply["ok"] != true {
            return Err(format!("request {} failed", n + 1));
        }
    }
    Ok(())
}
