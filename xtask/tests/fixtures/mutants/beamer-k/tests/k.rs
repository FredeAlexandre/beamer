#[test]
fn doubles() {
    assert_eq!(beamer_k::double(3), 6);
}

#[test]
fn doubles_again() {
    assert_eq!(beamer_k::double(4), 8);
}

#[test]
fn big() {
    assert!(beamer_k::is_big(11));
    assert!(!beamer_k::is_big(10));
}

#[test]
fn guards_nothing() {
    let _ = beamer_k::double(1);
}
