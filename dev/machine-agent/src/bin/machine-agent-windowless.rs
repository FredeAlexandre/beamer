//! `machine-agent-windowless`: the same command line as `machine-agent`, built for the Windows GUI
//! subsystem, so the PC agent's scheduled task starts it in the signed-in desktop session without
//! a console window on Alex's screen. It has no stderr: its task passes `serve --log FILE`.

#![cfg_attr(windows, windows_subsystem = "windows")]

fn main() -> std::process::ExitCode {
    machine_agent::cli::main()
}
