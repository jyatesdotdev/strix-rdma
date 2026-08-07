HIPCC ?= $(shell command -v hipcc 2>/dev/null || \
	if [ -x /opt/rocm-therock/bin/hipcc ]; then \
		echo /opt/rocm-therock/bin/hipcc; \
	else \
		echo /opt/rocm/bin/hipcc; \
	fi)
ROCM_ROOT := $(abspath $(dir $(HIPCC))/..)
HIPFLAGS ?= -O2 -Wall -Wextra -Werror --offload-arch=gfx1151
HIPLDFLAGS ?= -Wl,-rpath,$(ROCM_ROOT)/lib

all: tbstream-hip-map tbstream-hip-zc

tbstream-hip-map: tbstream-hip-map.cu ../pingpong/thunderbolt-stream.h
	$(HIPCC) $(HIPFLAGS) $(HIPLDFLAGS) -o $@ $<

tbstream-hip-zc: tbstream-hip-zc.cu ../pingpong/thunderbolt-stream.h
	$(HIPCC) $(HIPFLAGS) $(HIPLDFLAGS) -o $@ $<

clean:
	rm -f tbstream-hip-map tbstream-hip-zc

.PHONY: all clean
