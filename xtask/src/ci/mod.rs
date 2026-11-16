//! What the CI workflow and the nightly run (#33, #22, ADR 0002): which crates each runner covers,
//! the nextest gate, and the mutants gate.

pub mod crates;
pub mod mutants;
pub mod nextest;
