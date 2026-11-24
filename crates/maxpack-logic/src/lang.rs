//! The console's UI language as a toast language: the 11 the notices are written in, English for
//! any other (and when the console's setting can't be read).

/// A toast language, by the console's `SceSystemParamLang` code.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Lang {
    En,
    Ru,
    Zh,
    De,
    Es,
    Fr,
    Ja,
    Pt,
    It,
    Ko,
    Ar,
}

impl Lang {
    /// `SCE_SYSTEM_PARAM_LANG_*`: both Englishes, Spanishes, Frenches, Portugueses and Chineses
    /// map to the one we have (Chinese: Simplified, Portuguese: Brazil); any other is English.
    pub fn from_console(code: i32) -> Self {
        match code {
            0 => Self::Ja,
            2 | 22 => Self::Fr,
            3 | 20 => Self::Es,
            4 => Self::De,
            5 => Self::It,
            7 | 17 => Self::Pt,
            8 => Self::Ru,
            9 => Self::Ko,
            10 | 11 => Self::Zh,
            21 => Self::Ar,
            _ => Self::En,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::Lang;

    #[test]
    fn each_supported_console_language_maps_to_its_own() {
        // SCE_SYSTEM_PARAM_LANG_*: JAPANESE 0, ENGLISH_US 1, FRENCH 2, SPANISH 3, GERMAN 4,
        // ITALIAN 5, PORTUGUESE_PT 7, RUSSIAN 8, KOREAN 9, CHINESE_TRADITIONAL 10,
        // CHINESE_SIMPLIFIED 11, PORTUGUESE_BR 17, ENGLISH_GB 18, SPANISH_LA 20, ARABIC 21,
        // FRENCH_CA 22.
        let cases = [
            (0, Lang::Ja),
            (1, Lang::En),
            (2, Lang::Fr),
            (3, Lang::Es),
            (4, Lang::De),
            (5, Lang::It),
            (7, Lang::Pt),
            (8, Lang::Ru),
            (9, Lang::Ko),
            (10, Lang::Zh),
            (11, Lang::Zh),
            (17, Lang::Pt),
            (18, Lang::En),
            (20, Lang::Es),
            (21, Lang::Ar),
            (22, Lang::Fr),
        ];
        for (code, want) in cases {
            assert_eq!(Lang::from_console(code), want, "code {code}");
        }
    }

    #[test]
    fn a_language_without_translation_is_english() {
        // Dutch 6, Finnish 12, Swedish 13, Danish 14, Norwegian 15, Polish 16, Turkish 19,
        // Czech 23 ... Ukrainian 30, and values that are not a language at all.
        for code in [6, 12, 13, 14, 15, 16, 19, 23, 24, 25, 26, 27, 28, 29, 30, 31, 100, -1, i32::MIN, i32::MAX] {
            assert_eq!(Lang::from_console(code), Lang::En, "code {code}");
        }
    }
}
