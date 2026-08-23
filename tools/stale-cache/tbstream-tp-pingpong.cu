// SPDX-License-Identifier: MIT
/*
 * tbstream-tp-pingpong - tensor-parallel-shaped exchange latency probe.
 *
 * Both hosts import native HIP allocations as both frame pools, then
 * ping-pong fixed all-reduce-sized messages (default 7 frames = 28 KiB,
 * one fp32 hidden vector). Two arrival-detection arms:
 *
 *   --detect reap  The kernel notification path: block in
 *                  TBSTREAM_ZC_REAP (interrupt + wakeup), then submit.
 *   --detect spin  A persistent GPU kernel spin-polls the expected RX
 *                  slot's stamp word with system-scope atomic loads
 *                  (I/O-coherent NHI writes, proven by gate 5), then
 *                  raises a host-visible flag; the CPU spins on that
 *                  flag and immediately submits the pre-stamped
 *                  response. Driver events are drained off the
 *                  critical path.
 *
 * The initiator reports RTT percentiles. Payload is never touched by
 * the CPU in either arm; in spin mode the stamps are written only by
 * GPU system-scope atomic stores.
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
#include <time.h>
#include <unistd.h>

#include "../pingpong/thunderbolt-stream.h"

#define FRAME_SIZE	4096u
#define FRAME_WORDS	(FRAME_SIZE / 4u)
#define WARMUP		200u

/* Host-visible control block (hipHostMalloc, coherent). */
struct ctl {
	uint32_t tx_ready;	/* GPU: outbound slot n-1 is stamped */
	uint32_t rx_seen;	/* GPU: inbound message n-1 observed */
	uint32_t host_go;	/* CPU: iteration n-1 fully retired */
	uint32_t stop;
};

/*
 * One workgroup, one active lane. Iteration n:
 *   1. stamp outbound slot n with system-scope release store;
 *   2. announce tx_ready;
 *   3. spin on the inbound stamp with system-scope acquire loads;
 *   4. announce rx_seen;
 *   5. wait for the host to retire the iteration.
 */
static __global__ void pingpong_kernel(uint32_t *tx_pool, uint32_t *rx_pool,
				       uint32_t ring, uint32_t frames,
				       uint32_t iters, uint32_t out_magic,
				       uint32_t in_magic, struct ctl *c)
{
	uint32_t msgs_per_ring = ring / frames;

	if (blockIdx.x || threadIdx.x)
		return;

	for (uint32_t n = 0; n < iters; n++) {
		uint32_t slot = (n % msgs_per_ring) * frames;
		uint32_t last = (slot + frames) * FRAME_WORDS - 1;

		__hip_atomic_store(&tx_pool[last], out_magic ^ n,
				   __ATOMIC_RELEASE,
				   __HIP_MEMORY_SCOPE_SYSTEM);
		__hip_atomic_store(&c->tx_ready, n + 1, __ATOMIC_RELEASE,
				   __HIP_MEMORY_SCOPE_SYSTEM);

		while (__hip_atomic_load(&rx_pool[last], __ATOMIC_ACQUIRE,
					 __HIP_MEMORY_SCOPE_SYSTEM) !=
		       (in_magic ^ n)) {
			if (__hip_atomic_load(&c->stop, __ATOMIC_RELAXED,
					      __HIP_MEMORY_SCOPE_SYSTEM))
				return;
			__builtin_amdgcn_s_sleep(1);
		}

		__hip_atomic_store(&c->rx_seen, n + 1, __ATOMIC_RELEASE,
				   __HIP_MEMORY_SCOPE_SYSTEM);

		while (__hip_atomic_load(&c->host_go, __ATOMIC_ACQUIRE,
					 __HIP_MEMORY_SCOPE_SYSTEM) < n + 1) {
			if (__hip_atomic_load(&c->stop, __ATOMIC_RELAXED,
					      __HIP_MEMORY_SCOPE_SYSTEM))
				return;
			__builtin_amdgcn_s_sleep(1);
		}
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

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/* Bounded host spin with diagnostics; aborts on timeout. */
static void spin_wait(volatile uint32_t *p, uint32_t want, const char *what,
		      uint32_t iter)
{
	uint64_t deadline = now_ns() + 10ull * 1000000000ull;

	while (*p < want) {
		if (now_ns() > deadline) {
			fprintf(stderr,
				"TIMEOUT waiting for %s at iter %u (%u < %u)\n",
				what, iter, *p, want);
			exit(1);
		}
	}
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return x < y ? -1 : x > y;
}

struct session {
	int fd;
	uint32_t ring, frames;
	uint32_t rx_wait;	/* RX events seen but not yet reposted */
	uint64_t rx_events, tx_events;
};

/* Drain driver events; repost RX in arrival order. Off critical path. */
static void drain(struct session *s, int block)
{
	struct tbstream_zc_event evs[16];
	struct tbstream_zc_reap reap = {
		.max = 16,
		.flags = block ? 0u : TBSTREAM_ZC_REAP_NONBLOCK,
		.events = (uint64_t)(uintptr_t)evs,
	};
	int n = ioctl(s->fd, TBSTREAM_ZC_REAP, &reap);

	if (n < 0) {
		if (errno == EAGAIN)
			return;
		perror("TBSTREAM_ZC_REAP");
		exit(1);
	}
	for (int i = 0; i < n; i++) {
		if (evs[i].type == TBSTREAM_ZC_EV_RX) {
			s->rx_events++;
			s->rx_wait += evs[i].nframes;
		} else if (evs[i].type == TBSTREAM_ZC_EV_TX_DONE) {
			s->tx_events++;
		} else if (evs[i].type == TBSTREAM_ZC_EV_CLOSE) {
			fprintf(stderr, "peer closed\n");
			exit(1);
		}
	}
	while (s->rx_wait >= s->frames) {
		uint32_t nf = s->frames;

		if (ioctl(s->fd, TBSTREAM_ZC_POST_RX, &nf)) {
			perror("TBSTREAM_ZC_POST_RX");
			exit(1);
		}
		s->rx_wait -= s->frames;
	}
}

static void submit(struct session *s)
{
	struct tbstream_zc_tx tx = {
		.nframes = s->frames,
		.last_len = FRAME_SIZE,
		.first = 0,
		.reserved = 0,
	};

	while (ioctl(s->fd, TBSTREAM_ZC_SUBMIT_TX, &tx)) {
		if (errno != ENOBUFS) {
			perror("TBSTREAM_ZC_SUBMIT_TX");
			exit(1);
		}
		drain(s, 1);
	}
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: tbstream-tp-pingpong --role init|echo --detect reap|spin\n"
		"       [--device PATH] [--ring N] [--frames N] [--iters N]\n"
		"       [--gpu N]\n");
}

int main(int argc, char **argv)
{
	const char *device = "/dev/tbstream0";
	const char *role = NULL, *detect = NULL;
	uint32_t ring = 256, frames = 7, iters = 2000;
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
		else if (!strcmp(arg, "--role"))
			role = val;
		else if (!strcmp(arg, "--detect"))
			detect = val;
		else if (!strcmp(arg, "--ring"))
			ring = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--frames"))
			frames = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--iters"))
			iters = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--gpu"))
			gpu = (int)strtol(val, NULL, 0);
		else {
			usage(stderr);
			return 2;
		}
	}
	if (!role || !detect || !ring || !frames || ring % frames ||
	    iters <= WARMUP ||
	    (strcmp(role, "init") && strcmp(role, "echo")) ||
	    (strcmp(detect, "reap") && strcmp(detect, "spin"))) {
		usage(stderr);
		return 2;
	}
	int is_init = !strcmp(role, "init");
	int use_spin = !strcmp(detect, "spin");
	uint64_t pool_bytes = (uint64_t)ring * FRAME_SIZE;
	/*
	 * Small hipMalloc allocations are suballocated from shared slab
	 * BOs, and the exported DMA-BUF names the backing BO, not the
	 * fragment. Pad to a size gate 4 proved is a dedicated BO and
	 * import only the leading pool range.
	 */
	uint64_t alloc_bytes = pool_bytes < (16ull << 20) ? (16ull << 20) :
			       pool_bytes;
	uint32_t out_magic = is_init ? 0x496e4900u : 0x45636800u;
	uint32_t in_magic = is_init ? 0x45636800u : 0x496e4900u;

	HIP_CHECK("hipSetDevice", hipSetDevice(gpu));

	/*
	 * The spin arm's stamps are written and polled by long-running
	 * waves with no dispatch boundary, so they cannot rely on the
	 * dispatch-time cache invalidation that makes coarse memory
	 * appear coherent (gate 5). Uncached (MTYPE_UC) pools make the
	 * stamp stores and polls go straight to memory; a production
	 * design would place only the flag words in uncached memory and
	 * keep payload in coarse memory read by post-detection kernels.
	 */
	void *tx_pool = NULL, *rx_pool = NULL;
	if (use_spin) {
		HIP_CHECK("hipExtMallocWithFlags(tx,uc)",
			  hipExtMallocWithFlags(&tx_pool, alloc_bytes,
						hipDeviceMallocUncached));
		HIP_CHECK("hipExtMallocWithFlags(rx,uc)",
			  hipExtMallocWithFlags(&rx_pool, alloc_bytes,
						hipDeviceMallocUncached));
	} else {
		HIP_CHECK("hipMalloc(tx)", hipMalloc(&tx_pool, alloc_bytes));
		HIP_CHECK("hipMalloc(rx)", hipMalloc(&rx_pool, alloc_bytes));
	}
	for (int p = 0; p < 2; p++) {
		void *ptr = p ? rx_pool : tx_pool;
		hipDeviceptr_t base = 0;
		size_t range = 0;

		HIP_CHECK("hipMemGetAddressRange",
			  hipMemGetAddressRange(&base, &range,
						(hipDeviceptr_t)ptr));
		if ((void *)base != ptr || range != alloc_bytes) {
			fprintf(stderr,
				"pool %d is not a dedicated allocation\n", p);
			return 1;
		}
	}
	HIP_CHECK("hipMemset(tx)", hipMemset(tx_pool, 0, alloc_bytes));
	HIP_CHECK("hipMemset(rx)", hipMemset(rx_pool, 0, alloc_bytes));
	HIP_CHECK("hipDeviceSynchronize", hipDeviceSynchronize());

	int tx_fd = -1, rx_fd = -1;
	HIP_CHECK("export tx",
		  hipMemGetHandleForAddressRange(&tx_fd,
						 (hipDeviceptr_t)tx_pool,
						 alloc_bytes,
						 hipMemRangeHandleTypeDmaBufFd,
						 0));
	HIP_CHECK("export rx",
		  hipMemGetHandleForAddressRange(&rx_fd,
						 (hipDeviceptr_t)rx_pool,
						 alloc_bytes,
						 hipMemRangeHandleTypeDmaBufFd,
						 0));

	struct session s = { .fd = -1, .ring = ring, .frames = frames,
			     .rx_wait = 0, .rx_events = 0, .tx_events = 0 };
	s.fd = open(device, O_RDWR | O_CLOEXEC);
	if (s.fd < 0) {
		perror(device);
		return 1;
	}

	struct tbstream_zc_import imp = {};
	imp.version = TBSTREAM_ZC_IMPORT_VERSION;
	imp.tx.fd = tx_fd;
	imp.tx.length = pool_bytes;
	imp.rx.fd = rx_fd;
	imp.rx.length = pool_bytes;
	if (ioctl(s.fd, TBSTREAM_ZC_IMPORT, &imp)) {
		perror("TBSTREAM_ZC_IMPORT");
		return 1;
	}
	if (ioctl(s.fd, TBSTREAM_ZC_ENABLE)) {
		perror("TBSTREAM_ZC_ENABLE");
		return 1;
	}

	struct ctl *c = NULL;
	HIP_CHECK("hipHostMalloc", hipHostMalloc((void **)&c, sizeof(*c), 0));
	memset((void *)c, 0, sizeof(*c));
	volatile uint32_t *tx_ready = &c->tx_ready;
	volatile uint32_t *rx_seen = &c->rx_seen;

	hipStream_t stream;
	HIP_CHECK("hipStreamCreate", hipStreamCreate(&stream));
	if (use_spin) {
		hipLaunchKernelGGL(pingpong_kernel, dim3(1), dim3(64), 0,
				   stream, (uint32_t *)tx_pool,
				   (uint32_t *)rx_pool, ring, frames, iters,
				   out_magic, in_magic, c);
		HIP_CHECK("kernel launch", hipGetLastError());
		spin_wait(tx_ready, 1, "first tx_ready (kernel alive?)", 0);
		fprintf(stderr, "spin kernel is live\n");
	}

	uint64_t *lat = (uint64_t *)calloc(iters, sizeof(*lat));

	printf("role=%s detect=%s ring=%u frames=%u bytes=%u iters=%u\n",
	       role, detect, ring, frames, frames * FRAME_SIZE, iters);

	for (uint32_t n = 0; n < iters; n++) {
		if (use_spin) {
			/* Outbound slot n must be stamped before submit. */
			spin_wait(tx_ready, n + 1, "tx_ready", n);
		}
		if (is_init) {
			uint64_t t0 = now_ns();

			submit(&s);
			if (use_spin) {
				spin_wait(rx_seen, n + 1, "rx_seen", n);
			} else {
				while (s.rx_events <= n)
					drain(&s, 1);
			}
			lat[n] = now_ns() - t0;
		} else {
			if (use_spin) {
				spin_wait(rx_seen, n + 1, "rx_seen", n);
			} else {
				while (s.rx_events <= n)
					drain(&s, 1);
			}
			submit(&s);
		}
		/* Retire: drain events, repost RX, release the kernel. */
		drain(&s, 0);
		if (use_spin)
			c->host_go = n + 1;
	}
	/* Let the last messages' events settle and repost everything. */
	while (s.rx_events < iters || s.tx_events < iters)
		drain(&s, 1);

	if (use_spin) {
		c->stop = 1;
		HIP_CHECK("hipStreamSynchronize", hipStreamSynchronize(stream));
	}

	struct tbstream_zc_stats stats = {};
	if (ioctl(s.fd, TBSTREAM_ZC_GET_STATS, &stats)) {
		perror("TBSTREAM_ZC_GET_STATS");
		return 1;
	}
	if (stats.flags & (TBSTREAM_ZC_STATS_F_FAILED)) {
		fprintf(stderr, "FAIL: session failed (%u)\n",
			stats.last_error);
		return 1;
	}
	if (!(stats.flags & TBSTREAM_ZC_STATS_F_TX_IMPORTED) ||
	    !(stats.flags & TBSTREAM_ZC_STATS_F_RX_IMPORTED)) {
		fprintf(stderr, "FAIL: pools not imported\n");
		return 1;
	}
	printf("stats ok: rx_events=%" PRIu64 " tx_events=%" PRIu64
	       " drops=%llu\n",
	       s.rx_events, s.tx_events,
	       (unsigned long long)stats.event_drops);

	if (is_init) {
		uint64_t *m = lat + WARMUP;
		uint32_t n = iters - WARMUP;

		qsort(m, n, sizeof(*m), cmp_u64);
		printf("RTT ns: min=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64
		       " p99=%" PRIu64 " max=%" PRIu64 "\n",
		       m[0], m[n / 2], m[(uint64_t)n * 90 / 100],
		       m[(uint64_t)n * 99 / 100], m[n - 1]);
		printf("RESULT: %s p50=%.1f us one-way~%.1f us\n", detect,
		       m[n / 2] / 1000.0, m[n / 2] / 2000.0);
	} else {
		printf("RESULT: echo done\n");
	}

	close(s.fd);
	close(tx_fd);
	close(rx_fd);
	HIP_CHECK("hipFree tx", hipFree(tx_pool));
	HIP_CHECK("hipFree rx", hipFree(rx_pool));
	free(lat);
	return 0;
}
