/*
 * tbstream-dmabuf-probe - run the privileged, no-traffic DMA-BUF import
 * probe against a Thunderbolt stream device.
 *
 * The probe ioctl attaches one DMA-BUF to the stream's NHI DMA device,
 * pins/maps it in one direction, validates the mapped SG geometry, and
 * tears everything down before reporting aggregate statistics. It never
 * programs a ring descriptor and never exposes a DMA address.
 *
 * The buffer under test is either an inherited DMA-BUF fd (--fd, for
 * example a HIP DMA-BUF exported by a companion process) or a fresh
 * CPU-only buffer created through /dev/udmabuf (--udmabuf), which is
 * enough to smoke the import path without a GPU.
 *
 * Requires root (CAP_SYS_RAWIO) and
 * thunderbolt_stream.zc_diagnostic_dmabuf=1.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __linux__

#include <sys/mman.h>

#include "thunderbolt-stream.h"

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
#define UDMABUF_FLAGS_CLOEXEC	0x01
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC		0x0001U
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

static void usage(FILE *out)
{
	fprintf(out,
		"usage: tbstream-dmabuf-probe --direction tx|rx\n"
		"       (--udmabuf SIZE | --fd FD) [--device PATH]\n"
		"       [--offset N] [--length N]\n"
		"\n"
		"Run the no-traffic DMA-BUF import probe on a stream device.\n"
		"Sizes accept K/M/G suffixes. --length defaults to the whole\n"
		"object; --offset and --length must be 4 KiB multiples.\n");
}

static uint64_t parse_size(const char *arg, const char *what)
{
	uint64_t mult = 1;
	char *end = NULL;
	unsigned long long v;

	errno = 0;
	v = strtoull(arg, &end, 0);
	if (errno || end == arg) {
		fprintf(stderr, "invalid %s: %s\n", what, arg);
		exit(2);
	}
	if (*end) {
		if (end[1])
			goto bad;
		switch (*end) {
		case 'K': case 'k': mult = 1ULL << 10; break;
		case 'M': case 'm': mult = 1ULL << 20; break;
		case 'G': case 'g': mult = 1ULL << 30; break;
		default:
			goto bad;
		}
	}
	if (v > UINT64_MAX / mult)
		goto bad;
	return v * mult;
bad:
	fprintf(stderr, "invalid %s: %s\n", what, arg);
	exit(2);
}

static int create_udmabuf(uint64_t size)
{
	struct udmabuf_create create = { 0 };
	int memfd, udma, fd;

	if (!size || (size & (TBSTREAM_ZC_FRAME_SIZE - 1))) {
		fprintf(stderr, "--udmabuf size must be a nonzero 4 KiB multiple\n");
		exit(2);
	}

	memfd = memfd_create("tbstream-probe", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (memfd < 0) {
		perror("memfd_create");
		exit(1);
	}
	if (ftruncate(memfd, (off_t)size)) {
		perror("ftruncate");
		exit(1);
	}
	/* udmabuf requires the backing memfd to be sealed against shrinking. */
	if (fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK)) {
		perror("F_ADD_SEALS");
		exit(1);
	}
	udma = open("/dev/udmabuf", O_RDWR | O_CLOEXEC);
	if (udma < 0) {
		perror("open /dev/udmabuf");
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

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device",    required_argument, NULL, 'd' },
		{ "direction", required_argument, NULL, 't' },
		{ "offset",    required_argument, NULL, 'o' },
		{ "length",    required_argument, NULL, 'l' },
		{ "udmabuf",   required_argument, NULL, 'u' },
		{ "fd",        required_argument, NULL, 'f' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	struct tbstream_zc_dmabuf_probe probe = { 0 };
	const char *device = "/dev/tbstream0";
	uint64_t udmabuf_size = 0;
	int have_length = 0;
	int fd = -1, dev, c, ret;

	probe.version = TBSTREAM_ZC_DMABUF_PROBE_VERSION;

	while ((c = getopt_long(argc, argv, "d:t:o:l:u:f:h", opts, NULL)) >= 0) {
		switch (c) {
		case 'd':
			device = optarg;
			break;
		case 't':
			if (!strcmp(optarg, "tx"))
				probe.direction = TBSTREAM_ZC_DMABUF_TX;
			else if (!strcmp(optarg, "rx"))
				probe.direction = TBSTREAM_ZC_DMABUF_RX;
			else {
				fprintf(stderr, "invalid direction: %s\n", optarg);
				return 2;
			}
			break;
		case 'o':
			probe.offset = parse_size(optarg, "offset");
			break;
		case 'l':
			probe.length = parse_size(optarg, "length");
			have_length = 1;
			break;
		case 'u':
			udmabuf_size = parse_size(optarg, "udmabuf size");
			break;
		case 'f':
			fd = (int)parse_size(optarg, "fd");
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			return 2;
		}
	}

	if (!probe.direction || (udmabuf_size && fd >= 0) ||
	    (!udmabuf_size && fd < 0)) {
		usage(stderr);
		return 2;
	}
	if (probe.offset & (TBSTREAM_ZC_FRAME_SIZE - 1)) {
		fprintf(stderr, "--offset must be 4 KiB aligned\n");
		return 2;
	}

	if (udmabuf_size)
		fd = create_udmabuf(udmabuf_size);

	if (!have_length) {
		/* Default to the remainder of the object. */
		struct stat st;

		if (fstat(fd, &st) || !st.st_size) {
			fprintf(stderr,
				"--length is required when the object does not stat non-empty\n");
			return 2;
		}
		if ((uint64_t)st.st_size <= probe.offset) {
			fprintf(stderr, "--offset is past the end of the object\n");
			return 2;
		}
		probe.length = (uint64_t)st.st_size - probe.offset;
		probe.length &= ~(uint64_t)(TBSTREAM_ZC_FRAME_SIZE - 1);
	}
	if (!probe.length || (probe.length & (TBSTREAM_ZC_FRAME_SIZE - 1))) {
		fprintf(stderr, "--length must be a nonzero 4 KiB multiple\n");
		return 2;
	}

	probe.fd = fd;

	dev = open(device, O_RDWR | O_CLOEXEC);
	if (dev < 0) {
		perror(device);
		return 1;
	}
	ret = ioctl(dev, TBSTREAM_ZC_DMABUF_PROBE, &probe);
	if (ret) {
		fprintf(stderr, "TBSTREAM_ZC_DMABUF_PROBE: %s\n",
			strerror(errno));
		return 1;
	}

	printf("direction=%s offset=%llu length=%llu\n",
	       probe.direction == TBSTREAM_ZC_DMABUF_TX ? "tx" : "rx",
	       (unsigned long long)probe.offset,
	       (unsigned long long)probe.length);
	printf("covered=%llu orig_entries=%u mapped_entries=%u\n",
	       (unsigned long long)probe.covered, probe.orig_entries,
	       probe.mapped_entries);
	printf("min_alignment=%llu largest_segment=%llu\n",
	       (unsigned long long)probe.min_alignment,
	       (unsigned long long)probe.largest_segment);
	if (probe.covered != probe.length) {
		fprintf(stderr, "FAIL: requested range is not fully mapped\n");
		return 1;
	}
	printf("ok - range fully covered by frame-aligned DMA segments\n");
	return 0;
}

#else /* !__linux__ */

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	fprintf(stderr, "tbstream-dmabuf-probe is Linux-only\n");
	return 2;
}

#endif /* __linux__ */
