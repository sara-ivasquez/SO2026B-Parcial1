#!/usr/bin/env bash
# ============================================================================
# Pruebas del Parcial 2, Fase 7: Ctrl+C (SIGINT) y liberación de recursos.
# Uso: bash test_editor_senales.sh   (desde shell/, después de 'make')
#
#   1. Ctrl+C durante una descompresión larga: la tarea se cancela, NO queda archivo
#      parcial y el editor sigue vivo (se puede seguir usando y salir con q / exit).
#   2. Ctrl+C sin tarea en curso: el editor solo avisa y sigue funcionando.
#   3. Fuera del editor, Ctrl+C vuelve a comportarse como antes (termina el shell):
#      la máscara de señales se restauró al salir del editor.
#   4. (si valgrind está instalado) sesión completa sin fugas de memoria ni de
#      descriptores de archivo.
# ============================================================================
set -u
set -m   # control de trabajos: sin esto bash lanza los procesos en segundo plano
         # IGNORANDO SIGINT y no se podría simular Ctrl+C con kill -INT
cd "$(dirname "$0")" || exit 1
BIN=./eafitOS
[ -x "$BIN" ] || { echo "ERROR: compile primero con 'make'"; exit 1; }
make -s -C huffman huf || exit 1

T=$(mktemp -d); trap 'exec 3>&- 2>/dev/null; rm -rf "$T"' EXIT
pass=0; fail=0
ok()  { echo "  OK    $1"; pass=$((pass+1)); }
bad() { echo "  FALLO $1"; fail=$((fail+1)); }
has() { sed 's/\x1b\[[0-9;]*m//g' "$T/out.txt" | grep -q -- "$1"; }

# Lanza el shell leyendo de un FIFO para poder mandarle comandos y señales "en vivo".
start() { rm -f "$T/fifo"; mkfifo "$T/fifo"; "$BIN" < "$T/fifo" > "$T/out.txt" 2>&1 & PID=$!; exec 3> "$T/fifo"; }
send()  { echo "$1" >&3; }
finish(){ exec 3>&-; wait "$PID" 2>/dev/null; RC=$?; }
# Espera (máx. 10 s) a que aparezca un texto en la salida del shell.
wait_for() { for _ in $(seq 1 500); do has "$1" && return 0; sleep 0.02; done; return 1; }

echo "-- 1: Ctrl+C durante una tarea --"
# En un computador rápido la descompresión puede terminar antes del Ctrl+C. Por eso
# la señal se envía apenas el editor confirma que la tarea arrancó, y si aun así ya
# había terminado, se repite con un archivo 3 veces más grande (hasta 3 intentos).
size=45000000
for intento in 1 2 3; do
    head -c "$size" /dev/urandom | base64 -w 100 > "$T/grande.txt"
    ./huffman/huf pc "$T/grande.txt" "$T/grande.huf" >/dev/null 2>&1
    rm -f "$T/salida.txt"
    start
    send "editor"
    send "x $T/grande.huf $T/salida.txt"
    wait_for "iniciada en segundo plano"
    kill -INT "$PID"
    wait_for "(Ctrl+C)"; sleep 0.2
    vivo=0; kill -0 "$PID" 2>/dev/null && vivo=1
    send "st"; send "q"; send "exit"
    finish
    if has "Tarea en segundo plano cancelada" || [ "$intento" -eq 3 ]; then break; fi
    echo "    (la tarea terminó antes del Ctrl+C; se repite con un archivo más grande)"
    size=$((size * 3))
done
[ "$vivo" -eq 1 ] && ok "el shell sigue vivo después de Ctrl+C" || bad "el shell murió con Ctrl+C"
has "Tarea en segundo plano cancelada" && ok "Ctrl+C canceló la tarea" || bad "Ctrl+C no canceló la tarea"
[ -e "$T/salida.txt" ] && bad "quedó archivo parcial" || ok "no quedó archivo parcial"
[ "$RC" -eq 0 ] && ok "salida normal con q / exit (código 0)" || bad "código de salida $RC"
rm -f "$T/grande.txt" "$T/grande.huf"

echo "-- 2: Ctrl+C sin tarea --"
start
send "editor"
sleep 0.2
kill -INT "$PID"; sleep 0.3
send "q"; send "exit"
finish
has "No hay ninguna tarea en curso" && ok "solo avisa que no hay tarea" || bad "no avisó"
[ "$RC" -eq 0 ] && ok "el shell siguió y salió normal" || bad "código de salida $RC"

echo "-- 3: fuera del editor Ctrl+C vuelve a lo normal --"
start
send "editor"; send "q"
sleep 0.3
{ kill -INT "$PID"; sleep 0.3; finish; } 2>/dev/null   # (oculta el aviso "Interrupt" de bash)
[ "$RC" -eq 130 ] && ok "Ctrl+C en el prompt del shell lo termina (como en el Parcial 1)" \
                  || bad "código de salida $RC (se esperaba 130)"

if command -v valgrind >/dev/null; then
    echo "-- 4: valgrind (memoria y descriptores) --"
    head -c 2000000 /dev/urandom > "$T/peq.bin"
    printf 'hola\nmundo\n' > "$T/notas.txt"
    printf 'editor %s\nc %s\nw\nx %s.huf %s\ncancel\nx %s.huf %s\nw\na otra linea\nq\nexit\n' \
        "$T/notas.txt" "$T/peq.bin" "$T/peq.bin" "$T/c1" "$T/peq.bin" "$T/c2" |
        valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect \
                 --track-fds=yes --error-exitcode=9 "$BIN" > /dev/null 2> "$T/vg.txt"
    VRC=$?
    grep -q "ERROR SUMMARY: 0 errors" "$T/vg.txt" && [ "$VRC" -ne 9 ] \
        && ok "sin errores de memoria ni fugas" || bad "valgrind reportó errores (ver salida)"
    # Al terminar solo deben quedar abiertos stdin, stdout y stderr
    open_fds=$(grep -o "FILE DESCRIPTORS: [0-9]* open" "$T/vg.txt" | grep -o "[0-9]*" | head -1)
    [ "${open_fds:-99}" -le 3 ] && ok "sin descriptores de archivo abiertos de más ($open_fds)" \
                                || bad "descriptores abiertos al salir: $open_fds"
    cmp -s "$T/peq.bin" "$T/c2" && ok "descompresión bajo valgrind idéntica" || bad "descompresión distinta"
else
    echo "-- 4: valgrind no está instalado; se omite (sudo apt install valgrind) --"
fi

echo "Resultado: $pass OK, $fail fallos"
[ "$fail" -eq 0 ]
