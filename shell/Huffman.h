#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>
#include <stddef.h>

/**
 * ====================================================================================
 * MÓDULO: Núcleo Huffman por bloques (huffman.h / huffman.c)  -- PARCIAL 2, FASE 1
 * ====================================================================================
 * Esta fase es SECUENCIAL, pero la API está diseñada por BLOQUES para que las fases
 * 3 y 4 solo tengan que llamar las mismas funciones desde hilos distintos:
 *
 *   huf_count_block()   -> fase de conteo   (cada worker, con su histograma LOCAL)
 *   huf_build_tree()    -> árbol global      (hilo coordinador, una sola vez)
 *   huf_encode_block()  -> fase de codificar (cada worker, un bloque independiente)
 *   huf_decode_block()  -> descompresión     (cada worker, un bloque independiente)
 *
 * Ninguna de estas funciones usa variables globales ni estáticas: son REENTRANTES y
 * por eso son seguras para llamarlas desde varios hilos sin ningún mutex, siempre que
 * cada hilo use sus propios buffers de entrada/salida (HufTree y HufCodes son de solo
 * lectura una vez construidos).
 *
 * Todo el I/O de archivos usa solo open/read/write/lseek/close (igual que el Parcial 1).
 *
 * FORMATO DEL ARCHIVO .huf (enteros en little-endian):
 *   offset  tamaño  campo
 *   0       4       magic "HUF1"
 *   4       8       orig_size   (bytes del archivo original)
 *   12      4       block_size  (tamaño de bloque usado al comprimir)
 *   16      4       num_blocks  (= ceil(orig_size / block_size))
 *   20      2048    freq[256]   (uint64 por byte; permite reconstruir el MISMO árbol)
 *   2068    ...     num_blocks veces:  orig_len(4) | comp_len(4) | datos comprimidos
 */

#define HUF_MAGIC            "HUF1"
#define HUF_NSYM             256
#define HUF_MAX_NODES        511          /* 256 hojas + 255 nodos internos          */
#define HUF_MAX_CODE_LEN     64           /* un código cabe en un uint64_t            */
#define HUF_DEFAULT_BLOCK    (256u * 1024u)
#define HUF_MAX_BLOCK        (64u * 1024u * 1024u)
#define HUF_HEADER_SIZE      (4 + 8 + 4 + 4 + 8 * HUF_NSYM)

/* Tabla de códigos: code[s] son los 'len[s]' bits menos significativos (MSB primero). */
typedef struct {
    uint64_t code[HUF_NSYM];
    uint8_t  len[HUF_NSYM];
} HufCodes;

/* Árbol: nodos 0..255 son hojas (símbolo = índice); 256.. son internos. -1 = vacío. */
typedef struct {
    int left[HUF_MAX_NODES];
    int right[HUF_MAX_NODES];
    int root;                /* -1 si el archivo está vacío */
    int nsyms;               /* cantidad de símbolos distintos */
} HufTree;

/* Callback opcional de progreso (Fase 5). done/total en bytes ORIGINALES procesados. */
typedef void (*HufProgressFn)(uint64_t done, uint64_t total, void *ctx);

/* ------------------------- API por bloques (reentrante) ------------------------- */

/* Suma al histograma 'freq' los bytes de data[0..n). */
void huf_count_block(const uint8_t *data, size_t n, uint64_t freq[HUF_NSYM]);

/* Construye árbol (y tabla de códigos si codes != NULL) a partir de las frecuencias.
 * Determinista: las mismas frecuencias producen SIEMPRE el mismo árbol.
 * Retorna 0 en éxito, -1 si algún código excede HUF_MAX_CODE_LEN. */
int huf_build_tree(const uint64_t freq[HUF_NSYM], HufTree *tree, HufCodes *codes);

/* Codifica un bloque. Reserva *out con malloc (el llamador hace free).
 * Retorna 0 en éxito y deja en *out_len los bytes producidos; -1 en error. */
int huf_encode_block(const HufCodes *codes, const uint8_t *in, size_t n,
                     uint8_t **out, size_t *out_len);

/* Decodifica un bloque a 'out' (capacidad >= orig_len). 0 en éxito, -1 si está corrupto. */
int huf_decode_block(const HufTree *tree, const uint8_t *in, size_t comp_len,
                     uint8_t *out, size_t orig_len);

/* ------------------------- API de archivo (secuencial) -------------------------- */

/* Comprime 'in_path' en 'out_path'. block_size==0 usa HUF_DEFAULT_BLOCK.
 * Retorna 0 en éxito, -1 en error (mensaje en stderr; borra la salida parcial). */
int huf_compress_file(const char *in_path, const char *out_path, uint32_t block_size,
                      HufProgressFn progress, void *ctx);

/* Descomprime 'in_path' (.huf) en 'out_path'. Mismo contrato de retorno. */
int huf_decompress_file(const char *in_path, const char *out_path,
                        HufProgressFn progress, void *ctx);

#endif /* HUFFMAN_H */