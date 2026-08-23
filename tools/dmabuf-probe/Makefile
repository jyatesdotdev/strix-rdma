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

all: tbstream-dmabuf-probe

hip: tbstream-hip-export

tbstream-dmabuf-probe: tbstream-dmabuf-probe.c ../pingpong/thunderbolt-stream.h
	$(CC) $(CFLAGS) -I../pingpong -o $@ $<

tbstream-hip-export: tbstream-hip-export.cu
	$(HIPCC) $(HIPFLAGS) $(HIPLDFLAGS) -o $@ $<

clean:
	rm -f tbstream-dmabuf-probe tbstream-hip-export

.PHONY: all hip clean
