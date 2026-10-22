#ifndef THREADPOOL_H
#define THREADPOOL_H

/**
 * ====================================================================================
 * MÓDULO: Pool de hilos con cola acotada (threadpool.h / threadpool.c) -- PARCIAL 2, FASE 2
 * ====================================================================================
 * Un pool es un grupo FIJO de hilos trabajadores (workers) que se crean una sola vez y
 * se quedan esperando tareas en una cola compartida. Las fases 3 y 4 no crean hilos:
 * solo hacen pool_submit() de una tarea por bloque y pool_wait() para esperar.
 *
 *   productor (coordinador)            cola acotada              consumidores (workers)
 *   pool_submit(tarea) ──► [ t | t | t | _ | _ ] ──► worker 1, worker 2, ..., worker N
 *
 * SINCRONIZACIÓN (todo el estado compartido está protegido por UN solo mutex):
 *
 *   mutex      protege: la cola (head, tail, count), pending y shutdown.
 *   not_empty  los workers DUERMEN aquí cuando la cola está vacía.
 *              Lo señala pool_submit() al encolar una tarea.
 *   not_full   el productor DUERME aquí cuando la cola está llena.
 *              Lo señala un worker al sacar una tarea.  -> acota la memoria en vuelo.
 *   all_done   pool_wait() DUERME aquí mientras pending > 0.
 *              Lo señala el worker que termina la última tarea pendiente.
 *
 * Garantías que se defienden en la sustentación:
 *   - Sin condiciones de carrera: ningún campo compartido se lee/escribe sin el mutex.
 *   - Sin interbloqueo: hay un único mutex (no existe orden de adquisición que violar) y
 *     la tarea del usuario se ejecuta SIN tener el mutex tomado.
 *   - Sin espera activa: toda espera es pthread_cond_wait() dentro de un while que
 *     revisa la condición (protege contra despertares espurios); nunca sleep ni polling.
 */

/* Una tarea es una función y el argumento con el que se llamará. */
typedef void (*pool_task_fn)(void *arg);

/* Tipo opaco: los campos solo se tocan dentro de threadpool.c (encapsulamiento). */
typedef struct ThreadPool ThreadPool;

/* Crea el pool con 'nthreads' workers y una cola de capacidad 'queue_cap'.
 * Retorna NULL en error (y no deja hilos ni memoria colgando). */
ThreadPool *pool_create(int nthreads, int queue_cap);

/* Encola una tarea. Si la cola está llena, BLOQUEA (durmiendo) hasta que haya espacio.
 * Retorna 0 en éxito, -1 si el pool se está destruyendo. */
int pool_submit(ThreadPool *p, pool_task_fn fn, void *arg);

/* Espera (durmiendo) hasta que TODAS las tareas enviadas hayan TERMINADO de ejecutarse.
 * Es la "barrera" entre fases: p. ej. no se construye el árbol hasta que todo el
 * conteo termine. El pool sigue vivo y se puede reutilizar para la siguiente fase. */
void pool_wait(ThreadPool *p);

/* Apaga el pool: las tareas que ya estaban en la cola se terminan de ejecutar, luego
 * cada worker sale y se le hace pthread_join. Libera toda la memoria. Acepta NULL. */
void pool_destroy(ThreadPool *p);

/* Cantidad de workers del pool (útil para mensajes y mediciones). */
int pool_size(const ThreadPool *p);

#endif /* THREADPOOL_H */
