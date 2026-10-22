#define _POSIX_C_SOURCE 200809L
#include "threadpool.h"
#include <pthread.h>
#include <stdio.h>     /* solo fprintf(stderr) para reportar errores */
#include <stdlib.h>
#include <string.h>

/* ====================================================================================
 * Estructuras internas
 * ==================================================================================== */

typedef struct {
    pool_task_fn fn;
    void        *arg;
} Task;

struct ThreadPool {
    /* --- Estado compartido: SOLO se toca con 'mutex' tomado --- */
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;   /* hay tareas para sacar          */
    pthread_cond_t  not_full;    /* hay espacio para meter         */
    pthread_cond_t  all_done;    /* pending llegó a 0              */

    Task *queue;                 /* buffer circular de 'cap' posiciones */
    int   cap;
    int   head;                  /* próxima posición a SACAR       */
    int   tail;                  /* próxima posición a METER       */
    int   count;                 /* tareas en la cola (sin empezar) */
    int   pending;               /* en cola + ejecutándose          */
    int   shutdown;              /* 1 = no se aceptan más tareas    */

    /* --- Solo lo usa el hilo que crea/destruye el pool --- */
    pthread_t *threads;
    int        nthreads;
};

/* ====================================================================================
 * Hilo trabajador
 * ====================================================================================
 * Ciclo de vida: dormir hasta que haya una tarea -> sacarla -> soltar el mutex ->
 * ejecutarla -> reportar que terminó -> repetir. Sale cuando hay shutdown Y la cola
 * quedó vacía (así ninguna tarea encolada se pierde).
 */
static void *worker_main(void *arg) {
    ThreadPool *p = arg;

    for (;;) {
        pthread_mutex_lock(&p->mutex);

        /* 'while' y no 'if': tras despertar se vuelve a revisar la condición, porque
         * pthread_cond_wait puede despertar sin señal (despertar espurio) o porque otro
         * worker pudo haber tomado la tarea primero. */
        while (p->count == 0 && !p->shutdown)
            pthread_cond_wait(&p->not_empty, &p->mutex);

        if (p->count == 0 && p->shutdown) {      /* nada más por hacer: salir */
            pthread_mutex_unlock(&p->mutex);
            break;
        }

        Task t = p->queue[p->head];
        p->head = (p->head + 1) % p->cap;
        p->count--;
        pthread_cond_signal(&p->not_full);       /* se liberó un espacio en la cola */

        pthread_mutex_unlock(&p->mutex);

        /* La tarea corre SIN el mutex: así los demás workers trabajan en paralelo y
         * una tarea lenta nunca bloquea la cola (y no hay riesgo de interbloqueo si la
         * tarea toma sus propios locks). */
        t.fn(t.arg);

        pthread_mutex_lock(&p->mutex);
        p->pending--;
        if (p->pending == 0)
            pthread_cond_broadcast(&p->all_done); /* puede haber varios esperando */
        pthread_mutex_unlock(&p->mutex);
    }
    return NULL;
}

/* ====================================================================================
 * API pública
 * ==================================================================================== */

/* Libera las primitivas y la memoria. Solo se llama cuando ya no hay workers vivos. */
static void pool_free(ThreadPool *p) {
    pthread_cond_destroy(&p->all_done);
    pthread_cond_destroy(&p->not_full);
    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->mutex);
    free(p->threads);
    free(p->queue);
    free(p);
}

/* Avisa a todos que se apaguen y espera a que los 'n' primeros workers terminen. */
static void stop_and_join(ThreadPool *p, int n) {
    pthread_mutex_lock(&p->mutex);
    p->shutdown = 1;
    pthread_cond_broadcast(&p->not_empty);   /* despierta a TODOS los workers dormidos */
    pthread_cond_broadcast(&p->not_full);    /* y a cualquier productor bloqueado      */
    pthread_mutex_unlock(&p->mutex);

    for (int i = 0; i < n; i++) {
        int rc = pthread_join(p->threads[i], NULL);
        if (rc != 0) fprintf(stderr, "threadpool: pthread_join: %s\n", strerror(rc));
    }
}

ThreadPool *pool_create(int nthreads, int queue_cap) {
    if (nthreads < 1 || queue_cap < 1) {
        fprintf(stderr, "threadpool: parámetros inválidos (hilos=%d, cola=%d)\n",
                nthreads, queue_cap);
        return NULL;
    }

    ThreadPool *p = calloc(1, sizeof(*p));
    if (!p) { perror("threadpool: calloc"); return NULL; }

    p->queue   = calloc((size_t)queue_cap, sizeof(Task));
    p->threads = calloc((size_t)nthreads, sizeof(pthread_t));
    if (!p->queue || !p->threads) {
        perror("threadpool: calloc");
        free(p->queue); free(p->threads); free(p);
        return NULL;
    }
    p->cap = queue_cap;

    /* Inicialización de primitivas (con verificación de error en cada una). */
    int rc;
    if ((rc = pthread_mutex_init(&p->mutex, NULL)) != 0) goto err_mutex;
    if ((rc = pthread_cond_init(&p->not_empty, NULL)) != 0) goto err_ne;
    if ((rc = pthread_cond_init(&p->not_full, NULL)) != 0) goto err_nf;
    if ((rc = pthread_cond_init(&p->all_done, NULL)) != 0) goto err_ad;

    for (int i = 0; i < nthreads; i++) {
        rc = pthread_create(&p->threads[i], NULL, worker_main, p);
        if (rc != 0) {
            /* Falló a mitad de camino: apagar y unir SOLO los que sí se crearon,
             * para no dejar hilos huérfanos ni memoria sin liberar. */
            fprintf(stderr, "threadpool: pthread_create: %s\n", strerror(rc));
            stop_and_join(p, i);
            pool_free(p);
            return NULL;
        }
        p->nthreads++;
    }
    return p;

err_ad: pthread_cond_destroy(&p->not_full);
err_nf: pthread_cond_destroy(&p->not_empty);
err_ne: pthread_mutex_destroy(&p->mutex);
err_mutex:
    fprintf(stderr, "threadpool: init de primitivas: %s\n", strerror(rc));
    free(p->threads); free(p->queue); free(p);
    return NULL;
}

int pool_submit(ThreadPool *p, pool_task_fn fn, void *arg) {
    if (!p || !fn) return -1;

    pthread_mutex_lock(&p->mutex);

    /* Cola llena: el productor DUERME hasta que un worker saque algo. Esto limita
     * cuántos bloques hay en memoria a la vez (contrapresión / backpressure). */
    while (p->count == p->cap && !p->shutdown)
        pthread_cond_wait(&p->not_full, &p->mutex);

    if (p->shutdown) {
        pthread_mutex_unlock(&p->mutex);
        return -1;
    }

    p->queue[p->tail] = (Task){ fn, arg };
    p->tail = (p->tail + 1) % p->cap;
    p->count++;
    p->pending++;
    pthread_cond_signal(&p->not_empty);      /* basta despertar a UN worker */

    pthread_mutex_unlock(&p->mutex);
    return 0;
}

void pool_wait(ThreadPool *p) {
    if (!p) return;
    pthread_mutex_lock(&p->mutex);
    while (p->pending > 0)
        pthread_cond_wait(&p->all_done, &p->mutex);
    pthread_mutex_unlock(&p->mutex);
}

void pool_destroy(ThreadPool *p) {
    if (!p) return;
    stop_and_join(p, p->nthreads);
    pool_free(p);
}

int pool_size(const ThreadPool *p) {
    return p ? p->nthreads : 0;
}
