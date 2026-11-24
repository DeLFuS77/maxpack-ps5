//! Settings from /data/maxpack/config.toml (DESIGN.md "Settings").
//! ponytail: a flat TOML subset (key = string | bool | integer | array of strings, # comments),
//! no tables; a toml crate when the file needs more.
use serde_json::{json, Value};
pub const CONFIG_PATH: &str = "/data/maxpack/config.toml";

#[derive(Clone, PartialEq, Debug)]
pub struct Settings {
    pub scan_paths: Vec<String>,
    pub inject_ampr: bool,
    pub add_emulators: bool,
    /// Title ids that get no bundled PlayGo stub (pgo_stub): not added, not upgraded.
    pub playgo_off: Vec<String>,
    /// Title ids the page's library does not show unless "show hidden" is on. Page only: a hidden
    /// game stays built, registered and mounted.
    pub hidden: Vec<String>,
    pub ampr_auto_update: bool,
    pub backports: bool,
    pub scan_interval_secs: u64,
    /// A source changed less than this many seconds ago is still being copied (0 = off).
    pub stability_wait_secs: u64,
    pub low_space_gb: u64,
    /// SMP auto_remove_missing_games: unregister a title whose source is gone this long.
    pub auto_remove_missing: bool,
    pub auto_remove_delay_secs: u64,
    pub notify: bool,
    /// Write /data/maxpack/maxpack.log (the payload's stdout always gets the lines).
    pub log: bool,
    pub debug: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            scan_paths: Vec::new(),
            inject_ampr: true,
            add_emulators: true,
            playgo_off: Vec::new(),
            hidden: Vec::new(),
            ampr_auto_update: false,
            backports: true,
            scan_interval_secs: 10,
            stability_wait_secs: 10,
            low_space_gb: 20,
            auto_remove_missing: true,
            auto_remove_delay_secs: 300,
            notify: true,
            log: true,
            debug: false,
        }
    }
}


const HELP: &[(&str, &str)] = &[
    ("scanpath", "replaces the default scan roots (/data/homebrew, /data/etaHEN/games, <drive>/homebrew, ...)"),
    ("inject_ampr", "true = copy /data/maxpack/ampr/* on top of a folder game's fakelib (pkg games always get it; a change only refreshes fakelib/)"),
    ("add_emulators", "true = add the bundled DLC (dlc_emu) and PlayGo (pgo_stub) emulators to a game's fakelib when it needs them and ships none (a change only refreshes fakelib/)"),
    ("playgo_off", "title ids that get no bundled PlayGo stub (pgo_stub): not added, not upgraded; a libScePlayGo.sprx the game ships stays (a change only refreshes fakelib/)"),
    ("hidden", "title ids the web UI's library does not show unless its \"show hidden\" switch is on (set from the game's sheet; the game stays registered and mounted)"),
    ("ampr_auto_update", "true = once per start, download a newer ampr-emu-ng release from MaxPack's own repository (DeLFuS77/ampr-emu-ng); off by default until MaxPack is published, and it only ever downloads MaxPack's own build (a hand-placed libSceAmpr.sprx is kept)"),
    ("backports", "true = apply backports/<TID>* from the game's scan root, then the other roots, then /data/homebrew"),
    ("scan_interval_seconds", "scan loop period, 1..3600"),
    ("stability_wait_seconds", "skip a source changed less than this many seconds ago (still being copied), 0..3600, 0 = off"),
    ("low_space_gb", "warn when /data free space is below this"),
    ("auto_remove_missing", "true = remove a title from the home screen when its source is missing for auto_remove_delay_seconds (the cache stays; not while a game runs, not with DLC in /user/addcont/<TID>)"),
    ("auto_remove_delay_seconds", "how long a source must stay missing before auto_remove_missing removes its title, 1..86400"),
    ("notify", "true = system toasts"),
    ("log", "true = write /data/maxpack/maxpack.log (starts over past 1 MiB)"),
    ("debug", "true = klog capture to /data/maxpack/klog.txt and the /file/data/... route"),
];

fn flag(v: &str) -> Result<bool, String> {
    match v {
        "1" | "true" => Ok(true),
        "0" | "false" => Ok(false),
        _ => Err(format!("expected 0 or 1, got {v:?}")),
    }
}

fn num(v: &str) -> Result<u64, String> {
    v.parse().map_err(|_| format!("expected a number, got {v:?}"))
}

/// Applies one key=value. Unknown keys are an error too.
fn set(s: &mut Settings, key: &str, v: &str) -> Result<(), String> {
    match key {
        "scanpath" if v.starts_with('/') => {
            // A repeated root would list every game in it again as its own duplicate.
            let v = v.trim_end_matches('/');
            let v = if v.is_empty() { "/" } else { v };
            if !s.scan_paths.iter().any(|p| p == v) {
                s.scan_paths.push(v.to_string());
            }
        }
        "scanpath" => return Err(format!("expected an absolute path, got {v:?}")),
        "inject_ampr" => s.inject_ampr = flag(v)?,
        "add_emulators" => s.add_emulators = flag(v)?,
        "playgo_off" if crate::scan::valid_tid(v) => {
            if !s.playgo_off.iter().any(|t| t == v) {
                s.playgo_off.push(v.to_string());
            }
        }
        "playgo_off" => return Err(format!("expected a title id like PPSA01556, got {v:?}")),
        "hidden" if crate::scan::valid_tid(v) => {
            maxpack_logic::titles::set_listed(&mut s.hidden, v, true);
        }
        "hidden" => return Err(format!("expected a title id like PPSA01556, got {v:?}")),
        "ampr_auto_update" => s.ampr_auto_update = flag(v)?,
        // Removed setting (the game's fakelib is always used in full): ignored, dropped on the next save.
        "game_fakelib" => {}
        "backports" => s.backports = flag(v)?,
        "scan_interval_seconds" => match num(v)? {
            n @ 1..=3600 => s.scan_interval_secs = n,
            n => return Err(format!("{n} not in 1..3600")),
        },
        "stability_wait_seconds" => match num(v)? {
            n @ 0..=3600 => s.stability_wait_secs = n,
            n => return Err(format!("{n} not in 0..3600")),
        },
        "low_space_gb" => s.low_space_gb = num(v)?,
        "auto_remove_missing" => s.auto_remove_missing = flag(v)?,
        "auto_remove_delay_seconds" => match num(v)? {
            n @ 1..=86400 => s.auto_remove_delay_secs = n,
            n => return Err(format!("{n} not in 1..86400")),
        },
        "notify" => s.notify = flag(v)?,
        "log" => s.log = flag(v)?,
        "debug" => s.debug = flag(v)?,
        _ => return Err("unknown key".into()),
    }
    Ok(())
}

/// A TOML string at the start of `v` ("basic" with \\ \" \n \t escapes, or 'literal') and the rest.
fn toml_str(v: &str) -> Result<(String, &str), String> {
    if let Some(r) = v.strip_prefix('\'') {
        let end = r.find('\'').ok_or("unclosed string")?;
        return Ok((r[..end].to_string(), &r[end + 1..]));
    }
    let r = v.strip_prefix('"').ok_or_else(|| format!("expected a string, got {v:?}"))?;
    let mut out = String::new();
    let mut chars = r.char_indices();
    while let Some((i, c)) = chars.next() {
        match c {
            '"' => return Ok((out, &r[i + 1..])),
            '\\' => match chars.next().map(|(_, e)| e) {
                Some('"') => out.push('"'),
                Some('\\') => out.push('\\'),
                Some('n') => out.push('\n'),
                Some('t') => out.push('\t'),
                e => return Err(format!("unsupported escape \\{}", e.unwrap_or(' '))),
            },
            c => out.push(c),
        }
    }
    Err("unclosed string".into())
}

/// `v` without a trailing `# comment` outside strings.
fn strip_comment(v: &str) -> &str {
    let (mut quote, mut esc) = (None, false);
    for (i, c) in v.char_indices() {
        if esc {
            esc = false;
            continue;
        }
        match (quote, c) {
            (None, '#') => return &v[..i],
            (None, '"' | '\'') => quote = Some(c),
            (Some('"'), '\\') => esc = true,
            (Some(q), c) if c == q => quote = None,
            _ => {}
        }
    }
    v
}

/// A TOML value as the texts `set` takes: bool -> "1"/"0", integer, string, or one text per array item.
fn toml_value(v: &str) -> Result<Vec<String>, String> {
    let v = strip_comment(v).trim();
    if let Some(inner) = v.strip_prefix('[') {
        let mut rest = inner.strip_suffix(']').ok_or("unclosed array")?.trim();
        let mut out = Vec::new();
        while !rest.is_empty() {
            let (item, r) = toml_str(rest)?;
            out.push(item);
            rest = r.trim_start();
            rest = match rest.strip_prefix(',') {
                Some(r) => r.trim_start(),
                None if rest.is_empty() => rest,
                None => return Err(format!("expected , in the array, got {rest:?}")),
            };
        }
        return Ok(out);
    }
    if v.starts_with('"') || v.starts_with('\'') {
        let (text, rest) = toml_str(v)?;
        if !rest.trim().is_empty() {
            return Err(format!("unexpected {:?} after the string", rest.trim()));
        }
        return Ok(vec![text]);
    }
    match v {
        "true" => Ok(vec!["1".into()]),
        "false" => Ok(vec!["0".into()]),
        n if !n.is_empty() && n.bytes().all(|b| b.is_ascii_digit() || b == b'_') => Ok(vec![n.replace('_', "")]),
        _ => Err(format!("expected a string, true/false, a number or an array, got {v:?}")),
    }
}

fn toml_quote(v: &str) -> String {
    format!("\"{}\"", v.replace('\\', "\\\\").replace('"', "\\\""))
}

/// Config text of the last parse; warnings are logged only when the text changed, since
/// load() runs every scan loop.
static LAST_TEXT: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());

fn parse(text: &str) -> Settings {
    let quiet = {
        let mut last = LAST_TEXT.lock().unwrap_or_else(|e| e.into_inner());
        let same = *last == text;
        if !same {
            *last = text.to_owned();
        }
        same
    };
    let mut s = Settings::default();
    let mut lines = text.lines().enumerate();
    while let Some((n, line)) = lines.next() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        let Some((k, v)) = line.split_once('=') else {
            if !quiet {
                log!("config {CONFIG_PATH}:{}: not key = value: {line:?}", n + 1);
            }
            continue;
        };
        let (k, mut v) = (k.trim(), v.trim().to_string());
        // A multi-line array: join lines up to the closing ].
        if v.starts_with('[') {
            while !strip_comment(&v).trim_end().ends_with(']') {
                let Some((_, more)) = lines.next() else { break };
                v = format!("{} {}", strip_comment(&v).trim_end(), more.trim());
            }
        }
        let r = toml_value(&v).and_then(|vals| {
            if k == "scanpath" {
                s.scan_paths.clear(); // the array replaces the defaults (and an earlier scanpath line)
            } else if k == "playgo_off" {
                s.playgo_off.clear(); // a later playgo_off line replaces an earlier one
            } else if k == "hidden" {
                s.hidden.clear(); // the same for hidden
            } else if vals.len() != 1 {
                return Err("expected one value, not an array".into());
            } else {
                return set(&mut s, k, &vals[0]);
            }
            // An array item that is wrong is logged and skipped; the others stay, so the next save
            // does not drop them.
            for x in &vals {
                if let (Err(e), false) = (set(&mut s, k, x), quiet) {
                    log!("config {CONFIG_PATH}:{}: {k}: {e}; skipped", n + 1);
                }
            }
            Ok(())
        });
        if let (Err(e), false) = (r, quiet) {
            log!("config {CONFIG_PATH}:{}: {k}: {e}; using the default", n + 1);
        }
    }
    s
}

fn render(s: &Settings) -> String {
    let mut out = String::from("# MaxPack settings (TOML). The web UI edits this file.\n");
    for &(k, help) in HELP {
        out += &format!("\n# {help}\n");
        match k {
            // ponytail: no scanpath = default roots, shown as an example only.
            "scanpath" if s.scan_paths.is_empty() => out += "# scanpath = [\"/data/homebrew\"]\n",
            "scanpath" => {
                let items: Vec<String> = s.scan_paths.iter().map(|p| toml_quote(p)).collect();
                out += &format!("scanpath = [{}]\n", items.join(", "));
            }
            "inject_ampr" => out += &format!("{k} = {}\n", s.inject_ampr),
            "add_emulators" => out += &format!("{k} = {}\n", s.add_emulators),
            "playgo_off" => {
                let items: Vec<String> = s.playgo_off.iter().map(|t| toml_quote(t)).collect();
                out += &format!("playgo_off = [{}]\n", items.join(", "));
            }
            "hidden" => {
                let items: Vec<String> = s.hidden.iter().map(|t| toml_quote(t)).collect();
                out += &format!("hidden = [{}]\n", items.join(", "));
            }
            "ampr_auto_update" => out += &format!("{k} = {}\n", s.ampr_auto_update),
            "backports" => out += &format!("{k} = {}\n", s.backports),
            "scan_interval_seconds" => out += &format!("{k} = {}\n", s.scan_interval_secs),
            "stability_wait_seconds" => out += &format!("{k} = {}\n", s.stability_wait_secs),
            "low_space_gb" => out += &format!("{k} = {}\n", s.low_space_gb),
            "auto_remove_missing" => out += &format!("{k} = {}\n", s.auto_remove_missing),
            "auto_remove_delay_seconds" => out += &format!("{k} = {}\n", s.auto_remove_delay_secs),
            "notify" => out += &format!("{k} = {}\n", s.notify),
            "log" => out += &format!("{k} = {}\n", s.log),
            _ => out += &format!("{k} = {}\n", s.debug),
        }
    }
    out
}

/// Another mounter's settings file is never this large; a larger one is skipped.
const FOREIGN_MAX: u64 = 256 << 10;

/// The text of another mounter's settings file, read only: None when missing, not a regular file
/// (a FIFO would block), over FOREIGN_MAX, or unreadable (all but missing are logged). Bytes
/// that are not UTF-8 are replaced, so garbage just yields no root.
fn read_foreign(path: &str) -> Option<String> {
    use std::io::Read;
    let skip = |why: String| {
        log!("config: first start: {path}: {why}; skipped");
        None
    };
    match std::fs::metadata(path) {
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => return None,
        Err(e) => return skip(e.to_string()),
        Ok(m) if !m.is_file() => return skip("not a regular file".into()),
        Ok(m) if m.len() > FOREIGN_MAX => return skip(format!("{} bytes, over {FOREIGN_MAX}", m.len())),
        Ok(_) => {}
    }
    let mut b = Vec::new();
    match std::fs::File::open(path).and_then(|f| f.take(FOREIGN_MAX + 1).read_to_end(&mut b)) {
        Err(e) => skip(e.to_string()),
        // Grew since the stat.
        Ok(n) if n as u64 > FOREIGN_MAX => skip(format!("over {FOREIGN_MAX} bytes")),
        Ok(_) => Some(String::from_utf8_lossy(&b).into_owned()),
    }
}

/// The settings of a first start: the defaults, plus the scan roots added by hand in the other
/// mounters (maxpack_logic::roots), whose files are only read. Without such roots there is no
/// scanpath line, as before; with them, scanpath lists every usual root first.
fn first_settings() -> Settings {
    use maxpack_logic::roots::{first_scan_paths, KM_CONFIG_TOML, SMP_CONFIG_INI, SMP_MANUAL_LST};
    let (km, ini, lst) = (read_foreign(KM_CONFIG_TOML), read_foreign(SMP_CONFIG_INI), read_foreign(SMP_MANUAL_LST));
    let usual = crate::storage::all_default_scan_roots();
    let mut s = Settings::default();
    match first_scan_paths(&usual, km.as_deref(), ini.as_deref(), lst.as_deref()) {
        Some(paths) => {
            let theirs: Vec<&String> = paths.iter().filter(|p| !usual.contains(p)).collect();
            log!("config: first start: imported {} scan root(s) from the other mounters: {theirs:?}; scanpath keeps the {} usual roots too",
                theirs.len(), usual.len());
            s.scan_paths = paths;
        }
        None => log!("config: first start: imported 0 scan roots from the other mounters; the usual roots are scanned"),
    }
    s
}

/// Reads the config; creates it when missing (first_settings).
pub fn load() -> Settings {
    match std::fs::read_to_string(CONFIG_PATH) {
        Ok(t) => parse(&t),
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            let s = first_settings();
            if let Err(e) = save(&s) {
                log!("config {CONFIG_PATH}: cannot create: {e}");
            }
            s
        }
        Err(e) => {
            log!("config {CONFIG_PATH}: cannot read: {e}; using the defaults");
            Settings::default()
        }
    }
}

/// Writes the config atomically (tmp + rename).
pub fn save(s: &Settings) -> std::io::Result<()> {
    let path = std::path::Path::new(CONFIG_PATH);
    if let Some(dir) = path.parent() {
        std::fs::create_dir_all(dir)?;
    }
    let tmp = format!("{CONFIG_PATH}.tmp");
    std::fs::write(&tmp, render(s))?;
    std::fs::rename(&tmp, path)
}

pub fn to_json(s: &Settings) -> Value {
    json!({
        "scanpath": s.scan_paths,
        "inject_ampr": s.inject_ampr,
        "add_emulators": s.add_emulators,
        "playgo_off": s.playgo_off,
        "hidden": s.hidden,
        "ampr_auto_update": s.ampr_auto_update,
        "backports": s.backports,
        "scan_interval_seconds": s.scan_interval_secs,
        "stability_wait_seconds": s.stability_wait_secs,
        "low_space_gb": s.low_space_gb,
        "auto_remove_missing": s.auto_remove_missing,
        "auto_remove_delay_seconds": s.auto_remove_delay_secs,
        "notify": s.notify,
        "log": s.log,
        "debug": s.debug,
    })
}

/// Missing keys take the default; bools may be true/false or 0/1.
pub fn from_json(v: &Value) -> Result<Settings, String> {
    let obj = v.as_object().ok_or("settings: expected a JSON object")?;
    let mut s = Settings::default();
    for (k, val) in obj {
        let vals: Vec<&Value> = match (k.as_str(), val) {
            ("scanpath" | "playgo_off" | "hidden", Value::Array(a)) => a.iter().collect(),
            ("scanpath" | "playgo_off" | "hidden", _) => return Err(format!("{k}: expected an array of strings")),
            _ => vec![val],
        };
        for x in vals {
            let text = match x {
                Value::String(t) => t.trim().to_string(),
                Value::Bool(b) => (if *b { "1" } else { "0" }).to_string(),
                Value::Number(n) => n.to_string(),
                _ => return Err(format!("{k}: unsupported value {x}")),
            };
            set(&mut s, k, &text).map_err(|e| format!("{k}: {e}"))?;
        }
    }
    Ok(s)
}
