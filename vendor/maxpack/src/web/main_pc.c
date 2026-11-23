// SPDX-License-Identifier: GPL-3.0-or-later
// Instalador de PC: el mismo motor y la misma página que en la PS5, sobre carpetas del PC.
// Solo sirve para probar sin consola; no se distribuye.
//   maxpack-installer-pc --internal DIR [--drive DIR]... [--port 8095] [--runtime FILE] [--smp-port 10101]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mp_api.h"
#include "mp_inst_internal.h"
#include "mp_page.h"
#include "mp_platform.h"

static void say(void *ctx, const char *msg) {
    (void)ctx;
    fprintf(stderr, "[aviso] %s\n", msg);
}

static void usage(void) {
    fprintf(stderr, "uso: maxpack-installer-pc --internal DIR [--drive DIR]... [--port N] [--runtime FILE] "
                    "[--smp-port N]\n");
}

int main(int argc, char **argv) {
    mp_args_utf8(&argc, &argv);
    const char *internal = NULL, *runtime = NULL, *drives[10];
    size_t nd = 0;
    unsigned port = 8095, smp_port = 10101;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--internal") && v) internal = argv[++i];
        else if (!strcmp(a, "--drive") && v && nd < 10) drives[nd++] = argv[++i];
        else if (!strcmp(a, "--port") && v) port = (unsigned)atoi(argv[++i]);
        else if (!strcmp(a, "--runtime") && v) runtime = argv[++i];
        else if (!strcmp(a, "--smp-port") && v) smp_port = (unsigned)atoi(argv[++i]);
        else {
            usage();
            return 2;
        }
    }
    if (!internal || !port || port > 65535 || smp_port > 65535) {
        usage();
        return 2;
    }
    mp_err e = {{0}};
    if (mp_make_dirs(internal, &e) != 0) {
        fprintf(stderr, "error: %s\n", e.msg);
        return 1;
    }
    mp_inst_env env;
    mp_inst_env_default(&env);
    env.internal = internal;
    env.drive_candidates = drives;
    env.ndrive_candidates = nd;
    env.notify = say;
    char *rt = NULL;
    size_t rtlen = 0;
    if (runtime) {
        int64_t mt = 0;
        if (mpi_read_small(runtime, 64u << 20, &rt, &rtlen, &e) != 0 || mp_stat(runtime, NULL, &mt) != 0) {
            fprintf(stderr, "error: %s\n", e.msg[0] ? e.msg : runtime);
            return 1;
        }
        env.runtime.data = (const uint8_t *)rt;
        env.runtime.len = rtlen;
        env.runtime.mtime = mt;
        env.runtime.version = "archivo";
    }
    mp_smp_cfg smp;
    mp_smp_cfg_default(&smp);
    smp.port = (uint16_t)smp_port;
    mp_smp_cfg_load(&smp, &env);
    mp_api *api = mp_api_new(&env, &smp, 60000, &e);
    if (!api) {
        fprintf(stderr, "error: %s\n", e.msg);
        return 1;
    }
    mp_api_set_page(api, mp_page_html, mp_page_html_len);
    mp_http_server *srv = mp_http_start("127.0.0.1", (uint16_t)port, mp_api_handle, api, &e);
    if (!srv) {
        fprintf(stderr, "error: %s\n", e.msg);
        mp_api_free(api);
        return 1;
    }
    fprintf(stderr, "MaxPack (instalador de PC) en http://127.0.0.1:%u\n", port);
    for (;;) mp_sleep_ms(1000);
}
