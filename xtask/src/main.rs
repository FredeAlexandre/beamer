//! `cargo xtask`: the workspace's own commands (#22). Later tickets add them to [`Command`].

mod blind;
mod copy_tests;
mod deps;
mod red_first;
mod spec;
mod tracey;
mod workspace;

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
    /// Check the dependency and seam rules of #16 and ADR 0004: the allowed-dependency table, the
    /// `-ffi` fence, the sans-I/O crates' clippy lists and the `-ffi` branching lint.
    Deps {
        /// The workspace to check. Defaults to this one.
        #[arg(long, value_name = "PATH")]
        manifest_path: Option<PathBuf>,
    },
    /// Export the workspace for a test agent: every function body replaced by `todo!()`, no
    /// `.git`, the spec and the glossary included (#32, ADR 0002).
    Blind {
        /// Where to write the export. It must not exist yet, or be an empty directory.
        dir: PathBuf,
    },
    /// Copy a test agent's work from a blind export back onto this checkout: every crate's
    /// `tests/`, and the `Cargo.toml` and `Cargo.lock` changes its new dev-dependencies made.
    CopyTests {
        /// The blind export the tests were written in.
        export: PathBuf,
    },
    /// Check that every test this checkout adds to a crate's `tests/` is red against the blind
    /// export: it must panic on a `todo!()`, not pass and not fail to compile.
    RedFirst {
        /// The branch the new tests are counted from, through its merge base with HEAD.
        #[arg(long, default_value = "origin/rust")]
        base: String,
    },
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

/// What a command reports when it fails: printed as is, then `cargo xtask` exits 1.
pub type Result<T = ()> = std::result::Result<T, String>;

fn main() -> ExitCode {
    match Cli::parse().command {
        Command::Spec(SpecCommand::Validate(root)) => exit(spec::validate(&root.dir())),
        Command::Spec(SpecCommand::Stale(root)) => exit(spec::stale(&root.dir())),
        Command::Spec(SpecCommand::Parity(root)) => exit(spec::parity(&root.dir())),
        Command::Deps { manifest_path } => {
            let manifest = manifest_path
                .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../Cargo.toml"));
            deps::run(&manifest)
        }
        Command::Blind { dir } => report(blind::run(&dir)),
        Command::CopyTests { export } => report(copy_tests::run(&export)),
        Command::RedFirst { base } => report(red_first::run(&base)),
    }
}

fn report(result: Result) -> ExitCode {
    match result {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => {
            eprintln!("error: {message}");
            ExitCode::FAILURE
        }
    }
}

fn exit(ok: bool) -> ExitCode {
    if ok {
        ExitCode::SUCCESS
    } else {
        ExitCode::FAILURE
    }
}
