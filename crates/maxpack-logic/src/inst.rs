//! Forwarding the page's installer requests (`/api/inst/<x>`) to the phase-2 installer (the MaxPack
//! engine in C), which answers them as `/api/<x>` (its own API, vendor/maxpack src/web/mp_api.h).

/// The installer's route for a request target (path and query) of the payload's server:
/// `/api/inst/<x>[?query]` -> `/api/<x>[?query]` when `<x>` is a route the page uses with that
/// method (GET `state`, `icon`; POST `scan`, `probe`, `install`, `pause`, `verify`, `uninstall`,
/// `discard`). None for anything else: it never reaches the installer (its `/api/quit` included:
/// quitting is the payload's own `/api/quit`).
pub fn installer_path(method: &str, target: &str) -> Option<String> {
    let rest = target.strip_prefix("/api/inst/")?;
    let (name, query) = match rest.split_once('?') {
        Some((name, query)) => (name, Some(query)),
        None => (rest, None),
    };
    let known = match method {
        "GET" => matches!(name, "state" | "icon"),
        "POST" => matches!(name, "scan" | "probe" | "install" | "pause" | "verify" | "uninstall" | "discard"),
        _ => false,
    };
    known.then(|| match query {
        Some(q) => format!("/api/{name}?{q}"),
        None => format!("/api/{name}"),
    })
}

/// The value of header `name` (any case) in an HTTP request head (request line and headers),
/// trimmed; the first one when it repeats. None when absent.
pub fn header<'a>(head: &'a str, name: &str) -> Option<&'a str> {
    head.lines().skip(1).find_map(|l| {
        let (k, v) = l.split_once(':')?;
        k.trim().eq_ignore_ascii_case(name).then(|| v.trim())
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn installer_path_rewrites_the_page_routes() {
        assert_eq!(installer_path("GET", "/api/inst/state").as_deref(), Some("/api/state"));
        assert_eq!(installer_path("GET", "/api/inst/icon?p5z=%2Fmnt%2Fusb0%2Fa.p5z").as_deref(),
            Some("/api/icon?p5z=%2Fmnt%2Fusb0%2Fa.p5z"));
        for r in ["scan", "probe", "install", "pause", "verify", "uninstall", "discard"] {
            assert_eq!(installer_path("POST", &format!("/api/inst/{r}")), Some(format!("/api/{r}")), "{r}");
        }
        // A query on a POST is the installer's business (it ignores it).
        assert_eq!(installer_path("POST", "/api/inst/scan?x=1").as_deref(), Some("/api/scan?x=1"));
    }

    #[test]
    fn installer_path_refuses_other_methods_and_routes() {
        // The method of each route only.
        assert_eq!(installer_path("POST", "/api/inst/state"), None);
        assert_eq!(installer_path("GET", "/api/inst/install"), None);
        assert_eq!(installer_path("PUT", "/api/inst/scan"), None);
        assert_eq!(installer_path("get", "/api/inst/state"), None);
        // Quitting is the payload's own /api/quit; the installer's page is not served.
        assert_eq!(installer_path("POST", "/api/inst/quit"), None);
        assert_eq!(installer_path("GET", "/api/inst/"), None);
        assert_eq!(installer_path("GET", "/api/inst"), None);
        // Exactly one name after the prefix.
        for t in ["/api/inst/state/x", "/api/inst//state", "/api/inst/../state", "/api/inst/state%2F",
            "/api/installer/state", "/api/inststate", "/inst/state", "/api/state", "api/inst/state",
            "/API/INST/state", "/api/inst/State", "/api/inst/state#x"] {
            assert_eq!(installer_path("GET", t), None, "{t}");
        }
    }

    #[test]
    fn header_by_name_any_case() {
        let head = "POST /api/inst/install HTTP/1.1\r\nHost: 192.168.1.20:8095\r\n\
            content-type:  application/json; charset=utf-8 \r\nContent-Length: 2\r\nX-Empty:\r\nHost: second";
        assert_eq!(header(head, "Host"), Some("192.168.1.20:8095"));
        assert_eq!(header(head, "host"), Some("192.168.1.20:8095"));
        assert_eq!(header(head, "Content-Type"), Some("application/json; charset=utf-8"));
        assert_eq!(header(head, "content-length"), Some("2"));
        assert_eq!(header(head, "X-Empty"), Some(""));
        assert_eq!(header(head, "Origin"), None);
        // The request line is not a header, and a name must match whole.
        assert_eq!(header(head, "POST /api/inst/install HTTP/1.1\r\nHost"), None);
        assert_eq!(header(head, "Content"), None);
        assert_eq!(header("GET / HTTP/1.1", "Host"), None);
        // LF-only lines too.
        assert_eq!(header("GET / HTTP/1.1\nHost: 127.0.0.1", "Host"), Some("127.0.0.1"));
    }
}
