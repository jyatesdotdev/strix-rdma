# strix-rdma — single entry point for building, testing, and host deployment.
#
#   make            build the portable userspace tools (pingpong, dmabuf-probe)
#   make rocm       build the ROCm/HIP tools (needs hipcc, gfx1151 target)
#   make check      host-independent tests: device access policy, UAPI
#   make check-kernel  apply/verify the zero-copy patch series (needs a linux
#                      checkout with the USB4STREAM backport; KERNEL_SRC)
#   sudo make install-lifecycle
#                   install the host-side stream lifecycle (udev device access
#                   policy and its documentation) on a host
#   make clean

PREFIX     ?= /usr/local
DOCDIR     ?= $(PREFIX)/share/doc/strix-rdma
UDEVDIR    ?= /etc/udev/rules.d
KERNEL_SRC ?= $(CURDIR)/linux

.PHONY: all tools rocm check check-kernel check-native-verbs install-lifecycle clean

all: tools

tools:
	$(MAKE) -C tools/pingpong
	$(MAKE) -C tools/dmabuf-probe

rocm:
	$(MAKE) -C tools/rocm-map

check: tools
	$(MAKE) -C tools/tests test
	$(MAKE) -C kernel/tests check-uapi

check-kernel:
	$(MAKE) -C kernel/tests test KERNEL_SRC="$(KERNEL_SRC)"

# Experimental native-verbs offline behavior only; no build/install/live action.
check-native-verbs:
	$(MAKE) -C kernel/tests check-native-verbs
	CC="$(CC)" CFLAGS="$(CFLAGS)" sh providers/strix_nhi/test-identity.sh

# Installs the scoped /dev/tbstreamN access policy documented in
# tools/systemd/tbstream-lifecycle.md. The stream UAPI exposes DMA-mapped
# pages, so the device stays group-readable and is never world-writable.
install-lifecycle:
	install -d $(DESTDIR)$(DOCDIR) $(DESTDIR)$(UDEVDIR)
	install -m 0644 tools/udev/99-tbstream.rules $(DESTDIR)$(UDEVDIR)/
	install -m 0644 tools/systemd/tbstream-lifecycle.md $(DESTDIR)$(DOCDIR)/
	@echo ""
	@echo "Next steps on this host:"
	@echo "  sudo groupadd --force --system tbstream"
	@echo "  sudo usermod -aG tbstream \$$(id -un)   # then start a new login session"
	@echo "  sudo udevadm control --reload-rules"
	@echo "  sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match='tbstream*'"
	@echo "Create the stream itself with tools/scripts/tbstream-setup.sh (same name on both hosts)."

clean:
	$(MAKE) -C tools/pingpong clean
	$(MAKE) -C tools/dmabuf-probe clean
	$(MAKE) -C tools/rocm-map clean
