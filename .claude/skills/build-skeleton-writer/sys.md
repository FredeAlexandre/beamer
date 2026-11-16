# A `-sys` crate

`beamer-opus-sys` and `beamer-speexdsp-sys` wrap C that stays C: libopus and speexdsp, vendored
and built with `cc`, with no CMake (#16). Their issues' criteria take the place of `todo!()`
bodies; the contract in SKILL.md holds for everything else.

- **Vendor** the library's source into the crate, with its licence file, and compile it from
  `build.rs` with `cc`. `xtask blind` copies `build.rs` as it is, so the export builds the C too.
- **Bindings** are `extern "C"` declarations, which have no body to blind. Bind what the media
  engine calls today: the engine's C++ at `archive/python`, plus whatever the media and audio
  rules name. Every binding cites the rules it serves.
- **The smoke test** goes in `src/`, under `#[cfg(test)]`, never in `tests/`. A round trip through
  the C reaches no `todo!()`, and `xtask red-first` fails any new test in `tests/` that passes.
- **Three OSes.** Build on the Mac (clang). Build on `lebig` (MSVC, found through `vswhere`) as
  `dev/README.md` describes. Ubuntu runs in CI; until the CI workflow runs ubuntu, say in the PR
  that it was not checked.
