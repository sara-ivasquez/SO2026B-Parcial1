#include "shell.h"
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/**
 * ====================================================================================
 * MÓDULO: Editor de Texto en Unix (cat_editor.c)
 * ====================================================================================
 * Implementa un editor de texto interactivo operado por CLI, integrado como una
 * categoría nueva del shell ("edicion"). El editor abre su propio sub-REPL: una vez
 * invocado con el comando 'editor', el usuario queda dentro de un ciclo de lectura
 * de comandos propios (o, p, a, d, q) hasta que escribe 'q', momento en el cual el
 * control regresa al prompt principal del shell (eafitOS>).
 *
 * DECISIÓN ARQUITECTÓNICA: Se creó una categoría nueva ("edicion") en lugar de
 * reutilizar "datos", "memoria", "monitoreo" o "utilidades", porque el editor
 * mantiene ESTADO PERSISTENTE (un file descriptor abierto y un buffer de líneas en
 * memoria) a través de múltiples comandos consecutivos, mientras que el resto de
 * comandos del shell son operaciones atómicas de un solo disparo que no requieren
 * mantener contexto entre invocaciones.
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
    char *lines[MAX_LINES];  /* Arreglo de punteros a líneas, cada una reservada con malloc */
    int   num_lines;         /* Cantidad de líneas actualmente en memoria */
    int   fd;                /* File descriptor del archivo abierto (-1 si no hay ninguno) */
    char  filename[256];     /* Nombre del archivo actualmente abierto */
    int   modified;          /* Bandera: 1 si hay cambios en memoria sin guardar en disco */
} EditorState;

/* Prototipos internos (funciones auxiliares, no expuestas fuera de este archivo) */
static void editor_liberar(EditorState *ed);
static int  editor_abrir(EditorState *ed, const char *filename);
static void editor_imprimir(EditorState *ed, const char *arg);
static void editor_agregar(EditorState *ed, const char *texto);
static void editor_borrar(EditorState *ed, const char *arg);
static int  editor_flush(EditorState *ed);

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
    ed->modified = 0;
}

/**
 * ====================================================================================
 * editor_abrir <archivo>
 * ====================================================================================
 * Abre (o crea, si no existe) el archivo indicado y carga su contenido completo a
 * memoria, partiéndolo en líneas por el carácter '\n'.
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

    /* Partir el contenido acumulado por '\n' y poblar el arreglo lines[] */
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
    ed->modified = 0;

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
 * Añade una nueva línea al final del buffer EN MEMORIA. La escritura real a disco
 * se difiere hasta que el usuario confirme el guardado con 'q' (ver editor_flush).
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
    char *nueva = strdup(texto ? texto : "");
    if (!nueva) {
        printf(COLOR_ERROR "Error de memoria.\n" COLOR_RESET);
        return;
    }
    ed->lines[ed->num_lines++] = nueva;
    ed->modified = 1;
    printf(COLOR_RESULT "Línea %d añadida en memoria (use 'q' para guardar en disco).\n" COLOR_RESET, ed->num_lines);
}

/**
 * ====================================================================================
 * editor_borrar <n>
 * ====================================================================================
 * Elimina la línea n del buffer EN MEMORIA, desplazando los punteros posteriores
 * hacia arriba. Al igual que editor_agregar, no toca el disco de inmediato.
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
    free(ed->lines[n - 1]);
    for (int i = n - 1; i < ed->num_lines - 1; i++) {
        ed->lines[i] = ed->lines[i + 1];
    }
    ed->num_lines--;
    ed->modified = 1;
    printf(COLOR_RESULT "Línea %d eliminada en memoria (use 'q' para guardar en disco).\n" COLOR_RESET, n);
}

/**
 * ====================================================================================
 * editor_flush
 * ====================================================================================
 * Persiste el buffer en memoria hacia el archivo en disco. Se invoca únicamente
 * cuando el usuario confirma el guardado al salir con 'q'.
 *
 * Syscalls: lseek(2) para volver al inicio del archivo, write(2) por cada línea,
 * ftruncate(2) para eliminar cualquier remanente de una versión anterior más larga.
 */
static int editor_flush(EditorState *ed) {
    /* 1. LLAMADA AL SISTEMA: lseek (reposicionar el cursor al inicio absoluto) */
    LOG_SYSCALL("lseek", "%d, 0, SEEK_SET", ed->fd);
    off_t pos = lseek(ed->fd, 0, SEEK_SET);
    if (pos == (off_t)-1) {
        LOG_SYSCALL_ERROR(strerror(errno));
        return -1;
    }
    LOG_SYSCALL_RESULT(pos);

    off_t total = 0;
    for (int i = 0; i < ed->num_lines; i++) {
        size_t linelen = strlen(ed->lines[i]);

        /* 2. LLAMADA AL SISTEMA: write (contenido de la línea) */
        LOG_SYSCALL("write", "%d, linea_%d, %zu", ed->fd, i + 1, linelen);
        ssize_t w = write(ed->fd, ed->lines[i], linelen);
        if (w == -1) { LOG_SYSCALL_ERROR(strerror(errno)); return -1; }
        LOG_SYSCALL_RESULT(w);
        total += w;

        /* 3. LLAMADA AL SISTEMA: write (separador de línea) */
        LOG_SYSCALL("write", "%d, \"\\n\", 1", ed->fd);
        ssize_t wn = write(ed->fd, "\n", 1);
        if (wn == -1) { LOG_SYSCALL_ERROR(strerror(errno)); return -1; }
        LOG_SYSCALL_RESULT(wn);
        total += wn;
    }

    /* 4. LLAMADA AL SISTEMA: ftruncate (ajustar el tamaño exacto del archivo) */
    LOG_SYSCALL("ftruncate", "%d, %ld", ed->fd, (long)total);
    int tres = ftruncate(ed->fd, total);
    if (tres == -1) { LOG_SYSCALL_ERROR(strerror(errno)); return -1; }
    LOG_SYSCALL_RESULT(tres);

    ed->modified = 0;
    return 0;
}

/**
 * ====================================================================================
 * COMANDO: editor [archivo]
 * ====================================================================================
 * Punto de entrada registrado en la tabla commands[] del shell (categoría "edicion").
 * Lanza un sub-REPL propio que entiende únicamente los comandos base del editor:
 * o, p, a, d, q. Al recibir 'q', cierra el archivo, libera la memoria y retorna el
 * control al bucle principal del shell (eafitOS>).
 */
int cmd_editor(int argc, char **argv) {
    EditorState ed;
    memset(&ed, 0, sizeof(ed));
    ed.fd = -1;

    printf(COLOR_TITLE "\n--- Editor de Texto Unix (comandos: o, p, a, d, q) ---\n" COLOR_RESET);

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
        printf(COLOR_PROMPT "editor> " COLOR_RESET);
        fflush(stdout);

        if (fgets(line, sizeof(line), stdin) == NULL) {
            /* EOF (Ctrl+D): no se puede pedir confirmación porque ya no hay más
             * entrada disponible por STDIN. Se avisa explícitamente si había
             * cambios sin guardar (en vez de descartarlos en silencio) y, en
             * cualquier caso, se cierra el FD y se libera la memoria para no
             * dejar recursos abiertos durante el resto de la sesión del shell. */
            printf("\n");
            if (ed.fd != -1 && ed.modified) {
                printf(COLOR_ERROR "Advertencia: se salió con EOF (Ctrl+D) y había cambios sin guardar. Se descartaron.\n" COLOR_RESET);
            }
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
            /* Reconstruir el texto completo uniendo todos los tokens posteriores a 'a',
             * para que funcione tanto "a Hola mundo" (sin comillas) como
             * "a \"Hola mundo\"" (con comillas, vía parse_line). Sin esta unión,
             * solo se tomaría la primera palabra (eargv[1]) y se perdería el resto. */
            char texto_buf[MAX_LINE_LEN];
            texto_buf[0] = '\0';
            for (int i = 1; i < eargc; i++) {
                if (i > 1) {
                    strncat(texto_buf, " ", sizeof(texto_buf) - strlen(texto_buf) - 1);
                }
                strncat(texto_buf, eargv[i], sizeof(texto_buf) - strlen(texto_buf) - 1);
            }
            editor_agregar(&ed, texto_buf);
        } else if (strcmp(eargv[0], "d") == 0) {
            if (eargc < 2) {
                printf(COLOR_ERROR "Uso: d <n>\n" COLOR_RESET);
            } else {
                editor_borrar(&ed, eargv[1]);
            }
        } else if (strcmp(eargv[0], "q") == 0) {
            /* Confirmación de guardado si hay cambios pendientes en memoria */
            if (ed.fd != -1 && ed.modified) {
                char resp[16];
                printf(COLOR_PARAM "Hay cambios sin guardar. ¿Desea guardarlos? (s/n): " COLOR_RESET);
                fflush(stdout);
                if (fgets(resp, sizeof(resp), stdin) != NULL &&
                    (resp[0] == 's' || resp[0] == 'S')) {
                    if (editor_flush(&ed) == 0) {
                        printf(COLOR_RESULT "Cambios guardados.\n" COLOR_RESET);
                    } else {
                        printf(COLOR_ERROR "Error al guardar los cambios.\n" COLOR_RESET);
                    }
                } else {
                    printf(COLOR_INFO "Cambios descartados.\n" COLOR_RESET);
                }
            }
            if (ed.fd != -1) {
                LOG_SYSCALL("close", "%d", ed.fd);
                int cres = close(ed.fd);
                if (cres == -1) LOG_SYSCALL_ERROR(strerror(errno));
                else LOG_SYSCALL_RESULT(cres);
            }
            editor_liberar(&ed);
            printf(COLOR_INFO "Saliendo del editor...\n\n" COLOR_RESET);
            break;
        } else {
            printf(COLOR_ERROR "Comando '%s' no reconocido. Use: o, p, a, d, q\n" COLOR_RESET, eargv[0]);
        }
    }

    return 0;
}