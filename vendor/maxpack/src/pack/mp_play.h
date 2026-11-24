// SPDX-License-Identifier: GPL-3.0-or-later
// Jugar un .p5z sin instalarlo: la consola saca los sueltos del juego al disco interno y lee los
// volúmenes de los packs en su sitio, dentro del .p5z. Formato en docs/formato-p5z.md.
#ifndef MP_PLAY_H
#define MP_PLAY_H

#include <stddef.h>

typedef struct mp_play mp_play;

// Abre un .p5z para jugarlo sin instalar. Comprueba cabecera, pie e índice (mp_p5z_open), ampr_zstd.index
// y la cabecera de cada volumen (mp_p5z_zpk, que llama a check_volume), y que el archive_id de la cabecera
// del .p5z sea el del índice. NULL con el motivo en err (terminado en 0, truncado a errlen).
mp_play *mp_play_open(const char *p5z, char *err, size_t errlen);
// JSON con malloc (se libera con mp_play_free_str):
// {"title_id","version","name","archive_id":"<32 hex en minúsculas>","size":<bytes del .p5z>,
//  "loose_bytes":N,"loose":[{"path","size"}...],"volumes":[{"id","offset","length"}...]}
// loose: papeles 0, 2 y 3, en el orden del índice; volumes: papel 1, por id (el NNN de ampr_zstd-NNN.pak).
char *mp_play_json(mp_play *p);
// Escribe el suelto rel (papel 0, 2 o 3) en dest, ruta completa: crea las carpetas, sustituye lo que hubiera,
// pone su mtime y comprueba su SHA-256. 0 si va bien; -1 con el motivo en err (dest puede quedar a medias).
int mp_play_extract(mp_play *p, const char *rel, const char *dest, char *err, size_t errlen);
void mp_play_close(mp_play *p);
void mp_play_free_str(char *s);

#endif
