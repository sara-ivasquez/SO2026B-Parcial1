#define _POSIX_C_SOURCE 200809L
#include "huf_parallel.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>     /* SOLO fprintf(stderr) para mensajes; NO se usa para archivos */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Lee exactamente 'count' bytes desde 'offset' con pread (maneja lecturas parciales y
 * EINTR). pread no usa ni mueve el offset compartido del fd, por eso varios hilos
 * pueden leer el MISMO fd a la vez sin pisarse (con read()+lseek() habría una carrera
 * entre el lseek de un hilo y el read de otro).
 * Retorna 0 si leyó todo, -1 con errno en error (EIO si el archivo se acabó antes). */
static int pread_full(int fd, void *buf, size_t count, uint64_t offset) {
    size_t got = 0;
    while (got < count) {
        ssize_t r = pread(fd, (char *)buf + got, count - got, (off_t)(offset + got));
        if (r == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) { errno = EIO; return -1; }   /* archivo más corto de lo esperado */
        got += (size_t)r;
    }
    return 0;
}

/* ====================================================================================
 * FASE 3: conteo de frecuencias en paralelo
 * ==================================================================================== */

/* Estado compartido por TODAS las tareas de un conteo. */
typedef struct {
    pthread_mutex_t mutex;   /* protege freq y error */
    uint64_t       *freq;    /* histograma global (resultado) */
    int             error;   /* primer errno que ocurra; 0 = sin error */
} CountShared;

/* Lo que necesita UNA tarea: qué bloque leer y dónde dejar el resultado. */
typedef struct {
    int          fd;
    uint64_t     offset;
    uint32_t     len;
    CountShared *sh;
} CountTask;

static void count_task(void *arg) {
    CountTask   *t  = arg;
    CountShared *sh = t->sh;

    /* Si otra tarea ya falló, no vale la pena seguir leyendo (cancelación cooperativa:
     * la tarea revisa una bandera, nadie la mata a la fuerza). */
    pthread_mutex_lock(&sh->mutex);
    int abort_now = sh->error != 0;
    pthread_mutex_unlock(&sh->mutex);
    if (abort_now) return;

    int err = 0;
    uint64_t local[HUF_NSYM] = {0};          /* histograma LOCAL: solo lo toca este hilo */
    uint8_t *buf = malloc(t->len);           /* buffer PROPIO: solo lo toca este hilo   */

    if (!buf) err = ENOMEM;
    else if (pread_full(t->fd, buf, t->len, t->offset) == -1) err = errno;
    else huf_count_block(buf, t->len, local);   /* trabajo pesado, sin ningún lock */
    free(buf);

    /* Reducción: UNA sola sección crítica por bloque (256 sumas). */
    pthread_mutex_lock(&sh->mutex);
    if (err) {
        if (sh->error == 0) sh->error = err;
    } else {
        for (int s = 0; s < HUF_NSYM; s++) sh->freq[s] += local[s];
    }
    pthread_mutex_unlock(&sh->mutex);
}

int huf_count_parallel(int fd, uint64_t total, uint32_t block_size,
                       ThreadPool *pool, uint64_t freq[HUF_NSYM]) {
    memset(freq, 0, HUF_NSYM * sizeof(uint64_t));
    if (!pool || block_size == 0) { errno = EINVAL; return -1; }
    if (total == 0) return 0;

    uint64_t nblocks = (total + block_size - 1) / block_size;
    CountTask *tasks = calloc((size_t)nblocks, sizeof(CountTask));
    if (!tasks) return -1;

    CountShared sh = { .freq = freq, .error = 0 };
    int rc = pthread_mutex_init(&sh.mutex, NULL);
    if (rc != 0) { free(tasks); errno = rc; return -1; }

    for (uint64_t b = 0; b < nblocks; b++) {
        uint64_t off = b * block_size;
        uint64_t len = total - off < block_size ? total - off : block_size;
        tasks[b] = (CountTask){ fd, off, (uint32_t)len, &sh };
        if (pool_submit(pool, count_task, &tasks[b]) != 0) {
            pthread_mutex_lock(&sh.mutex);
            if (sh.error == 0) sh.error = ECANCELED;
            pthread_mutex_unlock(&sh.mutex);
            break;
        }
    }

    /* BARRERA: esperar a que terminen todas las tareas ya enviadas. Es obligatorio
     * incluso si hubo error, porque esas tareas apuntan a 'tasks' y a 'sh', que viven
     * en esta función: liberarlos antes sería usar memoria ya liberada. */
    pool_wait(pool);

    int err = sh.error;          /* ya no hay hilos escribiendo: lectura segura */
    pthread_mutex_destroy(&sh.mutex);
    free(tasks);

    if (err) {
        memset(freq, 0, HUF_NSYM * sizeof(uint64_t));
        errno = err;
        return -1;
    }
    return 0;
}

/* ====================================================================================
 * FASE 4: compresión y descompresión en paralelo con salida ordenada
 * ==================================================================================== */

/* ---------- Helpers de I/O y serialización (iguales a los de huffman.c) ---------- */

static int write_full(int fd, const void *buf, size_t count) {
    size_t done = 0;
    while (done < count) {
        ssize_t w = write(fd, (const char *)buf + done, count - done);
        if (w == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        done += (size_t)w;
    }
    return 0;
}
static void put_u32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint32_t get_u32(const uint8_t *p) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

/* ---------- Cola de salida ordenada ---------- */

/* Un "casillero" por bloque: el worker deja ahí su resultado y el escritor lo recoge. */
typedef struct {
    uint8_t *data;       /* bytes a escribir (malloc del worker; free del escritor) */
    size_t   len;        /* cuántos bytes hay en data                               */
    uint32_t olen;       /* bytes ORIGINALES del bloque (cabecera y progreso)       */
    int      ready;      /* 1 = el worker ya terminó este bloque                    */
} Slot;

typedef struct {
    /* --- Estado compartido: SOLO con 'mutex' tomado --- */
    pthread_mutex_t mutex;
    pthread_cond_t  ready;   /* el escritor duerme aquí esperando slots[next]   */
    pthread_cond_t  space;   /* el coordinador duerme aquí esperando la ventana */
    Slot    *slots;
    uint32_t next;           /* next_to_write: próximo bloque que va al archivo */
    int      error;          /* primer errno que ocurra (0 = todo bien)         */

    /* --- Configuración: se fija ANTES de crear hilos y luego solo se lee --- */
    uint32_t        nblocks;
    int             fd_out;
    int             with_block_header;   /* 1 al comprimir: escribe orig_len|comp_len */
    uint64_t        total;               /* bytes originales totales (progreso)       */
    const HufHooks *hooks;
} OutQueue;

/* Marca un error (solo se guarda el primero) y despierta a TODOS los que esperan,
 * para que nadie se quede dormido para siempre esperando algo que ya no va a llegar. */
static void q_fail(OutQueue *q, int err) {
    pthread_mutex_lock(&q->mutex);
    if (q->error == 0) q->error = err ? err : EIO;
    pthread_cond_broadcast(&q->ready);
    pthread_cond_broadcast(&q->space);
    pthread_mutex_unlock(&q->mutex);
}

static int q_failed(OutQueue *q) {
    pthread_mutex_lock(&q->mutex);
    int e = q->error;
    pthread_mutex_unlock(&q->mutex);
    return e;
}

static int hook_cancelled(const HufHooks *h) {
    return h && h->cancelled && h->cancelled(h->ctx);
}

/* El worker entrega su bloque terminado. */
static void q_deposit(OutQueue *q, uint32_t b, uint8_t *data, size_t len, uint32_t olen) {
    pthread_mutex_lock(&q->mutex);
    if (q->error) {                         /* ya se abortó: nadie va a escribir esto */
        pthread_mutex_unlock(&q->mutex);
        free(data);
        return;
    }
    q->slots[b] = (Slot){ data, len, olen, 1 };
    /* Solo vale la pena despertar al escritor si este es EXACTAMENTE el bloque que está
     * esperando. Si es otro, el escritor lo verá listo cuando le llegue su turno (revisa
     * la condición antes de dormirse), así que despertarlo ahora sería en vano. */
    if (b == q->next) pthread_cond_signal(&q->ready);
    pthread_mutex_unlock(&q->mutex);
}

/* ---------- Hilo escritor: el ÚNICO que escribe en el archivo de salida ---------- */

static void *writer_main(void *arg) {
    OutQueue *q = arg;
    uint64_t done_bytes = 0;     /* variable local: solo la toca este hilo */

    for (;;) {
        pthread_mutex_lock(&q->mutex);
        while (q->next < q->nblocks && !q->slots[q->next].ready && !q->error)
            pthread_cond_wait(&q->ready, &q->mutex);
        if (q->error || q->next == q->nblocks) {
            pthread_mutex_unlock(&q->mutex);
            break;
        }
        Slot s = q->slots[q->next];
        q->slots[q->next].data = NULL;      /* el escritor ahora es dueño del buffer */
        pthread_mutex_unlock(&q->mutex);

        /* La escritura a disco se hace SIN el mutex: mientras se escribe, los workers
         * siguen depositando bloques sin esperar. */
        int err = 0;
        if (q->with_block_header) {
            uint8_t bh[8];
            put_u32(bh, s.olen);
            put_u32(bh + 4, (uint32_t)s.len);
            if (write_full(q->fd_out, bh, 8) == -1) err = errno;
        }
        if (!err && write_full(q->fd_out, s.data, s.len) == -1) err = errno;
        free(s.data);
        if (err) {
            fprintf(stderr, "huffman: write: %s\n", strerror(err));
            q_fail(q, err);
            break;
        }

        pthread_mutex_lock(&q->mutex);
        q->next++;
        pthread_cond_signal(&q->space);     /* se abrió un lugar en la ventana */
        pthread_mutex_unlock(&q->mutex);

        done_bytes += s.olen;
        if (q->hooks && q->hooks->progress)
            q->hooks->progress(done_bytes, q->total, q->hooks->ctx);
        if (hook_cancelled(q->hooks)) { q_fail(q, ECANCELED); break; }
    }
    return NULL;
}

/* ---------- Tareas de los workers ---------- */

typedef struct {
    OutQueue       *q;
    uint32_t        b;          /* índice del bloque                         */
    int             fd_in;
    uint64_t        in_off;     /* dónde empieza el bloque en el archivo     */
    uint32_t        in_len;     /* cuántos bytes leer                        */
    uint32_t        olen;       /* bytes originales del bloque               */
    const HufCodes *codes;      /* comprimir: tabla (solo lectura)           */
    const HufTree  *tree;       /* descomprimir: árbol (solo lectura)        */
} BlockTask;

static void encode_task(void *arg) {
    BlockTask *t = arg;
    if (q_failed(t->q)) return;

    uint8_t *in = malloc(t->in_len), *out = NULL;
    size_t clen = 0;
    int err = 0;
    if (!in) err = ENOMEM;
    else if (pread_full(t->fd_in, in, t->in_len, t->in_off) == -1) err = errno;
    else if (huf_encode_block(t->codes, in, t->in_len, &out, &clen) == -1) err = EIO;
    free(in);

    if (err) {
        if (err == EIO) fprintf(stderr, "huffman: el archivo cambió durante la compresión\n");
        q_fail(t->q, err);
        return;
    }
    q_deposit(t->q, t->b, out, clen, t->olen);
}

static void decode_task(void *arg) {
    BlockTask *t = arg;
    if (q_failed(t->q)) return;

    uint8_t *in = malloc(t->in_len ? t->in_len : 1), *out = malloc(t->olen);
    int err = 0;
    if (!in || !out) err = ENOMEM;
    else if (pread_full(t->fd_in, in, t->in_len, t->in_off) == -1) err = errno;
    else if (huf_decode_block(t->tree, in, t->in_len, out, t->olen) == -1) {
        fprintf(stderr, "huffman: bloque %u corrupto\n", t->b);
        err = EBADMSG;
    }
    free(in);

    if (err) { free(out); q_fail(t->q, err); return; }
    q_deposit(t->q, t->b, out, t->olen, t->olen);
}

/* ---------- Coordinador: reparte bloques, controla la ventana y espera ---------- */

static int run_pipeline(OutQueue *q, ThreadPool *pool, BlockTask *tasks,
                        pool_task_fn fn) {
    pthread_t writer;
    int rc = pthread_create(&writer, NULL, writer_main, q);
    if (rc != 0) {
        fprintf(stderr, "huffman: pthread_create: %s\n", strerror(rc));
        return rc;
    }

    /* Ventana: como máximo VENTANA bloques "en vuelo" (encolados, procesándose o
     * esperando a ser escritos). Acota la memoria a ~VENTANA * block_size. */
    uint32_t window = (uint32_t)(4 * pool_size(pool));
    if (window < 4) window = 4;

    for (uint32_t b = 0; b < q->nblocks; b++) {
        pthread_mutex_lock(&q->mutex);
        while (b >= q->next + window && !q->error)
            pthread_cond_wait(&q->space, &q->mutex);
        int stop = q->error;
        pthread_mutex_unlock(&q->mutex);
        if (stop) break;

        if (hook_cancelled(q->hooks)) { q_fail(q, ECANCELED); break; }
        if (pool_submit(pool, fn, &tasks[b]) != 0) { q_fail(q, ECANCELED); break; }
    }

    /* Primero esperar a los workers (pueden estar usando 'tasks' y 'q'), luego al
     * escritor. Después de ambos ya no queda ningún hilo tocando estas estructuras. */
    pool_wait(pool);
    rc = pthread_join(writer, NULL);
    if (rc != 0) fprintf(stderr, "huffman: pthread_join: %s\n", strerror(rc));

    /* Si se abortó, pueden quedar bloques terminados que nadie escribió. */
    for (uint32_t b = 0; b < q->nblocks; b++) free(q->slots[b].data);

    if (q->error) return q->error;
    return q->next == q->nblocks ? 0 : EIO;
}

/* Inicializa la cola (todo menos los campos de configuración). */
static int q_init(OutQueue *q, uint32_t nblocks) {
    memset(q, 0, sizeof(*q));
    q->nblocks = nblocks;
    q->slots = calloc(nblocks ? nblocks : 1, sizeof(Slot));
    if (!q->slots) return ENOMEM;
    int rc;
    if ((rc = pthread_mutex_init(&q->mutex, NULL)) != 0) goto e0;
    if ((rc = pthread_cond_init(&q->ready, NULL)) != 0) goto e1;
    if ((rc = pthread_cond_init(&q->space, NULL)) != 0) goto e2;
    return 0;
e2: pthread_cond_destroy(&q->ready);
e1: pthread_mutex_destroy(&q->mutex);
e0: free(q->slots);
    q->slots = NULL;
    return rc;
}

static void q_destroy(OutQueue *q) {
    if (!q->slots) return;
    pthread_cond_destroy(&q->space);
    pthread_cond_destroy(&q->ready);
    pthread_mutex_destroy(&q->mutex);
    free(q->slots);
    q->slots = NULL;
}

/* ---------- API pública ---------- */

int huf_compress_file_mt(const char *in_path, const char *out_path, uint32_t block_size,
                         ThreadPool *pool, const HufHooks *hooks) {
    int err = 0, fin = -1, fout = -1, q_ready = 0, out_reg = 0;
    BlockTask *tasks = NULL;
    OutQueue q;
    uint64_t freq[HUF_NSYM];
    HufTree tree;
    HufCodes codes;
    struct stat st;

    if (!pool) { errno = EINVAL; return -1; }
    if (block_size == 0) block_size = HUF_DEFAULT_BLOCK;
    if (block_size > HUF_MAX_BLOCK) {
        fprintf(stderr, "huffman: block_size máximo %u\n", HUF_MAX_BLOCK);
        errno = EINVAL;
        return -1;
    }

    fin = open(in_path, O_RDONLY);
    if (fin == -1) {
        err = errno;
        fprintf(stderr, "huffman: open('%s'): %s\n", in_path, strerror(err));
        goto done;
    }
    if (fstat(fin, &st) == -1) { err = errno; perror("huffman: fstat"); goto done; }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "huffman: '%s' no es un archivo regular\n", in_path);
        err = EINVAL;
        goto done;
    }
    uint64_t total = (uint64_t)st.st_size;
    uint64_t nblocks64 = (total + block_size - 1) / block_size;
    if (nblocks64 > 0xFFFFFFFFu) { fprintf(stderr, "huffman: demasiados bloques\n"); err = EFBIG; goto done; }
    uint32_t nblocks = (uint32_t)nblocks64;

    /* ---- Fase 3: frecuencias en paralelo (con barrera interna) ---- */
    if (huf_count_parallel(fin, total, block_size, pool, freq) == -1) {
        err = errno;
        fprintf(stderr, "huffman: conteo: %s\n", strerror(err));
        goto done;
    }
    /* ---- Árbol global: un solo hilo, una sola vez ---- */
    if (huf_build_tree(freq, &tree, &codes) == -1) { err = EOVERFLOW; goto done; }

    /* ---- Cabecera (idéntica a la del compresor secuencial) ---- */
    fout = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fout == -1) {
        err = errno;
        fprintf(stderr, "huffman: open('%s'): %s\n", out_path, strerror(err));
        goto done;
    }
    {   /* solo se borra al fallar si es un archivo normal (no p.ej. /dev/full) */
        struct stat so;
        out_reg = fstat(fout, &so) == 0 && S_ISREG(so.st_mode);
    }
    uint8_t hdr[HUF_HEADER_SIZE];
    memcpy(hdr, HUF_MAGIC, 4);
    put_u64(hdr + 4, total);
    put_u32(hdr + 12, block_size);
    put_u32(hdr + 16, nblocks);
    for (int s = 0; s < HUF_NSYM; s++) put_u64(hdr + 20 + 8 * s, freq[s]);
    if (write_full(fout, hdr, sizeof(hdr)) == -1) { err = errno; perror("huffman: write"); goto done; }

    /* ---- Fase 4: codificar en paralelo + escritor ordenado ---- */
    if ((err = q_init(&q, nblocks)) != 0) goto done;
    q_ready = 1;
    q.fd_out = fout;
    q.with_block_header = 1;
    q.total = total;
    q.hooks = hooks;

    tasks = calloc(nblocks ? nblocks : 1, sizeof(BlockTask));
    if (!tasks) { err = ENOMEM; goto done; }
    for (uint32_t b = 0; b < nblocks; b++) {
        uint64_t off = (uint64_t)b * block_size;
        uint32_t len = (uint32_t)(total - off < block_size ? total - off : block_size);
        tasks[b] = (BlockTask){ &q, b, fin, off, len, len, &codes, NULL };
    }
    err = run_pipeline(&q, pool, tasks, encode_task);

done:
    free(tasks);
    if (q_ready) q_destroy(&q);
    if (fin != -1) close(fin);
    if (fout != -1 && close(fout) == -1 && !err) { err = errno; perror("huffman: close"); }
    if (err) {
        if (out_reg) unlink(out_path);       /* no dejar un .huf a medias */
        errno = err;
        return -1;
    }
    return 0;
}

int huf_decompress_file_mt(const char *in_path, const char *out_path,
                           ThreadPool *pool, const HufHooks *hooks) {
    int err = 0, fin = -1, fout = -1, q_ready = 0, out_reg = 0;
    BlockTask *tasks = NULL;
    OutQueue q;
    HufTree tree;
    struct stat st;

    if (!pool) { errno = EINVAL; return -1; }

    fin = open(in_path, O_RDONLY);
    if (fin == -1) {
        err = errno;
        fprintf(stderr, "huffman: open('%s'): %s\n", in_path, strerror(err));
        goto done;
    }
    if (fstat(fin, &st) == -1) { err = errno; perror("huffman: fstat"); goto done; }
    uint64_t fsize = (uint64_t)st.st_size;

    /* ---- Cabecera ---- */
    uint8_t hdr[HUF_HEADER_SIZE];
    if (fsize < sizeof(hdr) || pread_full(fin, hdr, sizeof(hdr), 0) == -1 ||
        memcmp(hdr, HUF_MAGIC, 4) != 0) {
        fprintf(stderr, "huffman: '%s' no es un archivo .huf válido\n", in_path);
        err = EBADMSG;
        goto done;
    }
    uint64_t total = get_u64(hdr + 4);
    uint32_t block_size = get_u32(hdr + 12);
    uint32_t nblocks = get_u32(hdr + 16);
    uint64_t freq[HUF_NSYM], fsum = 0;
    for (int s = 0; s < HUF_NSYM; s++) { freq[s] = get_u64(hdr + 20 + 8 * s); fsum += freq[s]; }
    if (block_size == 0 || block_size > HUF_MAX_BLOCK ||
        fsum != total || (total + block_size - 1) / block_size != nblocks) {
        fprintf(stderr, "huffman: cabecera inconsistente\n");
        err = EBADMSG;
        goto done;
    }
    if (huf_build_tree(freq, &tree, NULL) == -1) { err = EBADMSG; goto done; }

    /* ---- Índice: una pasada rápida (solo cabeceras de 8 bytes) para saber dónde
     *      empieza cada bloque. Así cada worker puede leer SU bloque con pread. ---- */
    tasks = calloc(nblocks ? nblocks : 1, sizeof(BlockTask));
    if (!tasks) { err = ENOMEM; goto done; }
    if ((err = q_init(&q, nblocks)) != 0) goto done;
    q_ready = 1;

    uint64_t off = HUF_HEADER_SIZE, sum = 0;
    uint64_t ccap = (uint64_t)block_size * 8 + 8;   /* cota: 64 bits por byte */
    for (uint32_t b = 0; b < nblocks; b++) {
        uint8_t bh[8];
        if (off + 8 > fsize || pread_full(fin, bh, 8, off) == -1) {
            fprintf(stderr, "huffman: archivo truncado (bloque %u)\n", b);
            err = EBADMSG;
            goto done;
        }
        uint32_t olen = get_u32(bh), clen = get_u32(bh + 4);
        if (olen == 0 || olen > block_size || clen > ccap) {
            fprintf(stderr, "huffman: bloque %u corrupto\n", b);
            err = EBADMSG;
            goto done;
        }
        if (off + 8 + clen > fsize) {
            fprintf(stderr, "huffman: archivo truncado (bloque %u)\n", b);
            err = EBADMSG;
            goto done;
        }
        tasks[b] = (BlockTask){ &q, b, fin, off + 8, clen, olen, NULL, &tree };
        sum += olen;
        off += 8 + (uint64_t)clen;
    }
    if (sum != total) { fprintf(stderr, "huffman: tamaño final no coincide\n"); err = EBADMSG; goto done; }

    fout = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fout == -1) {
        err = errno;
        fprintf(stderr, "huffman: open('%s'): %s\n", out_path, strerror(err));
        goto done;
    }
    {   /* solo se borra al fallar si es un archivo normal (no p.ej. /dev/full) */
        struct stat so;
        out_reg = fstat(fout, &so) == 0 && S_ISREG(so.st_mode);
    }
    q.fd_out = fout;
    q.with_block_header = 0;
    q.total = total;
    q.hooks = hooks;
    err = run_pipeline(&q, pool, tasks, decode_task);

done:
    free(tasks);
    if (q_ready) q_destroy(&q);
    if (fin != -1) close(fin);
    if (fout != -1 && close(fout) == -1 && !err) { err = errno; perror("huffman: close"); }
    if (err) {
        if (out_reg) unlink(out_path);
        errno = err;
        return -1;
    }
    return 0;
}
