# Kill matrix fixtures

Real `mutants.out` directories, trimmed to `outcomes.json` and `log/`, which are all `cargo xtask
kill-matrix` reads. Each comes from cargo-mutants 27.1 and cargo-nextest 0.9.152 on the crate in
`beamer-k/`, placed at `crates/beamer-k/` of a one-crate workspace whose `.config/nextest.toml` is
this repo's (`retries = 1`, `flaky-result = "fail"`). The toolchain path in the logs is shortened to
`cargo`.

| Directory | Made with |
|---|---|
| `shard-0/`, `shard-1/` | `cargo mutants --no-shuffle --test-tool=nextest --shard <k>/2 -- -E 'kind(test)' --no-fail-fast`, as the nightly runs it |
| `fail-fast/` | the same without `--no-fail-fast` and `--shard`, so each log names only the first killing test |
| `timeout-unviable/` | `shard-0/` with two outcomes edited by hand: `is_big -> bool with true` timed out, `with false` unviable |
| `with-unit/` | `--no-fail-fast` without `-E 'kind(test)'`, so the `#[cfg(test)]` unit test runs too |

In `beamer-k`, `doubles` and `doubles_again` kill every mutant of `double`, `big` every mutant of
`is_big`, `guards_nothing` asserts nothing, and only the unit test reaches `unused`.
