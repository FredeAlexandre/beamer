//! `cargo xtask`: the workspace's own commands (#22). Later tickets add them to [`Command`].

use clap::{Parser, Subcommand};

/// Beamer's dev commands, run as `cargo xtask <command>`.
#[derive(Parser)]
#[command(name = "xtask", arg_required_else_help = true)]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {}

fn main() {
    // With no command yet, parsing only prints help or an error and exits. Dispatch on
    // `Cli::parse().command` once the first command lands.
    Cli::parse();
}
