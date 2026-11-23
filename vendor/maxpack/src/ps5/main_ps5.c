// SPDX-License-Identifier: GPL-3.0-or-later
// maxpack.elf: el instalador en la PS5. Se carga con el cargador de ELF (puerto 9021) y sirve la
// página en el puerto 8095. El motor y la web son los mismos que se prueban en el PC.
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "mp_api.h"
#include "mp_json.h"
#include "mp_page.h"
#include "mp_smp.h"
#include "mp_version.h"

#define PORT 8095
#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

int sceNotificationSend(int userId, bool isLogged, const char *payload);

#ifdef MAXPACK_HAVE_RUNTIME
extern const unsigned char mp_runtime_sprx[];
extern const size_t mp_runtime_sprx_len;
#endif

// Aviso en la consola, con la plantilla del ejemplo samples/notify del SDK.
static void notify(void *ctx, const char *msg) {
    (void)ctx;
    mp_jbuf b = {0};
    mp_jbuf_raw(&b, "{\"rawData\":{\"viewTemplateType\":\"InteractiveToastTemplateB\",\"channelType\":\"Downloads\","
                    "\"useCaseId\":\"IDC\",\"toastOverwriteType\":\"No\",\"isImmediate\":true,\"priority\":100,"
                    "\"viewData\":{\"message\":{\"body\":");
    mp_jbuf_str(&b, msg);
    mp_jbuf_raw(&b, "},\"subMessage\":{\"body\":\"MaxPack\"}},\"platformViews\":{\"previewDisabled\":{\"viewData\":"
                    "{\"icon\":{\"type\":\"Predefined\",\"parameters\":{\"icon\":\"download\"}},\"message\":{\"body\":");
    mp_jbuf_str(&b, msg);
    mp_jbuf_raw(&b, "}}}}},\"createdDateTime\":\"2026-01-01T00:00:00.000Z\",\"localNotificationId\":\"8095\"}");
    if (!b.oom) sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true, b.p);
    mp_jbuf_free(&b);
    printf("[MaxPack] %s\n", msg);
}

int main(void) {
    mp_inst_env env;
    mp_inst_env_default(&env);   // /data y /mnt/usb0..7, /mnt/ext0..1
    env.notify = notify;
#ifdef MAXPACK_HAVE_RUNTIME
    env.runtime.data = mp_runtime_sprx;
    env.runtime.len = mp_runtime_sprx_len;
    env.runtime.mtime = MAXPACK_RUNTIME_MTIME;
    env.runtime.version = MAXPACK_RUNTIME_VERSION;
#endif
    mp_smp_cfg smp;
    mp_smp_cfg_default(&smp);
    mp_smp_cfg_load(&smp, &env);
    mp_err e = {{0}};
    mp_api *api = mp_api_new(&env, &smp, 60000, &e);
    if (!api) {
        notify(NULL, e.msg);
        return 1;
    }
    mp_api_set_page(api, mp_page_html, mp_page_html_len);
    mp_http_server *srv = mp_http_start("0.0.0.0", PORT, mp_api_handle, api, &e);
    if (!srv) {
        notify(NULL, "MaxPack ya está en marcha: el puerto 8095 está ocupado");
        mp_api_free(api);
        return 1;
    }
    notify(NULL, "MaxPack " MP_VERSION " listo: abre el puerto 8095 de la consola en el navegador");
    for (;;) sleep(60);
}
