//! Game crashes. When an app crashes, the PS5 writes `/user/devlog/system/sce_coredumps.0/<TID>_<n>/`
//! (the dump `prosperocore-<n>-0x<pid>-<process>.prosperodmp` inside) and deletes it seconds later.
//! The launch watcher looks there when a game it mounted exits (and once more when that session's
//! cleanup ends), matching the dump by pid; the last crash of each title is kept in FILE, so it is
//! still there after a payload restart or a reboot. See DESIGN.md "Crash report".
use std::collections::{BTreeMap, HashMap};
use std::path::Path;

/// The console's crash folder.
pub const DIR: &str = "/user/devlog/system/sce_coredumps.0";
/// Not in the game's cache: for a pkg or a .p5z the cache is /app0, so the game would see it.
pub const FILE: &str = "/data/maxpack/crashes.json";

#[derive(Clone, Debug, PartialEq)]
pub struct Crash {
    /// Unix seconds (the console's clock, which may be years behind).
    pub at: u64,
    /// From the dump's file name ("eboot.bin").
    pub process: String,
    /// The console's crash folder name ("PPSA22327_1795166239").
    pub folder: String,
    /// The exit status from NOTE_EXIT ("killed by signal 11").
    pub status: String,
}

/// "PPSA22327_1795166239" -> "PPSA22327".
pub fn folder_tid(name: &str) -> Option<&str> {
    let (tid, n) = name.split_once('_')?;
    let digits = !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit());
    (digits && crate::titles::valid_tid(tid)).then_some(tid)
}

/// "prosperocore-1795166239-0x000000e9-eboot.bin.prosperodmp" -> (0xe9, "eboot.bin").
pub fn dump_pid_process(file: &str) -> Option<(i32, &str)> {
    let rest = file.strip_prefix("prosperocore-")?.strip_suffix(".prosperodmp")?;
    let mut parts = rest.splitn(3, '-');
    let (_, pid, process) = (parts.next()?, parts.next()?, parts.next()?);
    let pid = i32::from_str_radix(pid.strip_prefix("0x")?, 16).ok()?;
    (!process.is_empty()).then_some((pid, process))
}

/// The NOTE_EXIT data, a wait(2) status: "exited with 0", "killed by signal 11".
pub fn exit_status(status: i32) -> String {
    match status & 0x7f {
        0 => format!("exited with {}", (status >> 8) & 0xff),
        0x7f => format!("status {status:#x}"),
        sig => format!("killed by signal {sig}"),
    }
}

/// The dump of `pid` among the `<tid>_*` folders in `dir`: (folder, process).
pub fn find_in(dir: &Path, tid: &str, pid: i32) -> Option<(String, String)> {
    for e in std::fs::read_dir(dir).ok()?.flatten() {
        let folder = e.file_name().to_string_lossy().into_owned();
        if folder_tid(&folder) != Some(tid) {
            continue;
        }
        let Ok(files) = std::fs::read_dir(e.path()) else { continue };
        for f in files.flatten() {
            let name = f.file_name().to_string_lossy().into_owned();
            if let Some((p, process)) = dump_pid_process(&name) {
                if p == pid {
                    return Some((folder, process.to_string()));
                }
            }
        }
    }
    None
}

/// The crashes of FILE's text: {"<TID>": {at, process, folder, status}}. Not JSON, not an object,
/// or a key that is not a title id: left out (a bad file is an empty list, never an error).
pub fn parse(text: &str) -> HashMap<String, Crash> {
    let Ok(serde_json::Value::Object(map)) = serde_json::from_str(text) else { return HashMap::new() };
    let s = |v: &serde_json::Value, k: &str| v[k].as_str().unwrap_or("").to_string();
    map.into_iter()
        .filter(|(tid, _)| crate::titles::valid_tid(tid))
        .map(|(tid, v)| {
            let c = Crash { at: v["at"].as_u64().unwrap_or(0), process: s(&v, "process"), folder: s(&v, "folder"), status: s(&v, "status") };
            (tid, c)
        })
        .collect()
}

/// FILE's text, keys sorted.
pub fn to_json(crashes: &HashMap<String, Crash>) -> String {
    let sorted: BTreeMap<&String, serde_json::Value> = crashes
        .iter()
        .map(|(tid, c)| (tid, serde_json::json!({"at": c.at, "process": c.process, "folder": c.folder, "status": c.status})))
        .collect();
    serde_json::to_string(&sorted).unwrap_or_else(|_| "{}".into())
}

pub fn load_from(path: &Path) -> HashMap<String, Crash> {
    std::fs::read_to_string(path).map(|t| parse(&t)).unwrap_or_default()
}

/// Temp file + rename: a crash while writing never leaves a half file.
pub fn save_to(path: &Path, crashes: &HashMap<String, Crash>) -> std::io::Result<()> {
    let tmp = path.with_extension("json.tmp");
    std::fs::write(&tmp, to_json(crashes))?;
    std::fs::rename(&tmp, path)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn scratch(name: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("maxpack-crash-{name}-{}", std::process::id()));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn crash(at: u64) -> Crash {
        Crash { at, process: "eboot.bin".into(), folder: "PPSA10001_1795166239".into(), status: "killed by signal 11".into() }
    }

    #[test]
    fn crash_folder_names() {
        assert_eq!(folder_tid("PPSA22327_1795166239"), Some("PPSA22327"));
        assert_eq!(folder_tid("CUSA00001_1"), Some("CUSA00001"));
        for bad in ["PPSA22327", "PPSA22327_", "PPSA22327_12a", "ppsa22327_1", "NPXS20001_5", "PPSA2232_1", "_1"] {
            assert_eq!(folder_tid(bad), None, "{bad}");
        }
    }

    #[test]
    fn dump_file_names() {
        assert_eq!(dump_pid_process("prosperocore-1795166239-0x000000e9-eboot.bin.prosperodmp"), Some((0xe9, "eboot.bin")));
        // A process name with dashes keeps them.
        assert_eq!(dump_pid_process("prosperocore-1-0x10-my-tool.elf.prosperodmp"), Some((0x10, "my-tool.elf")));
        for bad in [
            "prosperocore-1-0x10-.prosperodmp", "prosperocore-1-10-eboot.bin.prosperodmp", "prosperocore-1-0xzz-eboot.bin.prosperodmp",
            "core-1-0x10-eboot.bin.prosperodmp", "prosperocore-1-0x10-eboot.bin", "prosperocore-1.prosperodmp",
        ] {
            assert_eq!(dump_pid_process(bad), None, "{bad}");
        }
    }

    #[test]
    fn exit_statuses() {
        assert_eq!(exit_status(0), "exited with 0");
        assert_eq!(exit_status(3 << 8), "exited with 3");
        assert_eq!(exit_status(11), "killed by signal 11");
        // The core-dumped bit (0x80) does not change the signal.
        assert_eq!(exit_status(0x80 | 6), "killed by signal 6");
        assert_eq!(exit_status(0x137f), "status 0x137f");
    }

    #[test]
    fn the_dump_of_the_exiting_pid_is_found() {
        let d = scratch("find");
        let mk = |folder: &str, file: &str| {
            std::fs::create_dir_all(d.join(folder)).unwrap();
            std::fs::write(d.join(folder).join(file), b"x").unwrap();
        };
        mk("PPSA10001_100", "prosperocore-100-0x000000e9-eboot.bin.prosperodmp");
        mk("PPSA10001_200", "prosperocore-200-0x000000ea-launcher.elf.prosperodmp");
        mk("PPSA99999_300", "prosperocore-300-0x000000eb-eboot.bin.prosperodmp");
        std::fs::write(d.join("PPSA10001_400"), b"not a folder").unwrap();
        assert_eq!(find_in(&d, "PPSA10001", 0xe9), Some(("PPSA10001_100".into(), "eboot.bin".into())));
        assert_eq!(find_in(&d, "PPSA10001", 0xea), Some(("PPSA10001_200".into(), "launcher.elf".into())));
        // Another title's dump, another pid, a missing folder: nothing.
        assert_eq!(find_in(&d, "PPSA10001", 0xeb), None);
        assert_eq!(find_in(&d, "PPSA10001", 0x1), None);
        assert_eq!(find_in(&d.join("gone"), "PPSA10001", 0xe9), None);
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn the_crash_file_round_trips() {
        let d = scratch("file");
        let path = d.join("crashes.json");
        assert!(load_from(&path).is_empty());
        let mut all = HashMap::new();
        all.insert("PPSA10001".to_string(), crash(1_767_225_600));
        all.insert("CUSA00001".to_string(), Crash { process: "my \"tool\".elf".into(), ..crash(5) });
        save_to(&path, &all).unwrap();
        assert_eq!(load_from(&path), all);
        // No temp file is left beside it.
        let names: Vec<String> = std::fs::read_dir(&d).unwrap().flatten().map(|e| e.file_name().to_string_lossy().into_owned()).collect();
        assert_eq!(names, ["crashes.json"]);
        // Keys sorted: the same crashes always give the same text.
        let text = to_json(&all);
        assert!(text.find("CUSA00001").unwrap() < text.find("PPSA10001").unwrap(), "{text}");
        let _ = std::fs::remove_dir_all(&d);
    }

    #[test]
    fn a_bad_crash_file_is_an_empty_list() {
        assert!(parse("").is_empty());
        assert!(parse("not json").is_empty());
        assert!(parse("[1, 2]").is_empty());
        // A key that is not a title id is left out; missing fields read as empty.
        let m = parse(r#"{"../x": {"at": 1}, "PPSA10001": {"at": 7, "process": "eboot.bin"}}"#);
        assert_eq!(m.len(), 1);
        assert_eq!(m["PPSA10001"], Crash { at: 7, process: "eboot.bin".into(), folder: String::new(), status: String::new() });
    }
}
