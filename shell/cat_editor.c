#define _POSIX_C_SOURCE 200809L
#include "shell.h"
#include "huf_job.h"     /* Parcial 2: compresión Huffman en segundo plano */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/**
 * ====================================================================================
 * MÓDULO: Editor de Texto en Unix (cat_editor.c)
 * ====================================================================================
 * Implementa un editor de texto interactivo operado por CLI, integrado como una
 * categoría nueva del shell ("edicion"). El editor abre su propio sub-REPL: una vez
 * invocado con el comando 'editor', el usuario queda dentro de un ciclo de lectura
 * de comandos propios (o, p, a, i, d, s, q) hasta que escribe 'q', momento en el cual
 * el control regresa al prompt principal del shell (eafitOS>).
 *
 * DECISIÓN ARQUITECTÓNICA: Se creó una categoría nueva ("edicion") en lugar de
 * reutilizar "datos", "memoria", "monitoreo" o "utilidades", porque el editor
 * mantiene ESTADO PERSISTENTE (un file descriptor abierto y un buffer de líneas en
 * memoria) a través de múltiples comandos consecutivos, mientras que el resto de
 * comandos del shell son operaciones atómicas de un solo disparo que no requieren
 * mantener contexto entre invocaciones.
 *
 * MODELO DE PERSISTENCIA: cada comando mutador (a, i, d) escribe su cambio EN DISCO
 * de forma inmediata, usando exclusivamente open/read/write/lseek/ftruncate/close.
 * En particular, 'i' (inserción arbitraria) y 'd' (borrado) NO cargan y reescriben
 * el archivo completo: calculan el offset en bytes donde ocurre el cambio, guardan
 * en un buffer dinámico (malloc) todo lo que hay después de ese punto, reposicionan
 * el cursor con lseek() y vuelven a escribir ese buffer desplazado. Esto es la
 * técnica de "manejo avanzado de lseek y buffers dinámicos para no perder datos al
 * desplazar bytes" que exige el reto técnico del nivel de dos integrantes.
 *
 * Se mantiene además un arreglo en memoria (lines[]) como CACHÉ de lectura para que
 * 'p' y 's' no tengan que releer el disco en cada invocación; ese arreglo se
 * actualiza en el mismo instante en que se escribe a disco, así que nunca queda
 * desincronizado.
 *
 * RESTRICCIÓN DE I/O: Todo acceso al archivo de texto se realiza exclusivamente con
 * open(), read(), write(), lseek(), ftruncate() y close(). Las funciones de alto
 * nivel (fopen, fread, fwrite, fclose) están prohibidas por la evaluación y no se
 * utilizan en ningún punto de este archivo. printf/fgets se usan únicamente para
 * leer comandos de STDIN e imprimir en STDOUT, tal como permite el enunciado.
 */

#define MAX_LINE_LEN      1024   /* Longitud máxima permitida por línea de texto */
#define MAX_LINES         1000   /* Número máximo de líneas soportadas por archivo */
#define MAX_EDITOR_ARGS   64     /* Máximo de tokens por comando dentro del editor */

/**
 * Estructura que representa el estado completo del editor mientras está activo.
 * Vive en el stack de cmd_editor() durante toda la sesión del sub-REPL.
 */
typedef struct {
    char *lines[MAX_LINES];  /* Caché en memoria, sincronizada con el disco tras cada operación */
    int   num_lines;         /* Cantidad de líneas actualmente en memoria */
    int   fd;                /* File descriptor del archivo abierto (-1 si no hay ninguno) */
    char  filename[256];     /* Nombre del archivo actualmente abierto */
} EditorState;

/* Prototipos internos (funciones auxiliares, no expuestas fuera de este archivo) */
static void   editor_liberar(EditorState *ed);
static int    editor_abrir(EditorState *ed, const char *filename);
static void   editor_imprimir(EditorState *ed, const char *arg);
static void   editor_agregar(EditorState *ed, const char *texto);
static void   editor_borrar(EditorState *ed, const char *arg);
static void   editor_insertar(EditorState *ed, const char *arg_n, const char *texto);
static void   editor_buscar(EditorState *ed, const char *palabra);
static off_t  offset_de_linea(EditorState *ed, int n);
static int    leer_exacto(int fd, void *buf, size_t count);
static int    escribir_exacto(int fd, const void *buf, size_t count);
static int    necesita_salto_previo(int fd);

/**
 * ====================================================================================
 * leer_exacto / escribir_exacto
 * ====================================================================================
 * read() y write() pueden transferir MENOS bytes de los pedidos en una sola llamada
 * (lecturas/escrituras parciales), especialmente con buffers grandes. Estas dos
 * funciones envuelven el ciclo necesario para garantizar que se lean/escriban
 * exactamente 'count' bytes antes de continuar, evitando corrupción silenciosa de
 * datos al desplazar bytes.
 */
static int leer_exacto(int fd, void *buf, size_t count) {
    size_t leidos = 0;
    while (leidos < count) {
        LOG_SYSCALL("read", "%d, buffer+%zu, %zu", fd, leidos, count - leidos);
        ssize_t r = read(fd, (char *)buf + leidos, count - leidos);
        if (r == -1) { LOG_SYSCALL_ERROR(strerror(errno)); return -1; }
        LOG_SYSCALL_RESULT(r);
        if (r == 0) break; /* EOF antes de lo esperado */
        leidos += (size_t)r;
    }
    return (leidos == count) ? 0 : -1;
}

static int escribir_exacto(int fd, const void *buf, size_t count) {
    size_t escritos = 0;
    while (escritos < count) {
        LOG_SYSCALL("write", "%d, buffer+%zu, %zu", fd, escritos, count - escritos);
        ssize_t w = write(fd, (const char *)buf + escritos, count - escritos);
        if (w == -1) { LOG_SYSCALL_ERROR(strerror(errno)); return -1; }
        LOG_SYSCALL_RESULT(w);
        escritos += (size_t)w;
    }
    return 0;
}

/**
 * ====================================================================================
 * necesita_salto_previo
 * ====================================================================================
 * CORRECCIÓN DE BUG: cuando el archivo en disco NO termina en '\n' (por ejemplo,
 * si fue creado por otro programa sin ese salto final), escribir directamente al
 * final (SEEK_END) fusionaría el nuevo contenido con la última línea existente,
 * ya que no habría ningún separador entre ambos. Esta función revisa el ÚLTIMO
 * BYTE real del archivo: si el archivo no está vacío y ese byte no es '\n',
 * significa que hace falta escribir un '\n' de más ANTES de agregar contenido
 * nuevo al final, para no fusionar dos líneas en una sola.
 */
static int necesita_salto_previo(int fd) {
    LOG_SYSCALL("lseek", "%d, 0, SEEK_END", fd);
    off_t tam = lseek(fd, 0, SEEK_END);
    if (tam == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); return 0; }
    LOG_SYSCALL_RESULT(tam);

    if (tam == 0) return 0; /* archivo vacío: no hay nada que separar */

    char ultimo_byte;
    LOG_SYSCALL("lseek", "%d, %ld, SEEK_SET", fd, (long)(tam - 1));
    off_t p = lseek(fd, tam - 1, SEEK_SET);
    if (p == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); return 0; }
    LOG_SYSCALL_RESULT(p);

    if (leer_exacto(fd, &ultimo_byte, 1) == -1) return 0;

    return (ultimo_byte != '\n');
}

/**
 * ====================================================================================
 * offset_de_linea
 * ====================================================================================
 * Calcula el offset en bytes, dentro del archivo en disco, donde EMPIEZA la línea
 * n (1-indexada), sumando la longitud de cada línea anterior más su '\n'. Para
 * n == num_lines + 1 devuelve el offset del final del contenido conocido (útil
 * para insertar después de la última línea).
 */
static off_t offset_de_linea(EditorState *ed, int n) {
    off_t offset = 0;
    for (int i = 0; i < n - 1; i++) {
        offset += (off_t)strlen(ed->lines[i]) + 1; /* +1 por el '\n' */
    }
    return offset;
}

/**
 * ====================================================================================
 * editor_liberar
 * ====================================================================================
 * Libera toda la memoria dinámica reservada para las líneas y reinicia el estado.
 * No cierra el file descriptor (eso se hace explícitamente antes de llamarla).
 */
static void editor_liberar(EditorState *ed) {
    for (int i = 0; i < ed->num_lines; i++) {
        free(ed->lines[i]);
        ed->lines[i] = NULL;
    }
    ed->num_lines = 0;
    ed->fd = -1;
    ed->filename[0] = '\0';
}

/**
 * ====================================================================================
 * editor_abrir <archivo>
 * ====================================================================================
 * Abre (o crea, si no existe) el archivo indicado y carga su contenido completo a
 * memoria como CACHÉ de lectura, partiéndolo en líneas por el carácter '\n'. Los
 * cambios posteriores (a, i, d) escriben directamente a disco y actualizan esta
 * caché en el mismo momento, para que 'p' y 's' no tengan que releer el archivo.
 *
 * Syscalls: open(2) con O_RDWR | O_CREAT, read(2) en bucle hasta EOF.
 */
static int editor_abrir(EditorState *ed, const char *filename) {
    /* Si ya había un archivo abierto en esta sesión, cerrarlo primero */
    if (ed->fd != -1) {
        LOG_SYSCALL("close", "%d", ed->fd);
        int cres = close(ed->fd);
        if (cres == -1) LOG_SYSCALL_ERROR(strerror(errno));
        else LOG_SYSCALL_RESULT(cres);
        editor_liberar(ed);
    }

    /* 1. LLAMADA AL SISTEMA: open (crea el archivo si no existe, con permisos 0644) */
    LOG_SYSCALL("open", "\"%s\", O_RDWR|O_CREAT, 0644", filename);
    int fd = open(filename, O_RDWR | O_CREAT, 0644);
    if (fd == -1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return -1;
    }
    LOG_SYSCALL_RESULT(fd);

    /* Buffer dinámico acumulador para leer todo el contenido del archivo */
    size_t cap = 4096, len = 0;
    char *content = malloc(cap);
    if (!content) {
        fprintf(stderr, COLOR_ERROR "Error: no se pudo reservar memoria.\n" COLOR_RESET);
        close(fd);
        return -1;
    }

    char temp[4096];
    ssize_t bytes_read;
    while (1) {
        /* 2. LLAMADA AL SISTEMA: read (se repite hasta que devuelve 0 -> EOF) */
        LOG_SYSCALL("read", "%d, buffer, %zu", fd, sizeof(temp));
        bytes_read = read(fd, temp, sizeof(temp));
        if (bytes_read == -1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            free(content);
            close(fd);
            return -1;
        }
        LOG_SYSCALL_RESULT(bytes_read);
        if (bytes_read == 0) break; /* Fin de archivo */

        if (len + (size_t)bytes_read + 1 > cap) {
            cap = (len + (size_t)bytes_read + 1) * 2;
            char *tmp = realloc(content, cap);
            if (!tmp) {
                fprintf(stderr, COLOR_ERROR "Error: memoria insuficiente.\n" COLOR_RESET);
                free(content);
                close(fd);
                return -1;
            }
            content = tmp;
        }
        memcpy(content + len, temp, (size_t)bytes_read);
        len += (size_t)bytes_read;
    }
    content[len] = '\0';

    /* Partir el contenido acumulado por '\n' y poblar el arreglo lines[].
     * NOTA: si el archivo NO termina en '\n', esa última línea sin salto se
     * carga igual, pero a partir de aquí toda escritura (a/i/d) normaliza el
     * archivo agregando '\n' al final de cada línea que toca, así que tras la
     * primera operación mutadora el archivo siempre queda bien terminado. */
    ed->num_lines = 0;
    size_t start = 0;
    for (size_t i = 0; i < len && ed->num_lines < MAX_LINES; i++) {
        if (content[i] == '\n') {
            size_t linelen = i - start;
            if (linelen >= MAX_LINE_LEN) linelen = MAX_LINE_LEN - 1;
            char *l = malloc(linelen + 1);
            memcpy(l, content + start, linelen);
            l[linelen] = '\0';
            ed->lines[ed->num_lines++] = l;
            start = i + 1;
        }
    }
    /* Si queda contenido después del último '\n' (archivo sin salto final), guardarlo */
    if (start < len && ed->num_lines < MAX_LINES) {
        size_t linelen = len - start;
        if (linelen >= MAX_LINE_LEN) linelen = MAX_LINE_LEN - 1;
        char *l = malloc(linelen + 1);
        memcpy(l, content + start, linelen);
        l[linelen] = '\0';
        ed->lines[ed->num_lines++] = l;
    }

    free(content);
    ed->fd = fd;
    strncpy(ed->filename, filename, sizeof(ed->filename) - 1);
    ed->filename[sizeof(ed->filename) - 1] = '\0';

    printf(COLOR_RESULT "Archivo '%s' abierto (%d líneas).\n" COLOR_RESET, filename, ed->num_lines);
    return 0;
}

/**
 * ====================================================================================
 * editor_imprimir [n]
 * ====================================================================================
 * Sin argumento: imprime todas las líneas numeradas.
 * Con argumento n: imprime únicamente la línea n (1-indexada), validando el rango.
 */
static void editor_imprimir(EditorState *ed, const char *arg) {
    if (ed->fd == -1) {
        printf(COLOR_ERROR "No hay ningún archivo abierto. Use 'o <archivo>' primero.\n" COLOR_RESET);
        return;
    }
    if (arg == NULL) {
        for (int i = 0; i < ed->num_lines; i++) {
            printf("%3d: %s\n", i + 1, ed->lines[i]);
        }
        return;
    }
    int n = atoi(arg);
    if (n < 1 || n > ed->num_lines) {
        printf(COLOR_ERROR "Línea %d fuera de rango (1-%d).\n" COLOR_RESET, n, ed->num_lines);
        return;
    }
    printf("%3d: %s\n", n, ed->lines[n - 1]);
}

/**
 * ====================================================================================
 * editor_agregar <texto>
 * ====================================================================================
 * Añade una nueva línea al final del archivo, escribiendo directamente a disco.
 * Syscalls: lseek(SEEK_END) para posicionarse al final, write() del texto y del '\n'.
 */
static void editor_agregar(EditorState *ed, const char *texto) {
    if (ed->fd == -1) {
        printf(COLOR_ERROR "No hay ningún archivo abierto. Use 'o <archivo>' primero.\n" COLOR_RESET);
        return;
    }
    if (ed->num_lines >= MAX_LINES) {
        printf(COLOR_ERROR "Se alcanzó el máximo de %d líneas.\n" COLOR_RESET, MAX_LINES);
        return;
    }

    /* CORRECCIÓN: si el archivo ya tiene contenido pero su último byte no es
     * '\n' (por ejemplo, un archivo creado fuera del editor), hace falta
     * escribir ese salto de línea faltante ANTES del texto nuevo; de lo
     * contrario, la línea nueva quedaría pegada al final de la última línea
     * existente en el archivo (aunque en memoria se vieran como separadas). */
    int falta_salto = necesita_salto_previo(ed->fd);

    LOG_SYSCALL("lseek", "%d, 0, SEEK_END", ed->fd);
    off_t pos = lseek(ed->fd, 0, SEEK_END);
    if (pos == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); return; }
    LOG_SYSCALL_RESULT(pos);

    if (falta_salto && escribir_exacto(ed->fd, "\n", 1) == -1) {
        printf(COLOR_ERROR "Error al escribir el salto de línea faltante.\n" COLOR_RESET);
        return;
    }

    const char *t = texto ? texto : "";
    size_t tlen = strlen(t);
    if (escribir_exacto(ed->fd, t, tlen) == -1 || escribir_exacto(ed->fd, "\n", 1) == -1) {
        printf(COLOR_ERROR "Error al escribir la nueva línea en disco.\n" COLOR_RESET);
        return;
    }

    /* Reflejar en la caché de memoria */
    ed->lines[ed->num_lines++] = strdup(t);

    printf(COLOR_RESULT "Línea %d añadida y guardada en disco.\n" COLOR_RESET, ed->num_lines);
}

/**
 * ====================================================================================
 * editor_insertar <n> <texto>
 * ====================================================================================
 * Inserta una nueva línea en la posición n, DIRECTAMENTE EN DISCO, desplazando los
 * bytes posteriores mediante un buffer dinámico. Pasos:
 *
 *   1. Calcular el offset en bytes donde debe insertarse la nueva línea.
 *   2. Averiguar el tamaño total del archivo (lseek a SEEK_END).
 *   3. Leer TODO lo que hay después de ese offset hacia un buffer reservado con
 *      malloc(), para no perderlo al sobrescribir esa zona.
 *   4. Reposicionar el cursor al offset de inserción con lseek(SEEK_SET).
 *   5. Escribir el texto nuevo + '\n'.
 *   6. Escribir a continuación el contenido que se había guardado en el buffer
 *      (el resto del archivo, ahora desplazado hacia adelante).
 *   7. Liberar el buffer con free().
 *
 * Esta es la técnica de "manejo avanzado de lseek y buffers dinámicos (malloc/free)
 * para no perder datos al desplazar bytes" que exige el reto técnico del nivel de
 * dos integrantes.
 */
static void editor_insertar(EditorState *ed, const char *arg_n, const char *texto) {
    if (ed->fd == -1) {
        printf(COLOR_ERROR "No hay ningún archivo abierto. Use 'o <archivo>' primero.\n" COLOR_RESET);
        return;
    }
    int n = atoi(arg_n ? arg_n : "0");
    if (n < 1 || n > ed->num_lines + 1) {
        printf(COLOR_ERROR "Posición %d fuera de rango (1-%d).\n" COLOR_RESET, n, ed->num_lines + 1);
        return;
    }
    if (ed->num_lines >= MAX_LINES) {
        printf(COLOR_ERROR "Se alcanzó el máximo de %d líneas.\n" COLOR_RESET, MAX_LINES);
        return;
    }

    /* 1. Offset donde empieza la línea n actual (o el final conocido, si n es la
     *    posición siguiente a la última línea). */
    off_t offset = offset_de_linea(ed, n);

    /* Bloque a insertar: texto + '\n' */
    const char *t = texto ? texto : "";
    size_t texto_len = strlen(t);
    char *bloque_nuevo = malloc(texto_len + 1);
    if (!bloque_nuevo) { printf(COLOR_ERROR "Error de memoria.\n" COLOR_RESET); return; }
    memcpy(bloque_nuevo, t, texto_len);
    bloque_nuevo[texto_len] = '\n';
    size_t bloque_len = texto_len + 1;

    /* 2. Tamaño total actual del archivo */
    LOG_SYSCALL("lseek", "%d, 0, SEEK_END", ed->fd);
    off_t tam_total = lseek(ed->fd, 0, SEEK_END);
    if (tam_total == (off_t)-1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        free(bloque_nuevo);
        return;
    }
    LOG_SYSCALL_RESULT(tam_total);

    /* Salvaguarda: si por un archivo sin '\n' final el offset calculado quedó
     * más allá del tamaño real, se ajusta al final real del archivo.
     * CORRECCIÓN: en ese caso también hace falta escribir el '\n' que le
     * faltaba a la última línea existente, o de lo contrario el texto nuevo
     * quedaría pegado a ella en el archivo (aunque en memoria luzcan como
     * líneas separadas). Se detecta guardando esta condición ANTES de
     * recortar 'offset', ya que "offset > tam_total" ocurre precisamente
     * cuando se intenta insertar justo después de una última línea sin
     * salto de línea final. */
    int falta_salto_previo = (offset > tam_total);
    if (falta_salto_previo) offset = tam_total;

    off_t cola_len = tam_total - offset;
    char *buffer_cola = NULL;
    if (cola_len > 0) {
        /* 3. Leer todo lo posterior al punto de inserción hacia un buffer dinámico */
        buffer_cola = malloc((size_t)cola_len);
        if (!buffer_cola) {
            printf(COLOR_ERROR "Error de memoria.\n" COLOR_RESET);
            free(bloque_nuevo);
            return;
        }
        LOG_SYSCALL("lseek", "%d, %ld, SEEK_SET", ed->fd, (long)offset);
        off_t p1 = lseek(ed->fd, offset, SEEK_SET);
        if (p1 == (off_t)-1) {
            LOG_SYSCALL_ERROR(strerror(errno));
            free(bloque_nuevo); free(buffer_cola);
            return;
        }
        LOG_SYSCALL_RESULT(p1);

        if (leer_exacto(ed->fd, buffer_cola, (size_t)cola_len) == -1) {
            printf(COLOR_ERROR "Error al leer el resto del archivo.\n" COLOR_RESET);
            free(bloque_nuevo); free(buffer_cola);
            return;
        }
    }

    /* 4. Reposicionar al offset de inserción */
    LOG_SYSCALL("lseek", "%d, %ld, SEEK_SET", ed->fd, (long)offset);
    off_t p2 = lseek(ed->fd, offset, SEEK_SET);
    if (p2 == (off_t)-1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        free(bloque_nuevo); free(buffer_cola);
        return;
    }
    LOG_SYSCALL_RESULT(p2);

    /* 5. Si hacía falta, escribir primero el '\n' que le faltaba a la última
     *    línea existente, y luego el texto nuevo. */
    if (falta_salto_previo && escribir_exacto(ed->fd, "\n", 1) == -1) {
        printf(COLOR_ERROR "Error al escribir el salto de línea faltante.\n" COLOR_RESET);
        free(bloque_nuevo); free(buffer_cola);
        return;
    }
    if (escribir_exacto(ed->fd, bloque_nuevo, bloque_len) == -1) {
        printf(COLOR_ERROR "Error al escribir la nueva línea.\n" COLOR_RESET);
        free(bloque_nuevo); free(buffer_cola);
        return;
    }

    /* 6. Reescribir la cola guardada, ahora desplazada hacia adelante */
    if (buffer_cola) {
        if (escribir_exacto(ed->fd, buffer_cola, (size_t)cola_len) == -1) {
            printf(COLOR_ERROR "Error al reescribir el resto del archivo.\n" COLOR_RESET);
            free(bloque_nuevo); free(buffer_cola);
            return;
        }
    }

    /* 7. Liberar los buffers dinámicos */
    free(bloque_nuevo);
    free(buffer_cola);

    /* Reflejar el cambio en la caché de memoria, para que 'p' y 's' no
     * tengan que releer el archivo del disco. */
    char *nueva_linea_mem = strdup(t);
    for (int i = ed->num_lines; i > n - 1; i--) {
        ed->lines[i] = ed->lines[i - 1];
    }
    ed->lines[n - 1] = nueva_linea_mem;
    ed->num_lines++;

    printf(COLOR_RESULT "Línea insertada en la posición %d y guardada en disco.\n" COLOR_RESET, n);
}

/**
 * ====================================================================================
 * editor_borrar <n>
 * ====================================================================================
 * Elimina la línea n DIRECTAMENTE EN DISCO, usando la misma técnica de buffer
 * dinámico: guarda en memoria todo lo posterior a la línea borrada, reposiciona el
 * cursor al inicio de esa línea con lseek(), reescribe la cola desplazada y ajusta
 * el tamaño final del archivo con ftruncate().
 */
static void editor_borrar(EditorState *ed, const char *arg) {
    if (ed->fd == -1) {
        printf(COLOR_ERROR "No hay ningún archivo abierto. Use 'o <archivo>' primero.\n" COLOR_RESET);
        return;
    }
    int n = atoi(arg ? arg : "0");
    if (n < 1 || n > ed->num_lines) {
        printf(COLOR_ERROR "Línea %d fuera de rango (1-%d).\n" COLOR_RESET, n, ed->num_lines);
        return;
    }

    off_t pos_inicio = offset_de_linea(ed, n);
    off_t linea_len = (off_t)strlen(ed->lines[n - 1]) + 1; /* incluye su '\n' */
    off_t pos_fin = pos_inicio + linea_len;

    LOG_SYSCALL("lseek", "%d, 0, SEEK_END", ed->fd);
    off_t tam_total = lseek(ed->fd, 0, SEEK_END);
    if (tam_total == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); return; }
    LOG_SYSCALL_RESULT(tam_total);

    if (pos_fin > tam_total) pos_fin = tam_total; /* salvaguarda por archivos atípicos */

    off_t cola_len = tam_total - pos_fin;
    char *buffer_cola = NULL;
    if (cola_len > 0) {
        buffer_cola = malloc((size_t)cola_len);
        if (!buffer_cola) { printf(COLOR_ERROR "Error de memoria.\n" COLOR_RESET); return; }

        LOG_SYSCALL("lseek", "%d, %ld, SEEK_SET", ed->fd, (long)pos_fin);
        off_t p1 = lseek(ed->fd, pos_fin, SEEK_SET);
        if (p1 == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); free(buffer_cola); return; }
        LOG_SYSCALL_RESULT(p1);

        if (leer_exacto(ed->fd, buffer_cola, (size_t)cola_len) == -1) {
            printf(COLOR_ERROR "Error al leer el resto del archivo.\n" COLOR_RESET);
            free(buffer_cola);
            return;
        }
    }

    LOG_SYSCALL("lseek", "%d, %ld, SEEK_SET", ed->fd, (long)pos_inicio);
    off_t p2 = lseek(ed->fd, pos_inicio, SEEK_SET);
    if (p2 == (off_t)-1) { LOG_SYSCALL_ERROR(strerror(errno)); free(buffer_cola); return; }
    LOG_SYSCALL_RESULT(p2);

    if (buffer_cola) {
        if (escribir_exacto(ed->fd, buffer_cola, (size_t)cola_len) == -1) {
            printf(COLOR_ERROR "Error al reescribir el resto del archivo.\n" COLOR_RESET);
            free(buffer_cola);
            return;
        }
    }
    free(buffer_cola);

    LOG_SYSCALL("ftruncate", "%d, %ld", ed->fd, (long)(pos_inicio + cola_len));
    int tres = ftruncate(ed->fd, pos_inicio + cola_len);
    if (tres == -1) { LOG_SYSCALL_ERROR(strerror(errno)); return; }
    LOG_SYSCALL_RESULT(tres);

    /* Reflejar en la caché de memoria */
    free(ed->lines[n - 1]);
    for (int i = n - 1; i < ed->num_lines - 1; i++) {
        ed->lines[i] = ed->lines[i + 1];
    }
    ed->num_lines--;

    printf(COLOR_RESULT "Línea %d eliminada y guardada en disco.\n" COLOR_RESET, n);
}

/**
 * ====================================================================================
 * editor_buscar <palabra>
 * ====================================================================================
 * Búsqueda simple de una subcadena dentro de las líneas de la caché en memoria
 * (que siempre está sincronizada con el disco tras cada operación). No requiere
 * tocar el disco. Usa strstr() para detectar coincidencias como subcadena.
 */
static void editor_buscar(EditorState *ed, const char *palabra) {
    if (ed->fd == -1) {
        printf(COLOR_ERROR "No hay ningún archivo abierto. Use 'o <archivo>' primero.\n" COLOR_RESET);
        return;
    }
    if (palabra == NULL || palabra[0] == '\0') {
        printf(COLOR_ERROR "Uso: s <palabra>\n" COLOR_RESET);
        return;
    }
    int encontrados = 0;
    for (int i = 0; i < ed->num_lines; i++) {
        if (strstr(ed->lines[i], palabra) != NULL) {
            printf("%3d: %s\n", i + 1, ed->lines[i]);
            encontrados++;
        }
    }
    if (encontrados == 0) {
        printf(COLOR_INFO "No se encontraron coincidencias para '%s'.\n" COLOR_RESET, palabra);
    } else {
        printf(COLOR_RESULT "%d coincidencia(s) encontrada(s).\n" COLOR_RESET, encontrados);
    }
}

/**
 * ====================================================================================
 * PARCIAL 2 - Compresión Huffman concurrente en segundo plano
 * ====================================================================================
 * Comandos nuevos del editor:
 *   c [archivo]              comprime (por defecto el archivo abierto) -> archivo.huf
 *   x <archivo.huf> [salida] descomprime (por defecto quita la extensión .huf)
 *   st                       muestra el estado / porcentaje de la tarea
 *   w                        muestra la barra de progreso en vivo hasta que termine
 *   cancel                   cancela la tarea en curso (no deja salida parcial)
 *
 * El trabajo pesado corre en otro hilo (huf_job.c), así que el REPL vuelve al prompt
 * de inmediato y los demás comandos (p, s, ...) siguen respondiendo. El hilo de fondo
 * NUNCA imprime: este hilo (el del editor) es el único que escribe en la terminal,
 * por eso el progreso no se mezcla con el prompt ni con las trazas LOG_SYSCALL.
 */

#define HUF_MAX_HILOS 16

static int hilos_por_defecto(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 4;
    if (n > HUF_MAX_HILOS) n = HUF_MAX_HILOS;
    return (int)n;
}

static double mb(uint64_t bytes) { return bytes / (1024.0 * 1024.0); }

static int porcentaje(const JobStatus *st) {
    return st->total ? (int)(st->done * 100 / st->total) : 0;
}

static const char *verbo(const JobStatus *st) {
    return st->kind == JOB_COMPRESS ? "Compresión" : "Descompresión";
}

/* Barra de progreso: [##########----------]  50%  (12.0 / 24.0 MB) */
static void dibujar_barra(const JobStatus *st) {
    const int ancho = 30;
    int pct = porcentaje(st);
    if (st->state == JOB_DONE) pct = 100;
    int llenos = pct * ancho / 100;
    printf("\r%s [", st->kind == JOB_COMPRESS ? "comprimiendo  " : "descomprimiendo");
    for (int i = 0; i < ancho; i++) putchar(i < llenos ? '#' : '-');
    if (st->total == 0 && st->state == JOB_RUNNING)
        printf("]   preparando...                              ");
    else
        printf("] %3d%%  (%.1f / %.1f MB)%s", pct, mb(st->done), mb(st->total),
               st->state == JOB_CANCELLING ? "  cancelando..." : "              ");
    if (st->state != JOB_RUNNING && st->state != JOB_CANCELLING) printf("\n");
    fflush(stdout);
}

/* Si la tarea de fondo terminó y aún no se ha avisado, avisar ahora (se llama justo
 * antes de imprimir el prompt, desde el hilo del editor). */
static void notificar_job(void) {
    JobStatus st;
    job_status(&st);
    if (!st.unseen) return;
    job_mark_seen();

    if (st.state == JOB_DONE) {
        struct stat a, b;
        double in_mb = stat(st.in_path, &a) == 0 ? mb((uint64_t)a.st_size) : 0;
        double out_mb = stat(st.out_path, &b) == 0 ? mb((uint64_t)b.st_size) : 0;
        printf(COLOR_RESULT "[fondo] %s terminada: '%s' -> '%s' (%.2f MB -> %.2f MB) en %.2f s con %d hilos.\n" COLOR_RESET,
               verbo(&st), st.in_path, st.out_path, in_mb, out_mb, st.elapsed_ms / 1000.0, st.threads);
    } else if (st.state == JOB_CANCELLED) {
        printf(COLOR_INFO "[fondo] %s de '%s' cancelada. No quedó archivo parcial.\n" COLOR_RESET,
               verbo(&st), st.in_path);
    } else if (st.state == JOB_FAILED) {
        printf(COLOR_ERROR "[fondo] %s de '%s' falló: %s\n" COLOR_RESET,
               verbo(&st), st.in_path, strerror(st.err));
    }
}

static void imprimir_prompt(void) {
    JobStatus st;
    job_status(&st);
    if (st.state == JOB_RUNNING || st.state == JOB_CANCELLING)
        printf(COLOR_PROMPT "editor " COLOR_PARAM "[%s %d%%]" COLOR_PROMPT "> " COLOR_RESET,
               st.kind == JOB_COMPRESS ? "comprimiendo" : "descomprimiendo", porcentaje(&st));
    else
        printf(COLOR_PROMPT "editor> " COLOR_RESET);
    fflush(stdout);
}

static void lanzar_job(JobKind kind, const char *in, const char *out) {
    int n = hilos_por_defecto();
    if (job_start(kind, in, out, n) == -1) {
        if (errno == EBUSY)
            printf(COLOR_ERROR "Ya hay una tarea en segundo plano. Use 'st', 'w' o 'cancel'.\n" COLOR_RESET);
        else
            printf(COLOR_ERROR "No se pudo iniciar la tarea: %s\n" COLOR_RESET, strerror(errno));
        return;
    }
    printf(COLOR_RESULT "%s iniciada en segundo plano con %d hilos: '%s' -> '%s'.\n" COLOR_RESET,
           kind == JOB_COMPRESS ? "Compresión" : "Descompresión", n, in, out);
    printf(COLOR_INFO "Puede seguir usando el editor. 'st' = estado, 'w' = progreso en vivo, 'cancel' = cancelar.\n" COLOR_RESET);
}

static void editor_comprimir(EditorState *ed, const char *arg) {
    const char *in = arg ? arg : (ed->fd != -1 ? ed->filename : NULL);
    if (!in) {
        printf(COLOR_ERROR "Uso: c [archivo]  (sin argumento comprime el archivo abierto)\n" COLOR_RESET);
        return;
    }
    char out[512];
    snprintf(out, sizeof out, "%s.huf", in);
    lanzar_job(JOB_COMPRESS, in, out);
}

static void editor_descomprimir(const char *in, const char *salida) {
    if (!in) {
        printf(COLOR_ERROR "Uso: x <archivo.huf> [salida]\n" COLOR_RESET);
        return;
    }
    char out[512];
    if (salida) {
        snprintf(out, sizeof out, "%s", salida);
    } else {
        size_t n = strlen(in);
        if (n <= 4 || strcmp(in + n - 4, ".huf") != 0) {
            printf(COLOR_ERROR "'%s' no termina en .huf; indique la salida: x <archivo.huf> <salida>\n" COLOR_RESET, in);
            return;
        }
        snprintf(out, sizeof out, "%.*s", (int)(n - 4), in);
        struct stat sb;
        if (stat(out, &sb) == 0) {   /* no pisar un archivo existente sin que lo pidan */
            printf(COLOR_ERROR "'%s' ya existe. Indique otro nombre: x %s <salida>\n" COLOR_RESET, out, in);
            return;
        }
    }
    lanzar_job(JOB_DECOMPRESS, in, out);
}

static void editor_estado(void) {
    JobStatus st;
    job_status(&st);
    if (st.state == JOB_IDLE) {
        printf(COLOR_INFO "No se ha lanzado ninguna compresión en esta sesión.\n" COLOR_RESET);
        return;
    }
    if (st.state == JOB_RUNNING || st.state == JOB_CANCELLING) {
        dibujar_barra(&st);
        printf("\n");
        return;
    }
    job_mark_seen();
    const char *res = st.state == JOB_DONE ? "terminada" : st.state == JOB_CANCELLED ? "cancelada" : "fallida";
    printf(COLOR_INFO "Última tarea: %s de '%s' -> '%s': %s" COLOR_RESET, verbo(&st), st.in_path, st.out_path, res);
    if (st.state == JOB_FAILED) printf(COLOR_ERROR " (%s)" COLOR_RESET, strerror(st.err));
    printf("\n");
}

static void editor_ver_progreso(void) {
    if (!job_busy()) { editor_estado(); return; }
    job_watch(dibujar_barra);   /* duerme hasta cada avance; vuelve al terminar */
    notificar_job();
}

static void editor_cancelar(void) {
    if (job_cancel() == -1) {
        printf(COLOR_INFO "No hay ninguna tarea en curso.\n" COLOR_RESET);
        return;
    }
    printf(COLOR_INFO "Cancelando... " COLOR_RESET);
    fflush(stdout);
    job_wait();                 /* el job se detiene al terminar el bloque actual */
    printf("listo.\n");
    notificar_job();
}

/**
 * ====================================================================================
 * COMANDO: editor [archivo]
 * ====================================================================================
 * Punto de entrada registrado en la tabla commands[] del shell (categoría "edicion").
 * Lanza un sub-REPL propio que entiende únicamente los comandos base del editor:
 * o, p, a, i, d, s, q. Como cada comando mutador ya persiste en disco de forma
 * inmediata, 'q' solo necesita cerrar el file descriptor y liberar la memoria.
 */
int cmd_editor(int argc, char **argv) {
    EditorState ed;
    memset(&ed, 0, sizeof(ed));
    ed.fd = -1;

    printf(COLOR_TITLE "\n--- Editor de Texto Unix (comandos: o, p, a, i, d, s, q | Huffman: c, x, st, w, cancel) ---\n" COLOR_RESET);

    /* Si se invocó como 'editor <archivo>', abrirlo automáticamente al entrar */
    if (argc == 2) {
        editor_abrir(&ed, argv[1]);
    } else {
        printf(COLOR_INFO "Use 'o <archivo>' para abrir o crear un archivo.\n" COLOR_RESET);
    }

    /* Buffer de lectura de comandos: deliberadamente más grande que MAX_LINE_LEN
     * (que limita el tamaño de una línea GUARDADA en el archivo) para reducir la
     * probabilidad de truncar un comando "a <texto>" a mitad de camino. Aun así,
     * si el texto ingresado supera este buffer, se detecta y se descarta el
     * sobrante (ver más abajo) en vez de dejarlo corromper el siguiente comando. */
    #define EDITOR_CMD_BUF_LEN (MAX_LINE_LEN * 4)
    char line[EDITOR_CMD_BUF_LEN];
    char *eargv[MAX_EDITOR_ARGS];

    while (1) {
        notificar_job();       /* avisar si una tarea de fondo terminó */
        imprimir_prompt();     /* muestra el % si hay una tarea en curso */

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF (Ctrl+D): como todos los cambios ya se persistieron al hacerse,
             * basta con cerrar el FD y liberar la memoria; no hay nada pendiente
             * que se pueda perder. */
            printf("\n");
            /* Ctrl+D: no hay forma de seguir preguntando, así que se cancela la tarea
             * de fondo (si existe), se espera y se hace join antes de salir. */
            if (job_busy()) printf(COLOR_INFO "Cancelando la tarea en segundo plano...\n" COLOR_RESET);
            job_shutdown();
            if (ed.fd != -1) {
                LOG_SYSCALL("close", "%d", ed.fd);
                int cres = close(ed.fd);
                if (cres == -1) LOG_SYSCALL_ERROR(strerror(errno));
                else LOG_SYSCALL_RESULT(cres);
            }
            editor_liberar(&ed);
            break;
        }

        /* Detectar truncamiento: si la línea leída no terminó en '\n' (y no fue
         * por EOF), significa que el comando ingresado superó el tamaño del
         * buffer. En ese caso se debe descartar todo el sobrante que sigue
         * pendiente en stdin; de lo contrario, ese sobrante sería leído por el
         * siguiente fgets() y se interpretaría erróneamente como un comando
         * nuevo, corrompiendo la secuencia de comandos del usuario. */
        size_t line_len = strlen(line);
        if (line_len > 0 && line[line_len - 1] != '\n') {
            int c;
            while ((c = getchar()) != '\n' && c != EOF) {
                /* descartar carácter sobrante */
            }
            printf(COLOR_ERROR "Advertencia: el comando ingresado excede el máximo de %d caracteres y fue truncado.\n" COLOR_RESET, EDITOR_CMD_BUF_LEN - 1);
        }

        int eargc = parse_line(line, eargv);
        if (eargc == 0) continue;

        if (strcmp(eargv[0], "o") == 0) {
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: o <archivo>\n" COLOR_RESET);
            } else {
                editor_abrir(&ed, eargv[1]);
            }
        } else if (strcmp(eargv[0], "p") == 0) {
            editor_imprimir(&ed, eargc >= 2 ? eargv[1] : NULL);
        } else if (strcmp(eargv[0], "a") == 0) {
            char texto_buf[MAX_LINE_LEN];
            texto_buf[0] = '\0';
            for (int i = 1; i < eargc; i++) {
                if (i > 1) {
                    strncat(texto_buf, " ", sizeof(texto_buf) - strlen(texto_buf) - 1);
                }
                strncat(texto_buf, eargv[i], sizeof(texto_buf) - strlen(texto_buf) - 1);
            }
            editor_agregar(&ed, texto_buf);
        } else if (strcmp(eargv[0], "i") == 0) {
            if (eargc < 3) {
                printf(COLOR_ERROR "Uso: i <n> <texto>\n" COLOR_RESET);
            } else {
                char texto_buf[MAX_LINE_LEN];
                texto_buf[0] = '\0';
                for (int i = 2; i < eargc; i++) {
                    if (i > 2) {
                        strncat(texto_buf, " ", sizeof(texto_buf) - strlen(texto_buf) - 1);
                    }
                    strncat(texto_buf, eargv[i], sizeof(texto_buf) - strlen(texto_buf) - 1);
                }
                editor_insertar(&ed, eargv[1], texto_buf);
            }
        } else if (strcmp(eargv[0], "d") == 0) {
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: d <n>\n" COLOR_RESET);
            } else {
                editor_borrar(&ed, eargv[1]);
            }
        } else if (strcmp(eargv[0], "s") == 0) {
            char palabra_buf[MAX_LINE_LEN];
            palabra_buf[0] = '\0';
            for (int i = 1; i < eargc; i++) {
                if (i > 1) {
                    strncat(palabra_buf, " ", sizeof(palabra_buf) - strlen(palabra_buf) - 1);
                }
                strncat(palabra_buf, eargv[i], sizeof(palabra_buf) - strlen(palabra_buf) - 1);
            }
            editor_buscar(&ed, eargc >= 2 ? palabra_buf : NULL);
        } else if (strcmp(eargv[0], "q") == 0) {
            if (job_busy()) {
                printf(COLOR_ERROR "Hay una tarea en segundo plano. Use 'w' para esperarla o 'cancel' para cancelarla antes de salir.\n" COLOR_RESET);
                continue;
            }
            job_shutdown();    /* join del último hilo job (ya terminado) */
            if (ed.fd != -1) {
                LOG_SYSCALL("close", "%d", ed.fd);
                int cres = close(ed.fd);
                if (cres == -1) LOG_SYSCALL_ERROR(strerror(errno));
                else LOG_SYSCALL_RESULT(cres);
            }
            editor_liberar(&ed);
            printf(COLOR_INFO "Saliendo del editor...\n\n" COLOR_RESET);
            break;
        } else if (strcmp(eargv[0], "c") == 0) {
            editor_comprimir(&ed, eargc >= 2 ? eargv[1] : NULL);
        } else if (strcmp(eargv[0], "x") == 0) {
            editor_descomprimir(eargc >= 2 ? eargv[1] : NULL, eargc >= 3 ? eargv[2] : NULL);
        } else if (strcmp(eargv[0], "st") == 0) {
            editor_estado();
        } else if (strcmp(eargv[0], "w") == 0) {
            editor_ver_progreso();
        } else if (strcmp(eargv[0], "cancel") == 0) {
            editor_cancelar();
        } else {
            printf(COLOR_ERROR "Comando '%s' no reconocido. Use: o, p, a, i, d, s, q, c, x, st, w, cancel\n" COLOR_RESET, eargv[0]);
        }
    }

    return 0;
}