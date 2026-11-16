//! Builds the vendored speexdsp 1.2.1 with `cc`, as the Python app's engine did: float, with
//! kiss FFT and no `config.h`. No CMake, no configure (#16). `cc` finds clang on macOS, MSVC
//! through `vswhere` on Windows, and the system C compiler on Linux.

use std::path::PathBuf;

/// The release the vendored tree is.
const VERSION: &str = "1.2.1";

/// The echo canceller, its preprocessor, the FFT they share, and the resampler.
const SOURCES: &[&str] = &[
    "mdf.c",
    "preprocess.c",
    "filterbank.c",
    "fftwrap.c",
    "kiss_fft.c",
    "kiss_fftr.c",
    "resample.c",
];

fn main() {
    let crate_dir = PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("set by cargo"));
    let root = crate_dir.join(format!("speexdsp-{VERSION}"));
    let config = crate_dir.join("config");
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed={}", root.display());
    println!("cargo:rerun-if-changed={}", config.display());

    let mut build = cc::Build::new();
    build
        .files(
            SOURCES
                .iter()
                .map(|file| root.join("libspeexdsp").join(file)),
        )
        .include(root.join("include"))
        .include(&config)
        .define("FLOATING_POINT", None)
        .define("USE_KISS_FFT", None)
        .define("EXPORT", "")
        // The echo canceller runs in real time, in debug builds too.
        .opt_level(2)
        // Vendored code, compiled unmodified: its warnings are not ours to fix.
        .warnings(false);
    if build.get_compiler().is_like_msvc() {
        build.define("_CRT_SECURE_NO_WARNINGS", None);
    }
    build.compile("speexdsp");
}
