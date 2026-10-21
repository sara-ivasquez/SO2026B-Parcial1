# Parcial 2 — SO: Huffman concurrente integrado al editor

**Alternativa:** 1 · **Base:** editor del Parcial 1 (`cat_editor.c`) · **Entrega:** semana 12 · **Equipo:** ___ personas

Leyenda: `[x]` hecho · `[ ]` pendiente · 🔶 en curso

## Resumen

| Fase | Tema | Rúbrica | Estado |
|---|---|---|---|
| 1 | Núcleo Huffman secuencial por bloques | Corrección (15) | ✅ Hecha |
| 2 | Pool de hilos + cola (mutex/cond) | Sincronización (25) | ⬜ |
| 3 | Conteo de frecuencias paralelo | Concurrencia (25) | ⬜ |
| 4 | Compresión paralela + salida ordenada | Concurrencia + Corrección | ⬜ |
| 5 | Integración al editor en segundo plano | Integración (20) | ⬜ |
| 6 | Carreras editor vs. compresión | Integración + Sincronización | ⬜ |
| 7 | Errores, recursos, valgrind/helgrind | Errores (15) | ⬜ |
| 8 | Documentación, video, entrega | Sustentación | ⬜ |

---

## Fase 1 — Núcleo Huffman secuencial ✅

Archivos: `include/huffman.h`, `src/huffman.c`, `src/huf_cli.c`, `tests/test_huffman.sh`, `Makefile` (todo dentro de `shell/huffman/`).

- [x] Formato `.huf` definido (magic `HUF1`, tamaño original, block_size, nº de bloques, `freq[256]`, bloques con `orig_len | comp_len | datos`)
- [x] `huf_count_block` (conteo por bloque, reentrante)
- [x] `huf_build_tree` determinista (desempate por peso y menor símbolo), caso de 1 solo símbolo y archivo vacío
- [x] `huf_encode_block` / `huf_decode_block` (empaquetado de bits MSB primero)
- [x] `huf_compress_file` / `huf_decompress_file` solo con `open/read/write/lseek/close`
- [x] Validación de cabecera, truncado y bloques corruptos; borra la salida parcial si falla
- [x] Callback de progreso listo para la Fase 5
- [x] `make test`: 14/14 OK (vacío, 1 byte, un solo símbolo, aleatorio 5 MB, límites de bloque, binario, archivos inválidos)
- [x] `valgrind --leak-check=full` sin errores ni fugas
- [x] El shell (`make`) sigue compilando sin cambios

Cómo probar:
```bash
cd shell/huffman && make test
./huf c archivo.txt archivo.huf 65536     # block_size opcional
./huf x archivo.huf copia.txt && cmp archivo.txt copia.txt
```

Decisiones para la sustentación:
- Bloques de 256 KB por defecto: suficientemente grandes para amortizar el costo de una tarea, suficientemente pequeños para repartir carga y reportar progreso.
- Árbol **global** (como pide el enunciado): primero se cuentan todas las frecuencias y luego se codifica. Esa dependencia es la barrera entre fases que se sincronizará en las Fases 3 y 4.
- Se guardan las frecuencias (2 KB) en vez del árbol: el descompresor reconstruye el mismo árbol por ser determinista.
- Códigos de hasta 64 bits (`uint64_t`); superarlo exigiría un archivo de más de ~10¹³ bytes.
- Funciones sin estado global: se pueden llamar desde varios hilos sin mutex mientras cada hilo use sus propios buffers.
- Limitación conocida: comprimir un archivo vacío genera 2068 bytes de cabecera (normal en este formato).

---

## Fase 2 — Pool de hilos + cola de trabajos ⬜

Archivos: `threadpool.h`, `threadpool.c`

- [ ] `ThreadPool` con N workers (`pthread_create`)
- [ ] Cola de tareas acotada con `pthread_mutex_t` + `cond not_empty` + `cond not_full`
- [ ] `pool_submit()` bloquea en `cond` si la cola está llena (sin `sleep` ni `while(!flag)`)
- [ ] `pool_wait()` con contador de tareas pendientes y `cond all_done`
- [ ] `pool_destroy()`: flag `shutdown` + `broadcast` + `pthread_join` de todos los workers
- [ ] Prueba: 10 000 tareas triviales con 1, 4 y 16 workers, sin deadlock y con resultado correcto
- [ ] `helgrind` / `-fsanitize=thread` sin reportes

## Fase 3 — Conteo de frecuencias paralelo ⬜

- [ ] Tarea `count_task(block_id)`: lee su bloque con `pread()` (sin compartir el offset del fd) y cuenta en un histograma **local**
- [ ] Reducción: sumar histogramas locales bajo mutex, o por el coordinador tras `pool_wait()`
- [ ] Prueba: histograma paralelo == secuencial para varios tamaños y nº de hilos
- [ ] Medir tiempo secuencial vs. paralelo (tabla para la sustentación)

## Fase 4 — Compresión paralela y salida ordenada ⬜

- [ ] Construir árbol global una vez (barrera entre fases 3 y 4)
- [ ] Tarea `encode_task(block_id)`: `pread` + `huf_encode_block` → buffer propio
- [ ] Cola de salida: arreglo `results[num_blocks]` + `next_to_write` + `cond` para el hilo escritor
- [ ] Escritura estrictamente en orden de bloque; liberar cada buffer tras escribir
- [ ] Limitar bloques en vuelo (semáforo o `not_full`) para acotar memoria
- [ ] Descompresión por bloques en paralelo con la misma cola ordenada (el `.huf` ya tiene `comp_len` por bloque; se puede indexar con una pasada previa)
- [ ] Prueba: `cmp` idéntico con 1, 2, 8 workers; archivos de 100 MB+

## Fase 5 — Integración al editor en segundo plano ⬜

- [ ] `-pthread` en el `Makefile` del shell y `huffman.c`/`threadpool.c` en `SRCS`
- [ ] Comandos en el editor: `c [archivo]`, `x <archivo.huf>`, `status`, `cancel`
- [ ] Hilo "job" que lanza el coordinador; el REPL (`fgets`) sigue libre
- [ ] Progreso compartido (`atomic` o mutex) mostrado con `status` y/o barra `\r`
- [ ] Mutex de salida para que `printf` del progreso no se mezcle con el prompt ni con `LOG_SYSCALL`
- [ ] Prueba: durante una compresión larga, `p` y `s` responden al instante

## Fase 6 — Condiciones de carrera editor vs. compresión ⬜

- [ ] Estado del job: `IDLE / COMPRIMIENDO / DESCOMPRIMIENDO / CANCELANDO` protegido por mutex
- [ ] `a`, `i`, `d` rechazan (o esperan con `cond`) si el archivo abierto está en uso
- [ ] `o` (abrir otro archivo) y `q` esperan o cancelan el job antes de cerrar el fd
- [ ] Cancelación cooperativa: flag revisado entre bloques; limpieza de la salida parcial
- [ ] Prueba con script: editar mientras comprime, 100 repeticiones, sin corrupción

## Fase 7 — Errores, recursos y verificación ⬜

- [ ] Revisar retorno de `pthread_*`, `malloc`, `open`, `read`, `write`, `close`
- [ ] Todo `pthread_create` tiene su `join`; todo `malloc` su `free`; todo fd su `close`
- [ ] `SIGINT` durante un job: cancelar y salir limpio
- [ ] `valgrind --leak-check=full` y `valgrind --tool=helgrind` sin reportes
- [ ] `-fsanitize=thread` y `-fsanitize=address,undefined` limpios
- [ ] Casos borde: archivo vacío, sin permisos, disco lleno (`/dev/full`), `.huf` corrupto

## Fase 8 — Entrega ⬜

- [ ] README: diagrama de hilos y colas
- [ ] Documento de protocolo de concurrencia: qué protege cada mutex, orden de adquisición, por qué no hay deadlock ni busy waiting
- [ ] `Makefile` con `all` y `clean`; script de demo y de pruebas
- [ ] Tabla de rendimiento 1/2/4/8 hilos
- [ ] Video o sustentación presencial (oficina 19-424, con cita)
- [ ] **Cada integrante** sube evidencias a EAFIT Interactiva
- [ ] Asistir a las 3 horas de consultas en clase (obligatorio antes de pedir asesoría por Teams)

---

## Bitácora

| Fecha | Qué se hizo |
|---|---|
| 2026-10-19 | Fase 1 completa: núcleo Huffman secuencial, 14/14 pruebas, valgrind limpio |
