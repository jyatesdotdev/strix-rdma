// SPDX-License-Identifier: MIT
/*
 * tbstream-stale-tx - gate-5 sender.
 *
 * Sends generation-stamped messages through the ordinary page-backed
 * zero-copy TX pool so the receiving host can verify NHI writes into
 * its imported native GPU RX pool. Fully serialized: each message is
 * submitted and its TX_DONE reaped before the next, so pacing is
 * deterministic and E2E flow control never sees more than one message
 * in flight.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifdef __linux__

#include "../pingpong/thunderbolt-stream.h"
#include "stale-pattern.h"

static void usage(FILE *out)
{
	fprintf(out,
		"usage: tbstream-stale-tx [--device PATH] [--frames N]\n"
		"       [--msgs-per-gen N] [--generations N] [--seed N]\n"
		"\n"
		"Send generations * msgs-per-gen messages of frames * 4 KiB\n"
		"each through the zero-copy TX pool, every message stamped\n"
		"by the shared stale-cache pattern.\n");
}

int main(int argc, char **argv)
{
	const char *device = "/dev/tbstream0";
	uint32_t frames = 16, msgs_per_gen = 0, generations = 64;
	uint32_t seed = 0x5eed5001;
	struct tbstream_zc_info info;
	unsigned char *pool;
	uint64_t nmsg = 0;
	int fd;

	for (int i = 1; i < argc; i += 2) {
		const char *arg = argv[i];
		const char *val = (i + 1 < argc) ? argv[i + 1] : NULL;

		if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
			usage(stdout);
			return 0;
		}
		if (!val) {
			usage(stderr);
			return 2;
		}
		if (!strcmp(arg, "--device"))
			device = val;
		else if (!strcmp(arg, "--frames"))
			frames = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--msgs-per-gen"))
			msgs_per_gen = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--generations"))
			generations = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--seed"))
			seed = (uint32_t)strtoul(val, NULL, 0);
		else {
			usage(stderr);
			return 2;
		}
	}
	if (!frames || !generations) {
		usage(stderr);
		return 2;
	}

	fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		return 1;
	}
	if (ioctl(fd, TBSTREAM_ZC_ENABLE)) {
		perror("TBSTREAM_ZC_ENABLE");
		return 1;
	}
	if (ioctl(fd, TBSTREAM_ZC_GET_INFO, &info)) {
		perror("TBSTREAM_ZC_GET_INFO");
		return 1;
	}
	if (info.ring_size % frames) {
		fprintf(stderr,
			"ring size %u is not a multiple of --frames %u\n",
			info.ring_size, frames);
		return 1;
	}
	if (!msgs_per_gen)
		msgs_per_gen = info.ring_size / frames;
	if ((uint64_t)msgs_per_gen * frames % info.ring_size) {
		fprintf(stderr,
			"a generation must cover the ring a whole number of times\n");
		return 1;
	}

	pool = mmap(NULL, 2ull * info.ring_size * info.frame_size,
		    PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (pool == MAP_FAILED) {
		perror("mmap");
		return 1;
	}

	printf("ring=%u frames/msg=%u msgs/gen=%u generations=%u seed=%#x\n",
	       info.ring_size, frames, msgs_per_gen, generations, seed);

	uint32_t cursor = 0;

	for (uint32_t gen = 0; gen < generations; gen++) {
		for (uint32_t m = 0; m < msgs_per_gen; m++, nmsg++) {
			struct tbstream_zc_tx tx = {
				.nframes = frames,
				.last_len = STALE_FRAME_SIZE,
			};
			struct tbstream_zc_event ev;
			struct tbstream_zc_reap reap = {
				.max = 1,
				.events = (uint64_t)(uintptr_t)&ev,
			};

			for (uint32_t f = 0; f < frames; f++) {
				uint32_t slot = (cursor + f) % info.ring_size;
				uint32_t *w = (uint32_t *)(pool +
					(uint64_t)slot * info.frame_size);

				for (uint32_t j = 0; j < STALE_FRAME_WORDS; j++)
					w[j] = stale_mix(seed, (uint32_t)nmsg,
							 f, j);
			}

			while (ioctl(fd, TBSTREAM_ZC_SUBMIT_TX, &tx)) {
				if (errno != ENOBUFS) {
					perror("TBSTREAM_ZC_SUBMIT_TX");
					return 1;
				}
				if (ioctl(fd, TBSTREAM_ZC_REAP, &reap) < 0) {
					perror("TBSTREAM_ZC_REAP");
					return 1;
				}
			}
			if (tx.first != cursor) {
				fprintf(stderr,
					"slot cursor mismatch: kernel %u local %u\n",
					tx.first, cursor);
				return 1;
			}
			cursor = (cursor + frames) % info.ring_size;

			/* Serialize: wait for this message's TX_DONE. */
			for (;;) {
				int n = ioctl(fd, TBSTREAM_ZC_REAP, &reap);

				if (n < 0) {
					perror("TBSTREAM_ZC_REAP");
					return 1;
				}
				if (!n)
					continue;
				if (ev.type == TBSTREAM_ZC_EV_TX_DONE)
					break;
				if (ev.type == TBSTREAM_ZC_EV_CLOSE) {
					fprintf(stderr,
						"peer closed early at msg %" PRIu64 "\n",
						nmsg);
					return 1;
				}
			}
		}
	}

	printf("sent %" PRIu64 " messages (%" PRIu64 " MiB)\n", nmsg,
	       nmsg * frames * STALE_FRAME_SIZE >> 20);
	close(fd);
	return 0;
}

#else /* !__linux__ */

int main(void)
{
	fprintf(stderr, "tbstream-stale-tx is Linux-only\n");
	return 2;
}

#endif /* __linux__ */
