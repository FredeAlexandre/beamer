//! Builds the vendored libopus 1.6.1 with `cc`, from its own automake source lists, as the Python
//! app's engine did: float, no intrinsics and no run-time CPU detection, with `alloca` for scratch
//! memory. No CMake, no configure (#16). `cc` finds clang on macOS, MSVC through `vswhere` on
//! Windows, and the system C compiler on Linux.

use std::fs;
use std::path::{Path, PathBuf};

/// The version the vendored tree is, as libopus's own build would define it.
const VERSION: &str = "1.6.1";

/// The `.mk` lists and variables that make up the float build without intrinsics.
const SOURCE_LISTS: &[(&str, &str)] = &[
    ("celt_sources.mk", "CELT_SOURCES"),
    ("silk_sources.mk", "SILK_SOURCES"),
    ("silk_sources.mk", "SILK_SOURCES_FLOAT"),
    ("opus_sources.mk", "OPUS_SOURCES"),
    ("opus_sources.mk", "OPUS_SOURCES_FLOAT"),
];

fn main() {
    let root = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("set by cargo"))
        .join(format!("opus-{VERSION}"));
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed={}", root.display());

    let mut sources = Vec::new();
    for (list, var) in SOURCE_LISTS {
        sources.extend(
            mk_sources(&root.join(list), var)
                .into_iter()
                .map(|file| root.join(file)),
        );
    }
    sources.sort();
    sources.dedup();
    assert!(!sources.is_empty(), "no sources in {}", root.display());

    let mut build = cc::Build::new();
    build
        .files(&sources)
        .include(root.join("include"))
        .include(root.join("celt"))
        .include(root.join("silk"))
        .include(root.join("silk/float"))
        .include(root.join("src"))
        .include(&root)
        .define("OPUS_BUILD", None)
        .define("USE_ALLOCA", None)
        .define("HAVE_LRINTF", None)
        .define("PACKAGE_VERSION", format!("\"{VERSION}\"").as_str())
        // The codec runs in real time, in debug builds too.
        .opt_level(2)
        // Vendored code, compiled unmodified: its warnings are not ours to fix.
        .warnings(false);
    if build.get_compiler().is_like_msvc() {
        build.define("_CRT_SECURE_NO_WARNINGS", None);
    }
    build.compile("opus");
}

/// The `.c` files of `var = a.c b.c \` in an automake fragment, up to the first line that doesn't
/// end in a backslash.
fn mk_sources(list: &Path, var: &str) -> Vec<String> {
    let text = fs::read_to_string(list).unwrap_or_else(|e| panic!("{}: {e}", list.display()));
    let mut files = Vec::new();
    let mut inside = false;
    for line in text.lines() {
        let rest = if inside {
            line
        } else if let Some(rest) = line
            .strip_prefix(var)
            .and_then(|rest| rest.trim_start().strip_prefix('='))
        {
            inside = true;
            rest
        } else {
            continue;
        };
        let continues = rest.trim_end().ends_with('\\');
        files.extend(
            rest.trim_end()
                .trim_end_matches('\\')
                .split_whitespace()
                .filter(|word| word.ends_with(".c"))
                .map(str::to_owned),
        );
        if !continues {
            inside = false;
        }
    }
    assert!(!files.is_empty(), "{var} not found in {}", list.display());
    files
}
