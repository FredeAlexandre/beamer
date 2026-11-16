# Nightly triage fixtures

Nightly runs' artifacts as `gh run download <run> -D <dir>` leaves them, one directory per artifact,
for `cargo xtask triage` (`xtask/tests/triage.rs`).

| Fixture | Where it comes from |
|---|---|
| `mutants-run/` | Real: the eight `mutants-linux-shard-<k>` artifacts of nightly run [43951664303](https://github.com/FredeAlexandre/beamer/actions/runs/43951664303), #33's proof run on the weak-test demo crate `beamer-types`. Trimmed to `outcomes.json` and `diff/`, which are all the command reads; shards 5 to 7 had no mutant and wrote no `outcomes.json`. Two mutants of `is_landscape` survived, in shards 0 and 4 |
| `mutants-run.artifacts.json` | Real: that run's `gh api repos/FredeAlexandre/beamer/actions/runs/43951664303/artifacts`, trimmed to each artifact's `id` and `name` |
| `mutants-run.jobs.json` | Real: that run's `gh run view 43951664303 --json jobs`, trimmed to `name`, `conclusion` and `url`. Every job passed or was skipped |
| `two-functions/` | `mutants-linux-shard-0/outcomes.json` is `../mutants/timeout-unviable/`'s (`is_big -> bool with true` timed out), `mutants-linux-shard-1/outcomes.json` is `../mutants/shard-1/`'s (four mutants of `unused` missed). No `diff/`: their diffs were trimmed |
| `failures-run/` | Hand-made, as no nightly has crashed a fuzz harness or failed the model yet. The layout is what the nightly's uploads give: `fuzz-crashes/` and `fuzz-corpus/` keep each file's whole path in the checkout (their pattern starts with `**`), and `model-failures/` holds one `<package>.log` of `cargo test` output per crate. `beamer-model.log` has a failing test, `beamer-sim.log` passes, `beamer-broken.log` did not compile |
| `failures-run.jobs.json` | Hand-made, in the shape of `gh run view --json jobs`: a mutants shard failed, the fuzz and model jobs failed with their artifacts, the bench was cancelled |
