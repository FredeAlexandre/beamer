//! The key table names physical key positions, and gives each its native code on both OSes.

use machine_agent::Key;
use machine_agent::keys::KEYS;

/// The PC code is the set-1 scan code, with 0xE0 in the high byte for an extended key: Windows
/// reads a scan code by position, so `KeyQ` types `a` on the PC's French AZERTY layout.
#[test]
fn each_key_has_the_pc_scan_code_of_its_position() {
    for (name, scan) in [
        ("KeyQ", 0x10),
        ("KeyA", 0x1E),
        ("Digit1", 0x02),
        ("Digit0", 0x0B),
        ("IntlBackslash", 0x56),
        ("Enter", 0x1C),
        ("ShiftLeft", 0x2A),
        ("ShiftRight", 0x36),
        ("ControlRight", 0xE01D),
        ("AltRight", 0xE038),
        ("MetaLeft", 0xE05B),
        ("Delete", 0xE053),
        ("ArrowLeft", 0xE04B),
        ("PageDown", 0xE051),
        ("F11", 0x57),
        ("F13", 0x64),
        ("F19", 0x6A),
        ("F20", 0x6B),
    ] {
        assert_eq!(Key::named(name).unwrap().pc, scan, "{name}");
    }
}

#[test]
fn no_two_keys_share_a_native_code_on_either_os() {
    for (i, a) in KEYS.iter().enumerate() {
        for b in &KEYS[i + 1..] {
            assert_ne!(a.mac, b.mac, "{} and {} on the Mac", a.code, b.code);
            assert_ne!(a.pc, b.pc, "{} and {} on the PC", a.code, b.code);
        }
    }
}
