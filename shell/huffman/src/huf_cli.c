#define _POSIX_C_SOURCE 200809L
#include "huffman.h"
#include "huf_parallel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Driver de pruebas del módulo Huffman (se usa desde los scripts de pruebas; el editor
 * llamará directamente a las funciones en la Fase 5).
 *
 *   ./huf c  <entrada> <salida.huf> [block_size]          comprimir   (secuencial)
 *   ./huf x  <entrada.huf> <salida>                       descomprimir (secuencial)
 *   ./huf pc <entrada> <salida.huf> [hilos] [block_size]  comprimir   (paralelo)
 *   ./huf px <entrada.huf> <salida> [hilos]               descomprimir (paralelo)
 *   ./huf bench <archivo> [block_size]                    tabla de tiempos
 */

static void progreso(uint64_t done, uint64_t total, void *ctx) {
    (void)ctx;
    fprintf(stderr, "\r%3d%%", total ? (int)(done * 100 / total) : 100);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static int default_threads(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 4;
}

static int usage(const char *p) {
    fprintf(stderr,
        "Uso: %s c  <entrada> <salida.huf> [block_size]\n"
        "     %s x  <entrada.huf> <salida>\n"
        "     %s pc <entrada> <salida.huf> [hilos] [block_size]\n"
        "     %s px <entrada.huf> <salida> [hilos]\n"
        "     %s bench <archivo> [block_size]\n", p, p, p, p, p);
    return 2;
}

/* Compara dos archivos usando solo syscalls. 1 = iguales. */
static int same_file(const char *a, const char *b);

static int bench(const char *path, uint32_t bs) {
    char huf_path[] = "/tmp/huf_benchXXXXXX";
    int tmpfd = mkstemp(huf_path);
    if (tmpfd == -1) { perror("mkstemp"); return 1; }
    close(tmpfd);
    char out_path[64];
    snprintf(out_path, sizeof out_path, "%s.out", huf_path);

    printf("Archivo: %s  (bloques de %u KB, %d núcleos)\n", path, bs / 1024, default_threads());
    printf("  %-12s %12s %8s %14s %8s  %s\n",
           "modo", "comprimir", "speedup", "descomprimir", "speedup", "resultado");

    double t0 = now_ms();
    if (huf_compress_file(path, huf_path, bs, NULL, NULL) != 0) return 1;
    double tc_seq = now_ms() - t0;
    t0 = now_ms();
    if (huf_decompress_file(huf_path, out_path, NULL, NULL) != 0) return 1;
    double tx_seq = now_ms() - t0;
    int ok = same_file(path, out_path);
    printf("  %-12s %9.1f ms %7.2fx %11.1f ms %7.2fx  %s\n", "secuencial",
           tc_seq, 1.0, tx_seq, 1.0, ok ? "idéntico" : "DISTINTO");

    int threads[] = { 1, 2, 4, 8, 16 };
    int all_ok = ok;
    for (int i = 0; i < 5; i++) {
        ThreadPool *p = pool_create(threads[i], 2 * threads[i]);
        if (!p) return 1;
        t0 = now_ms();
        int rc = huf_compress_file_mt(path, huf_path, bs, p, NULL);
        double tc = now_ms() - t0;
        t0 = now_ms();
        rc |= huf_decompress_file_mt(huf_path, out_path, p, NULL);
        double tx = now_ms() - t0;
        pool_destroy(p);
        ok = rc == 0 && same_file(path, out_path);
        all_ok &= ok;
        char name[32];
        snprintf(name, sizeof name, "%d hilo%s", threads[i], threads[i] > 1 ? "s" : "");
        printf("  %-12s %9.1f ms %7.2fx %11.1f ms %7.2fx  %s\n", name,
               tc, tc_seq / tc, tx, tx_seq / tx, ok ? "idéntico" : "DISTINTO");
    }
    unlink(huf_path);
    unlink(out_path);
    return all_ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "bench") == 0) {
        uint32_t bs = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : 0;
        return bench(argv[2], bs ? bs : HUF_DEFAULT_BLOCK);
    }
    if (argc < 4) return usage(argv[0]);

    const char *cmd = argv[1];
    int rc;
    if (strcmp(cmd, "c") == 0) {
        uint32_t bs = argc >= 5 ? (uint32_t)strtoul(argv[4], NULL, 10) : 0;
        rc = huf_compress_file(argv[2], argv[3], bs, progreso, NULL);
    } else if (strcmp(cmd, "x") == 0) {
        rc = huf_decompress_file(argv[2], argv[3], progreso, NULL);
    } else if (strcmp(cmd, "pc") == 0 || strcmp(cmd, "px") == 0) {
        int n = argc >= 5 ? atoi(argv[4]) : 0;
        if (n <= 0) n = default_threads();
        ThreadPool *p = pool_create(n, 2 * n);
        if (!p) return 1;
        HufHooks hooks = { progreso, NULL, NULL };
        if (cmd[1] == 'c') {
            uint32_t bs = argc >= 6 ? (uint32_t)strtoul(argv[5], NULL, 10) : 0;
            rc = huf_compress_file_mt(argv[2], argv[3], bs, p, &hooks);
        } else {
            rc = huf_decompress_file_mt(argv[2], argv[3], p, &hooks);
        }
        pool_destroy(p);
    } else {
        return usage(argv[0]);
    }
    fprintf(stderr, "\n");
    return rc == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------ */
static int same_file(const char *a, const char *b) {
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY);
    int same = fa != -1 && fb != -1;
    static uint8_t ba[1 << 16], bb[1 << 16];
    while (same) {
        ssize_t ra = read(fa, ba, sizeof ba), rb = read(fb, bb, sizeof bb);
        if (ra != rb || ra < 0 || memcmp(ba, bb, (size_t)ra) != 0) same = 0;
        else if (ra == 0) break;
    }
    if (fa != -1) close(fa);
    if (fb != -1) close(fb);
    return same;
}
