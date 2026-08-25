// SPDX-License-Identifier: MIT
/*
 * tbstream-tp-exchange - full-duplex tensor-parallel exchange probe.
 *
 * Unlike tbstream-tp-pingpong (alternating request/response), both
 * hosts send simultaneously every iteration, modeling a two-way
 * all-reduce partial-sum exchange: write the local partial into the
 * TX slot, stamp it, submit, and detect the peer's partial arriving
 * in the RX slot. Sustained back-to-back iterations expose ring
 * contention and the real per-exchange cadence; results report both
 * per-exchange latency percentiles and per-token cost for a
 * configurable number of exchanges per token (default 90 = 43 layers
 * x ~2 sync points).
 *
 * Arms:
 *   --detect spin  Persistent GPU kernel writes the payload, stamps
 *                  with system-scope release stores, and spin-polls
 *                  the RX stamp in-wave. Pools are MTYPE_UC (stamps
 *                  must travel in-band inside the DMA'd message, and
 *                  a single imported pool has one MTYPE, so the whole
 *                  pool is uncached; streaming once-per-exchange
 *                  payload traffic loses nothing to UC).
 *   --detect reap  Per-iteration fill-kernel dispatch + stream sync,
 *                  submit, then block in TBSTREAM_ZC_REAP. Coarse
 *                  pools; dispatch boundaries provide coherence.
 *
 *   --reduce 0|1   After detection, accumulate the received partial
 *                  (float adds) into a local accumulator - the
 *                  all-reduce math. Verified against the analytic
 *                  expected sums at the end.
 *
 * Roles only order the start barrier (TCP over the given peer
 * address); the exchange itself is symmetric. The barrier runs after
 * both sides imported and enabled, so no traffic is ever submitted
 * toward an inactive peer device (the known NHI wedge trigger).
 */
#include <hip/hip_runtime.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "../pingpong/thunderbolt-stream.h"

#define FRAME_SIZE	4096u
#define FRAME_WORDS	(FRAME_SIZE / 4u)
#define WARMUP		200u
#define BARRIER_PORT	19777
#define KERNEL_THREADS	256u
#define VERIFY_SAMPLES	16u

/* Host-visible control block (hipHostMalloc, coherent). */
struct ctl {
	uint32_t tx_ready;	/* GPU: outbound slot n-1 is written+stamped */
	uint32_t rx_seen;	/* GPU: inbound message n-1 observed */
	uint32_t host_go;	/* CPU: iteration n-1 fully retired */
	uint32_t stop;
	uint32_t error_iter;	/* first bad payload iteration + 1, or 0 */
};

/* Deterministic float payload; identical on both hosts by (n, j). */
static __host__ __device__ inline float payload_value(uint32_t n, uint32_t j)
{
	uint32_t h = (n * 2654435761u) ^ (j * 40503u);

	return (float)(h & 0xFFFFu) * (1.0f / 65536.0f);
}

/*
 * Persistent exchange kernel, one workgroup. Iteration n:
 *   1. all threads write the payload words of TX slot n;
 *   2. lane 0 stamps the slot's last word (system-scope release) and
 *      announces tx_ready;
 *   3. lane 0 spin-polls the RX slot's stamp; workgroup synchronizes
 *      on a shared flag;
 *   4. optional reduce: all threads accumulate the received partial;
 *      lane 0 sample-verifies a few payload words;
 *   5. lane 0 announces rx_seen and waits for host retirement.
 */
static __global__ void exchange_kernel(uint32_t *tx_pool, uint32_t *rx_pool,
				       float *acc, uint32_t ring,
				       uint32_t frames, uint32_t words,
				       uint32_t iters, uint32_t out_magic,
				       uint32_t in_magic, int do_reduce,
				       struct ctl *c)
{
	__shared__ uint32_t seen;
	uint32_t msgs_per_ring = ring / frames;
	uint32_t tid = threadIdx.x;

	if (blockIdx.x)
		return;

	for (uint32_t n = 0; n < iters; n++) {
		uint32_t slot = (n % msgs_per_ring) * frames * FRAME_WORDS;
		uint32_t last = slot + frames * FRAME_WORDS - 1;
		uint32_t *tx = tx_pool + slot;
		uint32_t *rx = rx_pool + slot;

		for (uint32_t j = tid; j < words; j += KERNEL_THREADS) {
			float v = payload_value(n, j);

			tx[j] = __float_as_uint(v);
		}
		__syncthreads();
		if (tid == 0) {
			__hip_atomic_store(&tx_pool[last], out_magic ^ n,
					   __ATOMIC_RELEASE,
					   __HIP_MEMORY_SCOPE_SYSTEM);
			__hip_atomic_store(&c->tx_ready, n + 1,
					   __ATOMIC_RELEASE,
					   __HIP_MEMORY_SCOPE_SYSTEM);
		}

		if (tid == 0) {
			seen = 0;
			while (__hip_atomic_load(&rx_pool[last],
						 __ATOMIC_ACQUIRE,
						 __HIP_MEMORY_SCOPE_SYSTEM) !=
			       (in_magic ^ n)) {
				if (__hip_atomic_load(&c->stop,
						      __ATOMIC_RELAXED,
						      __HIP_MEMORY_SCOPE_SYSTEM)) {
					seen = 2;
					break;
				}
				__builtin_amdgcn_s_sleep(1);
			}
			if (seen != 2)
				seen = 1;
		}
		__syncthreads();
		if (seen == 2)
			return;

		if (do_reduce) {
			for (uint32_t j = tid; j < words;
			     j += KERNEL_THREADS)
				acc[j] += __uint_as_float(rx[j]);
			__syncthreads();
		}
		if (tid == 0) {
			for (uint32_t k = 0; k < VERIFY_SAMPLES; k++) {
				uint32_t j = (n + k * 449u) % words;

				if (rx[j] !=
				    __float_as_uint(payload_value(n, j))) {
					__hip_atomic_store(&c->error_iter,
							   n + 1,
							   __ATOMIC_RELEASE,
							   __HIP_MEMORY_SCOPE_SYSTEM);
					break;
				}
			}
			__hip_atomic_store(&c->rx_seen, n + 1,
					   __ATOMIC_RELEASE,
					   __HIP_MEMORY_SCOPE_SYSTEM);
			while (__hip_atomic_load(&c->host_go,
						 __ATOMIC_ACQUIRE,
						 __HIP_MEMORY_SCOPE_SYSTEM) <
			       n + 1) {
				if (__hip_atomic_load(&c->stop,
						      __ATOMIC_RELAXED,
						      __HIP_MEMORY_SCOPE_SYSTEM)) {
					seen = 2;
					break;
				}
				__builtin_amdgcn_s_sleep(1);
			}
		}
		__syncthreads();
		if (seen == 2)
			return;
	}
}

/* Reap-arm helpers: one dispatch fills a slot, another reduces it. */
static __global__ void fill_kernel(uint32_t *tx_pool, uint32_t ring,
				   uint32_t frames, uint32_t words,
				   uint32_t n, uint32_t out_magic)
{
	uint32_t msgs_per_ring = ring / frames;
	uint32_t slot = (n % msgs_per_ring) * frames * FRAME_WORDS;
	uint32_t last = slot + frames * FRAME_WORDS - 1;
	uint32_t *tx = tx_pool + slot;

	for (uint32_t j = threadIdx.x; j < words; j += KERNEL_THREADS) {
		float v = payload_value(n, j);

		tx[j] = __float_as_uint(v);
	}
	__syncthreads();
	if (threadIdx.x == 0)
		__hip_atomic_store(&tx_pool[last], out_magic ^ n,
				   __ATOMIC_RELEASE,
				   __HIP_MEMORY_SCOPE_SYSTEM);
}

static __global__ void reduce_kernel(uint32_t *rx_pool, float *acc,
				     uint32_t ring, uint32_t frames,
				     uint32_t words, uint32_t n,
				     uint32_t in_magic, struct ctl *c)
{
	uint32_t msgs_per_ring = ring / frames;
	uint32_t slot = (n % msgs_per_ring) * frames * FRAME_WORDS;
	uint32_t last = slot + frames * FRAME_WORDS - 1;
	uint32_t *rx = rx_pool + slot;

	if (threadIdx.x == 0 && rx_pool[last] != (in_magic ^ n))
		c->error_iter = n + 1;
	for (uint32_t j = threadIdx.x; j < words; j += KERNEL_THREADS)
		acc[j] += __uint_as_float(rx[j]);
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
	uint32_t rx_wait;
	uint64_t rx_events, tx_events;
};

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

/*
 * Start barrier: role b listens, role a connects; one byte each way.
 * Runs strictly after both sides imported and enabled their device.
 */
static void barrier_sync(int is_a, const char *peer)
{
	struct sockaddr_in sa;
	int fd = -1, one = 1;
	char byte = 0x42;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(BARRIER_PORT);

	if (is_a) {
		if (inet_pton(AF_INET, peer, &sa.sin_addr) != 1) {
			fprintf(stderr, "bad peer address %s\n", peer);
			exit(1);
		}
		for (int tries = 0; tries < 100; tries++) {
			fd = socket(AF_INET, SOCK_STREAM, 0);
			if (fd < 0) {
				perror("socket");
				exit(1);
			}
			if (!connect(fd, (struct sockaddr *)&sa, sizeof(sa)))
				break;
			close(fd);
			fd = -1;
			usleep(100000);
		}
		if (fd < 0) {
			fprintf(stderr, "barrier connect to %s failed\n",
				peer);
			exit(1);
		}
	} else {
		int lfd = socket(AF_INET, SOCK_STREAM, 0);

		if (lfd < 0) {
			perror("socket");
			exit(1);
		}
		setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
		sa.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) ||
		    listen(lfd, 1)) {
			perror("barrier bind/listen");
			exit(1);
		}
		fd = accept(lfd, NULL, NULL);
		if (fd < 0) {
			perror("accept");
			exit(1);
		}
		close(lfd);
	}
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	if (write(fd, &byte, 1) != 1 || read(fd, &byte, 1) != 1) {
		fprintf(stderr, "barrier exchange failed\n");
		exit(1);
	}
	close(fd);
}

static void report(const char *tag, uint64_t *lat, uint32_t n,
		   uint64_t wall_ns, uint32_t per_token)
{
	qsort(lat, n, sizeof(*lat), cmp_u64);
	printf("%s latency ns: min=%" PRIu64 " p50=%" PRIu64 " p90=%" PRIu64
	       " p99=%" PRIu64 " max=%" PRIu64 "\n",
	       tag, lat[0], lat[n / 2], lat[(uint64_t)n * 90 / 100],
	       lat[(uint64_t)n * 99 / 100], lat[n - 1]);
	printf("%s sustained: %.2f us/exchange, %.2f ms per %u-exchange token\n",
	       tag, wall_ns / 1000.0 / n,
	       (double)wall_ns / n * per_token / 1e6, per_token);
}

static void usage(FILE *out)
{
	fprintf(out,
		"usage: tbstream-tp-exchange --role a|b --detect reap|spin\n"
		"       --peer ADDR [--reduce 0|1] [--device PATH]\n"
		"       [--ring N] [--frames N] [--bytes N] [--iters N]\n"
		"       [--per-token N] [--gpu N]\n");
}

int main(int argc, char **argv)
{
	const char *device = "/dev/tbstream0";
	const char *role = NULL, *detect = NULL, *peer = NULL;
	uint32_t ring = 4096, frames = 8, bytes = 28672, iters = 2000;
	uint32_t per_token = 90;
	int gpu = 0, do_reduce = 0;

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
		else if (!strcmp(arg, "--peer"))
			peer = val;
		else if (!strcmp(arg, "--reduce"))
			do_reduce = (int)strtol(val, NULL, 0);
		else if (!strcmp(arg, "--ring"))
			ring = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--frames"))
			frames = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--bytes"))
			bytes = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--iters"))
			iters = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--per-token"))
			per_token = (uint32_t)strtoul(val, NULL, 0);
		else if (!strcmp(arg, "--gpu"))
			gpu = (int)strtol(val, NULL, 0);
		else {
			usage(stderr);
			return 2;
		}
	}
	uint32_t words = bytes / 4u;

	if (!role || !detect || !peer || !ring || !frames || ring % frames ||
	    iters <= WARMUP || !per_token || !bytes || bytes % 4u ||
	    bytes + 4u > frames * FRAME_SIZE ||
	    (strcmp(role, "a") && strcmp(role, "b")) ||
	    (strcmp(detect, "reap") && strcmp(detect, "spin"))) {
		usage(stderr);
		return 2;
	}
	int is_a = !strcmp(role, "a");
	int use_spin = !strcmp(detect, "spin");
	uint64_t pool_bytes = (uint64_t)ring * FRAME_SIZE;
	uint64_t alloc_bytes = pool_bytes < (16ull << 20) ? (16ull << 20) :
			       pool_bytes;
	uint32_t out_magic = is_a ? 0x45784100u : 0x45784200u;
	uint32_t in_magic = is_a ? 0x45784200u : 0x45784100u;

	HIP_CHECK("hipSetDevice", hipSetDevice(gpu));

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
	float *acc = NULL;
	HIP_CHECK("hipMalloc(acc)",
		  hipMalloc((void **)&acc, (size_t)words * sizeof(float)));
	HIP_CHECK("hipMemset(tx)", hipMemset(tx_pool, 0, alloc_bytes));
	HIP_CHECK("hipMemset(rx)", hipMemset(rx_pool, 0, alloc_bytes));
	HIP_CHECK("hipMemset(acc)",
		  hipMemset(acc, 0, (size_t)words * sizeof(float)));
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
	volatile uint32_t *error_iter = &c->error_iter;

	hipStream_t stream;
	HIP_CHECK("hipStreamCreate", hipStreamCreate(&stream));
	if (use_spin) {
		hipLaunchKernelGGL(exchange_kernel, dim3(1),
				   dim3(KERNEL_THREADS), 0, stream,
				   (uint32_t *)tx_pool, (uint32_t *)rx_pool,
				   acc, ring, frames, words, iters,
				   out_magic, in_magic, do_reduce, c);
		HIP_CHECK("kernel launch", hipGetLastError());
		spin_wait(tx_ready, 1, "first tx_ready (kernel alive?)", 0);
		fprintf(stderr, "spin kernel is live\n");
	}

	/* Both devices are imported+enabled: safe to start transmitting. */
	barrier_sync(is_a, peer);

	uint64_t *lat = (uint64_t *)calloc(iters, sizeof(*lat));
	uint64_t wall_start = 0;

	printf("role=%s detect=%s reduce=%d ring=%u frames=%u bytes=%u "
	       "iters=%u per_token=%u\n",
	       role, detect, do_reduce, ring, frames, bytes, iters,
	       per_token);

	for (uint32_t n = 0; n < iters; n++) {
		if (n == WARMUP)
			wall_start = now_ns();
		if (use_spin) {
			spin_wait(tx_ready, n + 1, "tx_ready", n);

			uint64_t t0 = now_ns();

			submit(&s);
			spin_wait(rx_seen, n + 1, "rx_seen", n);
			lat[n] = now_ns() - t0;
		} else {
			hipLaunchKernelGGL(fill_kernel, dim3(1),
					   dim3(KERNEL_THREADS), 0, stream,
					   (uint32_t *)tx_pool, ring, frames,
					   words, n, out_magic);
			HIP_CHECK("fill sync", hipStreamSynchronize(stream));

			uint64_t t0 = now_ns();

			submit(&s);
			while (s.rx_events <= n)
				drain(&s, 1);
			lat[n] = now_ns() - t0;
			if (do_reduce) {
				hipLaunchKernelGGL(reduce_kernel, dim3(1),
						   dim3(KERNEL_THREADS), 0,
						   stream,
						   (uint32_t *)rx_pool, acc,
						   ring, frames, words, n,
						   in_magic, c);
				HIP_CHECK("reduce sync",
					  hipStreamSynchronize(stream));
			}
		}
		drain(&s, 0);
		if (use_spin)
			c->host_go = n + 1;
		if (*error_iter) {
			fprintf(stderr, "FAIL: payload mismatch at iter %u\n",
				*error_iter - 1);
			return 1;
		}
	}

	uint64_t wall_ns = now_ns() - wall_start;

	while (s.rx_events < iters || s.tx_events < iters)
		drain(&s, 1);
	if (use_spin) {
		c->stop = 1;
		HIP_CHECK("hipStreamSynchronize", hipStreamSynchronize(stream));
	}

	if (do_reduce) {
		float sample[VERIFY_SAMPLES];
		uint32_t idx[VERIFY_SAMPLES];
		int bad = 0;

		for (uint32_t k = 0; k < VERIFY_SAMPLES; k++)
			idx[k] = (k * 449u) % words;
		for (uint32_t k = 0; k < VERIFY_SAMPLES; k++) {
			HIP_CHECK("acc readback",
				  hipMemcpy(&sample[k], acc + idx[k],
					    sizeof(float),
					    hipMemcpyDeviceToHost));
			float want = 0.0f;

			for (uint32_t n = 0; n < iters; n++)
				want += payload_value(n, idx[k]);
			if (sample[k] != want) {
				fprintf(stderr,
					"FAIL: acc[%u]=%.9g want %.9g\n",
					idx[k], sample[k], want);
				bad = 1;
			}
		}
		if (bad)
			return 1;
		printf("reduce verified: %u sampled accumulator sums exact\n",
		       VERIFY_SAMPLES);
	}

	struct tbstream_zc_stats stats = {};
	if (ioctl(s.fd, TBSTREAM_ZC_GET_STATS, &stats)) {
		perror("TBSTREAM_ZC_GET_STATS");
		return 1;
	}
	if (stats.flags & TBSTREAM_ZC_STATS_F_FAILED) {
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

	char tag[64];
	snprintf(tag, sizeof(tag), "RESULT %s/%s%s", role, detect,
		 do_reduce ? "+reduce" : "");
	report(tag, lat + WARMUP, iters - WARMUP, wall_ns, per_token);

	close(s.fd);
	close(tx_fd);
	close(rx_fd);
	HIP_CHECK("hipFree tx", hipFree(tx_pool));
	HIP_CHECK("hipFree rx", hipFree(rx_pool));
	HIP_CHECK("hipFree acc", hipFree(acc));
	free(lat);
	return 0;
}
