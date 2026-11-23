// SPDX-License-Identifier: GPL-3.0-or-later
#include "mp_smp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_http.h"
#include "mp_inst_internal.h"
#include "mp_json.h"
#include "mp_platform.h"

void mp_smp_cfg_default(mp_smp_cfg *c) {
    memset(c, 0, sizeof *c);
    c->host = "127.0.0.1";
    c->port = 10101;
    c->timeout_ms = 3000;
    c->fakelib_mode = MP_SMP_FAKELIB_DEFAULT;
}

void mp_smp_cfg_load(mp_smp_cfg *c, const mp_inst_env *env) {
    char *path = mpi_join(env->internal, "maxpack/config.json"), *buf = NULL;
    size_t n = 0;
    if (path && mp_path_kind(path) == 1 && mpi_read_small(path, 1u << 20, &buf, &n, NULL) == 0) {
        mp_json *j = mp_json_parse(buf, n, NULL);
        const char *m = mp_json_str(mp_json_get(j, "smp_fakelib_mode"));
        if (m && (!strcmp(m, "full") || !strcmp(m, "emulators") || !strcmp(m, "disabled") || !strcmp(m, "default"))) {
            snprintf(c->mode_buf, sizeof c->mode_buf, "%s", m);
            c->fakelib_mode = c->mode_buf;
        }
        mp_json_free(j);
    }
    free(buf);
    free(path);
}

// POST con cuerpo JSON. 0 con HTTP 200 (j: respuesta analizada, puede ser NULL); 1 si respondió
// con otro código; -1 si no hay ShadowMountPlus.
static int post(const mp_smp_cfg *c, const char *route, const char *body, mp_json **j, int *status) {
    char path[128];
    snprintf(path, sizeof path, "/api/v1/%s", route);
    char *resp = NULL;
    size_t rl = 0;
    int st = 0;
    if (j) *j = NULL;
    if (mp_http_request(c->host, c->port, "POST", path, body, strlen(body), c->timeout_ms, &st, &resp, &rl, NULL) != 0)
        return -1;
    if (status) *status = st;
    if (j) *j = mp_json_parse(resp, rl, NULL);
    free(resp);
    return st == 200 ? 0 : 1;
}

int mp_smp_alive(const mp_smp_cfg *c, char *version, size_t vlen) {
    mp_json *j = NULL;
    int rc = post(c, "version", "{}", &j, NULL);
    if (version && vlen) {
        const char *v = mp_json_str(mp_json_get(j, "shadowmount_version"));
        snprintf(version, vlen, "%s", v ? v : "");
    }
    mp_json_free(j);
    return rc == 0;
}

void mp_smp_games_free(mp_smp_game *v, size_t n) {
    if (!v) return;
    for (size_t i = 0; i < n; i++) {
        free(v[i].title_id);
        free(v[i].path);
        free(v[i].source_type);
    }
    free(v);
}

static char *dup_or_empty(const char *s) { return mp_strdup(s ? s : ""); }

int mp_smp_games(const mp_smp_cfg *c, mp_smp_game **out, size_t *n, mp_err *e) {
    *out = NULL;
    *n = 0;
    mp_json *j = NULL;
    int rc = post(c, "games", "{\"include_size\":false}", &j, NULL);
    if (rc != 0) {
        mp_json_free(j);
        mp_err_set(e, rc < 0 ? "ShadowMountPlus no está en marcha" : "ShadowMountPlus no dio la lista de juegos");
        return -1;
    }
    const mp_json *g = mp_json_get(j, "games");
    size_t k = mp_json_len(g);
    mp_smp_game *v = calloc(k + 1, sizeof *v);
    if (!v) {
        mp_json_free(j);
        mp_err_set(e, "sin memoria");
        return -1;
    }
    for (size_t i = 0; i < k; i++) {
        const mp_json *it = mp_json_at(g, i);
        v[i].title_id = dup_or_empty(mp_json_str(mp_json_get(it, "title_id")));
        v[i].path = dup_or_empty(mp_json_str(mp_json_get(it, "path")));
        v[i].source_type = dup_or_empty(mp_json_str(mp_json_get(it, "source_type")));
        v[i].running = -1;   // la API 1.7 no tiene un campo para esto
        if (!v[i].title_id || !v[i].path || !v[i].source_type) {
            mp_smp_games_free(v, i + 1);
            mp_json_free(j);
            mp_err_set(e, "sin memoria");
            return -1;
        }
    }
    mp_json_free(j);
    *out = v;
    *n = k;
    return 0;
}

static void path_body(char *out, size_t cap, const char *path) {
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"path\":");
    mp_jbuf_str(&b, path);
    mp_jbuf_raw(&b, "}");
    snprintf(out, cap, "%s", b.p ? b.p : "{}");
    mp_jbuf_free(&b);
}

static int rescan(const mp_smp_cfg *c, char *why, size_t wl) {
    mp_json *j = NULL;
    int rc = post(c, "scan", "{\"reset_attempts\":false}", &j, NULL);
    if (rc < 0) {
        snprintf(why, wl, "ShadowMountPlus no está en marcha");
    } else if (rc > 0) {
        snprintf(why, wl, "ShadowMountPlus no aceptó el escaneo");
    } else {
        int deferred = 0;
        mp_json_bool(mp_json_get(j, "scan_deferred"), &deferred);
        if (deferred) {
            const char *r = mp_json_str(mp_json_get(j, "scan_deferred_reason"));
            snprintf(why, wl, "escaneo aplazado: %s", r && *r ? r : "sin motivo");
            rc = 1;
        }
    }
    mp_json_free(j);
    return rc;
}

int mp_smp_rescan(const mp_smp_cfg *c, char *why, size_t wl) {
    why[0] = 0;
    return rescan(c, why, wl);
}

int mp_smp_register(const mp_smp_cfg *c, const char *title_id, const char *path, int set_fakelib, char *why,
                    size_t wl) {
    why[0] = 0;
    int pending = 0;
    char body[2400];
    if (set_fakelib) {
        mp_jbuf b = {0};
        mp_jbuf_raw(&b, "{\"title_id\":");
        mp_jbuf_str(&b, title_id);
        mp_jbuf_raw(&b, ",\"mode\":");
        mp_jbuf_str(&b, c->fakelib_mode);
        mp_jbuf_raw(&b, "}");
        int rc = post(c, "games/fakelib", b.p ? b.p : "{}", NULL, NULL);
        mp_jbuf_free(&b);
        if (rc < 0) {
            snprintf(why, wl, "ShadowMountPlus no está en marcha");
            return -1;
        }
        if (rc > 0) {   // aún no conoce el juego: después del escaneo se reintenta
            snprintf(why, wl, "falta fijar el fakelib (ShadowMountPlus aún no conoce el juego)");
            pending = 1;
        }
    }
    path_body(body, sizeof body, path);
    int rc = post(c, "manual/add", body, NULL, NULL);
    if (rc < 0) {
        snprintf(why, wl, "ShadowMountPlus no está en marcha");
        return -1;
    }
    if (rc > 0) {
        snprintf(why, wl, "ShadowMountPlus no aceptó la ruta %s", path);
        return 1;
    }
    char why2[256] = "";
    rc = rescan(c, why2, sizeof why2);
    if (rc < 0) {
        snprintf(why, wl, "%s", why2);
        return -1;
    }
    if (rc > 0 && !pending) snprintf(why, wl, "%s", why2);
    return pending || rc > 0 ? 1 : 0;
}

int mp_smp_forget(const mp_smp_cfg *c, const char *path, char *why, size_t wl) {
    why[0] = 0;
    char body[2400];
    path_body(body, sizeof body, path);
    int rc = post(c, "manual/remove", body, NULL, NULL);
    if (rc < 0) {
        snprintf(why, wl, "ShadowMountPlus no está en marcha");
        return -1;
    }
    return rescan(c, why, wl);
}

int mp_inst_smp_sync(const mp_inst_env *env, const mp_smp_cfg *c, mp_err *e) {
    mp_inst_record *v = NULL;
    size_t n = 0;
    if (mp_inst_list(env, &v, &n, e) != 0) return -1;
    for (size_t i = 0; i < n; i++) {
        if (!v[i].smp_pending) continue;
        char why[256];
        int rc = mp_smp_register(c, v[i].title_id, v[i].dir, v[i].packed, why, sizeof why);
        v[i].smp_pending = rc != 0;
        snprintf(v[i].smp_why, sizeof v[i].smp_why, "%s", rc ? why : "");
        mp_err se = {{0}};
        if (mp_inst_record_save(env, &v[i], &se) != 0 && env->notify) env->notify(env->ctx, se.msg);
        if (rc < 0) break;   // sin ShadowMountPlus no tiene sentido seguir probando
    }
    free(v);
    return 0;
}
