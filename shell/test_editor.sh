#!/usr/bin/env bash
# ============================================================================
# Script de pruebas - Editor de Texto (Sistemas Operativos, EAFIT)
# Valida los comandos base + nivel "parejas" (i, s) contra el binario ./editor
# ============================================================================
# Uso: ./test_editor.sh   (ejecutar desde la carpeta raíz del proyecto, donde
#                          está el binario 'editor' ya compilado con 'make')

set -u
ARCHIVO="test_prueba.txt"
BIN="./eafitOS"

if [ ! -x "$BIN" ]; then
    echo "ERROR: no se encontró el binario '$BIN'. Corre 'make' primero."
    exit 1
fi

rm -f "$ARCHIVO"

echo "============================================================"
echo " TEST 1: Flujo básico + insercion (i) + busqueda (s)"
echo "============================================================"
"$BIN" <<EOF
editor $ARCHIVO
a primera linea
a segunda linea
a cuarta linea
p
i 3 tercera linea
p
s linea
s xyz
q
s
exit
EOF

echo
echo "== Verificación en disco tras Test 1 =="
cat -A "$ARCHIVO" 2>/dev/null || echo "(no se pudo leer el archivo)"
echo

echo "============================================================"
echo " TEST 2: Borrar (d) + insertar al inicio y al final (i)"
echo "============================================================"
"$BIN" <<EOF
editor $ARCHIVO
p
d 2
p
i 1 nueva primera linea
i 5 nueva ultima linea
p
q
s
exit
EOF

echo
echo "== Verificación en disco tras Test 2 =="
cat -A "$ARCHIVO" 2>/dev/null
echo

echo "============================================================"
echo " TEST 3: Casos borde (posiciones inválidas, sin archivo abierto)"
echo "============================================================"
"$BIN" <<EOF
editor
p
a esto no deberia guardarse sin archivo
i 1 tampoco deberia funcionar
d 1
s algo
o $ARCHIVO
p
i 0 posicion invalida cero
i 999 posicion invalida muy grande
d 999
p
q
exit
EOF

echo
echo "== Verificación en disco tras Test 3 (no debio cambiar) =="
cat -A "$ARCHIVO" 2>/dev/null
echo
echo
echo "== Verificación en disco tras Test 3 (no debio cambiar, se respondio 'n') =="
cat -A "$ARCHIVO" 2>/dev/null
echo

echo "============================================================"
echo " TEST 4: Descartar cambios sin guardar (respuesta 'n' al salir)"
echo "============================================================"
"$BIN" <<EOF
editor $ARCHIVO
a esta linea no deberia persistir
p
q
n
exit
EOF

echo
echo "== Verificación final en disco (la linea de TEST 4 NO debe aparecer) =="
cat -A "$ARCHIVO" 2>/dev/null

echo
echo "============================================================"
echo " Pruebas terminadas. Revisa manualmente que:"
echo "  - En TEST 1: 'tercera linea' haya quedado en la posicion 3"
echo "  - En TEST 1: 's linea' encontro 3 coincidencias, 's xyz' encontro 0"
echo "  - En TEST 2: 'd 2' borro 'segunda linea' y las inserciones en 1 y al final funcionaron"
echo "  - En TEST 3: los mensajes de error de rango/posicion invalida aparecieron"
echo "  - En TEST 4: al responder 'n', el archivo en disco NO cambio"
echo "============================================================"