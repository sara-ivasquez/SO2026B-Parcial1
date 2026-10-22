#ifndef HUF_JOB_H
#define HUF_JOB_H

#include <stdint.h>

/**
 * ====================================================================================
 * MÓDULO: Tarea en segundo plano (huf_job.h / huf_job.c)  -- PARCIAL 2, FASE 5
 * ====================================================================================
 * Permite que el editor lance una compresión/descompresión SIN bloquearse:
 *
 *   hilo del editor (REPL)                     hilo "job" (uno a la vez)
 *   ──────────────────────                     ─────────────────────────
 *   c archivo ─► job_start() ─► pthread_create ─► crea pool de N workers
 *   (vuelve al prompt de inmediato)               huf_compress_file_mt(...)
 *   p, s, ... siguen funcionando                    │ progreso ─► actualiza 'done'
 *   st / prompt ─► job_status() (lee 'done')        │ cancelado? ◄─ lee 'cancel'
 *   w ─► job_watch(): duerme en 'changed'           ▼
 *        y redibuja la barra cada vez           estado = TERMINADO ; broadcast
 *        que el job avanza
 *
 * TODO el estado del job (estado, done/total, cancel, error...) vive en una sola
 * estructura protegida por UN mutex. El editor nunca lee esos campos directamente:
 * siempre pide una COPIA consistente con job_status().
 *
 * Variable de condición 'changed': el hilo job la señala cada vez que avanza el
 * progreso o termina. Así 'w' (ver progreso en vivo) y job_wait() duermen hasta que
 * haya algo nuevo que mostrar, sin espera activa.
 *
 * El hilo job NUNCA imprime en pantalla: solo actualiza el estado. Todo lo que se ve
 * lo imprime el hilo del editor, así los mensajes no se mezclan con el prompt.
 */

typedef enum { JOB_COMPRESS, JOB_DECOMPRESS } JobKind;

typedef enum {
    JOB_IDLE,          /* nunca se ha lanzado nada                   */
    JOB_RUNNING,       /* en curso                                   */
    JOB_CANCELLING,    /* se pidió cancelar; esperando que se detenga */
    JOB_DONE,          /* terminó bien                               */
    JOB_FAILED,        /* terminó con error (ver 'err')              */
    JOB_CANCELLED      /* se canceló; no quedó salida parcial        */
} JobState;

/* Copia (snapshot) del estado: el editor trabaja sobre esta copia, sin locks. */
typedef struct {
    JobState state;
    JobKind  kind;
    char     in_path[256];
    char     out_path[256];
    uint64_t done;         /* bytes originales procesados */
    uint64_t total;        /* bytes originales totales    */
    int      threads;
    int      err;          /* errno si state == JOB_FAILED */
    double   elapsed_ms;   /* duración (al terminar)       */
    int      unseen;       /* 1 = terminó y el editor aún no lo ha notificado */
} JobStatus;

/* Lanza la tarea en segundo plano y retorna DE INMEDIATO.
 * Retorna 0 si se lanzó; -1 con errno=EBUSY si ya hay una en curso, u otro errno. */
int  job_start(JobKind kind, const char *in_path, const char *out_path, int threads);

/* Copia consistente del estado actual. */
void job_status(JobStatus *st);

/* Marca el resultado del último job como ya notificado al usuario. */
void job_mark_seen(void);

/* Si el último job terminó y no se ha notificado, copia su estado en *st, lo marca
 * como notificado y retorna 1; si no, retorna 0. Revisar y marcar ocurren bajo el
 * MISMO lock, así dos hilos (el editor y el de señales) nunca avisan dos veces. */
int  job_take_unseen(JobStatus *st);

/* 1 si hay una tarea en curso (RUNNING o CANCELLING). */
int  job_busy(void);

/* Pide cancelar (cooperativo: el job lo revisa entre bloques). 0 si había algo que
 * cancelar, -1 si no había ninguna tarea en curso. */
int  job_cancel(void);

/* Bloquea (durmiendo) hasta que la tarea en curso termine. */
void job_wait(void);

/* Bloquea mostrando una barra de progreso en vivo hasta que la tarea termine.
 * 'draw' se llama desde el hilo que espera, con el mutex SUELTO. */
void job_watch(void (*draw)(const JobStatus *st));

/* Cancela lo que esté corriendo, espera y libera el hilo (pthread_join).
 * Se llama al salir del editor. */
void job_shutdown(void);

#endif /* HUF_JOB_H */
