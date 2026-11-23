//! The checks every request to the payload's server (port 8095) passes before its route
//! (src/web.rs `handle`): the page, opened at the console's IP or from the console itself, gets
//! through; a page of another site does not, neither by a plain cross-site POST nor by DNS
//! rebinding.

use crate::inst::header;

/// Why a request is refused before its route.
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Refusal {
    /// The Host header is not an IP literal or `localhost`: the page was opened by a name, or a
    /// page of another site rebound its own name to the console's IP (DNS rebinding).
    Host,
    /// A state-changing request sent by a page of another site (cross-site request forgery).
    CrossSite,
}

/// The engine's `host_ok` (vendor/maxpack src/web/mp_api.c), a little stricter so that whatever
/// passes here passes there too: an IPv4 literal (four groups of 0-255), a bracketed IPv6 literal
/// or `localhost`, each with an optional `:port`. A DNS name never passes.
pub fn host_ok(host: &str) -> bool {
    let rest = if let Some(v6) = host.strip_prefix('[') {
        let Some((addr, rest)) = v6.split_once(']') else { return false };
        if !addr.contains(':') || !addr.bytes().all(|b| b.is_ascii_hexdigit() || b == b':' || b == b'.') {
            return false;
        }
        rest
    } else {
        let (name, rest) = host.find(':').map_or((host, ""), |i| host.split_at(i));
        if name != "localhost" && !ipv4(name) {
            return false;
        }
        rest
    };
    rest.is_empty() || rest.strip_prefix(':').is_some_and(|p| p.bytes().all(|b| b.is_ascii_digit()) && p.parse::<u16>().is_ok())
}

/// Four dot-separated groups of 1-3 digits, each 0-255.
fn ipv4(s: &str) -> bool {
    let groups: Vec<&str> = s.split('.').collect();
    groups.len() == 4
        && groups.iter().all(|g| (1..=3).contains(&g.len()) && g.bytes().all(|b| b.is_ascii_digit()) && g.parse::<u8>().is_ok())
}

/// A state-changing request comes from the page itself: no `Sec-Fetch-Site: cross-site`, and an
/// `Origin`, when there is one, that is `http://` (or `https://`) and this very Host. No Origin
/// passes: browsers send one on every cross-origin POST, so only a client that is not a page of
/// another site (curl, the payload's own loopback quit) sends none.
pub fn same_origin(host: &str, origin: Option<&str>, fetch_site: Option<&str>) -> bool {
    if fetch_site.is_some_and(|s| s.eq_ignore_ascii_case("cross-site")) {
        return false;
    }
    let Some(origin) = origin else { return true };
    let (origin, host) = (origin.to_ascii_lowercase(), host.to_ascii_lowercase());
    ["http://", "https://"].iter().any(|scheme| origin.strip_prefix(scheme) == Some(host.as_str()))
}

/// Both, for one request head (request line and headers): the Host check on every request, the
/// origin check on every method other than GET and HEAD (the page changes state only with POST).
pub fn check(head: &str) -> Result<(), Refusal> {
    let host = header(head, "host").unwrap_or("");
    if !host_ok(host) {
        return Err(Refusal::Host);
    }
    let method = head.lines().next().and_then(|l| l.split(' ').next()).unwrap_or("");
    if !matches!(method, "GET" | "HEAD") && !same_origin(host, header(head, "origin"), header(head, "sec-fetch-site")) {
        return Err(Refusal::CrossSite);
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn host_takes_ip_literals_and_localhost() {
        for h in [
            "192.168.1.20", "192.168.1.20:8095", "127.0.0.1:8095", "10.0.0.1", "0.0.0.0:1",
            "255.255.255.255:65535", "localhost", "localhost:8095", "[::1]", "[::1]:8095",
            "[fe80::1c2b:3a]:8095", "[::ffff:192.168.1.20]:8095", "[2001:db8::1]",
        ] {
            assert!(host_ok(h), "{h}");
        }
    }

    #[test]
    fn host_refuses_names_and_junk() {
        for h in [
            "", "ps5", "ps5.local", "ps5.local:8095", "evil.example", "rebind.evil.example:8095",
            "LOCALHOST", "localhost.", "localhost.evil.example", "xlocalhost", "1.2.3", "1.2.3.4.5",
            "1..2.3", "256.1.1.1", "1.2.3.4a", "1.2.3.4:", "1.2.3.4:x", "1.2.3.4:65536",
            "1.2.3.4:123456", "1.2.3.4 :80", " 1.2.3.4", "[", "[]", "[::1", "[::1]x", "[::1]:",
            "[::1]:80x", "[g::1]", "[1.2.3.4]", "[::1]evil.example", "0x7f.0.0.1", "1.2.3.4:80:80",
        ] {
            assert!(!host_ok(h), "{h:?}");
        }
    }

    #[test]
    fn same_origin_by_origin_and_fetch_site() {
        let h = "192.168.1.20:8095";
        // The page itself (a browser sends Origin on POST), and no Origin at all.
        assert!(same_origin(h, Some("http://192.168.1.20:8095"), None));
        assert!(same_origin(h, Some("http://192.168.1.20:8095"), Some("same-origin")));
        assert!(same_origin(h, Some("HTTP://192.168.1.20:8095"), None));
        assert!(same_origin(h, Some("https://192.168.1.20:8095"), None));
        assert!(same_origin(h, None, None));
        assert!(same_origin(h, None, Some("none")));
        assert!(same_origin("localhost:8095", Some("http://LOCALHOST:8095"), None));
        assert!(same_origin("[::1]:8095", Some("http://[::1]:8095"), None));
        // Another site, another port of the console, an opaque origin.
        for o in [
            "http://evil.example", "http://192.168.1.20", "http://192.168.1.20:9021",
            "http://192.168.1.21:8095", "null", "", "192.168.1.20:8095", "http://192.168.1.20:8095/",
            "ftp://192.168.1.20:8095", "http://192.168.1.20:8095.evil.example",
        ] {
            assert!(!same_origin(h, Some(o), None), "{o:?}");
        }
        // A browser that says cross-site is refused whatever the Origin says.
        assert!(!same_origin(h, Some("http://192.168.1.20:8095"), Some("cross-site")));
        assert!(!same_origin(h, None, Some("Cross-Site")));
        // same-site is a same-site page on another port: its Origin decides.
        assert!(!same_origin(h, Some("http://192.168.1.20:9021"), Some("same-site")));
    }

    #[test]
    fn check_applies_host_to_all_and_origin_to_state_changes() {
        let req = |line: &str, headers: &str| format!("{line}\r\n{headers}");
        // The page at the console IP, and the payload's own loopback quit (no Origin).
        assert_eq!(check(&req("GET / HTTP/1.1", "Host: 192.168.1.20:8095")), Ok(()));
        assert_eq!(check(&req("POST /api/settings HTTP/1.1",
            "Host: 192.168.1.20:8095\r\nOrigin: http://192.168.1.20:8095\r\nContent-Type: application/json")), Ok(()));
        assert_eq!(check(&req("POST /api/quit HTTP/1.1", "Host: 127.0.0.1\r\nContent-Length: 0")), Ok(()));
        // A name in Host: refused on every route, the page too (DNS rebinding).
        assert_eq!(check(&req("GET / HTTP/1.1", "Host: rebind.evil.example:8095")), Err(Refusal::Host));
        assert_eq!(check(&req("GET /api/state HTTP/1.1", "Host: ps5.local")), Err(Refusal::Host));
        assert_eq!(check(&req("POST /api/purge HTTP/1.1", "Host: ps5.local\r\nOrigin: http://ps5.local")), Err(Refusal::Host));
        // No Host at all (HTTP/1.0).
        assert_eq!(check("GET / HTTP/1.0"), Err(Refusal::Host));
        // A cross-site POST (a form or fetch from another page on the LAN or the internet).
        assert_eq!(check(&req("POST /api/delete/PPSA01234 HTTP/1.1",
            "Host: 192.168.1.20:8095\r\nOrigin: http://evil.example\r\nContent-Type: text/plain")), Err(Refusal::CrossSite));
        assert_eq!(check(&req("POST /api/eject HTTP/1.1",
            "Host: 192.168.1.20:8095\r\nSec-Fetch-Site: cross-site")), Err(Refusal::CrossSite));
        assert_eq!(check(&req("POST /api/quit HTTP/1.1", "Host: 192.168.1.20:8095\r\nOrigin: null")), Err(Refusal::CrossSite));
        // Any method but GET/HEAD is checked like a POST; a GET with another Origin is only read.
        assert_eq!(check(&req("PUT /api/settings HTTP/1.1", "Host: 192.168.1.20\r\nOrigin: http://evil.example")), Err(Refusal::CrossSite));
        assert_eq!(check(&req("GET /api/state HTTP/1.1", "Host: 192.168.1.20\r\nOrigin: http://evil.example")), Ok(()));
        assert_eq!(check(&req("HEAD / HTTP/1.1", "Host: 192.168.1.20\r\nSec-Fetch-Site: cross-site")), Ok(()));
    }
}
