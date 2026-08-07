CC ?= cc
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror

all: tbstream-ds4-gate

tbstream-ds4-gate: tbstream-ds4-gate.c ../pingpong/thunderbolt-stream.h
	$(CC) $(CFLAGS) -o $@ $<

check: tbstream-ds4-gate
	./tbstream-ds4-gate selftest

clean:
	rm -f tbstream-ds4-gate

.PHONY: all check clean
