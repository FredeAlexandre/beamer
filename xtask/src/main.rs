//! `cargo xtask`: the workspace's own commands (#22). Later tickets add them to [`Command`].

mod audit;
mod blind;
mod checklist;
mod ci;
mod copy_tests;
mod deps;
mod digest;
mod kill_matrix;
mod red_first;
mod spec;
mod tracey;
mod triage;
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
    /// Export the workspace for a test agent: every function body under `crates/` replaced by
    /// `todo!()`, `xtask/` as it is, no `.git`, the spec and the glossary included (#32, ADR 0002).
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
    /// Check that spec tests are honest (spec/README.md): a current `r[verify]` and an assertion
    /// per test, a reason per skip, test and implementation commits apart, and the Python ledger.
    TestAudit {
        #[command(flatten)]
        root: Root,
        /// Check only the commits after this one, such as `origin/rust` on a task branch.
        /// Defaults to every commit of HEAD.
        #[arg(long, value_name = "REV")]
        since: Option<String>,
        /// The git ref holding the Python tests the ledger accounts for.
        #[arg(long, value_name = "REF", default_value = "archive/python")]
        python_ref: String,
        /// Require the ledger to give every Python test its fate, as before the build lands.
        #[arg(long)]
        complete: bool,
    },
    /// Print which spec test kills which mutant, as Markdown, from the mutants.out directories of
    /// a nightly run (each shard's). The run must pass nextest `--no-fail-fast`.
    KillMatrix {
        /// The mutants.out directories, one per shard.
        #[arg(required = true, value_name = "MUTANTS_OUT")]
        runs: Vec<PathBuf>,
    },
    /// Open the milestone's checklist issue for Alex: a box per `human`-tier rule, with what to
    /// do and what to see. Run again, it updates that issue (#49).
    Checklist {
        /// The milestone, such as `M1`.
        milestone: String,
        /// The GitHub repository the issue is in, as owner/name.
        #[arg(
            long,
            value_name = "OWNER/NAME",
            default_value = "FredeAlexandre/beamer"
        )]
        repo: String,
        /// The machine-tier report to link: a URL, or a file to upload as a secret gist.
        /// Defaults to the newest JSON report in dev/.reports/, where `cargo xtask machine`
        /// writes them.
        #[arg(long, value_name = "FILE|URL")]
        report: Option<String>,
        /// Give a box only to the rules under this rule-id prefix, such as `crossing.zone`: the
        /// milestone's rule groups. Repeat it for several.
        #[arg(long, value_name = "PREFIX")]
        only: Vec<String>,
        /// Print the issue's body and say whether it would be opened or updated, without writing
        /// to GitHub.
        #[arg(long)]
        dry_run: bool,
        #[command(flatten)]
        root: Root,
    },
    /// Print the cargo arguments that select the crates a CI runner covers for a job:
    /// `--workspace`, `-p` flags, or nothing (#33). The CI workflow and the nightly read it.
    Crates {
        #[arg(long, value_enum)]
        runner: ci::crates::Runner,
        #[arg(long = "for", value_name = "JOB", value_enum)]
        job: ci::crates::Job,
    },
    /// Run nextest as the merge gate does, on the crates a runner tests (#33). A spec test that
    /// panics at a `todo!()` is pending and passes; any other failure fails.
    Nextest {
        #[arg(long, value_enum)]
        runner: ci::crates::Runner,
        /// More arguments for `cargo nextest run`.
        #[arg(last = true)]
        args: Vec<String>,
    },
    /// Run cargo-mutants on the crates a runner mutates, as the merge gate (`--in-diff`) and the
    /// nightly do (#33): `.cargo/mutants.toml`'s test filter, less the spec tests merged red, and
    /// no mutants of `todo!()` bodies. Each mutant meets the spec tests of every crate that
    /// reaches it (#135). Exits with cargo-mutants' code: 2 missed, 3 timed out.
    Mutants {
        #[arg(long, value_enum)]
        runner: ci::crates::Runner,
        /// Mutate only the code changed since the merge base of HEAD and this rev.
        #[arg(long, value_name = "REV")]
        in_diff: Option<String>,
        /// Run one shard of the mutants, as `k/n`.
        #[arg(long, value_name = "K/N")]
        shard: Option<String>,
        /// More arguments for the test tool, after cargo-mutants' `--`.
        #[arg(last = true)]
        args: Vec<String>,
    },
    /// The nightly triage routine's deterministic half (#44, dev/routines/nightly-triage.md):
    /// what to file from a nightly run's artifacts, and which fuzz corpus entries are new.
    #[command(subcommand)]
    Triage(TriageCommand),
}

#[derive(Subcommand)]
enum TriageCommand {
    /// Print, as JSON, the issues a nightly run's findings call for: one `triage:mutant` issue
    /// per function with surviving mutants, one `triage:fuzz` per crashed harness, one
    /// `triage:model` per failing model test, and one `needs:alex` for jobs that failed leaving
    /// nothing to triage. A group whose issue is open in `--open` is not opened again.
    Issues {
        /// Where `gh run download <run>` put the run's artifacts, one directory per artifact.
        dir: PathBuf,
        /// The nightly run's id.
        #[arg(long, value_name = "ID")]
        run: String,
        /// The GitHub repository, as owner/name, of the run and the issues.
        #[arg(
            long,
            value_name = "OWNER/NAME",
            default_value = "FredeAlexandre/beamer"
        )]
        repo: String,
        /// The `rust` commit the run tested, to link the source at. Empty when unknown.
        #[arg(long, value_name = "SHA")]
        sha: Option<String>,
        /// `gh api repos/<repo>/actions/runs/<run>/artifacts`, saved: links each artifact.
        #[arg(long, value_name = "FILE")]
        artifacts: Option<PathBuf>,
        /// `gh run view <run> --json jobs`, saved: finds the jobs that failed.
        #[arg(long, value_name = "FILE")]
        jobs: Option<PathBuf>,
        /// `gh issue list --state open --json number,body`, saved: the issues already open.
        #[arg(long, value_name = "FILE")]
        open: Option<PathBuf>,
    },
    /// List the fuzz corpus entries in a nightly's `fuzz-corpus` artifact that the checkout
    /// lacks, one path per line, and with `--write` copy them in.
    Corpus {
        /// Where `gh run download <run> -n fuzz-corpus` put the artifact.
        dir: PathBuf,
        /// Copy the new entries into the checkout.
        #[arg(long)]
        write: bool,
        #[command(flatten)]
        root: Root,
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
    /// Print the spec digest: every rule added, bumped or removed between two commits, with the
    /// pull request that changed it, parity and human-tier changes first (#45). Prints nothing
    /// when no rule changed.
    Digest(Digest),
}

#[derive(Args)]
struct Digest {
    /// The commit the last digest ran through.
    #[arg(long, value_name = "REV")]
    since: String,
    /// The commit to run through.
    #[arg(long, value_name = "REV", default_value = "HEAD")]
    until: String,
    /// The GitHub repository, as owner/name, to look each commit's pull request up in with `gh`,
    /// and to link rules into. Without it, changes name their commits.
    #[arg(long, value_name = "OWNER/NAME")]
    repo: Option<String>,
    #[command(flatten)]
    root: Root,
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
        Command::Spec(SpecCommand::Digest(args)) => report(digest::run(
            &args.root.dir(),
            &args.since,
            &args.until,
            args.repo.as_deref(),
        )),
        Command::Deps { manifest_path } => {
            let manifest = manifest_path
                .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../Cargo.toml"));
            deps::run(&manifest)
        }
        Command::Blind { dir } => report(blind::run(&dir)),
        Command::CopyTests { export } => report(copy_tests::run(&export)),
        Command::RedFirst { base } => report(red_first::run(&base)),
        Command::TestAudit {
            root,
            since,
            python_ref,
            complete,
        } => exit(audit::run(&audit::Options {
            root: &root.dir(),
            since: since.as_deref(),
            python_ref: &python_ref,
            complete,
        })),
        Command::KillMatrix { runs } => exit(kill_matrix::run(&runs)),
        Command::Checklist {
            milestone,
            repo,
            report: given,
            only,
            dry_run,
            root,
        } => report(checklist::run(&checklist::Options {
            root: &root.dir(),
            milestone: &milestone,
            repo: &repo,
            report: given.as_deref(),
            only: &only,
            dry_run,
        })),
        Command::Crates { runner, job } => report(
            workspace::root()
                .and_then(|root| ci::crates::select(&root, runner, job))
                .map(|selection| println!("{selection}")),
        ),
        Command::Mutants {
            runner,
            in_diff,
            shard,
            args,
        } => {
            let options = ci::mutants::Options {
                runner,
                in_diff: in_diff.as_deref(),
                shard: shard.as_deref(),
                args: &args,
            };
            match workspace::root().and_then(|root| ci::mutants::run(&root, &options)) {
                Ok(code) => ExitCode::from(code),
                Err(message) => {
                    eprintln!("error: {message}");
                    ExitCode::FAILURE
                }
            }
        }
        Command::Triage(TriageCommand::Issues {
            dir,
            run,
            repo,
            sha,
            artifacts,
            jobs,
            open,
        }) => report(triage::issues(&triage::Options {
            dir: &dir,
            run: &run,
            repo: &repo,
            sha: sha.as_deref().filter(|sha| !sha.is_empty()),
            artifacts: artifacts.as_deref(),
            jobs: jobs.as_deref(),
            open: open.as_deref(),
        })),
        Command::Triage(TriageCommand::Corpus { dir, write, root }) => {
            report(triage::corpus(&dir, &root.dir(), write))
        }
        Command::Nextest { runner, args } => {
            report(workspace::root().and_then(|root| ci::nextest::run(&root, runner, &args)))
        }
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
