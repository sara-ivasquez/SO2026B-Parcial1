#define _POSIX_C_SOURCE 200809L
#include "huf_parallel.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Pruebas de la Fase 3 (conteo de frecuencias en paralelo).
 *
 *   ./test_count            correctitud: histograma paralelo == secuencial para muchos
 *                           tamaños de archivo, de bloque y cantidades de hilos.
 *   ./test_count quick      lo mismo con archivos pequeños (para helgrind / tsan).
 *   ./test_count bench <archivo> [block_size]
 *                           tabla de tiempos: secuencial vs. 1, 2, 4, 8 y 16 hilos.
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

/* Conteo de referencia: secuencial, leyendo con read() bloque a bloque. */
static int count_sequential(int fd, uint32_t bs, uint64_t freq[HUF_NSYM], uint64_t *total) {
    memset(freq, 0, HUF_NSYM * sizeof(uint64_t));
    *total = 0;
    uint8_t *buf = malloc(bs);
    if (!buf) return -1;
    if (lseek(fd, 0, SEEK_SET) == -1) { free(buf); return -1; }
    ssize_t r;
    while ((r = read(fd, buf, bs)) > 0) {
        huf_count_block(buf, (size_t)r, freq);
        *total += (uint64_t)r;
    }
    free(buf);
    return r == -1 ? -1 : 0;
}

/* Crea un archivo de 'n' bytes: mitad texto (distribución sesgada) y mitad aleatoria. */
static int make_file(const char *path, size_t n, unsigned seed) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd == -1) return -1;
    const char *txt = "concurrencia y sincronizacion en sistemas operativos\n";
    size_t tl = strlen(txt);
    uint8_t buf[4096];
    size_t done = 0;
    while (done < n) {
        size_t k = n - done < sizeof(buf) ? n - done : sizeof(buf);
        for (size_t i = 0; i < k; i++) {
            seed = seed * 1103515245u + 12345u;
            buf[i] = ((done + i) % 2) ? (uint8_t)(seed >> 16) : (uint8_t)txt[(done + i) % tl];
        }
        if (write(fd, buf, k) != (ssize_t)k) { close(fd); return -1; }
        done += k;
    }
    return close(fd);
}

static int same(const uint64_t a[HUF_NSYM], const uint64_t b[HUF_NSYM]) {
    return memcmp(a, b, HUF_NSYM * sizeof(uint64_t)) == 0;
}

/* ------------------------------------------------------------------------------------ */

static int run_tests(int quick) {
    char dir[] = "/tmp/test_countXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char path[64];
    snprintf(path, sizeof path, "%s/f.bin", dir);

    size_t sizes_full[]  = { 0, 1, 4095, 4096, 4097, 1000003, 5u << 20 };
    size_t sizes_quick[] = { 0, 1, 4095, 4096, 4097, 100003 };
    size_t *sizes = quick ? sizes_quick : sizes_full;
    int nsizes = quick ? 6 : 7;
    uint32_t blocks[] = { 1000, 4096, 65536, HUF_DEFAULT_BLOCK };
    int threads[] = { 1, 4, 16 };

    printf("-- histograma paralelo == secuencial --\n");
    for (int si = 0; si < nsizes; si++) {
        if (make_file(path, sizes[si], (unsigned)si + 7) == -1) { perror("make_file"); return 1; }
        int fd = open(path, O_RDONLY);
        uint64_t ref[HUF_NSYM], got[HUF_NSYM], total;
        count_sequential(fd, 4096, ref, &total);

        int ok = total == sizes[si], combos = 0;
        for (int bi = 0; bi < 4; bi++)
            for (int ti = 0; ti < 3; ti++) {
                ThreadPool *p = pool_create(threads[ti], 8);
                ok = ok && p && huf_count_parallel(fd, total, blocks[bi], p, got) == 0
                        && same(ref, got);
                pool_destroy(p);
                combos++;
            }
        CHECK(ok, "%8zu bytes  (%d combinaciones de bloque x hilos)", sizes[si], combos);
        close(fd);
    }

    printf("-- el offset del fd no se modifica (usa pread) --\n");
    {
        make_file(path, 50000, 3);
        int fd = open(path, O_RDONLY);
        lseek(fd, 123, SEEK_SET);
        uint64_t got[HUF_NSYM];
        ThreadPool *p = pool_create(4, 8);
        huf_count_parallel(fd, 50000, 4096, p, got);
        pool_destroy(p);
        CHECK(lseek(fd, 0, SEEK_CUR) == 123, "offset sigue en 123");
        close(fd);
    }

    printf("-- errores --\n");
    {
        make_file(path, 10000, 5);
        int fd = open(path, O_RDONLY);
        uint64_t got[HUF_NSYM];
        ThreadPool *p = pool_create(4, 8);
        /* Le decimos que el archivo mide 20000 pero solo tiene 10000 (como si alguien
         * lo hubiera truncado mientras se contaba): debe fallar con EIO. */
        int rc = huf_count_parallel(fd, 20000, 1000, p, got);
        CHECK(rc == -1 && errno == EIO, "archivo más corto de lo esperado -> -1 (EIO)");
        /* El pool sigue usable después de un error. */
        rc = huf_count_parallel(fd, 10000, 1000, p, got);
        CHECK(rc == 0, "el pool sigue funcionando después del error");
        close(fd);
        rc = huf_count_parallel(fd, 10000, 1000, p, got);   /* fd ya cerrado */
        CHECK(rc == -1 && errno == EBADF, "descriptor inválido -> -1 (EBADF)");
        CHECK(huf_count_parallel(0, 10, 0, p, got) == -1 && errno == EINVAL,
              "block_size 0 -> -1 (EINVAL)");
        pool_destroy(p);
    }

    unlink(path);
    rmdir(dir);
    printf("Resultado: %d OK, %d fallos\n", pass, fail);
    return fail == 0 ? 0 : 1;
}

/* ------------------------------------------------------------------------------------ */

static int run_bench(const char *path, uint32_t bs) {
    int fd = open(path, O_RDONLY);
    if (fd == -1) { perror(path); return 1; }

    uint64_t ref[HUF_NSYM], got[HUF_NSYM], total;
    count_sequential(fd, bs, ref, &total);          /* calienta la caché de disco */

    double t0 = now_ms();
    count_sequential(fd, bs, ref, &total);
    double tseq = now_ms() - t0;

    printf("Archivo: %s  (%.1f MB, bloques de %u KB, %ld núcleos)\n",
           path, total / 1048576.0, bs / 1024, sysconf(_SC_NPROCESSORS_ONLN));
    printf("  %-12s %10s %10s  %s\n", "modo", "tiempo", "speedup", "resultado");
    printf("  %-12s %8.1f ms %9.2fx  referencia\n", "secuencial", tseq, 1.0);

    int threads[] = { 1, 2, 4, 8, 16 };
    int all_ok = 1;
    for (int i = 0; i < 5; i++) {
        ThreadPool *p = pool_create(threads[i], 2 * threads[i]);
        t0 = now_ms();
        int rc = huf_count_parallel(fd, total, bs, p, got);
        double dt = now_ms() - t0;
        pool_destroy(p);
        int ok = rc == 0 && same(ref, got);
        all_ok &= ok;
        char name[32];
        snprintf(name, sizeof name, "%d hilo%s", threads[i], threads[i] > 1 ? "s" : "");
        printf("  %-12s %8.1f ms %9.2fx  %s\n", name, dt, tseq / dt, ok ? "igual" : "DISTINTO");
    }
    close(fd);
    return all_ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 3 && strcmp(argv[1], "bench") == 0) {
        uint32_t bs = argc >= 4 ? (uint32_t)strtoul(argv[3], NULL, 10) : HUF_DEFAULT_BLOCK;
        return run_bench(argv[2], bs ? bs : HUF_DEFAULT_BLOCK);
    }
    return run_tests(argc >= 2 && strcmp(argv[1], "quick") == 0);
}
