#define _POSIX_C_SOURCE 200809L
#include "huffman.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Driver de pruebas de la Fase 1:  ./huf c <entrada> <salida.huf> [block_size]
 *                                  ./huf x <entrada.huf> <salida>
 * Se retira (o se conserva solo para pruebas) cuando el módulo se integre al editor. */

static void progreso(uint64_t done, uint64_t total, void *ctx) {
    (void)ctx;
    fprintf(stderr, "\r%3d%%", total ? (int)(done * 100 / total) : 100);
}

int main(int argc, char **argv) {
    if (argc < 4 || (strcmp(argv[1], "c") != 0 && strcmp(argv[1], "x") != 0)) {
        fprintf(stderr, "Uso: %s c <entrada> <salida.huf> [block_size]\n"
                        "     %s x <entrada.huf> <salida>\n", argv[0], argv[0]);
        return 2;
    }
    int rc;
    if (argv[1][0] == 'c') {
        uint32_t bs = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 0;
        rc = huf_compress_file(argv[2], argv[3], bs, progreso, NULL);
    } else {
        rc = huf_decompress_file(argv[2], argv[3], progreso, NULL);
    }
    fprintf(stderr, "\n");
    return rc == 0 ? 0 : 1;
}
