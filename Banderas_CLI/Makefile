CC = gcc
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE
ARCHSALIDA = eafitOS
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_editor.c
OBJS = $(SRCS:.c=.o)

all: $(ARCHSALIDA)

$(ARCHSALIDA): $(OBJS)
	$(CC) $(CFLAGS) -o $(ARCHSALIDA) $(OBJS)

%.o: %.c shell.h
	$(CC) $(CFLAGS) -c $< -o $@

# --- Parcial 2: driver de pruebas del núcleo Huffman (Fase 1) ---
huf: huf_cli.c huffman.c huffman.h
	$(CC) $(CFLAGS) -pthread -o huf huf_cli.c huffman.c

clean:
	rm -f $(ARCHSALIDA) $(OBJS) huf

.PHONY: all clean huf