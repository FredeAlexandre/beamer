//! The allowed-dependency table: every crate of #16's layout, its group, and the workspace crates
//! it may depend on. A crate missing from here fails `cargo xtask deps` until a row is added, so
//! the table always covers the whole workspace.
//!
//! The five dependency rules (#16, ADR 0004) it encodes:
//! 1. Domain crates depend only on `beamer-types`, never on each other or on platform crates, and
//!    never read clocks, spawn threads, or touch `std::net`, `std::fs` or `std::thread`.
//! 2. `beamer-core` depends on the domain crates and `beamer-wire`, and on nothing that does I/O.
//! 3. A `-ffi` crate is used only by its own adapter crate. No other crate names `windows`,
//!    `objc2*` or `extern "C"` (the `-sys` crates excepted).
//! 4. `beamer-ui` depends on `beamer-viewmodel`, Rune and `tray-icon` only.
//! 5. Only `beamer-runtime`, the platform adapters and `beamer` do I/O or spawn threads.

/// The crates of #16's "Domain (sans-I/O)" group, `beamer-core` aside.
pub const DOMAIN: &[&str] = &[
    "beamer-link",
    "beamer-discovery",
    "beamer-pairing",
    "beamer-store",
    "beamer-routing",
    "beamer-crossing",
    "beamer-input",
    "beamer-clipboard",
    "beamer-settings",
    "beamer-display",
    "beamer-audio",
    "beamer-camera",
    "beamer-media",
];

/// Crates from outside that do I/O: async runtimes, pollers and sockets. No sans-I/O crate
/// depends on one, in any kind of dependency but dev (rules 2 and 5). `quinn-proto` is sans-I/O;
/// `quinn` is not.
pub const IO_CRATES: &[&str] = &[
    "tokio",
    "async-std",
    "smol",
    "async-io",
    "futures-executor",
    "mio",
    "polling",
    "socket2",
    "quinn",
    "quinn-udp",
];

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Group {
    /// `beamer-types` and `beamer-wire`.
    Vocabulary,
    /// [`DOMAIN`].
    Domain,
    Core,
    /// `beamer-platform`: one trait per capability, and its fakes.
    Seam,
    /// `beamer-mac` and `beamer-win`.
    Adapter,
    /// `beamer-mac-ffi` and `beamer-win-ffi`.
    Ffi,
    /// `beamer-opus-sys` and `beamer-speexdsp-sys`: vendored C.
    Sys,
    Runtime,
    ViewModel,
    Ui,
    /// `beamer`, the binary.
    Bin,
    /// `beamer-sim`, a dev-dependency only.
    Sim,
    /// `xtask`, which no crate of the app sees.
    Tool,
}

impl Group {
    /// Sans-I/O crates hold no clock, thread, socket or file (rules 1, 2 and 5). Each carries the
    /// `sans-io.clippy.toml` lists and none may depend on an I/O crate.
    pub fn is_sans_io(self) -> bool {
        matches!(
            self,
            Group::Vocabulary
                | Group::Domain
                | Group::Core
                | Group::Seam
                | Group::ViewModel
                | Group::Sim
        )
    }
}

pub struct Row {
    pub group: Group,
    /// The workspace crates it may have as normal or build dependencies.
    pub may_use: Vec<&'static str>,
    /// When set, the only crates from outside the workspace it may have as normal or build
    /// dependencies. Unset, any crate is allowed that no other rule forbids.
    pub outside: Option<&'static [&'static str]>,
}

/// The row of `name`, or `None` for a crate the table doesn't know.
pub fn row(name: &str) -> Option<Row> {
    let types = vec!["beamer-types"];
    let with = |more: &[&'static str]| -> Vec<&'static str> {
        let mut list = vec!["beamer-types"];
        list.extend_from_slice(more);
        list
    };
    let domain_and = |more: &[&'static str]| -> Vec<&'static str> {
        let mut list = with(DOMAIN);
        list.extend_from_slice(more);
        list
    };
    let (group, may_use) = match name {
        "beamer-types" => (Group::Vocabulary, vec![]),
        // Messages live with their domain; wire composes them into the one top-level enum.
        "beamer-wire" => (Group::Vocabulary, domain_and(&[])),
        _ if DOMAIN.contains(&name) => (Group::Domain, types),
        "beamer-core" => (Group::Core, domain_and(&["beamer-wire"])),
        "beamer-platform" => (Group::Seam, types),
        "beamer-mac" => (Group::Adapter, with(&["beamer-platform", "beamer-mac-ffi"])),
        "beamer-win" => (Group::Adapter, with(&["beamer-platform", "beamer-win-ffi"])),
        "beamer-mac-ffi" | "beamer-win-ffi" => (Group::Ffi, vec![]),
        "beamer-opus-sys" | "beamer-speexdsp-sys" => (Group::Sys, vec![]),
        "beamer-runtime" => (
            Group::Runtime,
            domain_and(&[
                "beamer-wire",
                "beamer-core",
                "beamer-platform",
                "beamer-opus-sys",
                "beamer-speexdsp-sys",
            ]),
        ),
        // The UI types come from core, through the view-model.
        "beamer-viewmodel" => (Group::ViewModel, with(&["beamer-core"])),
        "beamer-ui" => (Group::Ui, vec!["beamer-viewmodel"]),
        "beamer" => (
            Group::Bin,
            domain_and(&[
                "beamer-wire",
                "beamer-core",
                "beamer-platform",
                "beamer-mac",
                "beamer-win",
                "beamer-runtime",
                "beamer-viewmodel",
                "beamer-ui",
            ]),
        ),
        "beamer-sim" => (
            Group::Sim,
            domain_and(&["beamer-wire", "beamer-core", "beamer-platform"]),
        ),
        "xtask" => (Group::Tool, vec![]),
        _ => return None,
    };
    // Rule 4: Rune and tray-icon are the UI's only crates from outside.
    let outside: Option<&'static [&'static str]> =
        (group == Group::Ui).then_some(&["rune", "tray-icon"]);
    Some(Row {
        group,
        may_use,
        outside,
    })
}
