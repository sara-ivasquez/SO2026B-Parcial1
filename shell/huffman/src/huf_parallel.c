#define _POSIX_C_SOURCE 200809L
#include "huf_parallel.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Lee exactamente 'count' bytes desde 'offset' con pread (maneja lecturas parciales y
 * EINTR). pread no usa ni mueve el offset compartido del fd, por eso varios hilos
 * pueden leer el MISMO fd a la vez sin pisarse (con read()+lseek() habría una carrera
 * entre el lseek de un hilo y el read de otro).
 * Retorna 0 si leyó todo, -1 con errno en error (EIO si el archivo se acabó antes). */
static int pread_full(int fd, void *buf, size_t count, uint64_t offset) {
    size_t got = 0;
    while (got < count) {
        ssize_t r = pread(fd, (char *)buf + got, count - got, (off_t)(offset + got));
        if (r == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EIO; return -1; }   /* archivo más corto de lo esperado */
        got += (size_t)r;
    }
    return 0;
}

/* ====================================================================================
 * FASE 3: conteo de frecuencias en paralelo
 * ==================================================================================== */

/* Estado compartido por TODAS las tareas de un conteo. */
typedef struct {
    pthread_mutex_t mutex;   /* protege freq y error */
    uint64_t       *freq;    /* histograma global (resultado) */
    int             error;   /* primer errno que ocurra; 0 = sin error */
} CountShared;

/* Lo que necesita UNA tarea: qué bloque leer y dónde dejar el resultado. */
typedef struct {
    int          fd;
    uint64_t     offset;
    uint32_t     len;
    CountShared *sh;
} CountTask;

static void count_task(void *arg) {
    CountTask   *t  = arg;
    CountShared *sh = t->sh;

    /* Si otra tarea ya falló, no vale la pena seguir leyendo (cancelación cooperativa:
     * la tarea revisa una bandera, nadie la mata a la fuerza). */
    pthread_mutex_lock(&sh->mutex);
    int abort_now = sh->error != 0;
    pthread_mutex_unlock(&sh->mutex);
    if (abort_now) return;

    int err = 0;
    uint64_t local[HUF_NSYM] = {0};          /* histograma LOCAL: solo lo toca este hilo */
    uint8_t *buf = malloc(t->len);           /* buffer PROPIO: solo lo toca este hilo   */

    if (!buf) err = ENOMEM;
    else if (pread_full(t->fd, buf, t->len, t->offset) == -1) err = errno;
    else huf_count_block(buf, t->len, local);   /* trabajo pesado, sin ningún lock */
    free(buf);

    /* Reducción: UNA sola sección crítica por bloque (256 sumas). */
    pthread_mutex_lock(&sh->mutex);
    if (err) {
        if (sh->error == 0) sh->error = err;
    } else {
        for (int s = 0; s < HUF_NSYM; s++) sh->freq[s] += local[s];
    }
    pthread_mutex_unlock(&sh->mutex);
}

int huf_count_parallel(int fd, uint64_t total, uint32_t block_size,
                       ThreadPool *pool, uint64_t freq[HUF_NSYM]) {
    memset(freq, 0, HUF_NSYM * sizeof(uint64_t));
    if (!pool || block_size == 0) { errno = EINVAL; return -1; }
    if (total == 0) return 0;

    uint64_t nblocks = (total + block_size - 1) / block_size;
    CountTask *tasks = calloc((size_t)nblocks, sizeof(CountTask));
    if (!tasks) return -1;

    CountShared sh = { .freq = freq, .error = 0 };
    int rc = pthread_mutex_init(&sh.mutex, NULL);
    if (rc != 0) { free(tasks); errno = rc; return -1; }

    for (uint64_t b = 0; b < nblocks; b++) {
        uint64_t off = b * block_size;
        uint64_t len = total - off < block_size ? total - off : block_size;
        tasks[b] = (CountTask){ fd, off, (uint32_t)len, &sh };
        if (pool_submit(pool, count_task, &tasks[b]) != 0) {
            pthread_mutex_lock(&sh.mutex);
            if (sh.error == 0) sh.error = ECANCELED;
            pthread_mutex_unlock(&sh.mutex);
            break;
        }
    }

    /* BARRERA: esperar a que terminen todas las tareas ya enviadas. Es obligatorio
     * incluso si hubo error, porque esas tareas apuntan a 'tasks' y a 'sh', que viven
     * en esta función: liberarlos antes sería usar memoria ya liberada. */
    pool_wait(pool);

    int err = sh.error;          /* ya no hay hilos escribiendo: lectura segura */
    pthread_mutex_destroy(&sh.mutex);
    free(tasks);

    if (err) {
        memset(freq, 0, HUF_NSYM * sizeof(uint64_t));
        errno = err;
        return -1;
    }
    return 0;
}
