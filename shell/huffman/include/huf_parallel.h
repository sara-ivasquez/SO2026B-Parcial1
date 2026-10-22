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

/**
 * ====================================================================================
 * FASE 4 - COMPRESIÓN / DESCOMPRESIÓN EN PARALELO CON SALIDA ORDENADA
 * ====================================================================================
 *
 *   coordinador ──► pool_submit(encode_task b)  (b = 0, 1, 2, ...)
 *        │            solo si b < next_to_write + VENTANA   (si no, duerme en 'space')
 *        │
 *   workers:   pread(bloque b) ─► huf_encode_block ─► results[b] = {datos, listo}
 *        │                                             si b == next_to_write: signal(ready)
 *        ▼
 *   hilo escritor:  while (!results[next].listo) wait(ready)
 *                   write(results[next]) ; free ; next++ ; signal(space)
 *
 * Los bloques pueden TERMINAR en cualquier orden, pero el escritor solo escribe el que
 * sigue (next_to_write). Si ese aún no está, DUERME en la variable de condición
 * 'ready' (cero espera activa). La VENTANA limita cuántos bloques hay en memoria a la
 * vez: sin ella, si el bloque 0 es lento, los demás se acumularían sin límite.
 *
 * Por qué no hay interbloqueo: los workers nunca esperan al escritor ni al
 * coordinador; el escritor solo espera el bloque next_to_write, que SIEMPRE ya fue
 * enviado (la ventana deja enviar al menos next_to_write..next_to_write+VENTANA-1); y
 * el coordinador solo espera a que el escritor avance. No hay espera circular.
 *
 * El .huf generado es IDÉNTICO byte a byte al de huf_compress_file() (secuencial):
 * mismo formato, mismo árbol (determinista) y mismo orden de bloques. Por eso cada
 * versión puede descomprimir lo que produce la otra.
 */

/* Ganchos opcionales para el editor (Fases 5 y 6). Cualquiera puede ser NULL.
 *  progress : lo llama SOLO el hilo escritor, después de escribir cada bloque, con los
 *             bytes ORIGINALES ya procesados. Debe ser rápido y seguro entre hilos.
 *  cancelled: se consulta entre bloques; si devuelve != 0 la operación se aborta,
 *             se borra la salida parcial y la función retorna -1 con errno = ECANCELED.
 */
typedef struct {
    HufProgressFn progress;
    int         (*cancelled)(void *ctx);
    void         *ctx;
} HufHooks;

/* Comprime 'in_path' en 'out_path' usando 'pool'. block_size==0 usa HUF_DEFAULT_BLOCK.
 * Retorna 0 en éxito, -1 en error (mensaje en stderr, errno con la causa, y sin dejar
 * salida parcial). */
int huf_compress_file_mt(const char *in_path, const char *out_path, uint32_t block_size,
                         ThreadPool *pool, const HufHooks *hooks);

/* Descomprime 'in_path' (.huf) en 'out_path' decodificando bloques en paralelo. */
int huf_decompress_file_mt(const char *in_path, const char *out_path,
                           ThreadPool *pool, const HufHooks *hooks);

#endif /* HUF_PARALLEL_H */
