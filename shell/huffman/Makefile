# Módulo Huffman concurrente (Parcial 2). Independiente del Makefile del shell;
# en la Fase 5 el shell lo enlazará con -Ihuffman/include y los .c de huffman/src.
#
#   make           compila huf y test_pool
#   make test      pruebas de Huffman (Fase 1) y del pool de hilos (Fase 2)
#   make tsan      pool compilado con ThreadSanitizer (detecta condiciones de carrera)
#   make helgrind  pool revisado con valgrind --tool=helgrind
#   make clean
CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread -Iinclude
SRC    = src/huffman.c src/threadpool.c
HDR    = include/huffman.h include/threadpool.h
BIN    = huf
TPOOL  = test_pool

all: $(BIN) $(TPOOL)

$(BIN): src/huf_cli.c $(SRC) $(HDR)
	$(CC) $(CFLAGS) -o $@ src/huf_cli.c $(SRC)

$(TPOOL): tests/test_pool.c src/threadpool.c include/threadpool.h
	$(CC) $(CFLAGS) -o $@ tests/test_pool.c src/threadpool.c

test: $(BIN) $(TPOOL)
	bash tests/test_huffman.sh
	./$(TPOOL)

tsan: tests/test_pool.c src/threadpool.c include/threadpool.h
	$(CC) $(CFLAGS) -fsanitize=thread -O1 -o $(TPOOL)_tsan tests/test_pool.c src/threadpool.c
	./$(TPOOL)_tsan 2000

helgrind: $(TPOOL)
	valgrind --tool=helgrind --suppressions=tests/helgrind.supp --error-exitcode=1 ./$(TPOOL) 500

clean:
	rm -f $(BIN) $(TPOOL) $(TPOOL)_tsan

.PHONY: all test tsan helgrind clean
