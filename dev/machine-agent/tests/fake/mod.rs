//! A fake desktop behind a real agent on a loopback port.

use std::io::{BufRead, BufReader, Write};
use std::net::{TcpListener, TcpStream};
use std::sync::{Arc, Mutex, mpsc};
use std::time::Duration;

use machine_agent::{Desktop, Display, Input, Point};
use serde_json::Value;

/// The main display 1440x900 at the origin, and a 2560x1440 one to its right, 200 points higher.
pub fn displays() -> Vec<Display> {
    vec![
        Display {
            x: 0.0,
            y: 0.0,
            width: 1440.0,
            height: 900.0,
            main: true,
        },
        Display {
            x: 1440.0,
            y: -200.0,
            width: 2560.0,
            height: 1440.0,
            main: false,
        },
    ]
}

pub struct FakeDesktop {
    pub trusted: bool,
    pub pointer: Point,
    pub posted: Vec<Input>,
    pub mono_ns: u64,
}

impl Desktop for FakeDesktop {
    fn name(&self) -> &'static str {
        "fake"
    }
    fn trusted(&self) -> bool {
        self.trusted
    }
    fn displays(&self) -> Vec<Display> {
        displays()
    }
    fn pointer(&self) -> Point {
        self.pointer
    }
    fn mono_ns(&self) -> u64 {
        self.mono_ns
    }
    fn post(&mut self, input: Input) -> Result<(), String> {
        if let Input::Move { to, .. } = &input {
            self.pointer = *to;
        }
        self.posted.push(input);
        Ok(())
    }
    fn key_down(&self, key: &str) -> bool {
        self.posted.iter().rev().find_map(|i| match i {
            Input::Key { key: k, down } if k.code == key => Some(*down),
            _ => None,
        }) == Some(true)
    }
}

pub struct Agent {
    pub desktop: Arc<Mutex<FakeDesktop>>,
    pub port: u16,
    conn: Mutex<Option<(BufReader<TcpStream>, TcpStream)>>,
    stopped: mpsc::Receiver<()>,
}

impl Agent {
    pub fn start() -> Agent {
        Agent::start_with(true)
    }

    pub fn start_with(trusted: bool) -> Agent {
        Agent::start_full(trusted, Duration::from_secs(60))
    }

    pub fn start_idle(idle: Duration) -> Agent {
        Agent::start_full(true, idle)
    }

    fn start_full(trusted: bool, idle: Duration) -> Agent {
        let desktop = Arc::new(Mutex::new(FakeDesktop {
            trusted,
            pointer: Point { x: 100.0, y: 100.0 },
            posted: Vec::new(),
            mono_ns: 1_000,
        }));
        let listener = TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let served = desktop.clone();
        let (done, stopped) = mpsc::channel();
        std::thread::spawn(move || {
            machine_agent::serve(listener, served, idle).unwrap();
            let _ = done.send(());
        });
        Agent {
            desktop,
            port,
            conn: Mutex::new(None),
            stopped,
        }
    }

    /// Sends one request line on this agent's connection and reads one reply line.
    pub fn ask(&self, request: Value) -> Value {
        self.ask_line(&request.to_string())
    }

    pub fn ask_line(&self, line: &str) -> Value {
        let mut conn = self.conn.lock().unwrap();
        let (reader, writer) = conn.get_or_insert_with(|| connect(self.port));
        writer.write_all(format!("{line}\n").as_bytes()).unwrap();
        let mut reply = String::new();
        reader.read_line(&mut reply).unwrap();
        serde_json::from_str(&reply).unwrap_or_else(|e| panic!("reply {reply:?} is not JSON: {e}"))
    }

    /// Whether the agent stopped serving within `within`.
    pub fn stops_within(&self, within: Duration) -> bool {
        self.stopped.recv_timeout(within).is_ok()
    }

    pub fn posted(&self) -> Vec<Input> {
        self.desktop.lock().unwrap().posted.clone()
    }
}

pub fn connect(port: u16) -> (BufReader<TcpStream>, TcpStream) {
    let stream = TcpStream::connect(("127.0.0.1", port)).unwrap();
    stream
        .set_read_timeout(Some(Duration::from_secs(5)))
        .unwrap();
    (BufReader::new(stream.try_clone().unwrap()), stream)
}
