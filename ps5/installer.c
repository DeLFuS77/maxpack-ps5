/* The phase-2 installer (the MaxPack engine, vendor/maxpack) for src/installer.rs: the console's
 * installer env and its ShadowMountPlus client set up as the engine's src/ps5/main_ps5.c does, with
 * the runtime and the notify callback from Rust. C does the setup so Rust never mirrors the layout
 * of mp_inst_env or mp_smp_cfg (whose fakelib_mode may point into itself); the API copies both. */
#include <stdint.h>
#include <stdio.h>

#include "mp_api.h"
#include "mp_smp.h"

/* runtime/runtime_version must outlive the API (Rust passes static data). NULL with the engine's
 * message in err when the API cannot start. */
mp_api *maxpack_installer_new(void (*notify)(void *ctx, const char *msg), const uint8_t *runtime,
                              size_t runtime_len, int64_t runtime_mtime, const char *runtime_version,
                              char *err, size_t errlen) {
    mp_inst_env env;
    mp_inst_env_default(&env); /* /data and /mnt/usb0..7, /mnt/ext0..1 */
    env.notify = notify;
    env.runtime.data = runtime;
    env.runtime.len = runtime_len;
    env.runtime.mtime = runtime_mtime;
    env.runtime.version = runtime_version;
    mp_smp_cfg smp;
    mp_smp_cfg_default(&smp);
    mp_smp_cfg_load(&smp, &env); /* "smp_fakelib_mode" of /data/maxpack/config.json, as in phase 2 */
    mp_err e = {{0}};
    /* 60 s between retries of what is pending in ShadowMountPlus, as main_ps5.c. */
    mp_api *api = mp_api_new(&env, &smp, 60000, &e);
    if (!api && errlen)
        snprintf(err, errlen, "%s", e.msg[0] ? e.msg : "mp_api_new failed");
    return api;
}
