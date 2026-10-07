# SPDX-License-Identifier: GPL-2.0-only
CC ?= cc
CFLAGS ?= -O2 -g
CPPFLAGS ?=
LDLIBS ?= -libverbs
# Explicit Linux-only opt-in build; root make/check do not build this tool.
OUTPUT ?= strix-verbs-smoke
.PHONY: all check clean
all: $(OUTPUT)
$(OUTPUT): strix-verbs-smoke.c
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=gnu11 -Wall -Wextra -Werror $< $(LDFLAGS) $(LDLIBS) -o $@
check: $(OUTPUT)
	"$(abspath $(OUTPUT))" --selftest
clean:
	rm -f -- "$(OUTPUT)"
