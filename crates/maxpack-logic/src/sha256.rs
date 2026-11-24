//! SHA-256 (FIPS 180-4), for the self-update's check of `maxpack.elf` against the release's
//! `SHA256SUMS` (`update::verify`). Here, not from the engine in C, so the check is tested on the host.

/// The SHA-256 of `data`.
pub fn digest(data: &[u8]) -> [u8; 32] {
    let _ = data;
    [0; 32]
}

/// Lower-case hex, as `sha256sum` writes it.
pub fn hex(d: &[u8; 32]) -> String {
    let _ = d;
    String::new()
}

#[cfg(test)]
mod tests {
    use super::*;

    fn h(data: &[u8]) -> String {
        hex(&digest(data))
    }

    #[test]
    fn fips_vectors() {
        assert_eq!(h(b""), "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        assert_eq!(h(b"abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
        assert_eq!(
            h(b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
            "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
        );
        assert_eq!(h(&vec![b'a'; 1_000_000]), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }

    /// Around the padding's block edges (55/56 bytes leave or take a second block), from sha256sum.
    #[test]
    fn block_edges() {
        for (n, want) in [
            (55, "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"),
            (56, "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"),
            (63, "7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"),
            (64, "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"),
            (65, "635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"),
            (119, "31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"),
            (120, "2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"),
        ] {
            assert_eq!(h(&vec![b'a'; n]), want, "{n} bytes");
        }
    }
}
