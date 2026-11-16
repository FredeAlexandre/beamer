//! A crate that cites the rules it serves, as the skeleton does. tracey reads its comments; cargo
//! never builds it.

/// A zone's peer.
// r[impl example.zone.leads-to-peer]
pub fn zone_peer() -> u32 {
    todo!()
}

/// Crosses once push pressure passes the threshold.
// r[impl example.zone.push+2]
pub fn push(pressure: u32) -> bool {
    todo!()
}
