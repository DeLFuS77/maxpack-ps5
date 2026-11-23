//! Installed games of the console (Sony's own installs, and FPKGs installed through the system):
//! MaxPack never registers, mounts or uninstalls a title id one of them uses (design: never touch
//! the installed games).

/// What a look (`lstat`) at a path says.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Look {
    /// ENOENT or ENOTDIR: nothing there.
    Absent,
    /// Something is there.
    Present,
    /// Any other error (a drive that answers badly): it may be there.
    Unknown,
}

/// The paths that show a real install of title `tid`: `/user/app/<TID>/app.pkg` (MaxPack and the
/// other mounters only stage `sce_sys` there) and the copy on an M.2 or USB extended drive,
/// `/mnt/ext0/user/app/<TID>` and `/mnt/ext1/user/app/<TID>`.
pub fn install_paths(tid: &str) -> [String; 3] {
    [format!("/user/app/{tid}/app.pkg"), format!("/mnt/ext0/user/app/{tid}"), format!("/mnt/ext1/user/app/{tid}")]
}

/// The first of `install_paths` that shows an installed copy: present, or unknown (an error other
/// than ENOENT/ENOTDIR protects). None when every one is absent.
pub fn installed_copy(tid: &str, mut look: impl FnMut(&str) -> Look) -> Option<String> {
    install_paths(tid).into_iter().find(|p| look(p) != Look::Absent)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn tree(entries: &[(&str, Look)]) -> impl FnMut(&str) -> Look {
        let m: HashMap<String, Look> = entries.iter().map(|(p, l)| (p.to_string(), *l)).collect();
        move |p: &str| *m.get(p).unwrap_or(&Look::Absent)
    }

    #[test]
    fn install_paths_of_a_title() {
        assert_eq!(install_paths("PPSA01234"), [
            "/user/app/PPSA01234/app.pkg".to_string(),
            "/mnt/ext0/user/app/PPSA01234".to_string(),
            "/mnt/ext1/user/app/PPSA01234".to_string(),
        ]);
    }

    #[test]
    fn an_installed_copy_protects_the_title() {
        let t = "PPSA01234";
        // Nothing installed: a staged sce_sys of a mounter is not an install.
        assert_eq!(installed_copy(t, tree(&[("/user/app/PPSA01234/sce_sys/param.json", Look::Present)])), None);
        assert_eq!(installed_copy(t, tree(&[])), None);
        // Installed on the internal SSD, on the M.2 drive, on a USB extended drive.
        for p in ["/user/app/PPSA01234/app.pkg", "/mnt/ext0/user/app/PPSA01234", "/mnt/ext1/user/app/PPSA01234"] {
            assert_eq!(installed_copy(t, tree(&[(p, Look::Present)])).as_deref(), Some(p), "{p}");
            // An error other than ENOENT/ENOTDIR protects too.
            assert_eq!(installed_copy(t, tree(&[(p, Look::Unknown)])).as_deref(), Some(p), "{p}");
        }
        // The first one that shows it is named; another title's install does not count.
        let both = [("/mnt/ext1/user/app/PPSA01234", Look::Present), ("/user/app/PPSA01234/app.pkg", Look::Unknown)];
        assert_eq!(installed_copy(t, tree(&both)).as_deref(), Some("/user/app/PPSA01234/app.pkg"));
        assert_eq!(installed_copy(t, tree(&[("/user/app/PPSA01235/app.pkg", Look::Present)])), None);
    }

    #[test]
    fn every_path_is_looked_at_in_order() {
        let mut seen = Vec::new();
        assert_eq!(installed_copy("CUSA00001", |p: &str| { seen.push(p.to_string()); Look::Absent }), None);
        assert_eq!(seen, install_paths("CUSA00001").to_vec());
    }
}
