#define _POSIX_C_SOURCE 200809L
#include "huf_job.h"
#include "huf_parallel.h"
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ====================================================================================
 * Estado global del job (hay como máximo UNO a la vez)
 * ==================================================================================== */

static pthread_mutex_t g_mutex   = PTHREAD_MUTEX_INITIALIZER;  /* protege TODO lo de abajo */
static pthread_cond_t  g_changed = PTHREAD_COND_INITIALIZER;   /* hubo progreso o terminó  */

static JobStatus g_st      = { .state = JOB_IDLE };
static int       g_cancel  = 0;      /* 1 = el editor pidió cancelar               */
static uint64_t  g_version = 0;      /* sube cada vez que cambia algo (para 'w')    */
static pthread_t g_thread;
static int       g_joinable = 0;     /* 1 = hay un hilo job al que falta hacer join */

static int is_busy(JobState s) { return s == JOB_RUNNING || s == JOB_CANCELLING; }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------- Ganchos que llama huf_*_mt desde sus hilos ---------- */

/* Lo llama el hilo escritor de la Fase 4 después de cada bloque. */
static void on_progress(uint64_t done, uint64_t total, void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&g_mutex);
    g_st.done = done;
    g_st.total = total;
    g_version++;
    pthread_cond_broadcast(&g_changed);   /* despierta a quien esté viendo la barra */
    pthread_mutex_unlock(&g_mutex);
}

/* Lo consultan el coordinador y el escritor de la Fase 4 entre bloques. */
static int on_cancelled(void *ctx) {
    (void)ctx;
    pthread_mutex_lock(&g_mutex);
    int c = g_cancel;
    pthread_mutex_unlock(&g_mutex);
    return c;
}

/* ---------- Hilo job ---------- */

typedef struct {           /* copia PRIVADA de los parámetros: el hilo no lee g_st */
    JobKind kind;
    char    in[256];
    char    out[256];
    int     threads;
} JobArgs;

static void *job_main(void *arg) {
    JobArgs *a = arg;
    double t0 = now_ms();

    HufHooks hooks = { on_progress, on_cancelled, NULL };
    ThreadPool *pool = pool_create(a->threads, 2 * a->threads);
    int rc = -1, err = ENOMEM;
    if (pool) {
        rc = a->kind == JOB_COMPRESS
             ? huf_compress_file_mt(a->in, a->out, 0, pool, &hooks)
             : huf_decompress_file_mt(a->in, a->out, pool, &hooks);
        err = rc == 0 ? 0 : errno;          /* guardar errno ANTES de otras llamadas */
        pool_destroy(pool);                 /* join de todos los workers */
    }

    pthread_mutex_lock(&g_mutex);
    g_st.state = rc == 0 ? JOB_DONE : err == ECANCELED ? JOB_CANCELLED : JOB_FAILED;
    g_st.err = err;
    g_st.elapsed_ms = now_ms() - t0;
    g_st.unseen = 1;
    g_version++;
    pthread_cond_broadcast(&g_changed);     /* despierta a job_wait / job_watch */
    pthread_mutex_unlock(&g_mutex);

    free(a);
    return NULL;
}

/* Hace pthread_join del job anterior si ya terminó y nadie lo ha recogido.
 * Se llama SIN el mutex tomado (join puede esperar). */
static void reap_previous(void) {
    pthread_mutex_lock(&g_mutex);
    int need = g_joinable && !is_busy(g_st.state);
    pthread_t th = g_thread;
    if (need) g_joinable = 0;
    pthread_mutex_unlock(&g_mutex);
    if (need) pthread_join(th, NULL);
}

/* ====================================================================================
 * API pública (la llama SOLO el hilo del editor)
 * ==================================================================================== */

int job_start(JobKind kind, const char *in_path, const char *out_path, int threads) {
    if (!in_path || !out_path || threads < 1) { errno = EINVAL; return -1; }
    if (strlen(in_path) >= sizeof(g_st.in_path) || strlen(out_path) >= sizeof(g_st.out_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    reap_previous();

    JobArgs *a = malloc(sizeof *a);
    if (!a) return -1;
    a->kind = kind;
    a->threads = threads;
    strcpy(a->in, in_path);
    strcpy(a->out, out_path);

    /* Para comprimir ya sabemos el total (tamaño del archivo); al descomprimir lo
     * informa el primer bloque. */
    struct stat sb;
    uint64_t total = (kind == JOB_COMPRESS && stat(in_path, &sb) == 0) ? (uint64_t)sb.st_size : 0;

    pthread_mutex_lock(&g_mutex);
    if (is_busy(g_st.state)) {
        pthread_mutex_unlock(&g_mutex);
        free(a);
        errno = EBUSY;
        return -1;
    }
    memset(&g_st, 0, sizeof g_st);
    g_st.state = JOB_RUNNING;
    g_st.kind = kind;
    strcpy(g_st.in_path, in_path);
    strcpy(g_st.out_path, out_path);
    g_st.total = total;
    g_st.threads = threads;
    g_cancel = 0;
    g_version++;

    int rc = pthread_create(&g_thread, NULL, job_main, a);
    if (rc != 0) {
        g_st.state = JOB_FAILED;
        g_st.err = rc;
        pthread_mutex_unlock(&g_mutex);
        free(a);
        errno = rc;
        return -1;
    }
    g_joinable = 1;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

void job_status(JobStatus *st) {
    pthread_mutex_lock(&g_mutex);
    *st = g_st;                       /* copia completa y consistente */
    pthread_mutex_unlock(&g_mutex);
}

void job_mark_seen(void) {
    pthread_mutex_lock(&g_mutex);
    g_st.unseen = 0;
    pthread_mutex_unlock(&g_mutex);
}

int job_take_unseen(JobStatus *st) {
    pthread_mutex_lock(&g_mutex);
    int u = g_st.unseen;
    if (u) {
        *st = g_st;
        g_st.unseen = 0;
    }
    pthread_mutex_unlock(&g_mutex);
    return u;
}

int job_busy(void) {
    pthread_mutex_lock(&g_mutex);
    int b = is_busy(g_st.state);
    pthread_mutex_unlock(&g_mutex);
    return b;
}

int job_cancel(void) {
    pthread_mutex_lock(&g_mutex);
    int ok = g_st.state == JOB_RUNNING;
    if (ok) {
        g_cancel = 1;
        g_st.state = JOB_CANCELLING;
        g_version++;
        pthread_cond_broadcast(&g_changed);
    }
    pthread_mutex_unlock(&g_mutex);
    return ok ? 0 : -1;
}

void job_wait(void) {
    pthread_mutex_lock(&g_mutex);
    while (is_busy(g_st.state))
        pthread_cond_wait(&g_changed, &g_mutex);
    pthread_mutex_unlock(&g_mutex);
}

void job_watch(void (*draw)(const JobStatus *st)) {
    uint64_t seen;
    JobStatus copy;

    pthread_mutex_lock(&g_mutex);
    seen = g_version - 1;                       /* fuerza un primer dibujo inmediato */
    for (;;) {
        /* Dormir hasta que haya algo NUEVO (progreso o fin). Sin espera activa. */
        while (g_version == seen && is_busy(g_st.state))
            pthread_cond_wait(&g_changed, &g_mutex);
        copy = g_st;
        seen = g_version;
        int busy = is_busy(g_st.state);
        pthread_mutex_unlock(&g_mutex);

        draw(&copy);                            /* imprimir SIN tener el mutex */
        if (!busy) return;

        pthread_mutex_lock(&g_mutex);
    }
}

void job_shutdown(void) {
    job_cancel();
    job_wait();
    reap_previous();
}
