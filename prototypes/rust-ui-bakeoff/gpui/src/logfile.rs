// PROTOTYPE: one log file, plus GPUI's own `log` records at warn and above.
use std::{
    fs::{File, OpenOptions},
    io::Write,
    path::PathBuf,
    sync::{Mutex, OnceLock},
    time::Instant,
};

static FILE: OnceLock<Mutex<File>> = OnceLock::new();
static START: OnceLock<Instant> = OnceLock::new();

pub fn path() -> PathBuf {
    #[cfg(target_os = "macos")]
    let dir = PathBuf::from(std::env::var("HOME").unwrap()).join("Library/Logs/BeamerBakeoff");
    #[cfg(target_os = "windows")]
    let dir = PathBuf::from(std::env::var("LOCALAPPDATA").unwrap()).join("BeamerBakeoff");
    std::fs::create_dir_all(&dir).ok();
    dir.join("gpui.log")
}

pub fn init(start: Instant) {
    START.set(start).ok();
    let f = OpenOptions::new().create(true).write(true).truncate(true).open(path()).unwrap();
    FILE.set(Mutex::new(f)).ok();
    log::set_boxed_logger(Box::new(Forward)).ok();
    log::set_max_level(log::LevelFilter::Warn);
}

pub fn line(msg: &str) {
    let t = START.get().map(|s| s.elapsed().as_secs_f64()).unwrap_or(0.0);
    let text = format!("[{t:8.3}] {msg}\n");
    eprint!("{text}");
    if let Some(f) = FILE.get() {
        let mut f = f.lock().unwrap();
        f.write_all(text.as_bytes()).ok();
        f.flush().ok();
    }
}

#[macro_export]
macro_rules! logln {
    ($($t:tt)*) => { $crate::logfile::line(&format!($($t)*)) };
}

struct Forward;
impl log::Log for Forward {
    fn enabled(&self, m: &log::Metadata) -> bool {
        m.level() <= log::Level::Warn
    }
    fn log(&self, r: &log::Record) {
        if self.enabled(r.metadata()) {
            line(&format!("{} {}: {}", r.level(), r.target(), r.args()));
        }
    }
    fn flush(&self) {}
}
