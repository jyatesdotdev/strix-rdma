// SPDX-License-Identifier: MIT
/*
 * tbstream-gpu-tx - gate-6 sender.
 *
 * Imports a native HIP allocation as the stream's TX frame pool and
 * sends generation-stamped messages whose payload is written only by
 * GPU kernels. The CPU never touches payload: it fills nothing, maps
 * nothing, and only submits slot indexes.
 *
 * TX ownership contract (--release event, the default): after the
 * producer kernel, a timing-enabled hipEventReleaseToSystem event is
 * recorded in the producer stream and synchronized before SUBMIT_TX,
 * so dirty GPU cache lines are system-visible before the NHI reads
 * them. --release none is a diagnostic arm that relies on the plain
 * stream synchronize alone.
 */
#include <hip/hip_runtime.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "../pingpong/thunderbolt-stream.h"
#include "stale-pattern.h"

static __global__ void fill(uint32_t *buf, uint32_t nframes, uint32_t seed,
			    uint32_t msg)
{
	uint32_t nwords = nframes * STALE_FRAME_WORDS;
	size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
	size_t stride = (size_t)blockDim.x * gridDim.x;

	for (; i < nwords; i += stride) {
		uint32_t f = (uint32_t)(i / STALE_FRAME_WORDS);
		uint32_t w = (uint32_t)(i % STALE_FRAME_WORDS);

		buf[i] = stale_mix(seed, msg, f, w);
	}
}

#define HIP_CHECK(what, call)                                             \
	do {                                                              \
		hipError_t err_ = (call);                                 \
		if (err_ != hipSuccess) {                                 \
			fprintf(stderr, "%s: %s\n", (what),               \
				hipGetErrorString(err_));                 \
			exit(1);                                          \
		}                                                         \
	} while (0)

static void usage(FILE *out)
{
	fprintf(out,
		"usage: tbstream-gpu-tx [--device PATH] [--ring N]\n"
		"       [--frames N] [--generations N] [--seed N] [--gpu N]\n"
		"       [--release event|none]\n");
}

int main(int argc, char **argv)
{
	const char *device = "/dev/tbstream0";
	const char *release = "event";
	uint32_t ring = 256, frames = 16, generations = 64;
	uint32_t seed = 0x5eed5001;
	int gpu = 0;

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
		else if (!strcmp(arg, "--ring"))
			ring = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--frames"))
			frames = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--generations"))
			generations = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--seed"))
			seed = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--gpu"))
			gpu = (int)strtol(val, NULL, 0);
		else if (!strcmp(arg, "--release"))
			release = val;
		else {
			usage(stderr);
			return 2;
		}
	}
	if (!ring || !frames || ring % frames ||
	    (strcmp(release, "event") && strcmp(release, "none"))) {
		usage(stderr);
		return 2;
	}
	bool use_event = !strcmp(release, "event");
	uint32_t msgs_per_gen = ring / frames;
	uint64_t pool_bytes = (uint64_t)ring * STALE_FRAME_SIZE;

	HIP_CHECK("hipSetDevice", hipSetDevice(gpu));

	void *pool = NULL;
	HIP_CHECK("hipMalloc", hipMalloc(&pool, pool_bytes));
	{
		hipDeviceptr_t base = 0;
		size_t range = 0;

		HIP_CHECK("hipMemGetAddressRange",
			  hipMemGetAddressRange(&base, &range,
						(hipDeviceptr_t)pool));
		if ((void *)base != pool || range != pool_bytes) {
			fprintf(stderr, "pool is not a dedicated allocation\n");
			return 1;
		}
	}
	HIP_CHECK("hipDeviceSynchronize", hipDeviceSynchronize());

	int dmabuf_fd = -1;
	HIP_CHECK("hipMemGetHandleForAddressRange",
		  hipMemGetHandleForAddressRange(&dmabuf_fd,
						 (hipDeviceptr_t)pool,
						 pool_bytes,
						 hipMemRangeHandleTypeDmaBufFd,
						 0));

	int fd = open(device, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(device);
		return 1;
	}

	struct tbstream_zc_import imp = {};
	imp.version = TBSTREAM_ZC_IMPORT_VERSION;
	imp.tx.fd = dmabuf_fd;
	imp.tx.offset = 0;
	imp.tx.length = pool_bytes;
	imp.rx.fd = -1;
	if (ioctl(fd, TBSTREAM_ZC_IMPORT, &imp)) {
		perror("TBSTREAM_ZC_IMPORT");
		return 1;
	}
	if (ioctl(fd, TBSTREAM_ZC_ENABLE)) {
		perror("TBSTREAM_ZC_ENABLE");
		return 1;
	}

	struct tbstream_zc_info info;
	if (ioctl(fd, TBSTREAM_ZC_GET_INFO, &info)) {
		perror("TBSTREAM_ZC_GET_INFO");
		return 1;
	}
	if (info.ring_size != ring) {
		fprintf(stderr, "ring mismatch: configured %u, --ring %u\n",
			info.ring_size, ring);
		return 1;
	}

	struct tbstream_zc_stats stats = {};
	if (ioctl(fd, TBSTREAM_ZC_GET_STATS, &stats)) {
		perror("TBSTREAM_ZC_GET_STATS");
		return 1;
	}
	if (!(stats.flags & TBSTREAM_ZC_STATS_F_TX_IMPORTED)) {
		fprintf(stderr, "FAIL: TX pool is not imported\n");
		return 1;
	}

	hipStream_t stream;
	HIP_CHECK("hipStreamCreate", hipStreamCreate(&stream));
	hipEvent_t rel = NULL;
	if (use_event)
		HIP_CHECK("hipEventCreateWithFlags",
			  hipEventCreateWithFlags(&rel,
						  hipEventReleaseToSystem));

	printf("release=%s ring=%u frames/msg=%u msgs/gen=%u generations=%u seed=%#x\n",
	       release, ring, frames, msgs_per_gen, generations, seed);

	uint32_t cursor = 0;
	uint64_t nmsg = 0;

	for (uint32_t gen = 0; gen < generations; gen++) {
		for (uint32_t m = 0; m < msgs_per_gen; m++, nmsg++) {
			uint32_t *buf = (uint32_t *)
				((char *)pool +
				 (uint64_t)cursor * STALE_FRAME_SIZE);

			/* Producer kernel, then the TX ownership fence. */
			hipLaunchKernelGGL(fill, dim3(64), dim3(256), 0,
					   stream, buf, frames, seed,
					   (uint32_t)nmsg);
			HIP_CHECK("fill launch", hipGetLastError());
			if (use_event)
				HIP_CHECK("hipEventRecord",
					  hipEventRecord(rel, stream));
			HIP_CHECK("hipStreamSynchronize",
				  hipStreamSynchronize(stream));

			struct tbstream_zc_tx tx = {
				.nframes = frames,
				.last_len = STALE_FRAME_SIZE,
				.first = 0,
				.reserved = 0,
			};
			struct tbstream_zc_event ev;
			struct tbstream_zc_reap reap = {
				.max = 1,
				.flags = 0,
				.events = (uint64_t)(uintptr_t)&ev,
			};

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
			cursor = (cursor + frames) % ring;

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

	if (ioctl(fd, TBSTREAM_ZC_GET_STATS, &stats)) {
		perror("TBSTREAM_ZC_GET_STATS");
		return 1;
	}
	if (stats.flags & TBSTREAM_ZC_STATS_F_FAILED) {
		fprintf(stderr, "FAIL: zero-copy session failed (%u)\n",
			stats.last_error);
		return 1;
	}

	printf("sent %" PRIu64 " GPU-written messages (%" PRIu64 " MiB)\n",
	       nmsg, nmsg * frames * STALE_FRAME_SIZE >> 20);

	/* close() sends CLOSE through the dedicated control frame. */
	close(fd);
	close(dmabuf_fd);
	HIP_CHECK("hipFree", hipFree(pool));
	return 0;
}
