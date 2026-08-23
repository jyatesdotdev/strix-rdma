// SPDX-License-Identifier: MIT
/*
 * tbstream-stale-rx - gate-5 receiver.
 *
 * Imports a native HIP allocation as the stream's RX frame pool
 * (TBSTREAM_ZC_IMPORT), then verifies every received message with a
 * GPU digest kernel that recomputes the shared generation-stamped
 * pattern and counts mismatching words. Hot RX slots are reused across
 * generations, so a stale GPU cache shows up as nonzero mismatches in
 * the arms without a valid acquire.
 *
 * Memory arms:   --type coarse | uncached
 * Acquire arms:  --acquire event | none
 *   event: a timing-enabled hipEventReleaseToSystem event is recorded
 *          in the consumer stream before each digest launch (exact
 *          CLR 7.13 emits a system-scope acquire+release barrier for
 *          this marker).
 *   none:  the digest kernel is launched with no explicit acquire.
 *
 * The CPU never reads payload; it reads only the small per-message
 * result record.
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

struct result {
	unsigned long long mismatches;
	unsigned int xorsum;
	unsigned int pad;
};

/*
 * Adversarial cache warmer: pull a slot region into the GPU caches
 * with ordinary vector loads so the lines are resident while the NHI
 * DMA-writes the next message into them. The sum is written out only
 * to keep the loads from being optimized away.
 */
static __global__ void warm(const uint32_t *buf, uint32_t nframes,
			    unsigned int *sink)
{
	uint32_t nwords = nframes * STALE_FRAME_WORDS;
	size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
	size_t stride = (size_t)blockDim.x * gridDim.x;
	uint32_t x = 0;

	for (; i < nwords; i += stride)
		x += buf[i];
	if (x == 0x1badcafe)
		atomicAdd(sink, 1);
}

static __global__ void digest(const uint32_t *buf, uint32_t nframes,
			      uint32_t seed, uint32_t msg,
			      struct result *res)
{
	uint32_t nwords = nframes * STALE_FRAME_WORDS;
	size_t i = (size_t)blockIdx.x * blockDim.x + threadIdx.x;
	size_t stride = (size_t)blockDim.x * gridDim.x;
	unsigned long long bad = 0;
	uint32_t x = 0;

	for (; i < nwords; i += stride) {
		uint32_t v = buf[i];
		uint32_t f = (uint32_t)(i / STALE_FRAME_WORDS);
		uint32_t w = (uint32_t)(i % STALE_FRAME_WORDS);

		x ^= v;
		if (v != stale_mix(seed, msg, f, w))
			bad++;
	}
	if (bad)
		atomicAdd(&res->mismatches, bad);
	if (x)
		atomicXor(&res->xorsum, x);
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
		"usage: tbstream-stale-rx --type coarse|uncached\n"
		"       --acquire event|none [--device PATH] [--ring N]\n"
		"       [--frames N] [--expect-msgs N] [--gpu N] [--seed N]\n"
		"       [--prewarm 0|1]\n");
}

int main(int argc, char **argv)
{
	const char *device = "/dev/tbstream0";
	const char *type = NULL, *acquire = NULL;
	uint32_t ring = 256, frames = 16, seed = 0x5eed5001;
	uint64_t expect_msgs = 0;
	int gpu = 0, prewarm = 0;

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
		else if (!strcmp(arg, "--type"))
			type = val;
		else if (!strcmp(arg, "--acquire"))
			acquire = val;
		else if (!strcmp(arg, "--ring"))
			ring = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--frames"))
			frames = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--expect-msgs"))
			expect_msgs = strtoull(val, NULL, 0);
		else if (!strcmp(arg, "--gpu"))
			gpu = (int)strtol(val, NULL, 0);
		else if (!strcmp(arg, "--seed"))
			seed = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--prewarm")) {
			prewarm = (int)strtol(val, NULL, 0);
		} else {
			usage(stderr);
			return 2;
		}
	}
	if (!type || !acquire || !ring || !frames ||
	    (strcmp(acquire, "event") && strcmp(acquire, "none"))) {
		usage(stderr);
		return 2;
	}
	bool use_event = !strcmp(acquire, "event");
	uint64_t pool_bytes = (uint64_t)ring * STALE_FRAME_SIZE;

	HIP_CHECK("hipSetDevice", hipSetDevice(gpu));

	/* 1. Allocate the native RX pool. */
	void *pool = NULL;
	if (!strcmp(type, "coarse")) {
		HIP_CHECK("hipMalloc", hipMalloc(&pool, pool_bytes));
	} else if (!strcmp(type, "uncached")) {
		HIP_CHECK("hipExtMallocWithFlags(uncached)",
			  hipExtMallocWithFlags(&pool, pool_bytes,
						hipDeviceMallocUncached));
	} else {
		usage(stderr);
		return 2;
	}
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

	/*
	 * 2. Make the slots cache-hot and dirty from a previous life:
	 * write a poison pattern from the GPU, then quiesce before
	 * export so the device is idle at import time.
	 */
	HIP_CHECK("hipMemsetD32",
		  hipMemsetD32((hipDeviceptr_t)pool, 0xdeadbeef,
			       pool_bytes / 4));
	HIP_CHECK("hipDeviceSynchronize", hipDeviceSynchronize());

	/* 3. Export and import as the RX pool before any activation. */
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
	imp.tx.fd = -1;
	imp.rx.fd = dmabuf_fd;
	imp.rx.offset = 0;
	imp.rx.length = pool_bytes;
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

	/* 4. Consumer stream, acquire event, and result record. */
	hipStream_t stream;
	HIP_CHECK("hipStreamCreate", hipStreamCreate(&stream));
	hipEvent_t acq = NULL;
	if (use_event)
		HIP_CHECK("hipEventCreateWithFlags",
			  hipEventCreateWithFlags(&acq,
						  hipEventReleaseToSystem));

	struct result *res_dev;
	HIP_CHECK("hipMalloc(result)",
		  hipMalloc((void **)&res_dev, sizeof(*res_dev)));
	unsigned int *warm_sink;
	HIP_CHECK("hipMalloc(warm)",
		  hipMalloc((void **)&warm_sink, sizeof(*warm_sink)));

	printf("type=%s acquire=%s prewarm=%d ring=%u frames/msg=%u seed=%#x\n",
	       type, acquire, prewarm, ring, frames, seed);

	/* 5. Reap, digest, repost. */
	uint64_t nmsg = 0, bad_msgs = 0;
	unsigned long long bad_words = 0;
	uint32_t expect_slot = 0;
	bool closed = false;

	while (!closed) {
		struct tbstream_zc_event ev;
		struct tbstream_zc_reap reap = {
			.max = 1,
			.flags = 0,
			.events = (uint64_t)(uintptr_t)&ev,
		};
		int n = ioctl(fd, TBSTREAM_ZC_REAP, &reap);

		if (n < 0) {
			perror("TBSTREAM_ZC_REAP");
			return 1;
		}
		if (!n)
			continue;
		if (ev.type == TBSTREAM_ZC_EV_CLOSE) {
			closed = true;
			break;
		}
		if (ev.type != TBSTREAM_ZC_EV_RX)
			continue;
		if (ev.nframes != frames || ev.first != expect_slot ||
		    ev.bytes != (uint64_t)frames * STALE_FRAME_SIZE) {
			fprintf(stderr,
				"unexpected message geometry: first=%u nframes=%u bytes=%u (expected slot %u)\n",
				ev.first, ev.nframes, ev.bytes, expect_slot);
			return 1;
		}

		struct result res = {};
		HIP_CHECK("hipMemcpyAsync(reset)",
			  hipMemcpyAsync(res_dev, &res, sizeof(res),
					 hipMemcpyHostToDevice, stream));
		if (use_event)
			HIP_CHECK("hipEventRecord",
				  hipEventRecord(acq, stream));
		const uint32_t *buf = (const uint32_t *)
			((const char *)pool +
			 (uint64_t)ev.first * STALE_FRAME_SIZE);
		hipLaunchKernelGGL(digest, dim3(64), dim3(256), 0, stream,
				   buf, frames, seed, (uint32_t)nmsg,
				   res_dev);
		HIP_CHECK("digest launch", hipGetLastError());
		HIP_CHECK("hipMemcpyAsync(result)",
			  hipMemcpyAsync(&res, res_dev, sizeof(res),
					 hipMemcpyDeviceToHost, stream));
		/* GPU completion before the slots are reposted. */
		HIP_CHECK("hipStreamSynchronize",
			  hipStreamSynchronize(stream));

		if (res.mismatches) {
			bad_msgs++;
			bad_words += res.mismatches;
			if (bad_msgs <= 8)
				fprintf(stderr,
					"msg %" PRIu64 " slot %u: %llu mismatched words (xor %#x)\n",
					nmsg, ev.first, res.mismatches,
					res.xorsum);
		}
		nmsg++;
		expect_slot = (expect_slot + frames) % ring;

		uint32_t nframes = ev.nframes;
		if (ioctl(fd, TBSTREAM_ZC_POST_RX, &nframes)) {
			perror("TBSTREAM_ZC_POST_RX");
			return 1;
		}

		/*
		 * Adversarial arm: cache the next message's slots now so
		 * their lines are resident in the GPU caches while the
		 * NHI writes the next payload into them.
		 */
		if (prewarm) {
			const uint32_t *next = (const uint32_t *)
				((const char *)pool +
				 (uint64_t)expect_slot * STALE_FRAME_SIZE);

			hipLaunchKernelGGL(warm, dim3(64), dim3(256), 0,
					   stream, next, frames, warm_sink);
			HIP_CHECK("warm launch", hipGetLastError());
			HIP_CHECK("warm sync", hipStreamSynchronize(stream));
		}
	}

	/* 6. Final invariants. */
	struct tbstream_zc_stats stats = {};
	if (ioctl(fd, TBSTREAM_ZC_GET_STATS, &stats)) {
		perror("TBSTREAM_ZC_GET_STATS");
		return 1;
	}
	printf("stats: flags=%#x last_error=%u rx_data=%llu rx_data_more=%llu event_drops=%llu\n",
	       stats.flags, stats.last_error,
	       (unsigned long long)stats.rx_data,
	       (unsigned long long)stats.rx_data_more,
	       (unsigned long long)stats.event_drops);
	if (!(stats.flags & TBSTREAM_ZC_STATS_F_RX_IMPORTED)) {
		fprintf(stderr, "FAIL: RX pool is not imported\n");
		return 1;
	}
	if (stats.flags & TBSTREAM_ZC_STATS_F_FAILED) {
		fprintf(stderr, "FAIL: zero-copy session failed (%u)\n",
			stats.last_error);
		return 1;
	}

	printf("received %" PRIu64 " messages, %" PRIu64 " bad (%llu bad words)\n",
	       nmsg, bad_msgs, bad_words);
	if (expect_msgs && nmsg != expect_msgs) {
		fprintf(stderr, "FAIL: expected %" PRIu64 " messages\n",
			expect_msgs);
		return 1;
	}

	close(fd);
	close(dmabuf_fd);
	HIP_CHECK("hipFree", hipFree(pool));
	HIP_CHECK("hipFree(result)", hipFree(res_dev));
	HIP_CHECK("hipFree(warm)", hipFree(warm_sink));

	if (bad_msgs) {
		printf("RESULT: STALE (%s/%s%s)\n", type, acquire,
		       prewarm ? "/prewarm" : "");
		return 3;
	}
	printf("RESULT: CLEAN (%s/%s%s)\n", type, acquire,
	       prewarm ? "/prewarm" : "");
	return 0;
}
