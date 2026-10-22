#!/usr/bin/env bash
# ============================================================================
# Pruebas del Parcial 2, Fase 6: condiciones de carrera editor vs. compresión.
# Uso: bash test_editor_carreras.sh [repeticiones]   (desde shell/, tras 'make')
#
#   1. Mientras una tarea usa el archivo abierto, a / i / d se rechazan y p sí funciona.
#   2. Validaciones: no se puede lanzar una tarea cuya salida sea el archivo abierto o
#      su propia entrada, ni abrir con 'o' el archivo que se está generando.
#   3. ESTRÉS (100 repeticiones por defecto): abrir, comprimir y enseguida intentar
#      a / i / d. Invariante: el .huf SIEMPRE se descomprime al archivo original
#      intacto (nunca a una mezcla de versiones) y el archivo final es el original o
#      el original con las 3 ediciones completas, nunca algo intermedio.
# ============================================================================
set -u
cd "$(dirname "$0")" || exit 1
BIN=./eafitOS
REPS=${1:-100}
[ -x "$BIN" ] || { echo "ERROR: compile primero con 'make'"; exit 1; }
make -s -C huffman huf || { echo "ERROR: no compila huffman/huf"; exit 1; }
HUF=./huffman/huf

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0
ok()   { echo "  OK    $1"; pass=$((pass+1)); }
bad()  { echo "  FALLO $1"; fail=$((fail+1)); }
check(){ if grep -q -- "$2" "$T/out.txt"; then ok "$1"; else bad "$1  (no se encontró: $2)"; fi; }
run()  { "$BIN" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > "$T/out.txt"; }

# Archivo de texto dentro de los límites del editor (900 líneas de 900 caracteres)
ORIG="$T/original.txt"
head -c 700000 /dev/urandom | base64 -w 900 | head -n 900 > "$ORIG"
# .huf grande para tener una tarea que dure (descomprimir ~60 MB tarda un rato)
head -c 45000000 /dev/urandom | base64 -w 100 > "$T/grande.txt"
$HUF pc "$T/grande.txt" "$T/grande.huf" >/dev/null 2>&1

echo "-- 1: el archivo en uso no se puede modificar --"
{
    echo "editor"
    echo "x $T/grande.huf $T/salida.txt"   # tarea larga en segundo plano
    echo "o $T/grande.huf"                 # abrir la ENTRADA de la tarea
    echo "a hola"
    echo "i 1 hola"
    echo "d 1"
    echo "o $T/salida.txt"                 # abrir la SALIDA que se está generando
    echo "cancel"
    echo "q"
    echo "exit"
} | run
check "a rechazado mientras se usa el archivo"   "No se puede agregar"
check "i rechazado mientras se usa el archivo"   "No se puede insertar"
check "d rechazado mientras se usa el archivo"   "No se puede borrar"
check "o rechaza abrir la salida en generación"  "se está generando en segundo plano"

echo "-- 2: validaciones al lanzar tareas --"
cp "$ORIG" "$T/f.txt"
$HUF c "$T/f.txt" "$T/f.txt.huf" >/dev/null 2>&1
{
    echo "editor $T/f.txt.huf"             # el editor tiene abierto f.txt.huf
    echo "c $T/f.txt"                      # su salida sería f.txt.huf -> rechazar
    echo "x $T/f.txt.huf $T/f.txt.huf"     # salida == entrada -> rechazar
    echo "o $T/f.txt"
    echo "x $T/f.txt.huf $T/f.txt"         # salida == archivo abierto -> rechazar
    echo "q"
    echo "exit"
} | run
check "c no pisa el archivo abierto"            "es el archivo abierto en el editor"
check "x no usa la entrada como salida"         "es el mismo archivo que la entrada"
if [ "$(grep -c 'es el archivo abierto en el editor' "$T/out.txt")" -ge 2 ]; then
    ok "x no pisa el archivo abierto"; else bad "x no pisa el archivo abierto"; fi
cmp -s "$ORIG" "$T/f.txt" && ok "el archivo abierto quedó intacto" || bad "el archivo abierto cambió"

echo "-- 3: estrés: comprimir y editar al mismo tiempo ($REPS repeticiones) --"
# Las ediciones se aplican en orden: i 1 NUEVA, d 2, a FINAL. Cada una se rechaza si
# la compresión sigue en curso, y una vez que termina, las siguientes sí se aplican.
# Por eso los ÚNICOS archivos finales válidos son estos 4:
cp "$ORIG" "$T/E0"                                                     # ninguna
{ cat "$ORIG"; echo FINAL; } > "$T/E1"                                 # solo a
{ head -n 1 "$ORIG"; tail -n +3 "$ORIG"; echo FINAL; } > "$T/E2"      # d y a
{ echo NUEVA; tail -n +2 "$ORIG"; echo FINAL; } > "$T/E3"             # las 3
rechazadas=0; aplicadas=0; malas=0
for r in $(seq 1 "$REPS"); do
    cp "$ORIG" "$T/f.txt"; rm -f "$T/f.txt.huf"
    printf 'editor %s\nc\ni 1 NUEVA\nd 2\na FINAL\nw\nq\nexit\n' "$T/f.txt" | run
    if ! $HUF x "$T/f.txt.huf" "$T/d.txt" >/dev/null 2>&1 || ! cmp -s "$ORIG" "$T/d.txt"; then
        malas=$((malas+1)); echo "    repetición $r: el .huf NO corresponde al original"; continue
    fi
    if cmp -s "$T/f.txt" "$T/E3"; then
        aplicadas=$((aplicadas+1))
    elif cmp -s "$T/f.txt" "$T/E0" || cmp -s "$T/f.txt" "$T/E1" || cmp -s "$T/f.txt" "$T/E2"; then
        rechazadas=$((rechazadas+1))
    else
        malas=$((malas+1)); echo "    repetición $r: archivo final inesperado"
    fi
done
echo "    con alguna edición rechazada por estar comprimiendo: $rechazadas | todas aplicadas después: $aplicadas"
[ "$malas" -eq 0 ] && ok "$REPS repeticiones sin corrupción (.huf siempre == original)" \
                   || bad "$malas repeticiones con corrupción"

echo "Resultado: $pass OK, $fail fallos"
[ "$fail" -eq 0 ]
