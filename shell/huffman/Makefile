# Módulo Huffman concurrente (Parcial 2). Independiente del Makefile del shell;
# el Makefile del shell (../Makefile) enlaza estos mismos .c dentro de eafitOS.
#
#   make           compila huf y los programas de prueba
#   make test      pruebas de las Fases 1 a 4
#   make bench     tablas de tiempos (conteo y compresión): secuencial vs. 1/2/4/8/16 hilos
#   make tsan      pruebas concurrentes con ThreadSanitizer (detecta condiciones de carrera)
#   make helgrind  pruebas concurrentes con valgrind --tool=helgrind
#   make clean
CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread -Iinclude
HDR    = include/huffman.h include/threadpool.h include/huf_parallel.h include/huf_job.h
SRC    = src/huffman.c src/threadpool.c src/huf_parallel.c src/huf_job.c
BIN    = huf
TPOOL  = test_pool
TCOUNT = test_count
TPAR   = test_parallel
TESTS  = $(TPOOL) $(TCOUNT) $(TPAR)

BENCH_FILE = /tmp/huf_bench_100MB.bin

all: $(BIN) $(TESTS)

$(BIN): src/huf_cli.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -O2 -o $@ src/huf_cli.c $(SRC)

$(TPOOL): tests/test_pool.c src/threadpool.c include/threadpool.h
	$(CC) $(CFLAGS) -o $@ tests/test_pool.c src/threadpool.c

$(TCOUNT): tests/test_count.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -O2 -o $@ tests/test_count.c $(SRC)

$(TPAR): tests/test_parallel.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -O2 -o $@ tests/test_parallel.c $(SRC)

test: $(BIN) $(TESTS)
	bash tests/test_huffman.sh
	./$(TPOOL)
	./$(TCOUNT)
	./$(TPAR)

bench: $(BIN) $(TCOUNT)
	@[ -f $(BENCH_FILE) ] || { echo "Generando $(BENCH_FILE)..."; \
	  head -c 80000000 /dev/urandom | base64 -w 100 | head -c 100000000 > $(BENCH_FILE); }
	./$(TCOUNT) bench $(BENCH_FILE)
	./$(BIN) bench $(BENCH_FILE)

tsan: tests/test_pool.c tests/test_count.c tests/test_parallel.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -fsanitize=thread -O1 -o $(TPOOL)_tsan tests/test_pool.c src/threadpool.c
	$(CC) $(CFLAGS) -fsanitize=thread -O1 -o $(TCOUNT)_tsan tests/test_count.c $(SRC)
	$(CC) $(CFLAGS) -fsanitize=thread -O1 -o $(TPAR)_tsan tests/test_parallel.c $(SRC)
	./$(TPOOL)_tsan 2000
	./$(TCOUNT)_tsan quick
	./$(TPAR)_tsan quick

helgrind: $(TESTS)
	valgrind --tool=helgrind --suppressions=tests/helgrind.supp --error-exitcode=1 ./$(TPOOL) 500
	valgrind --tool=helgrind --suppressions=tests/helgrind.supp --error-exitcode=1 ./$(TCOUNT) quick
	valgrind --tool=helgrind --suppressions=tests/helgrind.supp --error-exitcode=1 ./$(TPAR) quick

clean:
	rm -f $(BIN) $(TESTS) $(TPOOL)_tsan $(TCOUNT)_tsan $(TPAR)_tsan

.PHONY: all test bench tsan helgrind clean
