// SPDX-License-Identifier: MIT
/*
 * tbstream-import-test - gate-6 rollback coverage for TBSTREAM_ZC_IMPORT.
 *
 * Drives every userspace-reachable error branch of the import ioctl
 * against a real device using CPU-only udmabuf objects, then proves the
 * device is left fully functional. No traffic is generated and no peer
 * participation is required: every case either fails before touching
 * ring state or is released without activation.
 *
 * Requires root and thunderbolt_stream.zc_diagnostic_dmabuf=1.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__

#include "../pingpong/thunderbolt-stream.h"

#if __has_include(<linux/udmabuf.h>)
#include <linux/udmabuf.h>
#else
struct udmabuf_create {
	uint32_t memfd;
	uint32_t flags;
	uint64_t offset;
	uint64_t size;
};
#define UDMABUF_CREATE		_IOW('u', 0x42, struct udmabuf_create)
#endif

#ifndef MFD_ALLOW_SEALING
#define MFD_ALLOW_SEALING	0x0002U
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS		1033
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK		0x0002
#endif

static int failures;

static int create_udmabuf(uint64_t size)
{
	struct udmabuf_create create = { 0 };
	int memfd, udma, fd;

	memfd = memfd_create("import-test", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (memfd < 0 || ftruncate(memfd, (off_t)size) ||
	    fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
		perror("memfd");
		exit(1);
	}
	udma = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udma < 0) {
		perror("/dev/udmabuf");
		exit(1);
	}
	create.memfd = (uint32_t)memfd;
	create.size = size;
	fd = ioctl(udma, UDMABUF_CREATE, &create);
	if (fd < 0) {
		perror("UDMABUF_CREATE");
		exit(1);
	}
	close(udma);
	close(memfd);
	return fd;
}

static void expect(const char *name, int fd, struct tbstream_zc_import *imp,
		   int want_errno)
{
	int ret = ioctl(fd, TBSTREAM_ZC_IMPORT, imp);

	if (!want_errno) {
		if (ret) {
			printf("FAIL %-28s: expected success, got %s\n",
			       name, strerror(errno));
			failures++;
		} else {
			printf("ok   %-28s: success\n", name);
		}
		return;
	}
	if (!ret) {
		printf("FAIL %-28s: unexpectedly succeeded\n", name);
		failures++;
	} else if (errno != want_errno) {
		printf("FAIL %-28s: expected %s, got %s\n", name,
		       strerror(want_errno), strerror(errno));
		failures++;
	} else {
		printf("ok   %-28s: %s\n", name, strerror(errno));
	}
}

static struct tbstream_zc_import valid_rx(int fd, uint64_t pool_bytes)
{
	struct tbstream_zc_import imp = { 0 };

	imp.version = TBSTREAM_ZC_IMPORT_VERSION;
	imp.tx.fd = -1;
	imp.rx.fd = fd;
	imp.rx.offset = 0;
	imp.rx.length = pool_bytes;
	return imp;
}

int main(int argc, char **argv)
{
	const char *device = "/dev/tbstream0";
	uint32_t ring = 4096;
	uint64_t pool_bytes;
	int udma, fd;

	if (argc >= 2)
		device = argv[1];
	if (argc >= 3)
		ring = (uint32_t)strtoul(argv[2], NULL, 0);
	pool_bytes = (uint64_t)ring * TBSTREAM_ZC_FRAME_SIZE;

	udma = create_udmabuf(pool_bytes);

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		return 1;
	}

	/* Argument validation: all fail before touching device state. */
	{
		struct tbstream_zc_import imp = valid_rx(udma, pool_bytes);

		imp.version = 0;
		expect("bad version", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.flags = 1;
		expect("bad flags", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.reserved[3] = 1;
		expect("nonzero reserved", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.fd = -1;
		imp.rx.length = 0;
		expect("no pool selected", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.tx.length = pool_bytes;	/* fd -1 but nonzero range */
		expect("range without fd", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.flags = 1;
		expect("bad range flags", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.fd = 998;
		expect("bad fd", fd, &imp, EBADF);

		int memfd = memfd_create("not-a-dmabuf", MFD_CLOEXEC);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.fd = memfd;
		expect("non-dmabuf fd", fd, &imp, EINVAL);
		close(memfd);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.length = pool_bytes - TBSTREAM_ZC_FRAME_SIZE;
		expect("short pool length", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.offset = 2048;
		expect("misaligned offset", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.rx.offset = TBSTREAM_ZC_FRAME_SIZE;
		expect("range past end", fd, &imp, EINVAL);

		imp = valid_rx(udma, pool_bytes);
		imp.tx.fd = udma;
		imp.tx.offset = 0;
		imp.tx.length = pool_bytes;
		expect("same buffer both dirs", fd, &imp, EINVAL);
	}

	/* Exclusivity: a second open blocks import. */
	{
		struct tbstream_zc_import imp = valid_rx(udma, pool_bytes);
		int fd2 = open(device, O_RDWR | O_CLOEXEC);

		if (fd2 < 0) {
			perror("second open");
			return 1;
		}
		expect("import with two opens", fd, &imp, EBUSY);
		close(fd2);
	}

	/* Activation ordering: import after enable is rejected. */
	{
		struct tbstream_zc_import imp = valid_rx(udma, pool_bytes);

		if (ioctl(fd, TBSTREAM_ZC_ENABLE)) {
			perror("TBSTREAM_ZC_ENABLE");
			return 1;
		}
		expect("import after activation", fd, &imp, EBUSY);
	}

	/*
	 * Fresh device: valid import, double import, then release by
	 * final close without ever activating.
	 */
	close(fd);
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		return 1;
	}
	{
		struct tbstream_zc_import imp = valid_rx(udma, pool_bytes);

		expect("valid import", fd, &imp, 0);
		expect("double import", fd, &imp, EBUSY);
	}
	close(fd);	/* never activated: no CLOSE, import released */

	/* The device must still work normally after all of the above. */
	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("reopen");
		return 1;
	}
	{
		struct tbstream_zc_info info;

		if (ioctl(fd, TBSTREAM_ZC_ENABLE) ||
		    ioctl(fd, TBSTREAM_ZC_GET_INFO, &info) ||
		    info.ring_size != ring) {
			printf("FAIL device unusable after rollback tests\n");
			failures++;
		} else {
			printf("ok   %-28s: ring %u\n",
			       "device healthy afterwards", info.ring_size);
		}
	}
	close(fd);
	close(udma);

	if (failures) {
		printf("RESULT: %d FAILURES\n", failures);
		return 1;
	}
	printf("RESULT: ALL ROLLBACK PATHS OK\n");
	return 0;
}

#else /* !__linux__ */

int main(void)
{
	fprintf(stderr, "tbstream-import-test is Linux-only\n");
	return 2;
}

#endif /* __linux__ */
