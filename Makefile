CC = gcc
CFLAGS = -Wall -g -O2
LDFLAGS = -libverbs -lrdmacm

SRCDIR = src
SOURCES = $(wildcard $(SRCDIR)/*.c)
TARGETS = $(patsubst $(SRCDIR)/%.c, %, $(SOURCES))

.PHONY: all clean

all: $(TARGETS)

%: $(SRCDIR)/%.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS)