# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDLIBS ?= -libverbs -ldl
# Explicit Linux-only opt-in build; root make/check do not build this tool.
OUTPUT ?= strix-verbs-smoke
.PHONY: all check clean gpu-kernels
all: $(OUTPUT)
$(OUTPUT): strix-verbs-smoke.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $< $(LDFLAGS) $(LDLIBS) -o $@
check: $(OUTPUT)
	"$(abspath $(OUTPUT))" --selftest
# Opt-in GPU code object for --gpu-kernels (needs ROCm's hipcc on the host).
GPU_ARCH ?= gfx1151
gpu-kernels: strix-verbs-gpu-kernels.hsaco
strix-verbs-gpu-kernels.hsaco: strix-verbs-gpu-kernels.hip
	hipcc --genco --offload-arch=$(GPU_ARCH) -O2 $< -o $@
clean:
	rm -f -- "$(OUTPUT)" strix-verbs-gpu-kernels.hsaco
