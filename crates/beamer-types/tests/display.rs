//! A deliberately weak spec test: it never tries a square display, so `>` mutated to `>=`, or the
//! body replaced by `true`, still passes.

// r[verify types.display.landscape]
#[test]
fn a_wide_display_is_landscape() {
    assert!(
        beamer_types::is_landscape(1920, 1080),
        "types.display.landscape"
    );
}
