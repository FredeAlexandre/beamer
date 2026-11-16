# Crates

Every crate of the app lives here, flat: `crates/beamer-<name>/`, with the directory name equal to
the crate name. The crates and their dependency rules are decided in #16 and ADR 0004. The root
`Cargo.toml` takes every directory here as a workspace member, and `xtask`'s tests reject a
directory that breaks the layout.

`cargo xtask deps` checks the dependency rules. A new crate needs its row in
`xtask/src/deps/table.rs`. A sans-I/O crate (`beamer-types`, `beamer-wire`, the domain crates,
`beamer-core`, `beamer-platform`, `beamer-viewmodel`, `beamer-sim`) also needs a copy of
`xtask/sans-io.clippy.toml` as its `clippy.toml`.

This file also keeps `crates/` in git while it holds no crate: Cargo refuses a `crates/*` member
glob that matches nothing.
