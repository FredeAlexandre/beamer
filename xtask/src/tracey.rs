//! Runs the pinned tracey on a project. tracey answers queries from a daemon it starts per
//! project; a daemon this module started is stopped again, so test and CI runs leave none behind.

use std::path::Path;
use std::process::{Command, Output};

/// The tracey release the spec is checked with. Moving it is its own commit.
pub const VERSION: &str = "1.4.0";

const INSTALL: &str = "\
install it from the release (macOS arm64, Linux):
    curl -LsSf https://github.com/bearcove/tracey/releases/download/v1.4.0/tracey-installer.sh | sh
or from source (any OS):
    cargo install --locked --git https://github.com/bearcove/tracey --tag v1.4.0 tracey";

/// What a tracey query printed, and whether it exited zero.
pub struct Answer {
    pub ok: bool,
    pub text: String,
}

/// Runs `tracey query <query>` in `root`. Fails with a message when tracey is missing, is not
/// [`VERSION`], or `root` has no tracey config.
pub fn query(root: &Path, query: &str) -> Result<Answer, String> {
    check_version()?;
    if !root.join(".config/tracey/config.styx").is_file() {
        return Err(format!(
            "{} has no .config/tracey/config.styx",
            root.display()
        ));
    }
    let was_running = run(root, &["status"])?.text.contains("Daemon is running");
    let answer = run(root, &["query", query]);
    if !was_running {
        // Best effort: an idle daemon also exits by itself after ten minutes.
        let _ = run(root, &["kill"]);
    }
    answer
}

fn check_version() -> Result<(), String> {
    let out = Command::new("tracey")
        .arg("--version")
        .output()
        .map_err(|_| format!("tracey {VERSION} is not on PATH; {INSTALL}"))?;
    let version = String::from_utf8_lossy(&out.stdout);
    if version.split_whitespace().nth(1) != Some(VERSION) {
        return Err(format!(
            "found {}, but the spec is checked with tracey {VERSION}; {INSTALL}",
            version.trim()
        ));
    }
    Ok(())
}

fn run(root: &Path, args: &[&str]) -> Result<Answer, String> {
    // tracey takes its project from the working directory and colours its output by default.
    let out: Output = Command::new("tracey")
        .args(args)
        .current_dir(root)
        .env("NO_COLOR", "1")
        .env("RUST_LOG", "warn")
        .output()
        .map_err(|e| format!("tracey {}: {e}", args.join(" ")))?;
    Ok(Answer {
        ok: out.status.success(),
        text: String::from_utf8_lossy(&out.stdout).into_owned()
            + &String::from_utf8_lossy(&out.stderr),
    })
}
