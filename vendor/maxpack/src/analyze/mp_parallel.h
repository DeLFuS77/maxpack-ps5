// SPDX-License-Identifier: GPL-3.0-or-later
// Reparto de ítems entre hilos y progreso en stderr.
#ifndef MP_PARALLEL_H
#define MP_PARALLEL_H

#include <stddef.h>
#include <stdint.h>

#include "mp_err.h"
#include "mp_platform.h"

// Enganche de progreso: lo llama mp_progress_add como mucho cada 100 ms y siempre al terminar la
// fase (done == total). Se llama con el cerrojo de la fase tomado, así que no hay dos llamadas a la vez.
typedef struct {
    void (*cb)(void *ctx, const char *phase, uint64_t done, uint64_t total);
    void *ctx;
} mp_progress_hook;

// Un enganche por proceso (NULL lo quita). mp_progress_init lo copia: ponerlo antes de empezar.
void mp_progress_set_hook(const mp_progress_hook *h);

// Cancelación: bandera atómica, se puede pedir desde cualquier hilo. mp_parallel_run deja de repartir
// y pack, unpack y verify salen con -1 y el mensaje "cancelado".
void mp_cancel_request(void);
int mp_cancelled(void);
void mp_cancel_reset(void);
// Si hay cancelación pendiente pone "cancelado" en e y devuelve -1; si no, 0.
int mp_cancel_check(mp_err *e);

typedef struct mp_progress {
    mp_mutex *mu;
    uint64_t done, total;
    const char *phase;
    int print;
    uint64_t last_ns;   // última vez que se imprimió (mp_progress_maybe_print)
    mp_progress_hook hook;
    uint64_t hook_ns;   // última llamada al enganche
    int hook_final;     // ya se avisó de done == total
} mp_progress;

void mp_progress_init(mp_progress *p, const char *phase, uint64_t total, int print);
void mp_progress_add(mp_progress *p, uint64_t n);   // p puede ser NULL
void mp_progress_free(mp_progress *p);
// Para bucles que no usan mp_parallel_run para todo: imprime como mucho una vez por segundo.
void mp_progress_maybe_print(mp_progress *p);

// Procesa los ítems 0..n-1 con `threads` hilos (tid en [0, threads)). Si un work devuelve != 0,
// se dejan de repartir ítems y se devuelve el primer código. Con mp_cancelled() también se deja de
// repartir y se devuelve -1 si quedaban ítems sin repartir. Si prog->print, el hilo principal
// escribe "\r<fase>: NN %" en stderr cada segundo y al acabar.
int mp_parallel_run(unsigned threads, size_t n, int (*work)(void *ctx, size_t item, unsigned tid),
                    void *ctx, mp_progress *prog);

#endif
