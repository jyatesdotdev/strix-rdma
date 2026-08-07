CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

all: pingpong

pingpong: pingpong.c thunderbolt-stream.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f pingpong

.PHONY: all clean
