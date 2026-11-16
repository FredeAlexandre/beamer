//! The protocol of `PROTOCOL.md` over TCP: one JSON request per line, one reply line each.

use std::io::{self, BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use serde_json::{Map, Value, json};

use crate::{Button, Desktop, Display, Input, Key, PROTOCOL, Point};

/// Serves requests on `listener` until a `quit` request, or until `idle` passes without one.
/// Each connection gets a thread; their requests take turns on the one desktop.
pub fn serve<D: Desktop + 'static>(
    listener: TcpListener,
    desktop: Arc<Mutex<D>>,
    idle: Duration,
) -> io::Result<()> {
    let state = Arc::new(State {
        last: Mutex::new(Instant::now()),
        quit: AtomicBool::new(false),
    });
    listener.set_nonblocking(true)?;
    loop {
        if state.quit.load(Ordering::SeqCst) || state.last.lock().unwrap().elapsed() >= idle {
            return Ok(());
        }
        match listener.accept() {
            Ok((stream, _)) => {
                // An accepted socket inherits the listener's non-blocking mode on BSD.
                stream.set_nonblocking(false)?;
                let (desktop, state) = (desktop.clone(), state.clone());
                std::thread::spawn(move || {
                    let _ = connection(stream, &desktop, &state);
                });
            }
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                std::thread::sleep(Duration::from_millis(10));
            }
            Err(e) => return Err(e),
        }
    }
}

/// What every connection shares: when the last request came, and whether one asked to quit.
struct State {
    last: Mutex<Instant>,
    quit: AtomicBool,
}

fn connection<D: Desktop>(stream: TcpStream, desktop: &Mutex<D>, state: &State) -> io::Result<()> {
    let mut writer = stream.try_clone()?;
    for line in BufReader::new(stream).lines() {
        let line = line?;
        *state.last.lock().unwrap() = Instant::now();
        let (reply, quit) = answer(&line, &mut *desktop.lock().unwrap());
        writer.write_all(format!("{reply}\n").as_bytes())?;
        if quit {
            state.quit.store(true, Ordering::SeqCst);
            return Ok(());
        }
    }
    Ok(())
}

/// Why a request failed: an error code from `PROTOCOL.md` and a message for a person.
struct Fault {
    code: &'static str,
    message: String,
}

fn bad(message: impl Into<String>) -> Fault {
    Fault {
        code: "bad-request",
        message: message.into(),
    }
}

/// The reply to one request line, and whether it asked the agent to quit.
fn answer<D: Desktop>(line: &str, desktop: &mut D) -> (Value, bool) {
    let request = serde_json::from_str::<Value>(line);
    let id = match &request {
        Ok(r) => r.get("id").cloned().unwrap_or(Value::Null),
        Err(_) => Value::Null,
    };
    let quit = matches!(&request, Ok(r) if r.get("op") == Some(&Value::from("quit")));
    let result = match request {
        Ok(Value::Object(request)) => run(&request, desktop),
        Ok(_) => Err(bad("a request is a JSON object")),
        Err(e) => Err(bad(format!("not JSON: {e}"))),
    };
    let mut reply = Map::new();
    reply.insert("id".into(), id);
    match result {
        Ok(fields) => {
            reply.insert("ok".into(), true.into());
            reply.extend(fields);
        }
        Err(fault) => {
            reply.insert("ok".into(), false.into());
            reply.insert("error".into(), fault.code.into());
            reply.insert("message".into(), fault.message.into());
        }
    }
    (Value::Object(reply), quit)
}

/// Runs one request. The reply's fields come back without `id` and `ok`.
fn run<D: Desktop>(
    request: &Map<String, Value>,
    desktop: &mut D,
) -> Result<Map<String, Value>, Fault> {
    let op = request
        .get("op")
        .and_then(Value::as_str)
        .ok_or_else(|| bad("no \"op\""))?;
    let mut reply = Map::new();
    match op {
        "hello" => {
            reply.insert("protocol".into(), PROTOCOL.into());
            reply.insert("agent".into(), desktop.name().into());
            reply.insert("trusted".into(), desktop.trusted().into());
            let displays = desktop
                .displays()
                .iter()
                .map(|d| json!({"x": d.x, "y": d.y, "width": d.width, "height": d.height, "main": d.main}))
                .collect::<Vec<_>>();
            reply.insert("displays".into(), displays.into());
        }
        "pointer" => {
            let at = desktop.pointer();
            reply.insert("x".into(), at.x.into());
            reply.insert("y".into(), at.y.into());
        }
        "move" => {
            let from = desktop.pointer();
            let (to, delta) = match (request.get("to"), request.get("by")) {
                (Some(to), None) => {
                    let (x, y) = pair(to, "to")?;
                    (Point { x, y }, (x - from.x, y - from.y))
                }
                (None, Some(by)) => {
                    let (dx, dy) = pair(by, "by")?;
                    let target = Point {
                        x: from.x + dx,
                        y: from.y + dy,
                    };
                    (held_at_edge(from, target, &desktop.displays()), (dx, dy))
                }
                _ => return Err(bad("\"move\" takes one of \"to\" and \"by\"")),
            };
            let input = Input::Move { to, delta };
            post(desktop, &mut reply, [input])?;
            reply.insert("x".into(), to.x.into());
            reply.insert("y".into(), to.y.into());
        }
        "button" => {
            let button = match request.get("button").and_then(Value::as_str) {
                Some("left") => Button::Left,
                Some("right") => Button::Right,
                Some("middle") => Button::Middle,
                _ => return Err(bad("\"button\" is \"left\", \"right\" or \"middle\"")),
            };
            let downs = action(request, "click")?;
            post(
                desktop,
                &mut reply,
                downs.iter().map(|&down| Input::Button { button, down }),
            )?;
        }
        "key" => {
            let key = key(request)?;
            let downs = action(request, "press")?;
            post(
                desktop,
                &mut reply,
                downs.iter().map(|&down| Input::Key { key, down }),
            )?;
        }
        "key-state" => {
            let key = key(request)?;
            reply.insert("down".into(), desktop.key_down(key.code).into());
        }
        "scroll" => {
            let lines = |name| match request.get(name) {
                None => Ok(0),
                Some(n) => n
                    .as_i64()
                    .and_then(|n| i32::try_from(n).ok())
                    .ok_or_else(|| bad(format!("\"{name}\" is a whole number of lines"))),
            };
            let (dx, dy) = (lines("dx")?, lines("dy")?);
            if (dx, dy) == (0, 0) {
                return Err(bad("\"scroll\" needs \"dx\" or \"dy\""));
            }
            post(desktop, &mut reply, [Input::Scroll { dx, dy }])?;
        }
        "clock" => {
            reply.insert("mono_ns".into(), desktop.mono_ns().into());
            let unix_ns = SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .map_or(0, |t| u64::try_from(t.as_nanos()).unwrap_or(u64::MAX));
            reply.insert("unix_ns".into(), unix_ns.into());
        }
        "quit" => {}
        _ => return Err(bad(format!("unknown op {op:?}"))),
    }
    reply
        .entry("mono_ns")
        .or_insert_with(|| desktop.mono_ns().into());
    Ok(reply)
}

/// Posts `inputs` in order, once the OS is known to take them. The reply's `mono_ns` is the
/// moment just before the first one.
fn post<D: Desktop>(
    desktop: &mut D,
    reply: &mut Map<String, Value>,
    inputs: impl IntoIterator<Item = Input>,
) -> Result<(), Fault> {
    if !desktop.trusted() {
        return Err(Fault {
            code: "not-trusted",
            message: format!(
                "the OS does not let the {} agent post input",
                desktop.name()
            ),
        });
    }
    reply.insert("mono_ns".into(), desktop.mono_ns().into());
    for input in inputs {
        desktop.post(input).map_err(|message| Fault {
            code: "failed",
            message,
        })?;
    }
    Ok(())
}

/// Where a relative move from `from` towards `target` leaves the pointer: at `target` when a
/// display holds it, else clamped into the display the pointer is on (or the main one), the way
/// the OS holds a pointer pushed against an edge.
fn held_at_edge(from: Point, target: Point, displays: &[Display]) -> Point {
    if displays.iter().any(|d| d.contains(target)) {
        return target;
    }
    let Some(d) = displays
        .iter()
        .find(|d| d.contains(from))
        .or_else(|| displays.iter().find(|d| d.main))
        .or(displays.first())
    else {
        return target;
    };
    Point {
        x: target.x.clamp(d.x, d.x + d.width - 1.0),
        y: target.y.clamp(d.y, d.y + d.height - 1.0),
    }
}

/// The presses and releases an `action` stands for: "down", "up", or `pair` for both in turn.
fn action(request: &Map<String, Value>, pair: &str) -> Result<&'static [bool], Fault> {
    match request.get("action").and_then(Value::as_str) {
        Some("down") => Ok(&[true]),
        Some("up") => Ok(&[false]),
        Some(a) if a == pair => Ok(&[true, false]),
        _ => Err(bad(format!("\"action\" is \"down\", \"up\" or \"{pair}\""))),
    }
}

/// The request's `key`, which must be a name from [`crate::keys::KEYS`].
fn key(request: &Map<String, Value>) -> Result<Key, Fault> {
    let name = request
        .get("key")
        .and_then(Value::as_str)
        .ok_or_else(|| bad("no \"key\""))?;
    Key::named(name).ok_or_else(|| Fault {
        code: "unknown-key",
        message: format!("no key is named {name:?}; keys are W3C `code` names, like \"KeyA\""),
    })
}

/// A field holding two numbers, `[x, y]`.
fn pair(value: &Value, name: &str) -> Result<(f64, f64), Fault> {
    match value.as_array().map(Vec::as_slice) {
        Some([x, y]) => match (x.as_f64(), y.as_f64()) {
            (Some(x), Some(y)) => Ok((x, y)),
            _ => Err(bad(format!("\"{name}\" holds two numbers"))),
        },
        _ => Err(bad(format!("\"{name}\" is [x, y]"))),
    }
}
