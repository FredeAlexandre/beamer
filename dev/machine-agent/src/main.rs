//! `machine-agent`: the agent and its client, run from a terminal or the Mac agent's `.app`
//! (`cli` has the command line).

fn main() -> std::process::ExitCode {
    machine_agent::cli::main()
}
