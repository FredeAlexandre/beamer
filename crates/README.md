# Crates

Every crate of the app lives here, flat: `crates/beamer-<name>/`, with the directory name equal to
the crate name. The crates and their dependency rules are decided in #16 and ADR 0004. The root
`Cargo.toml` takes every directory here as a workspace member, and `xtask`'s tests reject a
directory that breaks the layout.

This file also keeps `crates/` in git while it holds no crate: Cargo refuses a `crates/*` member
glob that matches nothing.
