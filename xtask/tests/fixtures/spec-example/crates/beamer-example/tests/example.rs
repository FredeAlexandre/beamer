//! Spec tests: each cites the rules it verifies, at their current version.

// r[verify example.zone.leads-to-peer]
#[test]
fn a_zone_leads_to_one_peer() {
    assert_eq!(beamer_example::zone_peer(), 1, "example.zone.leads-to-peer");
}

// r[verify example.zone.push+2]
#[test]
fn a_push_below_the_threshold_does_not_cross() {
    assert!(!beamer_example::push(0), "example.zone.push");
}

// r[verify example.driving.move-number+3]
#[test]
fn the_driving_model_matches_the_spec() {
    // A proptest-state-machine test, one per model, citing every rule marked `model: driving`.
}

// r[verify example.frame.decode]
#[test]
fn take_frames_fuzz() {
    // A bolero harness, named for the `fuzz:` marker.
}
