//! `cargo xtask`: the workspace's own commands (#22). Later tickets add them to [`Command`].

mod spec;
mod tracey;

use std::path::PathBuf;
use std::process::ExitCode;

use clap::{Args, Parser, Subcommand};

/// Beamer's dev commands, run as `cargo xtask <command>`.
#[derive(Parser)]
#[command(name = "xtask", arg_required_else_help = true)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Check the spec in spec/ and the tests that cite it (spec/README.md).
    #[command(subcommand)]
    Spec(SpecCommand),
}

#[derive(Subcommand)]
enum SpecCommand {
    /// Check the spec format, then run `tracey query validate`: rule ids, references, stale tests.
    Validate(Root),
    /// Run `tracey query stale`: fail when a reference cites an older version of its rule.
    Stale(Root),
    /// List every parity change, for Alex to approve in one sitting.
    Parity(Root),
}

#[derive(Args)]
struct Root {
    /// The project to check: the directory holding spec/ and .config/tracey/. Defaults to this
    /// workspace.
    #[arg(long)]
    root: Option<PathBuf>,
}

impl Root {
    fn dir(self) -> PathBuf {
        self.root
            .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(".."))
    }
}

fn main() -> ExitCode {
    let ok = match Cli::parse().command {
        Command::Spec(SpecCommand::Validate(root)) => spec::validate(&root.dir()),
        Command::Spec(SpecCommand::Stale(root)) => spec::stale(&root.dir()),
        Command::Spec(SpecCommand::Parity(root)) => spec::parity(&root.dir()),
    };
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
