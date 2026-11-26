// SPDX-License-Identifier: GPL-3.0-or-later
#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif defined(__FreeBSD__) || defined(__PROSPERO__)
#define __BSD_VISIBLE 1
#endif
#endif

#include "mp_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_platform.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock_t;
#define BAD_SOCK INVALID_SOCKET
#define close_sock closesocket
static void net_init(void) {
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);   // counted: can be called several times
}
static int last_err(void) { return WSAGetLastError(); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
typedef int sock_t;
#define BAD_SOCK (-1)
#define close_sock close
static void net_init(void) {}
static int last_err(void) { return errno; }
#endif

#ifdef MSG_NOSIGNAL
#define SEND_FLAGS MSG_NOSIGNAL
#else
#define SEND_FLAGS 0
#endif

#define MAX_HEADER (16u << 10)

static void set_timeouts(sock_t s, unsigned ms) {
#ifdef _WIN32
    DWORD t = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&t, sizeof t);
#else
    struct timeval t = {(time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000)};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
#endif
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

static int send_all(sock_t s, const char *p, size_t n) {
    while (n > 0) {
        int k = (int)send(s, p, (int)(n > (1u << 20) ? (1u << 20) : n), SEND_FLAGS);
        if (k <= 0) return -1;
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

static const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default: return "OK";
    }
}

static void send_response(sock_t s, int status, const char *ctype, const char *body, size_t n) {
    char head[512];
    int k = snprintf(head, sizeof head,
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\n\r\n",
                     status, reason(status), ctype, n);
    if (send_all(s, head, (size_t)k) == 0 && n) send_all(s, body, n);
}

static void send_simple(sock_t s, int status) {
    char body[64];
    int n = snprintf(body, sizeof body, "{\"ok\":false,\"code\":\"http_%d\"}", status);
    send_response(s, status, "application/json", body, (size_t)n);
}

// Finds a header's value (case-insensitive) in head (NUL-terminated).
static const char *find_header(const char *head, const char *name) {
    size_t n = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p; p = strstr(p + 2, "\r\n")) {
        const char *l = p + 2;
        size_t i = 0;
        for (; i < n && l[i]; i++) {
            char a = l[i], b = name[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a + 32);
            if (b >= 'A' && b <= 'Z') b = (char)(b + 32);
            if (a != b) break;
        }
        if (i == n && l[n] == ':') {
            l += n + 1;
            while (*l == ' ' || *l == '\t') l++;
            return l;
        }
    }
    return NULL;
}

// Reads up to the end of the headers. Returns the bytes read (may include part of the body)
// and in *hend the position of the body; -1 if it is cut off or too long.
static long read_head(sock_t s, char *buf, size_t cap, size_t *hend) {
    size_t n = 0;
    while (n < cap - 1) {
        int k = (int)recv(s, buf + n, (int)(cap - 1 - n), 0);
        if (k <= 0) return -1;
        n += (size_t)k;
        buf[n] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (end) {
            *hend = (size_t)(end - buf) + 4;
            return (long)n;
        }
    }
    return -1;
}

static void serve_one(mp_http_server *srv, sock_t c, int loopback);

struct mp_http_server {
    sock_t ls;
    mp_thread *t;
    volatile int stop;
    mp_http_handler h;
    void *ctx;
    uint16_t port;
};

static void serve_one(mp_http_server *srv, sock_t c, int loopback) {
    set_timeouts(c, 5000);
    char *buf = malloc(MAX_HEADER + 1);
    if (!buf) return;
    size_t hend = 0;
    long got = read_head(c, buf, MAX_HEADER + 1, &hend);
    if (got < 0) {
        free(buf);
        return;
    }
    // Request line: METHOD SP target SP HTTP/x.y
    char *line_end = strstr(buf, "\r\n");
    *line_end = 0;
    char *sp1 = strchr(buf, ' '), *sp2 = sp1 ? strchr(sp1 + 1, ' ') : NULL;
    if (!sp1 || !sp2 || strncmp(sp2 + 1, "HTTP/1.", 7) != 0) {
        send_simple(c, 400);
        free(buf);
        return;
    }
    *sp1 = 0;
    *sp2 = 0;
    *line_end = '\r';   // so that find_header sees "\r\n" after the line
    char method[16], target[2048];
    snprintf(method, sizeof method, "%s", buf);
    snprintf(target, sizeof target, "%s", sp1 + 1);
    buf[hend - 2] = 0;   // the headers end in NUL so they can be searched
    const char *cl = find_header(line_end, "Content-Length");
    char ctype[128] = "", host[256] = "";
    const char *ct = find_header(line_end, "Content-Type"), *ho = find_header(line_end, "Host");
    if (ct) snprintf(ctype, sizeof ctype, "%.*s", (int)strcspn(ct, "\r"), ct);
    if (ho) snprintf(host, sizeof host, "%.*s", (int)strcspn(ho, "\r"), ho);
    unsigned long long len = cl ? strtoull(cl, NULL, 10) : 0;
    if (len > MP_HTTP_MAX_BODY) {
        send_simple(c, 413);
        free(buf);
        return;
    }
    char *body = malloc((size_t)len + 1);
    if (!body) {
        free(buf);
        return;
    }
    size_t have = (size_t)got - hend;
    if (have > len) have = (size_t)len;
    memcpy(body, buf + hend, have);
    free(buf);
    while (have < len) {
        int k = (int)recv(c, body + have, (int)(len - have), 0);
        if (k <= 0) {
            free(body);
            return;
        }
        have += (size_t)k;
    }
    body[len] = 0;
    char *q = strchr(target, '?');
    const char *query = "";
    if (q) {
        *q = 0;
        query = q + 1;
    }
    mp_http_req rq = {method, target, query, body, (size_t)len, ctype, host, loopback};
    mp_http_resp rs = {200, NULL, NULL, 0};
    srv->h(srv->ctx, &rq, &rs);
    send_response(c, rs.status, rs.content_type ? rs.content_type : "application/json", rs.body ? rs.body : "",
                  rs.body ? rs.body_len : 0);
    free(rs.body);
    free(body);
}

static void accept_loop(void *arg) {
    mp_http_server *srv = arg;
    while (!srv->stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(srv->ls, &rd);
        struct timeval tv = {0, 200000};
        int k = select((int)srv->ls + 1, &rd, NULL, NULL, &tv);
        if (k <= 0 || srv->stop) continue;
        struct sockaddr_in peer;
        socklen_t pl = sizeof peer;
        memset(&peer, 0, sizeof peer);
        sock_t c = accept(srv->ls, (struct sockaddr *)&peer, &pl);
        if (c == BAD_SOCK) continue;
        int lo = pl >= sizeof peer && peer.sin_family == AF_INET && (ntohl(peer.sin_addr.s_addr) >> 24) == 127;
        serve_one(srv, c, lo);
        close_sock(c);
    }
}

mp_http_server *mp_http_start(const char *bind_addr, uint16_t port, mp_http_handler h, void *ctx, mp_err *e) {
    net_init();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, bind_addr, &a.sin_addr) != 1) {
        mp_err_set(e, "invalid address: %s", bind_addr);
        return NULL;
    }
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BAD_SOCK) {
        mp_err_set(e, "cannot create the socket (%d)", last_err());
        return NULL;
    }
#ifndef _WIN32
    int one = 1;   // on Windows SO_REUSEADDR would allow stealing a port in use
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 16) != 0) {
        mp_err_set(e, "cannot listen on %s:%u (is it busy?) (%d)", bind_addr, (unsigned)port, last_err());
        close_sock(s);
        return NULL;
    }
    socklen_t al = sizeof a;
    getsockname(s, (struct sockaddr *)&a, &al);
    mp_http_server *srv = calloc(1, sizeof *srv);
    if (!srv) {
        close_sock(s);
        mp_err_set(e, "out of memory");
        return NULL;
    }
    srv->ls = s;
    srv->h = h;
    srv->ctx = ctx;
    srv->port = ntohs(a.sin_port);
    if (mp_thread_start(&srv->t, accept_loop, srv) != 0) {
        close_sock(s);
        free(srv);
        mp_err_set(e, "could not create the server thread");
        return NULL;
    }
    return srv;
}

uint16_t mp_http_port(const mp_http_server *s) { return s->port; }

void mp_http_stop(mp_http_server *s) {
    if (!s) return;
    s->stop = 1;
    mp_thread_join(s->t);
    close_sock(s->ls);
    free(s);
}

// ---- client ---------------------------------------------------------------------------------

static int set_nonblock(sock_t s, int on) {
#ifdef _WIN32
    u_long v = (u_long)on;
    return ioctlsocket(s, FIONBIO, &v);
#else
    int fl = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, on ? (fl | O_NONBLOCK) : (fl & ~O_NONBLOCK));
#endif
}

static int connect_timeout(sock_t s, const struct sockaddr_in *a, unsigned ms) {
    set_nonblock(s, 1);
    int rc = connect(s, (const struct sockaddr *)a, sizeof *a);
    if (rc != 0) {
        fd_set wr, ex;
        FD_ZERO(&wr);
        FD_ZERO(&ex);
        FD_SET(s, &wr);
        FD_SET(s, &ex);
        struct timeval tv = {(long)(ms / 1000), (long)((ms % 1000) * 1000)};
        if (select((int)s + 1, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex)) return -1;
        int err = 0;
        socklen_t el = sizeof err;
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el);
        if (err) return -1;
    }
    set_nonblock(s, 0);
    return 0;
}

int mp_http_request(const char *host, uint16_t port, const char *method, const char *path, const char *body,
                    size_t body_len, unsigned timeout_ms, int *status, char **resp, size_t *resp_len, mp_err *e) {
    *status = 0;
    *resp = NULL;
    *resp_len = 0;
    net_init();
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
        mp_err_set(e, "invalid address: %s", host);
        return -1;
    }
    sock_t s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == BAD_SOCK) {
        mp_err_set(e, "cannot create the socket");
        return -1;
    }
    if (connect_timeout(s, &a, timeout_ms) != 0) {
        close_sock(s);
        mp_err_set(e, "cannot connect to %s:%u", host, (unsigned)port);
        return -1;
    }
    set_timeouts(s, timeout_ms);
    char head[2600];
    int k = snprintf(head, sizeof head,
                     "%s %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     method, path, host, (unsigned)port, body_len);
    if (k < 0 || (size_t)k >= sizeof head || send_all(s, head, (size_t)k) != 0 ||
        (body_len && send_all(s, body, body_len) != 0)) {
        close_sock(s);
        mp_err_set(e, "could not send the request to %s:%u", host, (unsigned)port);
        return -1;
    }
    size_t cap = 64u << 10, n = 0;
    char *buf = malloc(cap + 1);
    int rc = buf ? 0 : -1;
    while (rc == 0) {
        if (n == cap) {
            if (cap >= (64u << 20)) {
                rc = -1;
                break;
            }
            char *nb = realloc(buf, cap * 2 + 1);
            if (!nb) {
                rc = -1;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        int got = (int)recv(s, buf + n, (int)(cap - n), 0);
        if (got < 0) rc = -1;
        if (got <= 0) break;
        n += (size_t)got;
    }
    close_sock(s);
    if (rc == 0) {
        buf[n] = 0;
        char *end = strstr(buf, "\r\n\r\n");
        if (n < 12 || strncmp(buf, "HTTP/1.", 7) != 0 || !end) {
            rc = -1;
        } else {
            *status = atoi(buf + 9);
            size_t off = (size_t)(end - buf) + 4, blen = n - off;
            *end = 0;
            const char *cl = find_header(buf, "Content-Length");
            if (cl) {
                unsigned long long want = strtoull(cl, NULL, 10);
                if (want > blen) rc = -1;
                else blen = (size_t)want;
            }
            if (rc == 0) {
                memmove(buf, buf + off, blen);
                buf[blen] = 0;
                *resp = buf;
                *resp_len = blen;
                buf = NULL;
            }
        }
    }
    free(buf);
    if (rc) mp_err_set(e, "%s:%u did not respond properly", host, (unsigned)port);
    return rc;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int decode_n(const char *in, size_t n, char *out, size_t cap) {
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (c == '+') {
            c = ' ';
        } else if (c == '%') {
            if (i + 2 >= n) return -1;
            int h = hexv(in[i + 1]), l = hexv(in[i + 2]);
            if (h < 0 || l < 0) return -1;
            c = (char)(h << 4 | l);
            i += 2;
        }
        if (k + 1 >= cap) return -1;
        out[k++] = c;
    }
    if (k >= cap) return -1;
    out[k] = 0;
    return 0;
}

int mp_url_decode(const char *in, char *out, size_t cap) { return decode_n(in, strlen(in), out, cap); }

int mp_http_query_get(const char *query, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = query; p && *p;) {
        const char *amp = strchr(p, '&');
        size_t len = amp ? (size_t)(amp - p) : strlen(p);
        if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') return decode_n(p + kl + 1, len - kl - 1, out, cap);
        p = amp ? amp + 1 : NULL;
    }
    return -1;
}
