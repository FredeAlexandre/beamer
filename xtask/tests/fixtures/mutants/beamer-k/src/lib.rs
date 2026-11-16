pub fn double(x: u32) -> u32 {
    x * 2
}

pub fn is_big(x: u32) -> bool {
    x > 10
}

pub fn unused(x: u32) -> u32 {
    x + 1
}

#[cfg(test)]
mod tests {
    #[test]
    fn unit() {
        assert_eq!(super::unused(1), 2);
    }
}
