// PROTOTYPE: compile the .slint UI into Rust at build time.
fn main() {
    let config = slint_build::CompilerConfiguration::new().with_edition((1, 19));
    slint_build::compile_with_config("ui/app.slint", config).unwrap();
}
