.PHONY: test check-uapi

CC ?= cc
KERNEL_SRC ?= ../../linux

test:
	./test-zerocopy-series.sh "$(KERNEL_SRC)"

check-uapi:
	$(CC) -std=c11 -Wall -Wextra -Werror -nostdinc \
		-Iinclude -I../../tools/pingpong \
		-DTBSTREAM_TEST_U64_ALIGNMENT=4 \
		-DTBSTREAM_TEST_USERSPACE \
		-fsyntax-only test-uapi-layout.c
	$(CC) -std=c11 -Wall -Wextra -Werror -nostdinc \
		-Iinclude -I../../tools/pingpong \
		-DTBSTREAM_TEST_U64_ALIGNMENT=8 \
		-DTBSTREAM_TEST_USERSPACE \
		-fsyntax-only test-uapi-layout.c
