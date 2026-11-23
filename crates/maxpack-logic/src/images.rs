/// ShadowMountPlus image kind by file extension (case-insensitive). MaxPack does not mount these yet (stage 3).
pub fn smp_image_kind(file_name: &str) -> Option<&'static str> {
    let (stem, ext) = file_name.rsplit_once('.')?;
    if stem.is_empty() {
        return None;
    }
    match ext.to_ascii_lowercase().as_str() {
        "ffpkg" => Some("ffpkg"),
        "exfat" => Some("exfat"),
        "ffpfs" => Some("ffpfs"),
        "ffpfsc" => Some("ffpfsc"),
        _ => None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn smp_image_kind_by_extension() {
        assert_eq!(smp_image_kind("Juego.FFPKG"), Some("ffpkg"));
        assert_eq!(smp_image_kind("x.exfat"), Some("exfat"));
        assert_eq!(smp_image_kind("x.ffpfs"), Some("ffpfs"));
        assert_eq!(smp_image_kind("x.ffpfsc"), Some("ffpfsc"));
        assert_eq!(smp_image_kind("a.b.exfat"), Some("exfat"));
        assert_eq!(smp_image_kind("x.pkg"), None);
        assert_eq!(smp_image_kind("x.p5z"), None);
        assert_eq!(smp_image_kind("ffpkg"), None);
        assert_eq!(smp_image_kind(".ffpkg.txt"), None);
    }
}
