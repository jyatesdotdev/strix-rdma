# strix-rdma — single entry point for building, testing, and host deployment.
#
#   make            build the portable userspace tools (pingpong, ds4-shape)
#   make rocm       build the ROCm/HIP tools (needs hipcc, gfx1151 target)
#   make check      host-independent tests: selftests, lifecycle mocks, python, UAPI
#   make check-kernel  apply/verify the zero-copy patch series (needs a linux
#                      checkout with the USB4STREAM backport; KERNEL_SRC)
#   sudo make install-lifecycle ROLE=allocator|follower
#                   install the managed stream lifecycle (udev, systemd,
#                   modules-load, sysconfig, libexec helpers) on a host
#   make clean

PREFIX     ?= /usr/local
LIBEXECDIR ?= $(PREFIX)/libexec
DOCDIR     ?= $(PREFIX)/share/doc/strix-rdma
UNITDIR    ?= /etc/systemd/system
UDEVDIR    ?= /etc/udev/rules.d
MODLOADDIR ?= /etc/modules-load.d
SYSCONFDIR ?= /etc/sysconfig
KERNEL_SRC ?= $(CURDIR)/linux

.PHONY: all tools rocm check check-kernel install-lifecycle clean

all: tools

tools:
	$(MAKE) -C tools/pingpong
	$(MAKE) -C tools/ds4-shape
	$(MAKE) -C tools/dmabuf-probe

rocm:
	$(MAKE) -C tools/rocm-map

check: tools
	$(MAKE) -C tools/ds4-shape check
	$(MAKE) -C tools/tests test
	python3 bench/scripts/tests/test_analyze_ds4_rocm_events.py
	$(MAKE) -C kernel/tests check-uapi

check-kernel:
	$(MAKE) -C kernel/tests test KERNEL_SRC="$(KERNEL_SRC)"

# Installs the fail-closed tbstream lifecycle documented in
# tools/systemd/tbstream-lifecycle.md. ROLE selects the sysconfig template;
# exactly one endpoint of the link may be the allocator.
install-lifecycle:
	@case "$(ROLE)" in \
	    allocator|follower) ;; \
	    *) echo "error: set ROLE=allocator or ROLE=follower" >&2; exit 1 ;; \
	esac
	install -d $(DESTDIR)$(LIBEXECDIR) $(DESTDIR)$(DOCDIR) \
	    $(DESTDIR)$(UNITDIR) $(DESTDIR)$(UDEVDIR) \
	    $(DESTDIR)$(MODLOADDIR) $(DESTDIR)$(SYSCONFDIR)
	install -m 0755 tools/scripts/ds4-tbstream-reconcile.sh \
	    tools/scripts/ds4-tbstream-cleanup.sh $(DESTDIR)$(LIBEXECDIR)/
	install -m 0644 tools/systemd/ds4-tbstream-reconcile.service \
	    tools/systemd/ds4-tbstream-reconcile.timer \
	    tools/systemd/ds4-tbstream-reconcile-watchdog.service \
	    $(DESTDIR)$(UNITDIR)/
	install -m 0644 tools/udev/98-ds4-tbstream-reconcile.rules \
	    tools/udev/99-tbstream.rules $(DESTDIR)$(UDEVDIR)/
	install -m 0644 tools/modules-load/ds4-tbstream.conf $(DESTDIR)$(MODLOADDIR)/
	install -m 0644 tools/systemd/tbstream-lifecycle.md $(DESTDIR)$(DOCDIR)/
	@if [ -e $(DESTDIR)$(SYSCONFDIR)/ds4-tbstream ] && [ "$(FORCE)" != 1 ]; then \
	    existing_role=$$(awk -F= '/^TBSTREAM_ROLE=/{print $$2; exit}' \
	        $(DESTDIR)$(SYSCONFDIR)/ds4-tbstream); \
	    if [ "$$existing_role" != "$(ROLE)" ]; then \
	        echo "error: existing $(DESTDIR)$(SYSCONFDIR)/ds4-tbstream has TBSTREAM_ROLE=$$existing_role, requested ROLE=$(ROLE) (FORCE=1 overwrites)" >&2; \
	        exit 1; \
	    fi; \
	    echo "kept existing $(DESTDIR)$(SYSCONFDIR)/ds4-tbstream (TBSTREAM_ROLE=$$existing_role; FORCE=1 overwrites)"; \
	else \
	    install -m 0644 tools/sysconfig/ds4-tbstream.$(ROLE) \
	        $(DESTDIR)$(SYSCONFDIR)/ds4-tbstream; \
	    echo "installed $(SYSCONFDIR)/ds4-tbstream ($(ROLE)); review its HopID/netdev values"; \
	fi
	@echo ""
	@echo "Next steps on this host:"
	@echo "  sudo groupadd --force --system tbstream"
	@echo "  sudo usermod -aG tbstream \$$(id -un)   # then start a new login session"
	@echo "  sudo udevadm control --reload-rules"
	@echo "  sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match='tbstream*'"
	@echo "  sudo systemctl daemon-reload"
	@echo "  sudo systemctl enable --now ds4-tbstream-reconcile.timer"
	@echo "  sudo systemctl start ds4-tbstream-reconcile.service"
	@echo "The converged device is published at /run/ds4-tbstream/device."

clean:
	$(MAKE) -C tools/pingpong clean
	$(MAKE) -C tools/ds4-shape clean
	$(MAKE) -C tools/dmabuf-probe clean
	$(MAKE) -C tools/rocm-map clean
