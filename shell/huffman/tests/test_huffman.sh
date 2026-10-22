#!/bin/bash
# Pruebas de integridad del núcleo Huffman (Fase 1).  Uso: make test  (o ./tests/test_huffman.sh)
# Cada caso: comprimir -> descomprimir -> cmp con el original.
cd "$(dirname "$0")/.." || exit 1      # raíz del módulo: shell/huffman
make >/dev/null || { echo "FALLO: no compila"; exit 1; }

T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
pass=0; fail=0

caso() {   # caso <nombre> <archivo> [block_size]
    local nombre=$1 f=$2 bs=$3
    if ./huf c "$f" "$T/o.huf" $bs 2>/dev/null && ./huf x "$T/o.huf" "$T/o.out" 2>/dev/null \
       && cmp -s "$f" "$T/o.out"; then
        printf "  OK    %-34s %9d -> %9d bytes  md5 %s\n" "$nombre" "$(stat -c%s "$f")" \
            "$(stat -c%s "$T/o.huf")" "$(md5sum < "$f" | cut -c1-8)"
        pass=$((pass+1))
    else
        echo "  FALLO $nombre"; fail=$((fail+1))
    fi
}

: > "$T/vacio";                         caso "archivo vacío" "$T/vacio"
printf 'A' > "$T/1byte";                caso "1 byte" "$T/1byte"
head -c 1000000 /dev/zero > "$T/ceros"; caso "1 MB de un solo símbolo" "$T/ceros"
printf 'hola\nmundo sin salto' > "$T/txt"; caso "texto sin '\\n' final" "$T/txt"
for i in $(seq 1 3000); do echo "línea $i: concurrencia y sincronización en SO"; done > "$T/texto"
caso "texto repetitivo (bs 4096)" "$T/texto" 4096
head -c 5000000 /dev/urandom > "$T/rand";  caso "5 MB aleatorios" "$T/rand"
head -c 4096 /dev/urandom > "$T/exacto";   caso "tamaño == block_size" "$T/exacto" 4096
head -c 4097 /dev/urandom > "$T/masuno";   caso "tamaño == block_size + 1" "$T/masuno" 4096
head -c 8192 /dev/urandom > "$T/doble";    caso "tamaño == 2 * block_size" "$T/doble" 4096
cat src/*.c > "$T/fuentes";                  caso "código fuente C" "$T/fuentes" 1000
caso "binario (el propio ejecutable)" ./huf

# Archivos inválidos: deben FALLAR (retorno != 0) y no dejar salida parcial
echo "-- casos que deben fallar --"
./huf c "$T/texto" "$T/t.huf" 4096 2>/dev/null
head -c 3000 "$T/t.huf" > "$T/trunc.huf"
if ./huf x "$T/trunc.huf" "$T/trunc.out" 2>/dev/null; then echo "  FALLO truncado aceptado"; fail=$((fail+1))
else echo "  OK    .huf truncado rechazado"; pass=$((pass+1)); fi
[ -e "$T/trunc.out" ] && { echo "  FALLO quedó salida parcial"; fail=$((fail+1)); }
if ./huf x "$T/texto" "$T/x.out" 2>/dev/null; then echo "  FALLO no-.huf aceptado"; fail=$((fail+1))
else echo "  OK    archivo sin magic rechazado"; pass=$((pass+1)); fi
if ./huf c "$T/no_existe" "$T/y.huf" 2>/dev/null; then echo "  FALLO entrada inexistente"; fail=$((fail+1))
else echo "  OK    entrada inexistente rechazada"; pass=$((pass+1)); fi

echo "Resultado: $pass OK, $fail fallos"
[ "$fail" -eq 0 ]
