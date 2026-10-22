#!/usr/bin/env bash
# ============================================================================
# Pruebas del Parcial 2, Fase 5: Huffman concurrente integrado al editor.
# Uso: bash test_editor_huffman.sh   (desde shell/, después de 'make')
#
# Se alimenta el editor con comandos "en vivo" (con pausas) para comprobar que:
#   1. 'c' lanza la compresión en segundo plano y el prompt vuelve de inmediato.
#   2. Mientras corre, el editor sigue respondiendo (p, s) y 'st' muestra el %.
#   3. 'w' muestra la barra en vivo y espera; el resultado es idéntico (cmp/md5).
#   4. 'cancel' detiene la tarea y no deja archivo parcial.
#   5. 'q' se niega a salir mientras haya una tarea en curso.
# ============================================================================
set -u
cd "$(dirname "$0")" || exit 1
BIN=./eafitOS
[ -x "$BIN" ] || { echo "ERROR: compile primero con 'make'"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
BIG="$T/grande.txt"
SMALL="$T/notas.txt"
echo "Generando archivo de prueba de ~60 MB..."
head -c 45000000 /dev/urandom | base64 -w 100 > "$BIG"
printf 'primera linea\nsegunda linea con huffman\ntercera linea\n' > "$SMALL"

pass=0; fail=0
ok()   { echo "  OK    $1"; pass=$((pass+1)); }
bad()  { echo "  FALLO $1"; fail=$((fail+1)); }
check(){ if grep -q -- "$2" "$T/out.txt"; then ok "$1"; else bad "$1  (no se encontró: $2)"; fi; }

# Quita los códigos de color ANSI para poder buscar texto
run() { "$BIN" 2>&1 | sed 's/\x1b\[[0-9;]*m//g' > "$T/out.txt"; }

echo "-- 1-3: compresión y descompresión en segundo plano --"
{
    echo "editor $SMALL"
    echo "c $BIG"
    echo "p 2"                       # el editor responde mientras comprime
    echo "s huffman"
    echo "w"                         # esperar viendo la barra
    echo "x $BIG.huf $T/copia.txt"
    echo "st"                        # justo después: debe seguir en curso
    echo "q"                         # debe negarse a salir
    echo "w"
    echo "q"
    echo "exit"
} | run

check "c inicia en segundo plano"               "Compresión iniciada en segundo plano"
check "p responde durante la tarea"             "2: segunda linea con huffman"
check "s responde durante la tarea"             "1 coincidencia"
check "w muestra la barra al 100%"              "100%"
check "aviso de compresión terminada"           "Compresión terminada"
check "prompt muestra el porcentaje"            "editor \[descomprimiendo"
check "q se niega a salir con tarea en curso"   "Hay una tarea en segundo plano"
check "aviso de descompresión terminada"        "Descompresión terminada"
if cmp -s "$BIG" "$T/copia.txt"; then
    ok "descomprimido idéntico al original (md5 $(md5sum < "$BIG" | cut -c1-8))"
else
    bad "descomprimido distinto al original"
fi

echo "-- 4: cancelación --"
{
    echo "editor"
    echo "x $BIG.huf $T/cancelado.txt"
    echo "cancel"
    echo "st"
    echo "q"
    echo "exit"
} | run
check "cancel detiene la tarea"                 "cancelada"
if [ -e "$T/cancelado.txt" ]; then bad "quedó salida parcial"; else ok "no quedó salida parcial"; fi

echo "-- 5: errores --"
{
    echo "editor"
    echo "c"                          # sin archivo abierto ni argumento
    echo "c $T/no_existe.txt"
    echo "w"
    echo "x $SMALL"                   # no termina en .huf
    echo "q"
    echo "exit"
} | run
check "c sin archivo muestra el uso"            "Uso: c \[archivo\]"
check "archivo inexistente informa el error"    "falló: No such file or directory"
check "x sin .huf pide la salida"               "no termina en .huf"

echo "Resultado: $pass OK, $fail fallos"
[ "$fail" -eq 0 ]
