#ifndef HUF_PARALLEL_H
#define HUF_PARALLEL_H

#include <stdint.h>
#include "huffman.h"
#include "threadpool.h"

/**
 * ====================================================================================
 * MÓDULO: Huffman concurrente (huf_parallel.h / huf_parallel.c)  -- PARCIAL 2, FASE 3+
 * ====================================================================================
 * Usa el pool de hilos (Fase 2) y las funciones reentrantes del núcleo (Fase 1).
 *
 * FASE 3 - CONTEO DE FRECUENCIAS EN PARALELO CON REDUCCIÓN LOCAL
 *
 *   archivo:   [ bloque 0 ][ bloque 1 ][ bloque 2 ] ... [ bloque N-1 ]
 *                   │           │           │                │
 *   una tarea   count_task  count_task  count_task  ...  count_task   (en el pool)
 *   por bloque:     │ pread() de SU bloque a SU buffer (no comparte el offset del fd)
 *                   │ cuenta en un histograma LOCAL de 256 posiciones (sin locks)
 *                   ▼
 *              lock(mutex) ─► freq_global[s] += local[s]  (256 sumas) ─► unlock
 *
 *   pool_wait()  = BARRERA: el coordinador no lee freq_global hasta que TODAS las
 *                  tareas terminaron. Después de la barrera ya no hay escritores.
 *
 * Por qué así y no "freq_global[c]++ con un mutex por byte":
 *   - Un mutex por byte serían millones de lock/unlock y todos los hilos peleando por
 *     el mismo lock: más lento que el secuencial.
 *   - freq_global[c]++ SIN mutex es una condición de carrera (leer-sumar-escribir no es
 *     atómico): se pierden conteos y el árbol saldría mal.
 *   - Con reducción local cada hilo toma el mutex UNA vez por bloque (p.ej. 1 vez cada
 *     256 KB), así que la contención es prácticamente nula.
 */

/* Cuenta las frecuencias de los 'total' bytes del archivo abierto en 'fd', partiéndolo
 * en bloques de 'block_size' que se procesan en paralelo en 'pool'.
 * 'freq' se sobrescribe con el resultado. El offset de 'fd' NO se modifica (pread).
 * Retorna 0 en éxito, -1 en error (errno indica la causa; EIO si el archivo es más
 * corto que 'total', p.ej. porque alguien lo truncó mientras se contaba). */
int huf_count_parallel(int fd, uint64_t total, uint32_t block_size,
                       ThreadPool *pool, uint64_t freq[HUF_NSYM]);

#endif /* HUF_PARALLEL_H */
