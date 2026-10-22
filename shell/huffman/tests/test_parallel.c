#define _POSIX_C_SOURCE 200809L
#include "huf_parallel.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Pruebas de la Fase 4 (compresión y descompresión en paralelo).
 *
 *   ./test_parallel         completo
 *   ./test_parallel quick   archivos pequeños (para helgrind / tsan)
 *
 * 1. Para muchos tamaños de archivo, de bloque y cantidades de hilos:
 *      - el .huf paralelo es IDÉNTICO byte a byte al secuencial (salida en orden), y
 *      - descomprimir en paralelo devuelve exactamente el archivo original.
 * 2. El progreso llega en orden, una vez por bloque, y termina en el 100 %.
 * 3. Cancelar a mitad de camino: retorna -1 (ECANCELED) y no deja salida parcial.
 * 4. Archivos .huf dañados se rechazan sin dejar salida parcial.
 */

static int pass = 0, fail = 0;
#define CHECK(cond, ...) do {                                   \
        if (cond) { printf("  OK    "); pass++; }               \
        else      { printf("  FALLO "); fail++; }               \
        printf(__VA_ARGS__); printf("\n");                      \
    } while (0)

static char dir[] = "/tmp/test_parallelXXXXXX";
static char p_in[64], p_ref[64], p_mt[64], p_out[64];

static int make_file(const char *path, size_t n, unsigned seed, int zeros) {
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
            buf[i] = zeros ? 0 : ((done + i) % 2) ? (uint8_t)(seed >> 16)
                                                   : (uint8_t)txt[(done + i) % tl];
        }
        if (write(fd, buf, k) != (ssize_t)k) { close(fd); return -1; }
        done += k;
    }
    return close(fd);
}

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

static int exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

/* ---------- Ganchos de prueba ----------
 * progress() lo llama el hilo escritor y cancelled() lo llaman el escritor Y el
 * coordinador, así que el contexto compartido se protege con un mutex (si no, la
 * propia prueba tendría una condición de carrera y tsan/helgrind la reportarían). */
typedef struct {
    pthread_mutex_t mtx;
    int      calls;          /* cuántas veces se llamó progress */
    uint64_t last;           /* último valor de 'done'           */
    int      monotonic;      /* 1 si siempre fue creciente        */
    int      cancel_after;   /* cancelar después de N bloques (0 = nunca) */
} Probe;

static void probe_progress(uint64_t done, uint64_t total, void *ctx) {
    (void)total;
    Probe *pr = ctx;
    pthread_mutex_lock(&pr->mtx);
    if (done <= pr->last && pr->calls > 0) pr->monotonic = 0;
    pr->last = done;
    pr->calls++;
    pthread_mutex_unlock(&pr->mtx);
}

static int probe_cancelled(void *ctx) {
    Probe *pr = ctx;
    pthread_mutex_lock(&pr->mtx);
    int c = pr->cancel_after > 0 && pr->calls >= pr->cancel_after;
    pthread_mutex_unlock(&pr->mtx);
    return c;
}

static void probe_init(Probe *pr, int cancel_after) {
    memset(pr, 0, sizeof *pr);
    pthread_mutex_init(&pr->mtx, NULL);
    pr->monotonic = 1;
    pr->cancel_after = cancel_after;
}

/* Escribe 4 bytes little-endian en 'off' de un archivo existente. */
static void poke_u32(const char *path, off_t off, uint32_t v) {
    int fd = open(path, O_WRONLY);
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    if (pwrite(fd, b, 4, off) != 4) perror("pwrite");
    close(fd);
}

int main(int argc, char **argv) {
    int quick = argc >= 2 && strcmp(argv[1], "quick") == 0;
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(p_in,  sizeof p_in,  "%s/in",      dir);
    snprintf(p_ref, sizeof p_ref, "%s/ref.huf", dir);
    snprintf(p_mt,  sizeof p_mt,  "%s/mt.huf",  dir);
    snprintf(p_out, sizeof p_out, "%s/out",     dir);

    /* ------------------------------------------------------------------ 1 */
    printf("-- paralelo == secuencial (byte a byte) y descompresión idéntica --\n");
    struct { size_t n; int zeros; } files_full[] = {
        {0,0}, {1,0}, {4095,0}, {4096,0}, {4097,0}, {1000003,0}, {1000000,1}, {5u<<20,0} };
    struct { size_t n; int zeros; } files_quick[] = {
        {0,0}, {1,0}, {4097,0}, {60001,0}, {20000,1} };
    int nfiles = quick ? 5 : 8;
    uint32_t blocks[] = { 1000, 4096, 65536, HUF_DEFAULT_BLOCK };
    int threads[] = { 1, 4, 16 };

    for (int fi = 0; fi < nfiles; fi++) {
        size_t n = quick ? files_quick[fi].n : files_full[fi].n;
        int zeros = quick ? files_quick[fi].zeros : files_full[fi].zeros;
        make_file(p_in, n, (unsigned)fi + 11, zeros);
        int ok = 1, combos = 0;
        for (int bi = 0; bi < 4; bi++) {
            if (huf_compress_file(p_in, p_ref, blocks[bi], NULL, NULL) != 0) ok = 0;
            for (int ti = 0; ti < 3; ti++) {
                ThreadPool *p = pool_create(threads[ti], 2 * threads[ti]);
                ok = ok && p
                     && huf_compress_file_mt(p_in, p_mt, blocks[bi], p, NULL) == 0
                     && same_file(p_ref, p_mt)
                     && huf_decompress_file_mt(p_mt, p_out, p, NULL) == 0
                     && same_file(p_in, p_out);
                pool_destroy(p);
                combos++;
            }
        }
        CHECK(ok, "%8zu bytes%s  (%d combinaciones de bloque x hilos)",
              n, zeros ? " (un solo símbolo)" : "", combos);
    }

    ThreadPool *pool = pool_create(4, 8);
    size_t big = quick ? 200000 : 3000000;
    make_file(p_in, big, 99, 0);

    /* ------------------------------------------------------------------ 2 */
    printf("-- progreso --\n");
    {
        Probe pr; probe_init(&pr, 0);
        HufHooks h = { probe_progress, probe_cancelled, &pr };
        uint32_t bs = 4096;
        int rc = huf_compress_file_mt(p_in, p_mt, bs, pool, &h);
        int nb = (int)((big + bs - 1) / bs);
        CHECK(rc == 0 && pr.calls == nb && pr.last == big && pr.monotonic,
              "comprimir: %d llamadas (una por bloque), creciente, termina en %llu/%zu",
              pr.calls, (unsigned long long)pr.last, big);
        probe_init(&pr, 0);
        rc = huf_decompress_file_mt(p_mt, p_out, pool, &h);
        CHECK(rc == 0 && pr.calls == nb && pr.last == big && pr.monotonic,
              "descomprimir: igual");
    }

    /* ------------------------------------------------------------------ 3 */
    printf("-- cancelación --\n");
    {
        Probe pr; probe_init(&pr, 5);
        HufHooks h = { probe_progress, probe_cancelled, &pr };
        unlink(p_mt);
        int rc = huf_compress_file_mt(p_in, p_mt, 4096, pool, &h);
        int e = errno;
        CHECK(rc == -1 && e == ECANCELED && !exists(p_mt),
              "comprimir cancelado tras 5 bloques -> -1 (ECANCELED), sin .huf parcial");

        huf_compress_file_mt(p_in, p_mt, 4096, pool, NULL);
        probe_init(&pr, 5);
        rc = huf_decompress_file_mt(p_mt, p_out, pool, &h);
        e = errno;
        CHECK(rc == -1 && e == ECANCELED && !exists(p_out),
              "descomprimir cancelado -> -1 (ECANCELED), sin salida parcial");

        rc = huf_compress_file_mt(p_in, p_mt, 4096, pool, NULL);
        CHECK(rc == 0 && huf_decompress_file_mt(p_mt, p_out, pool, NULL) == 0
              && same_file(p_in, p_out), "el pool sigue funcionando después de cancelar");
    }

    /* ------------------------------------------------------------------ 4 */
    printf("-- archivos dañados --\n");
    {
        huf_compress_file_mt(p_in, p_mt, 4096, pool, NULL);
        struct stat st; stat(p_mt, &st);
        char p_bad[80];
        snprintf(p_bad, sizeof p_bad, "%s/bad.huf", dir);

        /* truncado a la mitad */
        int src = open(p_mt, O_RDONLY), dst = open(p_bad, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        static uint8_t buf[1 << 16];
        off_t left = st.st_size / 2;
        while (left > 0) {
            ssize_t r = read(src, buf, left < (off_t)sizeof buf ? (size_t)left : sizeof buf);
            if (r <= 0) break;
            if (write(dst, buf, (size_t)r) != r) break;
            left -= r;
        }
        close(src); close(dst);
        unlink(p_out);
        int rc = huf_decompress_file_mt(p_bad, p_out, pool, NULL);
        CHECK(rc == -1 && errno == EBADMSG && !exists(p_out), ".huf truncado rechazado");

        /* magic incorrecto */
        poke_u32(p_bad, 0, 0x58585858);
        rc = huf_decompress_file_mt(p_bad, p_out, pool, NULL);
        CHECK(rc == -1 && errno == EBADMSG && !exists(p_out), "magic incorrecto rechazado");

        /* cabecera de bloque imposible (orig_len > block_size) */
        huf_compress_file_mt(p_in, p_bad, 4096, pool, NULL);
        poke_u32(p_bad, HUF_HEADER_SIZE, 999999);
        rc = huf_decompress_file_mt(p_bad, p_out, pool, NULL);
        CHECK(rc == -1 && errno == EBADMSG && !exists(p_out), "bloque corrupto rechazado");

        /* entrada inexistente / salida imposible */
        rc = huf_compress_file_mt("/no/existe", p_mt, 0, pool, NULL);
        CHECK(rc == -1 && errno == ENOENT, "entrada inexistente -> -1 (ENOENT)");
        rc = huf_compress_file_mt(p_in, "/no/existe/x.huf", 0, pool, NULL);
        CHECK(rc == -1, "salida en carpeta inexistente -> -1");
        struct stat sf;
        if (stat("/dev/full", &sf) == 0) {
            rc = huf_compress_file_mt(p_in, "/dev/full", 4096, pool, NULL);
            int e = errno;
            CHECK(rc == -1 && e == ENOSPC && stat("/dev/full", &sf) == 0,
                  "disco lleno (/dev/full) -> -1 (ENOSPC) y no borra el dispositivo");
        }
        unlink(p_bad);
    }

    pool_destroy(pool);
    unlink(p_in); unlink(p_ref); unlink(p_mt); unlink(p_out);
    rmdir(dir);
    printf("Resultado: %d OK, %d fallos\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
