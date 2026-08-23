CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra

HIPCC ?= $(shell command -v hipcc 2>/dev/null || \
	if [ -x /opt/rocm-therock/bin/hipcc ]; then \
		echo /opt/rocm-therock/bin/hipcc; \
	else \
		echo /opt/rocm/bin/hipcc; \
	fi)
ROCM_ROOT := $(abspath $(dir $(HIPCC))/..)
HIPFLAGS ?= -O2 -Wall -Wextra -Werror --offload-arch=gfx1151
HIPLDFLAGS ?= -Wl,-rpath,$(ROCM_ROOT)/lib

all: tbstream-stale-tx tbstream-import-test

hip: tbstream-stale-rx tbstream-gpu-tx tbstream-tp-pingpong

tbstream-stale-tx: tbstream-stale-tx.c stale-pattern.h ../pingpong/thunderbolt-stream.h
	$(CC) $(CFLAGS) -o $@ $<

tbstream-import-test: tbstream-import-test.c ../pingpong/thunderbolt-stream.h
	$(CC) $(CFLAGS) -o $@ $<

tbstream-stale-rx: tbstream-stale-rx.cu stale-pattern.h ../pingpong/thunderbolt-stream.h
	$(HIPCC) $(HIPFLAGS) $(HIPLDFLAGS) -o $@ $<

tbstream-gpu-tx: tbstream-gpu-tx.cu stale-pattern.h ../pingpong/thunderbolt-stream.h
	$(HIPCC) $(HIPFLAGS) $(HIPLDFLAGS) -o $@ $<

tbstream-tp-pingpong: tbstream-tp-pingpong.cu ../pingpong/thunderbolt-stream.h
	$(HIPCC) $(HIPFLAGS) $(HIPLDFLAGS) -o $@ $<

clean:
	rm -f tbstream-stale-tx tbstream-stale-rx tbstream-gpu-tx \
		tbstream-import-test tbstream-tp-pingpong

.PHONY: all hip clean
