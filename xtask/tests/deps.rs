//! `cargo xtask deps`: the dependency and seam rules of #16 and ADR 0004, run on fixture
//! workspaces. Every rule has a fixture that breaks it and must fail.

mod common;

use common::Fixture;
use std::process::Command;

#[test]
fn this_workspace_keeps_every_rule() {
    let out = Command::new(env!("CARGO_BIN_EXE_xtask"))
        .arg("deps")
        .output()
        .expect("the xtask binary runs");
    let text =
        String::from_utf8_lossy(&out.stdout).into_owned() + &String::from_utf8_lossy(&out.stderr);
    assert!(out.status.success(), "`cargo xtask deps` failed:\n{text}");
}

#[test]
fn a_workspace_that_keeps_every_rule_passes() {
    let ws = Fixture::new("keeps_every_rule");
    ws.sans_io("beamer-types", "").sans_io(
        "beamer-crossing",
        "[dependencies]\nbeamer-types = { path = \"../beamer-types\" }",
    );
    ws.deps().passes();
}

/// A path dependency on another crate of the fixture.
fn on(names: &[&str]) -> String {
    let mut toml = String::from("[dependencies]\n");
    for name in names {
        toml += &format!("{name} = {{ path = \"../{name}\" }}\n");
    }
    toml
}

#[test]
fn rule_1_a_domain_crate_that_depends_on_another_domain_crate_fails() {
    let ws = Fixture::new("domain_on_domain");
    ws.sans_io("beamer-types", "")
        .sans_io("beamer-routing", &on(&["beamer-types"]))
        .sans_io("beamer-crossing", &on(&["beamer-types", "beamer-routing"]));
    ws.deps()
        .fails_naming(&["beamer-crossing", "beamer-routing"]);
}

/// Every crate of #16's layout, typed out from the ticket rather than taken from the table.
const LAYOUT: &[&str] = &[
    "beamer-types",
    "beamer-wire",
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
    "beamer-core",
    "beamer-platform",
    "beamer-mac",
    "beamer-mac-ffi",
    "beamer-win",
    "beamer-win-ffi",
    "beamer-opus-sys",
    "beamer-speexdsp-sys",
    "beamer-runtime",
    "beamer-viewmodel",
    "beamer-ui",
    "beamer",
    "beamer-sim",
];

#[test]
fn the_table_covers_every_crate_of_the_layout() {
    let ws = Fixture::new("whole_layout");
    for name in LAYOUT {
        ws.sans_io(name, "");
    }
    ws.deps().passes();
}

#[test]
fn a_crate_missing_from_the_table_fails() {
    let ws = Fixture::new("unknown_crate");
    ws.sans_io("beamer-types", "").krate("beamer-gadgets", "");
    ws.deps()
        .fails_naming(&["beamer-gadgets", "not in the allowed-dependency table"]);
}

#[test]
fn rule_2_core_that_depends_on_an_io_crate_fails() {
    let ws = Fixture::new("core_on_runtime");
    ws.sans_io("beamer-types", "")
        .krate("beamer-runtime", &on(&["beamer-types"]))
        .sans_io("beamer-core", &on(&["beamer-types", "beamer-runtime"]));
    ws.deps()
        .fails_naming(&["beamer-core: depends on beamer-runtime"]);
}

#[test]
fn beamer_sim_is_a_dev_dependency_only() {
    let ws = Fixture::new("sim_as_dependency");
    ws.sans_io("beamer-types", "")
        .sans_io("beamer-sim", &on(&["beamer-types"]))
        .sans_io(
            "beamer-crossing",
            "[dev-dependencies]\nbeamer-sim = { path = \"../beamer-sim\" }",
        );
    ws.deps().passes();
    ws.sans_io("beamer-core", &on(&["beamer-types", "beamer-sim"]));
    ws.deps()
        .fails_naming(&["beamer-core: depends on beamer-sim"]);
}

#[test]
fn rule_3_an_ffi_crate_used_by_any_crate_but_its_adapter_fails() {
    let ws = Fixture::new("ffi_used_elsewhere");
    ws.sans_io("beamer-types", "")
        .krate("beamer-win-ffi", "")
        .krate("beamer-win", &on(&["beamer-types", "beamer-win-ffi"]));
    ws.deps().passes();
    // Even a test may not reach past the adapter.
    ws.sans_io(
        "beamer-crossing",
        "[dev-dependencies]\nbeamer-win-ffi = { path = \"../beamer-win-ffi\" }",
    );
    ws.deps()
        .fails_naming(&["beamer-crossing", "only beamer-win may use beamer-win-ffi"]);
}

#[test]
fn rule_3_windows_and_objc2_belong_to_their_own_ffi_crate() {
    let ws = Fixture::new("os_bindings");
    ws.krate(
        "beamer-win-ffi",
        "[target.'cfg(windows)'.dependencies]\nwindows = \"0.62\"",
    )
    .krate("beamer-mac-ffi", "[dependencies]\nobjc2-app-kit = \"0.3\"");
    ws.deps().passes();
    ws.krate(
        "beamer-win",
        "[target.'cfg(windows)'.dependencies]\nwindows-sys = \"0.61\"",
    )
    .krate(
        "beamer-mac",
        "[dependencies]\nappkit = { package = \"objc2\", version = \"0.6\" }",
    )
    .krate(
        "beamer-runtime",
        "[dev-dependencies]\nobjc2-foundation = \"0.3\"",
    );
    ws.deps().fails_naming(&[
        "beamer-win: names windows-sys",
        "beamer-mac: names objc2",
        "beamer-runtime: names objc2-foundation",
    ]);
}

#[test]
fn rule_3_windows_in_the_mac_ffi_crate_fails() {
    let ws = Fixture::new("windows_in_mac_ffi");
    ws.krate("beamer-mac-ffi", "[dependencies]\nwindows = \"0.62\"");
    ws.deps().fails_naming(&["beamer-mac-ffi: names windows"]);
}

#[test]
fn rule_3_an_extern_abi_outside_the_ffi_and_sys_crates_fails() {
    let ws = Fixture::new("extern_abi");
    ws.krate("beamer-mac-ffi", "")
        .file(
            "crates/beamer-mac-ffi/src/lib.rs",
            "unsafe extern \"C\" { fn CGMainDisplayID() -> u32; }",
        )
        .krate("beamer-opus-sys", "")
        .file(
            "crates/beamer-opus-sys/src/lib.rs",
            "unsafe extern \"C\" { pub fn opus_get_version_string() -> *const u8; }",
        )
        // A comment or a string naming it is not code: extern "C"
        .krate("beamer-runtime", "")
        .file(
            "crates/beamer-runtime/src/lib.rs",
            "// extern \"C\"\npub const ABI: &str = \"extern \\\"C\\\"\";",
        );
    ws.deps().passes();
    ws.file(
        "crates/beamer-runtime/src/hook.rs",
        "pub type Proc = unsafe extern \"system\" fn(i32) -> isize;\n",
    )
    .file(
        "crates/beamer-runtime/tests/callback.rs",
        "\n\nextern \"C\" fn on_tick() {}\n",
    );
    ws.deps().fails_naming(&[
        "beamer-runtime: names extern \"system\"",
        "src/hook.rs:1",
        "beamer-runtime: names extern \"C\"",
        "tests/callback.rs:3",
    ]);
}

#[test]
fn rule_4_the_ui_uses_the_view_model_rune_and_tray_icon_only() {
    let ws = Fixture::new("ui");
    ws.sans_io("beamer-types", "")
        .sans_io("beamer-core", &on(&["beamer-types"]))
        .sans_io("beamer-viewmodel", &on(&["beamer-types", "beamer-core"]))
        .krate(
            "beamer-ui",
            &(on(&["beamer-viewmodel"])
                + "rune = \"0.1\"\ntray-icon = \"0.21\"\n[dev-dependencies]\ninsta = \"1\""),
        );
    ws.deps().passes();
    ws.krate(
        "beamer-ui",
        &(on(&["beamer-viewmodel", "beamer-core"]) + "rune = \"0.1\"\nserde = \"1\""),
    );
    ws.deps().fails_naming(&[
        "beamer-ui: depends on beamer-core",
        "beamer-ui: depends on serde",
    ]);
}

#[test]
fn rules_2_and_5_a_sans_io_crate_that_depends_on_an_io_crate_fails() {
    let ws = Fixture::new("io_crates");
    ws.sans_io("beamer-link", "[dependencies]\nquinn-proto = \"0.11\"")
        .krate(
            "beamer-runtime",
            "[dependencies]\npolling = \"3\"\nsocket2 = \"0.6\"",
        );
    ws.deps().passes();
    ws.sans_io("beamer-core", "[dependencies]\ntokio = \"1\"")
        .sans_io("beamer-discovery", "[dependencies]\nsocket2 = \"0.6\"");
    ws.deps().fails_naming(&[
        "beamer-core: depends on tokio",
        "beamer-discovery: depends on socket2",
    ]);
}

#[test]
fn rules_1_2_and_5_a_sans_io_crate_without_the_clippy_lists_fails() {
    let ws = Fixture::new("clippy_lists");
    ws.sans_io("beamer-types", "")
        // A Windows checkout may turn the copy's line endings into CRLF.
        .file(
            "crates/beamer-types/clippy.toml",
            &common::SANS_IO_CLIPPY.replace('\n', "\r\n"),
        )
        // The other crates need no lists.
        .krate("beamer-runtime", "");
    ws.deps().passes();
    ws.krate("beamer-core", "")
        .sans_io("beamer-store", "")
        .file(
            "crates/beamer-store/clippy.toml",
            &common::SANS_IO_CLIPPY.replace(
                "{ path = \"std::fs::write\"",
                "# { path = \"std::fs::write\"",
            ),
        );
    ws.deps().fails_naming(&[
        "beamer-core: has no clippy.toml",
        "beamer-store: clippy.toml differs from xtask/sans-io.clippy.toml",
    ]);
}

/// Shims that hold nothing but calls and error mapping.
const THIN_SHIMS: &str = r#"
pub struct OsError(pub i32);

pub fn main_display() -> Result<u32, OsError> {
    let id = unsafe { os::main_display() };
    if id == 0 { return Err(OsError(0)); }
    Ok(id)
}

pub fn move_pointer(x: f64, y: f64) -> Result<(), OsError> {
    match unsafe { os::warp(x, y) } {
        0 => Ok(()),
        code => Err(OsError(code)),
    }
}

pub fn window(handle: *mut u8) -> Option<*mut u8> {
    if handle.is_null() { None } else { Some(handle) }
}

pub fn name(id: u32) -> Result<String, OsError> {
    let Some(raw) = (unsafe { os::name(id) }) else { return Err(OsError(-1)) };
    let text = os::utf8(raw).map_err(|_| OsError(-2))?;
    Ok(text)
}

pub fn check(status: i32) -> Result<(), OsError> {
    if status == 0 { Ok(()) } else if status > 0 { Err(OsError(status)) } else { Err(OsError(-status)) }
}

impl OsError {
    pub fn code(&self) -> i32 { self.0 }
}

#[cfg(test)]
mod tests {
    #[test]
    fn many() { for _ in 0..3 { if true { continue } } }
}
"#;

/// Shims that make decisions, each of which belongs in the adapter.
const THICK_SHIMS: &str = r#"
pub fn displays() -> Vec<u32> {
    let mut all = Vec::new();
    for i in 0..unsafe { os::count() } { all.push(i); }
    all
}

pub fn drain() { while unsafe { os::pending() } { unsafe { os::pop() }; } }

pub fn pump() { loop { unsafe { os::wait() }; } }

pub fn post(down: bool) {
    if down { unsafe { os::press() } } else { unsafe { os::release() } }
}

pub fn scale(dpi: u32) -> Result<f64, ()> {
    match dpi {
        0 => Err(()),
        d if d > 96 => Ok(d as f64 / 96.0),
        _ => Ok(1.0),
    }
}

pub struct Tap;
impl Tap {
    pub fn on_event(&self, kind: u32) -> u32 {
        let Some(k) = kind.checked_sub(1) else { return 0 };
        k
    }
}
"#;

#[test]
fn an_ffi_crate_of_thin_shims_passes() {
    let ws = Fixture::new("thin_shims");
    ws.krate("beamer-mac-ffi", "")
        .file("crates/beamer-mac-ffi/src/lib.rs", THIN_SHIMS)
        // Its tests, checked on the real machines, aren't shims.
        .file(
            "crates/beamer-mac-ffi/tests/displays.rs",
            "#[test]\nfn each() { for _ in 0..2 { if true { continue } } }\n",
        );
    ws.deps().passes();
}

#[test]
fn an_ffi_function_that_branches_or_loops_beyond_error_mapping_fails() {
    let ws = Fixture::new("thick_shims");
    ws.krate("beamer-win-ffi", "")
        .file("crates/beamer-win-ffi/src/shims.rs", THICK_SHIMS);
    ws.deps().fails_naming(&[
        "beamer-win-ffi: `for` in displays (src/shims.rs:4)",
        "beamer-win-ffi: `while` in drain (src/shims.rs:8)",
        "beamer-win-ffi: `loop` in pump (src/shims.rs:10)",
        "beamer-win-ffi: `if` in post (src/shims.rs:13)",
        "beamer-win-ffi: `match` in scale (src/shims.rs:17)",
        "beamer-win-ffi: `let else` in Tap::on_event (src/shims.rs:27)",
    ]);
}
