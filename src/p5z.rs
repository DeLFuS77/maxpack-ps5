//! A .p5z game file played without installing it, through the MaxPack engine's `mp_play_*`
//! (vendor/maxpack src/pack/mp_play.h, linked by ps5/build.sh): open and check it, describe its
//! loose files and volumes (maxpack_logic::p5z::PlayInfo), extract one loose file.
use maxpack_logic::p5z::{parse_play_json, PlayInfo, Unplayable, MAX_VOLUMES};
use std::ffi::{c_char, c_int, CStr, CString};
use std::fs::File;
use std::io::{self, Error, ErrorKind};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::FileExt;
use std::path::Path;

/// `mp_play`, opaque.
#[repr(C)]
struct MpPlay {
    _private: [u8; 0],
}

extern "C" {
    // NULL with the reason in err (NUL-terminated, cut to errlen).
    fn mp_play_open(p5z: *const c_char, err: *mut c_char, errlen: usize) -> *mut MpPlay;
    // malloc'd JSON (mp_play_free_str frees it); NULL when out of memory.
    fn mp_play_json(p: *mut MpPlay) -> *mut c_char;
    // 0, or -1 with the reason in err (dest may be left half-written).
    fn mp_play_extract(p: *mut MpPlay, rel: *const c_char, dest: *const c_char, err: *mut c_char, errlen: usize) -> c_int;
    fn mp_play_close(p: *mut MpPlay);
    fn mp_play_free_str(s: *mut c_char);
}

const ERR_LEN: usize = 512;

/// The engine's handle, closed on drop. A raw pointer: neither Send nor Sync, as the engine wants
/// (each mp_play is single-threaded).
struct Handle(*mut MpPlay);

impl Drop for Handle {
    fn drop(&mut self) {
        // SAFETY: a handle mp_play_open returned, closed once.
        unsafe { mp_play_close(self.0) }
    }
}

/// An open .p5z.
pub struct Play {
    handle: Handle,
    pub info: PlayInfo,
}

pub enum OpenError {
    /// The file could not be opened (a USB drive waking up): try again later.
    Io(Error),
    /// The engine refused it, or its description: the same file always is.
    Unplayable(Unplayable),
}

/// A short English reason for the log and the page.
pub fn reason(u: &Unplayable) -> String {
    match u {
        Unplayable::Damaged(why) => why.clone(),
        Unplayable::TooManyVolumes(n) => format!("{n} volumes (at most {MAX_VOLUMES})"),
    }
}

impl From<OpenError> for Error {
    fn from(e: OpenError) -> Error {
        match e {
            OpenError::Io(e) => e,
            OpenError::Unplayable(u) => Error::new(ErrorKind::InvalidData, format!("cannot be played: {}", reason(&u))),
        }
    }
}

fn cpath(p: &Path) -> io::Result<CString> {
    CString::new(p.as_os_str().as_bytes()).map_err(|_| Error::new(ErrorKind::InvalidInput, "NUL in path"))
}

fn err_text(buf: &[c_char]) -> String {
    // SAFETY: c_char and u8 have the same size; the engine NUL-terminates within errlen.
    let bytes = unsafe { std::slice::from_raw_parts(buf.as_ptr().cast::<u8>(), buf.len()) };
    // Lossy: the engine's reasons are Spanish, and one cut at errlen inside a character keeps the
    // rest (U+FFFD for the cut character) instead of becoming "?".
    let msg = CStr::from_bytes_until_nul(bytes).map(|s| String::from_utf8_lossy(s.to_bytes()).into_owned()).unwrap_or_default();
    if msg.is_empty() { "no reason given".to_string() } else { msg }
}

/// Opens and checks `path` (header, footer, index, ampr_zstd.index, each volume's header) and reads
/// its description.
pub fn open(path: &Path) -> Result<Play, OpenError> {
    // The engine's NULL says nothing of why: a file that cannot even be opened is not "damaged".
    File::open(path).map_err(OpenError::Io)?;
    let c = cpath(path).map_err(OpenError::Io)?;
    let mut err = [0 as c_char; ERR_LEN];
    // SAFETY: a NUL-terminated path and an err buffer of err.len() bytes, both outliving the call.
    let ptr = unsafe { mp_play_open(c.as_ptr(), err.as_mut_ptr(), err.len()) };
    if ptr.is_null() {
        return Err(OpenError::Unplayable(Unplayable::Damaged(err_text(&err))));
    }
    let handle = Handle(ptr);
    // SAFETY: an open handle.
    let json = unsafe { mp_play_json(handle.0) };
    if json.is_null() {
        return Err(OpenError::Io(Error::new(ErrorKind::OutOfMemory, "mp_play_json: out of memory")));
    }
    // SAFETY: a NUL-terminated string the engine allocated; freed right after the copy.
    let text = unsafe { CStr::from_ptr(json) }.to_str().map(str::to_owned);
    unsafe { mp_play_free_str(json) };
    let text = text.map_err(|_| OpenError::Unplayable(Unplayable::Damaged("bad info: not UTF-8".into())))?;
    let info = parse_play_json(&text).map_err(OpenError::Unplayable)?;
    Ok(Play { handle, info })
}

impl Play {
    /// Writes loose file `rel` (as `info.loose` names it) to `dest`: parent dirs made, whatever is
    /// at `dest` replaced, its mtime set, its SHA-256 checked. A failure may leave `dest`
    /// half-written: the cache has no `.done` then, so it is rebuilt.
    pub fn extract(&self, rel: &str, dest: &Path) -> io::Result<()> {
        let r = CString::new(rel).map_err(|_| Error::new(ErrorKind::InvalidInput, "NUL in path"))?;
        let d = cpath(dest)?;
        let mut err = [0 as c_char; ERR_LEN];
        // SAFETY: an open handle, two NUL-terminated paths and an err buffer, all outliving the call.
        let rc = unsafe { mp_play_extract(self.handle.0, r.as_ptr(), d.as_ptr(), err.as_mut_ptr(), err.len()) };
        if rc != 0 {
            return Err(Error::other(format!("extract {rel}: {}", err_text(&err))));
        }
        Ok(())
    }
}

/// The identity of the .p5z at `path` (maxpack_logic::p5z::p5z_id): its size, its 64-byte header
/// and 32-byte footer. Survives a rename or a move.
pub fn id(path: &Path) -> io::Result<String> {
    let f = File::open(path)?;
    let len = f.metadata()?.len();
    let (mut head, mut foot) = ([0u8; 64], [0u8; 32]);
    let (h, t) = (head.len() as u64, foot.len() as u64);
    if len < h + t {
        return Err(Error::new(ErrorKind::UnexpectedEof, format!("{}: too short for a .p5z", path.display())));
    }
    f.read_exact_at(&mut head, 0)?;
    f.read_exact_at(&mut foot, len - t)?;
    Ok(maxpack_logic::p5z::p5z_id(&head, &foot, len))
}
