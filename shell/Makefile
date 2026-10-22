CC = gcc
# -pthread: el editor lanza la compresión Huffman en hilos (Parcial 2)
# -Ihuffman/include: cabeceras del módulo Huffman concurrente
CFLAGS = -Wall -Wextra -std=gnu99 -g -D_GNU_SOURCE -pthread -Ihuffman/include
ARCHSALIDA = eafitOS
SRCS = main.c cat_datos.c cat_memoria.c cat_monitoreo.c cat_util.c cat_editor.c

# Módulo Huffman concurrente (Parcial 2), compilado con optimización
HUF_SRCS = huffman/src/huffman.c huffman/src/threadpool.c \
           huffman/src/huf_parallel.c huffman/src/huf_job.c
HUF_HDRS = $(wildcard huffman/include/*.h)

OBJS = $(SRCS:.c=.o) $(HUF_SRCS:.c=.o)

all: $(ARCHSALIDA)

$(ARCHSALIDA): $(OBJS)
	$(CC) $(CFLAGS) -o $(ARCHSALIDA) $(OBJS)

%.o: %.c shell.h $(HUF_HDRS)
	$(CC) $(CFLAGS) -c $< -o $@

huffman/src/%.o: huffman/src/%.c $(HUF_HDRS)
	$(CC) $(CFLAGS) -O2 -c $< -o $@

clean:
	rm -f $(ARCHSALIDA) $(OBJS)

.PHONY: all clean
