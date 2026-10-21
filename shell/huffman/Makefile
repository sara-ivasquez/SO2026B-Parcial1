# Módulo Huffman concurrente (Parcial 2). Independiente del Makefile del shell;
# en la Fase 5 el shell lo enlazará con -Ihuffman/include y los .c de huffman/src.
CC     = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread -Iinclude
SRC    = src/huffman.c
BIN    = huf

all: $(BIN)

$(BIN): src/huf_cli.c $(SRC) include/huffman.h
	$(CC) $(CFLAGS) -o $@ src/huf_cli.c $(SRC)

test: $(BIN)
	./tests/test_huffman.sh

clean:
	rm -f $(BIN)

.PHONY: all test clean
