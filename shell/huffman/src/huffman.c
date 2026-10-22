#define _POSIX_C_SOURCE 200809L
#include "huffman.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>     /* SOLO fprintf(stderr) para mensajes; NO se usa para archivos */
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ====================================================================================
 * Helpers de I/O con syscalls (manejan lecturas/escrituras parciales y EINTR)
 * ==================================================================================== */

/* Lee hasta 'count' bytes; devuelve los leídos (< count solo si hubo EOF) o -1. */
static ssize_t read_full(int fd, void *buf, size_t count) {
    size_t got = 0;
    while (got < count) {
        ssize_t r = read(fd, (char *)buf + got, count - got);
        if (r == -1) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

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

/* 1 si fd es un archivo regular. Al fallar solo se borra la salida si es un archivo
 * normal que creamos: si alguien pasa un dispositivo como salida (p.ej. /dev/full),
 * unlink() borraría el dispositivo en vez de un .huf a medias. */
static int es_regular(int fd) {
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
}

/* Serialización little-endian explícita (independiente de la arquitectura). */
static void put_u32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_u64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
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

/* ====================================================================================
 * Conteo de frecuencias
 * ==================================================================================== */

void huf_count_block(const uint8_t *data, size_t n, uint64_t freq[HUF_NSYM]) {
    for (size_t i = 0; i < n; i++) freq[data[i]]++;
}

/* ====================================================================================
 * Construcción del árbol
 * ====================================================================================
 * Con a lo sumo 256 símbolos activos basta una selección lineal de los 2 menores en
 * cada paso (O(256^2) = 65 mil operaciones, despreciable frente a comprimir datos) y
 * evita escribir un min-heap con su posible fuente de bugs.
 *
 * DESEMPATE DETERMINISTA: el orden es (peso, menor símbolo del subárbol). Así el
 * compresor y el descompresor, que reconstruye desde freq[], obtienen el MISMO árbol.
 */
static int gen_codes(const HufTree *t, int node, uint64_t code, int len, HufCodes *c) {
    if (node < 0) return 0;
    if (node < HUF_NSYM) {                 /* hoja */
        if (len > HUF_MAX_CODE_LEN) return -1;
        c->code[node] = code;
        c->len[node] = (uint8_t)len;
        return 0;
    }
    if (len + 1 > HUF_MAX_CODE_LEN) return -1;
    if (gen_codes(t, t->left[node],  code << 1,       len + 1, c) == -1) return -1;
    if (gen_codes(t, t->right[node], (code << 1) | 1, len + 1, c) == -1) return -1;
    return 0;
}

int huf_build_tree(const uint64_t freq[HUF_NSYM], HufTree *t, HufCodes *codes) {
    uint64_t w[HUF_MAX_NODES];
    int ms[HUF_MAX_NODES];     /* menor símbolo contenido en el subárbol (desempate) */
    int act[HUF_NSYM];
    int na = 0, next = HUF_NSYM;

    for (int i = 0; i < HUF_MAX_NODES; i++) { t->left[i] = -1; t->right[i] = -1; }
    if (codes) memset(codes, 0, sizeof(*codes));

    for (int s = 0; s < HUF_NSYM; s++) {
        if (freq[s] > 0) { w[s] = freq[s]; ms[s] = s; act[na++] = s; }
    }
    t->nsyms = na;
    t->root = -1;
    if (na == 0) return 0;                 /* archivo vacío */

    if (na == 1) {
        /* Un solo símbolo: sin esto el código tendría longitud 0 y no habría bits que
         * decodificar. Se crea una raíz con un único hijo izquierdo => código "0". */
        t->left[HUF_NSYM] = act[0];
        t->right[HUF_NSYM] = -1;
        t->root = HUF_NSYM;
    } else {
        while (na > 1) {
            int a = 0, b = -1;             /* a = menor, b = segundo menor */
            for (int i = 1; i < na; i++) {
                int x = act[i], y = act[a];
                if (w[x] < w[y] || (w[x] == w[y] && ms[x] < ms[y])) a = i;
            }
            for (int i = 0; i < na; i++) {
                if (i == a) continue;
                if (b == -1) { b = i; continue; }
                int x = act[i], y = act[b];
                if (w[x] < w[y] || (w[x] == w[y] && ms[x] < ms[y])) b = i;
            }
            int na_, nb_ = act[b];
            na_ = act[a];
            int node = next++;
            t->left[node] = na_;
            t->right[node] = nb_;
            w[node] = w[na_] + w[nb_];
            ms[node] = ms[na_] < ms[nb_] ? ms[na_] : ms[nb_];
            act[a] = node;
            act[b] = act[na - 1];
            na--;
        }
        t->root = act[0];
    }

    if (codes && gen_codes(t, t->root, 0, 0, codes) == -1) {
        fprintf(stderr, "huffman: código más largo que %d bits\n", HUF_MAX_CODE_LEN);
        return -1;
    }
    return 0;
}

/* ====================================================================================
 * Codificación / decodificación de un bloque
 * ==================================================================================== */

int huf_encode_block(const HufCodes *c, const uint8_t *in, size_t n,
                     uint8_t **out, size_t *out_len) {
    uint64_t bits = 0;
    for (size_t i = 0; i < n; i++) {
        if (c->len[in[i]] == 0) {          /* byte que no estaba en la tabla */
            fprintf(stderr, "huffman: byte %u sin código (tabla inconsistente)\n", in[i]);
            return -1;
        }
        bits += c->len[in[i]];
    }
    size_t cap = (size_t)((bits + 7) / 8);
    uint8_t *buf = malloc(cap ? cap : 1);
    if (!buf) { perror("huffman: malloc"); return -1; }

    size_t o = 0;
    uint8_t cur = 0;
    int used = 0;                          /* bits ya ocupados en 'cur' (0..7) */
    for (size_t i = 0; i < n; i++) {
        uint64_t code = c->code[in[i]];
        int len = c->len[in[i]];
        while (len > 0) {                  /* empaqueta MSB primero, por trozos */
            int space = 8 - used;
            int take = len < space ? len : space;
            uint8_t chunk = (uint8_t)((code >> (len - take)) & ((1u << take) - 1u));
            cur |= (uint8_t)(chunk << (space - take));
            used += take;
            len -= take;
            if (used == 8) { buf[o++] = cur; cur = 0; used = 0; }
        }
    }
    if (used > 0) buf[o++] = cur;          /* último byte con relleno de ceros */

    *out = buf;
    *out_len = o;
    return 0;
}

int huf_decode_block(const HufTree *t, const uint8_t *in, size_t comp_len,
                     uint8_t *out, size_t orig_len) {
    if (orig_len == 0) return 0;
    if (t->root < 0) return -1;
    size_t produced = 0;
    int node = t->root;
    for (size_t i = 0; i < comp_len; i++) {
        for (int b = 7; b >= 0; b--) {
            int bit = (in[i] >> b) & 1;
            node = bit ? t->right[node] : t->left[node];
            if (node < 0) return -1;       /* camino inexistente => datos corruptos */
            if (node < HUF_NSYM) {
                out[produced++] = (uint8_t)node;
                node = t->root;
                if (produced == orig_len) return 0;
            }
        }
    }
    return -1;                             /* se acabaron los bits antes de tiempo */
}

/* ====================================================================================
 * Compresión de archivo (secuencial)
 * ====================================================================================
 * Dos pasadas sobre el archivo, ambas en streaming (memoria O(block_size)):
 *   Pasada 1: contar frecuencias de todo el archivo y su tamaño real.
 *   Pasada 2: codificar bloque por bloque y escribir.
 * Si el archivo cambia entre pasadas (p.ej. el usuario lo edita), se detecta porque el
 * total leído difiere, y se aborta en vez de generar un .huf inconsistente.
 */
int huf_compress_file(const char *in_path, const char *out_path, uint32_t block_size,
                      HufProgressFn progress, void *ctx) {
    int rc = -1, fin = -1, fout = -1, out_created = 0;
    uint8_t *inbuf = NULL, *encoded = NULL;
    uint64_t freq[HUF_NSYM] = {0};
    HufTree tree;
    HufCodes codes;

    if (block_size == 0) block_size = HUF_DEFAULT_BLOCK;
    if (block_size > HUF_MAX_BLOCK) {
        fprintf(stderr, "huffman: block_size máximo %u\n", HUF_MAX_BLOCK);
        return -1;
    }

    fin = open(in_path, O_RDONLY);
    if (fin == -1) { fprintf(stderr, "huffman: open('%s'): %s\n", in_path, strerror(errno)); goto done; }

    inbuf = malloc(block_size);
    if (!inbuf) { perror("huffman: malloc"); goto done; }

    /* ---- Pasada 1: frecuencias ---- */
    uint64_t total = 0;
    for (;;) {
        ssize_t r = read_full(fin, inbuf, block_size);
        if (r == -1) { perror("huffman: read"); goto done; }
        if (r == 0) break;
        huf_count_block(inbuf, (size_t)r, freq);
        total += (uint64_t)r;
        if ((size_t)r < block_size) break;
    }
    if (huf_build_tree(freq, &tree, &codes) == -1) goto done;

    uint64_t nblocks64 = (total + block_size - 1) / block_size;
    if (nblocks64 > 0xFFFFFFFFu) { fprintf(stderr, "huffman: demasiados bloques\n"); goto done; }

    /* ---- Cabecera ---- */
    fout = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fout == -1) { fprintf(stderr, "huffman: open('%s'): %s\n", out_path, strerror(errno)); goto done; }
    out_created = es_regular(fout);   /* solo se borra si es un archivo normal */

    uint8_t hdr[HUF_HEADER_SIZE];
    memcpy(hdr, HUF_MAGIC, 4);
    put_u64(hdr + 4, total);
    put_u32(hdr + 12, block_size);
    put_u32(hdr + 16, (uint32_t)nblocks64);
    for (int s = 0; s < HUF_NSYM; s++) put_u64(hdr + 20 + 8 * s, freq[s]);
    if (write_full(fout, hdr, sizeof(hdr)) == -1) { perror("huffman: write"); goto done; }

    /* ---- Pasada 2: codificar y escribir en orden ---- */
    if (lseek(fin, 0, SEEK_SET) == (off_t)-1) { perror("huffman: lseek"); goto done; }
    uint64_t processed = 0;
    for (uint64_t b = 0; b < nblocks64; b++) {
        ssize_t r = read_full(fin, inbuf, block_size);
        if (r == -1) { perror("huffman: read"); goto done; }
        if (r == 0) { fprintf(stderr, "huffman: el archivo cambió durante la compresión\n"); goto done; }

        size_t clen;
        if (huf_encode_block(&codes, inbuf, (size_t)r, &encoded, &clen) == -1) goto done;

        uint8_t bh[8];
        put_u32(bh, (uint32_t)r);
        put_u32(bh + 4, (uint32_t)clen);
        if (write_full(fout, bh, 8) == -1 || write_full(fout, encoded, clen) == -1) {
            perror("huffman: write");
            goto done;
        }
        free(encoded);
        encoded = NULL;

        processed += (uint64_t)r;
        if (progress) progress(processed, total, ctx);
    }
    if (processed != total) { fprintf(stderr, "huffman: el archivo cambió durante la compresión\n"); goto done; }
    rc = 0;

done:
    free(encoded);
    free(inbuf);
    if (fin != -1) close(fin);
    if (fout != -1 && close(fout) == -1 && rc == 0) { perror("huffman: close"); rc = -1; }
    if (rc == -1 && out_created) unlink(out_path);   /* no dejar un .huf a medias */
    return rc;
}

/* ====================================================================================
 * Descompresión de archivo (secuencial)
 * ==================================================================================== */
int huf_decompress_file(const char *in_path, const char *out_path,
                        HufProgressFn progress, void *ctx) {
    int rc = -1, fin = -1, fout = -1, out_created = 0;
    uint8_t *cbuf = NULL, *obuf = NULL;
    HufTree tree;

    fin = open(in_path, O_RDONLY);
    if (fin == -1) { fprintf(stderr, "huffman: open('%s'): %s\n", in_path, strerror(errno)); goto done; }

    uint8_t hdr[HUF_HEADER_SIZE];
    ssize_t r = read_full(fin, hdr, sizeof(hdr));
    if (r == -1) { perror("huffman: read"); goto done; }
    if ((size_t)r != sizeof(hdr) || memcmp(hdr, HUF_MAGIC, 4) != 0) {
        fprintf(stderr, "huffman: '%s' no es un archivo .huf válido\n", in_path);
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
        goto done;
    }
    if (huf_build_tree(freq, &tree, NULL) == -1) goto done;

    fout = open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fout == -1) { fprintf(stderr, "huffman: open('%s'): %s\n", out_path, strerror(errno)); goto done; }
    out_created = es_regular(fout);   /* solo se borra si es un archivo normal */

    size_t ccap = (size_t)block_size * 8 + 8;      /* cota superior: 64 bits por byte */
    cbuf = malloc(ccap);
    obuf = malloc(block_size);
    if (!cbuf || !obuf) { perror("huffman: malloc"); goto done; }

    uint64_t processed = 0;
    for (uint32_t b = 0; b < nblocks; b++) {
        uint8_t bh[8];
        r = read_full(fin, bh, 8);
        if (r == -1) { perror("huffman: read"); goto done; }
        if (r != 8) { fprintf(stderr, "huffman: archivo truncado (bloque %u)\n", b); goto done; }
        uint32_t olen = get_u32(bh), clen = get_u32(bh + 4);
        if (olen == 0 || olen > block_size || clen > ccap) {
            fprintf(stderr, "huffman: bloque %u corrupto\n", b);
            goto done;
        }
        r = read_full(fin, cbuf, clen);
        if (r == -1) { perror("huffman: read"); goto done; }
        if ((size_t)r != clen) { fprintf(stderr, "huffman: archivo truncado (bloque %u)\n", b); goto done; }

        if (huf_decode_block(&tree, cbuf, clen, obuf, olen) == -1) {
            fprintf(stderr, "huffman: bloque %u corrupto\n", b);
            goto done;
        }
        if (write_full(fout, obuf, olen) == -1) { perror("huffman: write"); goto done; }

        processed += olen;
        if (progress) progress(processed, total, ctx);
    }
    if (processed != total) { fprintf(stderr, "huffman: tamaño final no coincide\n"); goto done; }
    rc = 0;

done:
    free(cbuf);
    free(obuf);
    if (fin != -1) close(fin);
    if (fout != -1 && close(fout) == -1 && rc == 0) { perror("huffman: close"); rc = -1; }
    if (rc == -1 && out_created) unlink(out_path);
    return rc;
}
