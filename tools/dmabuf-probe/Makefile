CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: tbstream-dmabuf-probe

tbstream-dmabuf-probe: tbstream-dmabuf-probe.c ../pingpong/thunderbolt-stream.h
	$(CC) $(CFLAGS) -I../pingpong -o $@ $<

clean:
	rm -f tbstream-dmabuf-probe

.PHONY: all clean
