# Módulo Huffman concurrente (Parcial 2). Independiente del Makefile del shell;
# en la Fase 5 el shell lo enlazará con -Ihuffman/include y los .c de huffman/src.
#
#   make           compila huf, test_pool y test_count
#   make test      pruebas de Huffman (Fase 1), pool de hilos (Fase 2) y conteo (Fase 3)
#   make bench     tabla de tiempos del conteo: secuencial vs. 1/2/4/8/16 hilos
#   make tsan      pruebas concurrentes con ThreadSanitizer (detecta condiciones de carrera)
#   make helgrind  pruebas concurrentes con valgrind --tool=helgrind
#   make clean
CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread -Iinclude
HDR    = include/huffman.h include/threadpool.h include/huf_parallel.h
SRC    = src/huffman.c src/threadpool.c src/huf_parallel.c
BIN    = huf
TPOOL  = test_pool
TCOUNT = test_count
TESTS  = $(TPOOL) $(TCOUNT)

BENCH_FILE = /tmp/huf_bench_100MB.bin

all: $(BIN) $(TESTS)

$(BIN): src/huf_cli.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ src/huf_cli.c $(SRC)

$(TPOOL): tests/test_pool.c src/threadpool.c include/threadpool.h
	$(CC) $(CFLAGS) -o $@ tests/test_pool.c src/threadpool.c

$(TCOUNT): tests/test_count.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -O2 -o $@ tests/test_count.c $(SRC)

test: $(BIN) $(TESTS)
	bash tests/test_huffman.sh
	./$(TPOOL)
	./$(TCOUNT)

bench: $(TCOUNT)
	@[ -f $(BENCH_FILE) ] || { echo "Generando $(BENCH_FILE)..."; \
	  head -c 80000000 /dev/urandom | base64 -w 100 | head -c 100000000 > $(BENCH_FILE); }
	./$(TCOUNT) bench $(BENCH_FILE)

tsan: tests/test_pool.c tests/test_count.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -fsanitize=thread -O1 -o $(TPOOL)_tsan tests/test_pool.c src/threadpool.c
	$(CC) $(CFLAGS) -fsanitize=thread -O1 -o $(TCOUNT)_tsan tests/test_count.c $(SRC)
	./$(TPOOL)_tsan 2000
	./$(TCOUNT)_tsan quick

helgrind: $(TESTS)
	valgrind --tool=helgrind --suppressions=tests/helgrind.supp --error-exitcode=1 ./$(TPOOL) 500
	valgrind --tool=helgrind --suppressions=tests/helgrind.supp --error-exitcode=1 ./$(TCOUNT) quick

clean:
	rm -f $(BIN) $(TESTS) $(TPOOL)_tsan $(TCOUNT)_tsan

.PHONY: all test bench tsan helgrind clean
