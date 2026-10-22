#define _POSIX_C_SOURCE 200809L
#include "threadpool.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Pruebas de la Fase 2 (pool de hilos).   Uso: ./test_pool [num_tareas]
 *
 * 1. Cada tarea se ejecuta EXACTAMENTE una vez (1, 4 y 16 workers; colas de 1 y 8
 *    para forzar que el productor se bloquee en not_full).
 * 2. pool_wait() funciona como barrera entre dos fases y el pool se puede reutilizar.
 * 3. Las tareas corren en PARALELO de verdad (8 tareas de 200 ms con 8 workers).
 * 4. pool_destroy() termina las tareas que quedaban en la cola antes de salir.
 * 5. Parámetros inválidos se rechazan sin fugas.
 */

static int pass = 0, fail = 0;
#define CHECK(cond, ...) do {                                   \
        if (cond) { printf("  OK    "); pass++; }               \
        else      { printf("  FALLO "); fail++; }               \
        printf(__VA_ARGS__); printf("\n");                      \
    } while (0)

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

/* ---------- Prueba 1 y 4: cada tarea una sola vez ---------- */

static int             *hits;          /* hits[i] lo escribe SOLO la tarea i: sin carrera */
static long             counter;       /* compartido por todas: protegido por counter_mtx */
static pthread_mutex_t  counter_mtx = PTHREAD_MUTEX_INITIALIZER;

static void task_count(void *arg) {
    long i = (long)arg;
    hits[i]++;
    pthread_mutex_lock(&counter_mtx);
    counter++;
    pthread_mutex_unlock(&counter_mtx);
}

static int run_once(int workers, int cap, long n, int wait_before_destroy) {
    for (long i = 0; i < n; i++) hits[i] = 0;
    counter = 0;

    ThreadPool *p = pool_create(workers, cap);
    if (!p) return 0;
    for (long i = 0; i < n; i++)
        if (pool_submit(p, task_count, (void *)i) != 0) { pool_destroy(p); return 0; }
    if (wait_before_destroy) pool_wait(p);
    pool_destroy(p);           /* tras join, leer hits/counter es seguro */

    if (counter != n) return 0;
    for (long i = 0; i < n; i++) if (hits[i] != 1) return 0;
    return 1;
}

/* ---------- Prueba 2: barrera entre fases ---------- */

static long *fase_a, *fase_b;
static long  nfase;

static void task_a(void *arg) { long i = (long)arg; fase_a[i] = i; }
/* Si la barrera fallara, alguna fase_a[] todavía estaría en -1 y la suma no daría n-1 */
static void task_b(void *arg) { long i = (long)arg; fase_b[i] = fase_a[i] + fase_a[nfase - 1 - i]; }

/* ---------- Prueba 3: paralelismo real ---------- */

static void task_sleep(void *arg) {
    (void)arg;
    struct timespec ts = { 0, 200 * 1000 * 1000 };   /* 200 ms */
    nanosleep(&ts, NULL);
}

int main(int argc, char **argv) {
    long n = argc > 1 ? strtol(argv[1], NULL, 10) : 10000;
    if (n < 1) n = 10000;
    hits = calloc((size_t)n, sizeof(int));
    fase_a = malloc((size_t)n * sizeof(long));
    fase_b = malloc((size_t)n * sizeof(long));
    if (!hits || !fase_a || !fase_b) { perror("malloc"); return 1; }

    printf("-- cada tarea se ejecuta exactamente una vez (%ld tareas) --\n", n);
    int workers[] = { 1, 4, 16 };
    int caps[]    = { 1, 8 };
    for (int w = 0; w < 3; w++)
        for (int c = 0; c < 2; c++)
            CHECK(run_once(workers[w], caps[c], n, 1),
                  "%2d workers, cola de %d", workers[w], caps[c]);

    printf("-- pool_wait como barrera entre fases (pool reutilizado) --\n");
    {
        nfase = n;
        for (long i = 0; i < n; i++) fase_a[i] = fase_b[i] = -1;
        ThreadPool *p = pool_create(8, 16);
        int ok = p != NULL;
        for (long i = 0; ok && i < n; i++) ok = pool_submit(p, task_a, (void *)i) == 0;
        pool_wait(p);                                   /* barrera */
        for (long i = 0; ok && i < n; i++) ok = pool_submit(p, task_b, (void *)i) == 0;
        pool_wait(p);
        pool_destroy(p);
        for (long i = 0; ok && i < n; i++) ok = fase_b[i] == n - 1;
        CHECK(ok, "fase B nunca ve datos incompletos de la fase A");
    }

    printf("-- paralelismo real --\n");
    {
        ThreadPool *p = pool_create(8, 8);
        double t0 = now_ms();
        for (int i = 0; i < 8; i++) pool_submit(p, task_sleep, NULL);
        pool_wait(p);
        double dt = now_ms() - t0;
        pool_destroy(p);
        CHECK(p && dt < 600, "8 tareas de 200 ms con 8 workers: %.0f ms (secuencial ~1600 ms)", dt);
    }

    printf("-- apagado --\n");
    CHECK(run_once(4, 8, n, 0), "pool_destroy sin pool_wait completa todas las tareas");
    CHECK(pool_create(0, 8) == NULL && pool_create(4, 0) == NULL,
          "parámetros inválidos rechazados");
    pool_destroy(NULL);
    CHECK(1, "pool_destroy(NULL) no falla");

    free(hits); free(fase_a); free(fase_b);
    printf("Resultado: %d OK, %d fallos\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
