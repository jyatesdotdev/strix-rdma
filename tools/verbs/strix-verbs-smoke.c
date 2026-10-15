// SPDX-License-Identifier: GPL-2.0-only
/* Ordinary libibverbs client. TCP carries only 64-byte metadata and barriers;
 * every tested payload byte goes through registered native SEND/RECV, RDMA
 * WRITE or RDMA READ. */
#define _POSIX_C_SOURCE 200809L
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define WINDOW 4u
#define CHUNK (2u * 1024u * 1024u)
#define POOL (WINDOW * CHUNK)
#define META_SIZE 64u
/* The native zero-copy burst path posts MR pages directly, and a header-less
 * data frame never spans a page, so a remote target must be this aligned for a
 * large WRITE to leave the staged path. */
#define RDMA_ALIGN 4096u
#define TIMEOUT_MS 30000
/* raddr/rkey name the remotely accessible target buffer this side exposes;
 * both are zero in the SEND-only modes. */
struct endpoint { uint64_t nonce, raddr; uint32_t qpn, psn, rkey; union ibv_gid gid; };
struct counters { unsigned long long tx, rx, bad, send, write, read; };
/* --gpu: every registered buffer is GPU memory (hipMalloc), exported as a
 * DMA-BUF and registered with ibv_reg_dmabuf_mr (docs/GPU_DIRECT_VERBS.md).
 * HIP is loaded at runtime so the tool keeps building as plain C. Patterns are
 * built in a host shadow and copied in; results are copied out after a
 * system-scope acquire and checked on the host. STRIX_GPU_NO_ACQUIRE=1 drops
 * that acquire, a diagnostic arm for the stale-cache gate. */
static bool gpu;
/* --len/--off: each window slot carries one transfer of xfer_len bytes starting
 * xfer_off bytes into its 2 MiB slot (default: the whole slot). Small or
 * unaligned geometry takes the staged path; an aligned odd length takes the
 * zero-copy path with a partial last frame. The rest of every landing slot must
 * stay zero, so an overrun is caught too. */
static unsigned xfer_len = CHUNK, xfer_off;
/* --recv-off N (send mode): post the receive buffers N bytes into their slots
 * while the sends keep --off, so a frame-aligned (zero-copy) sender can meet an
 * unaligned receive buffer -- legal in verbs, and common (malloc returns large
 * buffers 16 bytes past a page). Defaults to --off. */
static unsigned recv_off;
static bool recv_off_set;
/* --linger-ms N: stay idle (connection up, no traffic) for N ms after the last
 * transfer, so the recovery paths that only run while a connection idles -- a
 * lost READ_ACK answered by SN_READ_PROBE -- can be exercised. */
static unsigned linger_ms;
static struct {
	int (*set_device)(int);
	int (*malloc)(void **, size_t);
	int (*free)(void *);
	int (*memcpy)(void *, const void *, size_t, int);
	int (*memset)(void *, int, size_t);
	int (*synchronize)(void);
	int (*address_range)(void **, size_t *, void *);
	int (*export)(void *, void *, size_t, int, unsigned long long);
	int (*event_create)(void **, unsigned);
	int (*event_record)(void *, void *);
	const char *(*error)(int);
	int (*module_load)(void **, const char *);
	int (*module_function)(void **, void *, const char *);
	int (*launch)(void *, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
		      unsigned, void *, void **, void **);
	int (*event_sync)(void *);
	void *acquire, *no_fence, *fill, *check;
	unsigned *bad;		/* device counter written by sn_check */
} hip;
/* --gpu-kernels PATH: GPU kernels from that code object produce the pattern and
 * check what landed (the G4 stale-cache gate). STRIX_GPU_NO_RELEASE=1 waits for
 * the producer without a system-scope release; STRIX_GPU_NO_ACQUIRE=1 checks
 * without a system-scope acquire. Mismatches are counted, not fatal mid-run, so a
 * diagnostic arm reports a rate; any mismatch still fails the run at the end.
 * STRIX_GPU_CHECK_SKEW=1 is the positive control: every iteration must fail.
 * STRIX_GPU_HOT=1 keeps the probed lines L2-resident (see sn_check). */
static const char *gpu_kernels;
static unsigned long long kernel_bad_bytes;
static unsigned kernel_bad_iters;
#define HIP_H2D 1
#define HIP_D2H 2
#define HIP_DMABUF_FD 1
#define HIP_EVENT_RELEASE_TO_SYSTEM 0x80000000u
#define HIP_EVENT_DISABLE_TIMING 0x2u
#define HIP_EVENT_DISABLE_SYSTEM_FENCE 0x20000000u
static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", what, errno, strerror(errno));
	exit(1); /* Kernel context close, never manual unpin of outstanding memory. */
}
static void require(bool ok, const char *what) { if (!ok) fail(what); }
static void verb(int ret, const char *what) { if (ret) { errno = ret; fail(what); } }
static void hip_ok(int ret, const char *what)
{
	if (ret) { fprintf(stderr, "FAIL: %s (hip %d: %s)\n", what, ret, hip.error(ret)); exit(1); }
}
static void gpu_load(void)
{
	void *h = dlopen("libamdhip64.so.7", RTLD_NOW);
	if (!h) h = dlopen("libamdhip64.so", RTLD_NOW);
	require(h != NULL, "load the HIP runtime (libamdhip64)");
#define SYM(f, n) require((*(void **)&hip.f = dlsym(h, n)) != NULL, n)
	SYM(set_device, "hipSetDevice"); SYM(malloc, "hipMalloc"); SYM(free, "hipFree");
	SYM(memcpy, "hipMemcpy"); SYM(memset, "hipMemset");
	SYM(synchronize, "hipDeviceSynchronize"); SYM(address_range, "hipMemGetAddressRange");
	SYM(export, "hipMemGetHandleForAddressRange"); SYM(event_create, "hipEventCreateWithFlags");
	SYM(event_record, "hipEventRecord"); SYM(error, "hipGetErrorString");
	hip_ok(hip.set_device(0), "hipSetDevice(0)");
	/* Timing-enabled, system-scope: recording it emits the acquire. */
	hip_ok(hip.event_create(&hip.acquire, HIP_EVENT_RELEASE_TO_SYSTEM), "acquire event");
	if (gpu_kernels) {
		void *module = NULL;

		SYM(module_load, "hipModuleLoad"); SYM(module_function, "hipModuleGetFunction");
		SYM(launch, "hipModuleLaunchKernel"); SYM(event_sync, "hipEventSynchronize");
		hip_ok(hip.module_load(&module, gpu_kernels), "load GPU kernels code object");
		hip_ok(hip.module_function(&hip.fill, module, "sn_fill"), "kernel sn_fill");
		hip_ok(hip.module_function(&hip.check, module, "sn_check"), "kernel sn_check");
		hip_ok(hip.malloc((void **)&hip.bad, sizeof(*hip.bad)), "mismatch counter");
		hip_ok(hip.event_create(&hip.no_fence, HIP_EVENT_DISABLE_TIMING |
			HIP_EVENT_DISABLE_SYSTEM_FENCE), "fence-free completion event");
	}
#undef SYM
}
/* A registered buffer and the host memory its contents are staged through.
 * Without --gpu they are the same allocation. */
static void buffer_alloc(unsigned char **buf, unsigned char **host)
{
	verb(posix_memalign((void **)host, 4096, POOL), "host buffer allocation");
	if (gpu) hip_ok(hip.malloc((void **)buf, POOL), "hipMalloc");
	else *buf = *host;
}
static struct ibv_mr *buffer_register(struct ibv_pd *pd, unsigned char *buf, int access)
{
	void *base = NULL; size_t size = 0; int dmabuf = -1;
	struct ibv_mr *mr;

	if (!gpu) return ibv_reg_mr(pd, buf, POOL, access);
	/* The exported fd names the whole backing allocation, so the MR's offset
	 * into it is the pointer's distance from that allocation's base. */
	hip_ok(hip.address_range(&base, &size, buf), "hipMemGetAddressRange");
	hip_ok(hip.export(&dmabuf, buf, POOL, HIP_DMABUF_FD, 0), "export hipMalloc as DMA-BUF");
	mr = ibv_reg_dmabuf_mr(pd, (uintptr_t)buf - (uintptr_t)base, POOL, (uintptr_t)buf, dmabuf, access);
	if (mr)
		fprintf(stderr, "GPU MR: hipMalloc %p (allocation %zu B, offset %zu), DMA-BUF fd %d, lkey %u\n",
			(void *)buf, size, (size_t)((uintptr_t)buf - (uintptr_t)base), dmabuf, mr->lkey);
	close(dmabuf);	/* the MR holds its own reference to the DMA-BUF */
	return mr;
}
static void buffer_free(unsigned char *buf, unsigned char *host)
{
	if (gpu && buf) hip_ok(hip.free(buf), "hipFree");
	free(host);
}
/* Make a host-built pattern the buffer's content and clear a landing buffer,
 * then release: the NHI must not race dirty GPU cache lines (a later write-back
 * of the cleared lines would overwrite received data). */
static void buffers_publish(unsigned char *src, const unsigned char *src_host, unsigned char *dst,
			    uint64_t nonce, unsigned it)
{
	if (!gpu) return;
	if (gpu_kernels) {
		unsigned long long n = nonce;
		void *args[] = { &src, &n, &it, &xfer_len, &xfer_off };

		hip_ok(hip.launch(hip.fill, 256, 1, 1, 256, 1, 1, 0, NULL, args, NULL), "launch sn_fill");
	} else {
		hip_ok(hip.memcpy(src, src_host, POOL, HIP_H2D), "publish pattern to GPU buffer");
	}
	/* STRIX_GPU_HOT=1 clears the landing buffer only once, so neither this
	 * clear nor its write-back evicts the lines the stale-cache gate probes. */
	if (!(gpu_kernels && getenv("STRIX_GPU_HOT")) || it == 1)
		hip_ok(hip.memset(dst, 0, POOL), "clear GPU landing buffer");
	if (gpu_kernels && getenv("STRIX_GPU_NO_RELEASE")) {
		/* Diagnostic: wait for the GPU without a system-scope release. */
		hip_ok(hip.event_record(hip.no_fence, NULL), "fence-free completion");
		hip_ok(hip.event_sync(hip.no_fence), "wait without release");
	} else {
		hip_ok(hip.synchronize(), "system-scope release");
	}
}
/* Check a landing buffer with sn_check on the GPU, after a system-scope acquire
 * unless STRIX_GPU_NO_ACQUIRE is set. Counts, and reports the first few. */
static void gpu_kernel_verify(const unsigned char *buf, uint64_t nonce, unsigned it)
{
	unsigned long long n = nonce;
	unsigned bad = 0;
	/* Positive control: STRIX_GPU_CHECK_SKEW=1 checks against the next
	 * iteration's pattern, so a live check must report nearly every byte. */
	unsigned want = it + (getenv("STRIX_GPU_CHECK_SKEW") ? 1u : 0u);
	unsigned full = getenv("STRIX_GPU_HOT") ? 0u : 1u;
	void *args[] = { &buf, &n, &want, &xfer_len, &xfer_off, &hip.bad, &full };

	hip_ok(hip.memset(hip.bad, 0, sizeof(bad)), "reset mismatch counter");
	if (!getenv("STRIX_GPU_NO_ACQUIRE"))
		hip_ok(hip.event_record(hip.acquire, NULL), "system-scope acquire");
	hip_ok(hip.launch(hip.check, 256, 1, 1, 256, 1, 1, 0, NULL, args, NULL), "launch sn_check");
	hip_ok(hip.memcpy(&bad, hip.bad, sizeof(bad), HIP_D2H), "read mismatch counter");
	if (bad) {
		if (kernel_bad_iters < 10)
			fprintf(stderr, "GPU-KERNEL MISMATCH iter %u: %u of %u bytes wrong\n", it, bad, POOL);
		kernel_bad_iters++; kernel_bad_bytes += bad;
	}
}
static void gpu_kernel_summary(unsigned iters, const char *role)
{
	if (!gpu_kernels) return;
	printf("GPU kernels: produced and checked on the GPU, release=%s acquire=%s hot=%s, %u of %u iterations mismatched (%llu bytes) (%s)\n",
	       getenv("STRIX_GPU_NO_RELEASE") ? "OFF" : "on", getenv("STRIX_GPU_NO_ACQUIRE") ? "OFF" : "on",
	       getenv("STRIX_GPU_HOT") ? "yes" : "no",
	       kernel_bad_iters, iters, kernel_bad_bytes, role);
	fflush(stdout);
	require(!kernel_bad_iters, "GPU-kernel payload verification");
}
static void buffer_collect(unsigned char *host, const unsigned char *buf)
{
	if (!gpu) return;
	if (!getenv("STRIX_GPU_NO_ACQUIRE"))
		hip_ok(hip.event_record(hip.acquire, NULL), "system-scope acquire");
	hip_ok(hip.memcpy(host, buf, POOL, HIP_D2H), "collect GPU landing buffer");
}
static unsigned iterations(const char *text, unsigned long limit, const char *what)
{
	char *end = NULL; unsigned long v = strtoul(text, &end, 10);
	require(end && !*end && v && v <= limit, what);
	return (unsigned)v;
}
static int64_t milliseconds(void)
{
	struct timespec ts;
	require(!clock_gettime(CLOCK_MONOTONIC, &ts), "clock_gettime");
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int64_t microseconds(void)
{
	struct timespec ts;
	require(!clock_gettime(CLOCK_MONOTONIC, &ts), "clock_gettime");
	return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
static void wait_fd(int fd, short events, int64_t deadline)
{
	struct pollfd p = { .fd = fd, .events = events };
	for (;;) {
		int64_t remaining = deadline - milliseconds();
		int n;
		if (remaining <= 0) { errno = ETIMEDOUT; fail("control deadline"); }
		n = poll(&p, 1, (int)remaining);
		if (n < 0 && errno == EINTR) continue;
		require(n > 0 && (p.revents & events), "control socket poll");
		return;
	}
}
static void control_io(int fd, void *buf, size_t size, bool writing, int64_t deadline)
{
	unsigned char *p = buf;
	while (size) {
		ssize_t n;
		wait_fd(fd, writing ? POLLOUT : POLLIN, deadline);
		n = writing ? send(fd, p, size, MSG_NOSIGNAL) : recv(fd, p, size, 0);
		if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
		if (n == 0) { errno = 0; fail("control I/O: peer closed the connection"); }
		require(n > 0, "control I/O"); p += n; size -= (size_t)n;
	}
}
static int control_socket(bool listening, const char *address, const char *port)
{
	struct sockaddr_in addr = { .sin_family = AF_INET };
	char *end;
	long number = strtol(port, &end, 10);
	int fd, ret;
	int64_t deadline = milliseconds() + TIMEOUT_MS;

	require(*port && !*end && number > 0 && number <= 65535, "TCP port");
	addr.sin_port = htons((uint16_t)number);
	require(inet_pton(AF_INET, address, &addr.sin_addr) == 1 &&
		addr.sin_addr.s_addr != INADDR_ANY, "explicit IPv4 address");
	fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	require(fd >= 0, "socket");
	if (listening) {
		verb(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) ? errno : 0, "reuseaddr");
		require(!bind(fd, (struct sockaddr *)&addr, sizeof(addr)) && !listen(fd, 1), "listen");
		wait_fd(fd, POLLIN, deadline);
		ret = accept(fd, NULL, NULL); require(ret >= 0, "accept"); close(fd); fd = ret;
		require(fcntl(fd, F_SETFL, O_NONBLOCK) != -1, "accepted nonblock");
	} else {
		ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
		require(!ret || errno == EINPROGRESS, "connect");
		if (ret) {
			int err = 0; socklen_t len = sizeof(err);
			wait_fd(fd, POLLOUT, deadline);
			require(!getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len), "connect status");
			verb(err, "connect completion");
		}
	}
	return fd;
}
static void put32(unsigned char *p, uint32_t n)
{ p[0] = n >> 24; p[1] = n >> 16; p[2] = n >> 8; p[3] = n; }
static uint32_t get32(const unsigned char *p)
{ return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }
static void metadata_encode(unsigned char out[META_SIZE], const struct endpoint *e)
{
	memset(out, 0, META_SIZE); put32(out, 0x53564d31); put32(out+4, 2);
	put32(out+8, e->nonce >> 32); put32(out+12, e->nonce);
	put32(out+16, e->qpn); put32(out+20, e->psn); put32(out+24, IBV_MTU_4096);
	put32(out+28, e->rkey);
	memcpy(out+32, e->gid.raw, 16);
	put32(out+48, e->raddr >> 32); put32(out+52, e->raddr);
}
static bool metadata_decode(struct endpoint *e, const unsigned char in[META_SIZE])
{
	union ibv_gid zero = {};
	if (get32(in) != 0x53564d31 || get32(in+4) != 2 || get32(in+56) || get32(in+60) ||
	    get32(in+24) != IBV_MTU_4096 || !get32(in+16) || get32(in+16) > 0xffffff ||
	    get32(in+20) > 0xffffff || !memcmp(in+32, &zero, 16)) return false;
	e->nonce = (uint64_t)get32(in+8)<<32 | get32(in+12);
	e->qpn = get32(in+16); e->psn = get32(in+20); e->rkey = get32(in+28);
	memcpy(e->gid.raw, in+32, 16);
	e->raddr = (uint64_t)get32(in+48)<<32 | get32(in+52);
	/* rkey and remote address describe one buffer: both or neither, and frame
	 * aligned so a large WRITE can take the zero-copy data ring. */
	if ((e->rkey && !e->raddr) || (!e->rkey && e->raddr)) return false;
	if (e->raddr && (e->raddr & (RDMA_ALIGN - 1))) return false;
	return e->nonce != 0;
}
static void barrier(int fd, unsigned window)
{
	unsigned char out[4], in[4]; int64_t deadline = milliseconds() + TIMEOUT_MS;
	put32(out, window); control_io(fd, out, sizeof(out), true, deadline);
	control_io(fd, in, sizeof(in), false, deadline);
	require(!memcmp(out, in, sizeof(out)), "window barrier");
}
static unsigned char pattern(uint64_t nonce, unsigned window, unsigned slot, unsigned byte)
{
	return (unsigned char)((nonce >> ((byte % 8) * 8)) ^ (byte * 131u) ^
		(byte >> 11) ^ (window * 29u) ^ (slot * 53u));
}
/* On a payload mismatch, say what the bad bytes ARE, not only that they are
 * wrong. Each failure hypothesis predicts a different signature: zeros mean the
 * bytes never landed or the for-CPU sync was missed; the previous iteration's
 * pattern means a stale transmit or a late frame from the last iteration; another
 * slot's pattern means a frame landed at the wrong offset. Classification uses a
 * run of up to 64 bytes so a 1-in-256 coincidence cannot mislabel it. */
static int pattern_run(const unsigned char *p, unsigned n, uint64_t nonce,
		       unsigned window, unsigned slot, unsigned byte)
{
	for (unsigned k = 0; k < n; k++)
		if (p[k] != pattern(nonce, window, slot, byte + k)) return 0;
	return 1;
}
/* Within one 4 KiB frame, pattern() reduces to the source nonce byte, k*131, and
 * one constant C = 2*frame ^ 29*iter ^ 53*slot. So each bad frame reveals whose
 * buffer it came from (peer or this side's own) and C_delta against the expected
 * C: a delta that is constant across frames means a whole-burst source mix-up
 * (wrong iteration or slot); one that varies frame to frame means frames landed
 * in the wrong order or at the wrong offset. */
static void frame_diag(const unsigned char *s, unsigned chunk, uint64_t peer_n,
		       uint64_t own_n, unsigned it, unsigned slot)
{
	unsigned shown = 0;

	for (unsigned f = 0; f < chunk / 4096 && shown < 6; f++) {
		const unsigned char *p = s + (size_t)f * 4096;
		unsigned base = f * 4096, ok_peer = 1, ok_own = 1;
		unsigned char cp = p[0] ^ (unsigned char)peer_n, co = p[0] ^ (unsigned char)own_n;
		unsigned char want = (unsigned char)((2u * f) ^ (it * 29u) ^ (slot * 53u));

		if (p[0] == pattern(peer_n, it, slot, base)) continue;
		for (unsigned k = 1; k < 8; k++) {
			unsigned char t = (unsigned char)((base + k) * 131u);
			if ((unsigned char)(p[k] ^ t ^ (unsigned char)(peer_n >> (k * 8))) != cp) ok_peer = 0;
			if ((unsigned char)(p[k] ^ t ^ (unsigned char)(own_n >> (k * 8))) != co) ok_own = 0;
		}
		fprintf(stderr, "  bad frame %u: source=%s C_delta=0x%02x zero=%d\n", f,
			ok_peer ? "peer-buffer" : ok_own ? "OWN-buffer" : "neither",
			(unsigned)((unsigned char)((ok_peer ? cp : co) ^ want)), !p[0] && !p[1] && !p[2]);
		shown++;
	}
}
static void report_mismatch(const unsigned char *landed, uint64_t nonce, uint64_t own_nonce,
			    unsigned it, unsigned slot, unsigned chunk, unsigned slots)
{
	const unsigned char *s = landed + (size_t)slot * chunk;
	unsigned first = chunk, last = 0, bad = 0, n, k;
	const char *kind = "unexplained";
	char other[96];

	for (k = 0; k < chunk; k++)
		if (s[k] != pattern(nonce, it, slot, k)) {
			if (first == chunk) first = k;
			last = k; bad++;
		}
	n = chunk - first < 64 ? chunk - first : 64;
	for (k = 0; k < n && !s[first + k]; k++)
		;
	if (k == n)
		kind = "zeros: never landed, or the for-CPU sync was missed";
	else if (it > 1 && pattern_run(s + first, n, nonce, it - 1, slot, first))
		kind = "previous iteration's bytes: stale transmit or a late frame";
	else
		for (unsigned o = 0; o < slots; o++)
			if (o != slot && pattern_run(s + first, n, nonce, it, o, first)) {
				snprintf(other, sizeof(other), "slot %u's bytes: landed at the wrong offset", o);
				kind = other;
				break;
			}
	fprintf(stderr, "PAYLOAD MISMATCH iter=%u slot=%u first_bad=%u (frame %u +%u) "
		"last_bad=%u bad_bytes=%u/%u got=0x%02x want=0x%02x -> %s\n",
		it, slot, first, first / 4096, first % 4096, last, bad, chunk,
		s[first], pattern(nonce, it, slot, first), kind);
	frame_diag(s, chunk, nonce, own_nonce, it, slot);
}
static struct counters stats(const char *device)
{
	char path[256]; struct counters c = {}; FILE *f; int n;
	require(snprintf(path, sizeof(path), "/sys/class/infiniband/%s/native_stats", device) < (int)sizeof(path), "stats path");
	f = fopen(path, "r"); require(f != NULL, "native_stats (no fallback permitted)");
	n = fscanf(f, "tx_frames %llu\nrx_frames %llu\nbad_frames %llu\nplaced_send_bytes %llu\nplaced_write_bytes %llu\nplaced_read_bytes %llu",
		&c.tx, &c.rx, &c.bad, &c.send, &c.write, &c.read);
	require(!fclose(f) && n == 6, "native_stats format"); return c;
}
static void selftest(void)
{
	struct endpoint e = { .nonce = UINT64_C(0x12345678abcdef01), .qpn = 17, .psn = 0xffffff,
		.rkey = 0x2a, .raddr = UINT64_C(0x7f8000001000) }, d;
	unsigned char bytes[META_SIZE];
	e.gid.raw[0] = 0xfe; e.gid.raw[1] = 0x80; e.gid.raw[15] = 3;
	metadata_encode(bytes, &e); require(metadata_decode(&d, bytes), "metadata decode");
	require(d.nonce == e.nonce && d.qpn == e.qpn && d.psn == e.psn && !memcmp(&d.gid, &e.gid, 16), "metadata round trip");
	require(d.rkey == e.rkey && d.raddr == e.raddr, "RDMA target round trip");
	metadata_encode(bytes, &e); bytes[56] = 1; require(!metadata_decode(&d, bytes), "reserved rejection");
	metadata_encode(bytes, &e); put32(bytes+4, 1); require(!metadata_decode(&d, bytes), "metadata version rejection");
	metadata_encode(bytes, &e); bytes[20] = 1; require(!metadata_decode(&d, bytes), "PSN rejection");
	metadata_encode(bytes, &e); put32(bytes+16, 0); require(!metadata_decode(&d, bytes), "QPN rejection");
	metadata_encode(bytes, &e); memset(bytes+32, 0, 16); require(!metadata_decode(&d, bytes), "zero GID rejection");
	metadata_encode(bytes, &e); put32(bytes+28, 0); require(!metadata_decode(&d, bytes), "remote address without rkey rejection");
	metadata_encode(bytes, &e); put32(bytes+48, 0); put32(bytes+52, 0);
	require(!metadata_decode(&d, bytes), "rkey without remote address rejection");
	metadata_encode(bytes, &e); put32(bytes+48, 0); put32(bytes+52, 0x1001);
	require(!metadata_decode(&d, bytes), "unaligned remote address rejection");
	metadata_encode(bytes, &e); put32(bytes+28, 0); put32(bytes+48, 0); put32(bytes+52, 0);
	require(metadata_decode(&d, bytes) && !d.rkey && !d.raddr, "SEND-only metadata carries no RDMA target");
	require(pattern(e.nonce, 1, 0, 0) != pattern(e.nonce, 1, 1, 0), "distinct slots");
	require(pattern(e.nonce, 1, 0, 0) != pattern(e.nonce, 2, 0, 0), "distinct iterations");
	puts("ok - smoke metadata codec/selftest only; no device opened");
}
/* One landing slot: the transfer range holds the peer's pattern for this
 * iteration and slot, and every byte outside it is still zero (no overrun). On
 * a whole-slot transfer the existing classifier explains a mismatch. */
static bool slot_verified(const unsigned char *landed, uint64_t nonce, uint64_t own_nonce,
			  unsigned it, unsigned slot, unsigned off)
{
	const unsigned char *b = landed + (size_t)slot * CHUNK;
	unsigned j;

	for (j = 0; j < xfer_len; j++)
		if (b[off + j] != pattern(nonce, it, slot, j))
			break;
	if (j < xfer_len) {
		if (!off && xfer_len == CHUNK)
			report_mismatch(landed, nonce, own_nonce, it, slot, CHUNK, WINDOW);
		else
			fprintf(stderr, "MISMATCH iter %u slot %u: first bad byte %u of %u (offset %u): got 0x%02x want 0x%02x\n",
				it, slot, j, xfer_len, off, b[off + j], pattern(nonce, it, slot, j));
		return false;
	}
	/* Only the bytes outside the transfer range: a full-slot transfer has none,
	 * so the default geometry costs nothing extra here. */
	for (j = 0; j < off; j++)
		if (b[j]) goto overrun;
	for (j = off + xfer_len; j < CHUNK; j++)
		if (b[j]) goto overrun;
	return true;
overrun:
	fprintf(stderr, "MISMATCH iter %u slot %u: overrun, byte %u outside [%u, %u) is 0x%02x\n",
		it, slot, j, off, off + xfer_len, b[j]);
	return false;
}
int main(int argc, char **argv)
{
	struct ibv_device **devices; struct ibv_context *ctx = NULL;
	struct ibv_device_attr dev; struct ibv_port_attr port;
	struct ibv_pd *pd; struct ibv_cq *cq; struct ibv_qp *qp;
	struct ibv_mr *tx_mr, *rx_mr, *rd_mr = NULL; unsigned char *tx, *rx, *rd = NULL;
	unsigned char *htx, *hrx, *hrd = NULL;
	int64_t xfer_us = 0;
	struct endpoint local = {}, peer; struct counters before, after;
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr init = { .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = WINDOW, .max_recv_wr = WINDOW, .max_send_sge = 1, .max_recv_sge = 1 } };
	unsigned char mine[META_SIZE], theirs[META_SIZE];
	unsigned long long expected = 0;
	unsigned bench_iters = 0, ping_iters = 0, rdma_iters = 0, send_iters = 0;
	int count, fd; bool listening, rdma_read = false;
	if (argc == 2 && !strcmp(argv[1], "--selftest")) { selftest(); return 0; }
	while (argc >= 2 && strcmp(argv[1], "--live")) {
		if (!strcmp(argv[1], "--gpu")) {
			gpu = true;
			argv[1] = argv[0]; argv++; argc--;
		} else if (argc >= 3 && !strcmp(argv[1], "--gpu-kernels")) {
			gpu = true; gpu_kernels = argv[2];
			argv[2] = argv[0]; argv += 2; argc -= 2;
		} else if (argc >= 3 && !strcmp(argv[1], "--linger-ms")) {
			linger_ms = iterations(argv[2], 60000, "--linger-ms 1..60000");
			argv[2] = argv[0]; argv += 2; argc -= 2;
		} else if (argc >= 3 && !strcmp(argv[1], "--recv-off")) {
			char *end = NULL; unsigned long v = strtoul(argv[2], &end, 10);
			require(end && !*end && v < CHUNK, "--recv-off 0..2097151");
			recv_off = (unsigned)v; recv_off_set = true;
			argv[2] = argv[0]; argv += 2; argc -= 2;
		} else if (argc >= 3 && !strcmp(argv[1], "--len")) {
			xfer_len = iterations(argv[2], CHUNK, "--len 1..2097152");
			argv[2] = argv[0]; argv += 2; argc -= 2;
		} else if (argc >= 3 && !strcmp(argv[1], "--off")) {
			char *end = NULL; unsigned long v = strtoul(argv[2], &end, 10);
			require(end && !*end && v < CHUNK, "--off 0..2097151");
			xfer_off = (unsigned)v;
			argv[2] = argv[0]; argv += 2; argc -= 2;
		} else break;
	}
	if (!recv_off_set) recv_off = xfer_off;
	require(xfer_off + (unsigned long)xfer_len <= CHUNK && recv_off + (unsigned long)xfer_len <= CHUNK,
		"--off/--recv-off + --len must fit one 2 MiB slot");
	if ((argc < 6 || argc > 8) || strcmp(argv[1], "--live") ||
	    (strcmp(argv[3], "listen") && strcmp(argv[3], "connect"))) {
		fprintf(stderr, "usage: %s --selftest\n       %s --live strix_nhiN listen|connect explicit-IPv4 TCP-port [bench-iterations | ping [iters] | send [iters] | write [iters] | read [iters]]\n       %s [--gpu | --gpu-kernels CODE_OBJECT] [--len N] [--off N] [--linger-ms N] --live ... send|write|read iters\n", argv[0], argv[0], argv[0]);
		return 2;
	}
	if (argc >= 7 && !strcmp(argv[6], "ping")) {
		if (argc == 8) ping_iters = iterations(argv[7], 1000000, "ping iteration count");
		else ping_iters = 1000;
	} else if (argc >= 7 && !strcmp(argv[6], "send")) {
		if (argc == 8) send_iters = iterations(argv[7], 100000, "SEND iteration count");
		else send_iters = 4;
	} else if (argc >= 7 && (!strcmp(argv[6], "write") || !strcmp(argv[6], "read"))) {
		rdma_read = !strcmp(argv[6], "read");
		if (argc == 8) rdma_iters = iterations(argv[7], 100000, "RDMA iteration count");
		else rdma_iters = 4;
	} else if (argc == 7) {
		bench_iters = iterations(argv[6], 100000, "iteration count");
	}
	require(!strncmp(argv[2], "strix_nhi", 9) && argv[2][9] &&
		strspn(argv[2]+9, "0123456789") == strlen(argv[2]+9), "explicit native device");
	listening = !strcmp(argv[3], "listen");
	devices = ibv_get_device_list(&count); require(devices != NULL, "device list");
	for (int i = 0; i < count; i++) if (!strcmp(ibv_get_device_name(devices[i]), argv[2])) {
		ctx = ibv_open_device(devices[i]); break;
	}
	ibv_free_device_list(devices); require(ctx != NULL, "open selected native device");
	verb(ibv_query_device(ctx, &dev), "query device"); verb(ibv_query_port(ctx, 1, &port), "query port");
	require(dev.phys_port_cnt == 1 && dev.max_qp_wr >= 4 && dev.max_cqe >= 8 && dev.max_sge >= 1 &&
		dev.atomic_cap == IBV_ATOMIC_NONE && port.state == IBV_PORT_ACTIVE &&
		port.link_layer == IBV_LINK_LAYER_ETHERNET && port.active_mtu == IBV_MTU_4096 && port.gid_tbl_len == 1,
		"native limits / bilateral port readiness");
	verb(ibv_query_gid(ctx, 1, 0, &local.gid), "GID0");
	{
		struct ibv_gid_entry gid = {};
		verb(ibv_query_gid_ex(ctx, 1, 0, &gid, 0), "GID classification");
		require(gid.gid_type == IBV_GID_TYPE_IB, "native GID must not claim RoCE");
	}
	pd = ibv_alloc_pd(ctx); require(pd != NULL, "allocate PD");
	cq = ibv_create_cq(ctx, 8, NULL, NULL, 0); require(cq != NULL && cq->cqe >= 8, "create shared CQ8");
	init.send_cq = init.recv_cq = cq;
	qp = ibv_create_qp(pd, &init); require(qp != NULL, "create RC QP4/4");
	if (gpu) {
		require(send_iters || rdma_iters, "--gpu needs send, write or read with an iteration count");
		gpu_load();
	}
	buffer_alloc(&tx, &htx);
	buffer_alloc(&rx, &hrx);
	tx_mr = buffer_register(pd, tx, IBV_ACCESS_LOCAL_WRITE);
	rx_mr = buffer_register(pd, rx, IBV_ACCESS_LOCAL_WRITE);
	require(tx_mr && rx_mr, "two LOCAL_WRITE-only 8MiB MRs / memlock");
	if (rdma_iters) {
		/* The RDMA target is the only remotely accessible buffer. It exists
		 * solely for the WRITE/READ modes so the SEND modes keep proving that
		 * remote access is not granted by default. */
		buffer_alloc(&rd, &hrd);
		require(!((uintptr_t)rd & (RDMA_ALIGN - 1)), "RDMA target alignment");
		rd_mr = buffer_register(pd, rd, IBV_ACCESS_LOCAL_WRITE |
			IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ);
		require(rd_mr != NULL, "remotely accessible 8MiB RDMA MR / memlock");
		local.rkey = rd_mr->rkey; local.raddr = (uintptr_t)rd;
	}
	require(getrandom(&local.nonce, sizeof(local.nonce), 0) == sizeof(local.nonce) && local.nonce, "nonce");
	require(getrandom(&local.psn, sizeof(local.psn), 0) == sizeof(local.psn), "PSN");
	local.psn &= 0xffffff; local.qpn = qp->qp_num;
	attr.qp_state = IBV_QPS_INIT; attr.port_num = 1; attr.pkey_index = 0;
	/* Remote access is granted only where a mode exercises it. */
	attr.qp_access_flags = rdma_iters ?
		(IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ) : 0;
	verb(ibv_modify_qp(qp, &attr, IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS), "INIT");
	fd = control_socket(listening, argv[4], argv[5]);
	metadata_encode(mine, &local);
	control_io(fd, mine, sizeof(mine), true, milliseconds()+TIMEOUT_MS);
	control_io(fd, theirs, sizeof(theirs), false, milliseconds()+TIMEOUT_MS);
	require(metadata_decode(&peer, theirs) && peer.nonce != local.nonce, "peer metadata");
	if (rdma_iters)
		require(peer.rkey && peer.raddr && !(peer.raddr & (RDMA_ALIGN - 1)) &&
			rd_mr != NULL, "peer RDMA target metadata");
	memset(&attr, 0, sizeof(attr)); attr.qp_state = IBV_QPS_RTR; attr.path_mtu = IBV_MTU_4096;
	attr.dest_qp_num = peer.qpn; attr.rq_psn = peer.psn; attr.max_dest_rd_atomic = 1; attr.min_rnr_timer = 12;
	attr.ah_attr.is_global = 1; attr.ah_attr.port_num = 1; attr.ah_attr.grh.dgid = peer.gid;
	attr.ah_attr.grh.sgid_index = 0; attr.ah_attr.grh.hop_limit = 64;
	verb(ibv_modify_qp(qp, &attr, IBV_QP_STATE|IBV_QP_AV|IBV_QP_PATH_MTU|IBV_QP_DEST_QPN|IBV_QP_RQ_PSN|IBV_QP_MAX_DEST_RD_ATOMIC|IBV_QP_MIN_RNR_TIMER), "RTR/read responder1");
	memset(&attr, 0, sizeof(attr)); attr.qp_state = IBV_QPS_RTS; attr.sq_psn = local.psn;
	attr.timeout = 14; attr.retry_cnt = 3; attr.rnr_retry = 3; attr.max_rd_atomic = 1;
	verb(ibv_modify_qp(qp, &attr, IBV_QP_STATE|IBV_QP_SQ_PSN|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_MAX_QP_RD_ATOMIC), "RTS/read requester1");
	verb(ibv_query_qp(qp, &attr, IBV_QP_STATE|IBV_QP_SQ_PSN|IBV_QP_RQ_PSN|IBV_QP_TIMEOUT|IBV_QP_RETRY_CNT|IBV_QP_RNR_RETRY|IBV_QP_MAX_QP_RD_ATOMIC|IBV_QP_MAX_DEST_RD_ATOMIC, &init), "query accepted QP attrs");
	require(attr.qp_state == IBV_QPS_RTS && attr.sq_psn == local.psn && attr.rq_psn == peer.psn &&
		attr.timeout == 14 && attr.retry_cnt == 3 && attr.rnr_retry == 3 && attr.max_rd_atomic == 1 && attr.max_dest_rd_atomic == 1, "preserved QP attributes");
	if (ping_iters) { /* Small-message (256 B, staged path) ping-pong RTT. */
		struct ibv_sge rs = { .addr = (uintptr_t)rx, .length = 256, .lkey = rx_mr->lkey };
		struct ibv_sge ss = { .addr = (uintptr_t)tx, .length = 256, .lkey = tx_mr->lkey };
		struct ibv_recv_wr rw = { .wr_id = 1, .sg_list = &rs, .num_sge = 1 }, *rbad = NULL;
		struct ibv_send_wr sw = { .wr_id = 2, .sg_list = &ss, .num_sge = 1,
			.opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED }, *sbad = NULL;
		int64_t best = INT64_MAX, worst = 0, total = 0;
		barrier(fd, 1); /* align start */
		if (listening)
			verb(ibv_post_recv(qp, &rw, &rbad), "ping prime receive");
		for (unsigned it = 0; it < ping_iters; it++) {
			if (listening) {
				unsigned done = 0; int64_t deadline = milliseconds() + TIMEOUT_MS;
				while (!done) {
					struct ibv_wc wc; int n = ibv_poll_cq(cq, 1, &wc);
					require(n >= 0 && milliseconds() < deadline, "ping poll/deadline");
					if (n && wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_RECV) done = 1;
					require(n == 0 || wc.status == IBV_WC_SUCCESS, "ping WC status");
				}
				/* Re-arm BEFORE echoing. The peer only sends its next ping
				 * after receiving this echo, so arming first guarantees a
				 * receive is posted when it arrives. Arming after the echo's
				 * send completion instead lost the race on ~1992/2000
				 * iterations and measured RNR-retry latency, not the path. */
				verb(ibv_post_recv(qp, &rw, &rbad), "ping re-arm receive");
				verb(ibv_post_send(qp, &sw, &sbad), "ping post echo");
				done = 0; deadline = milliseconds() + TIMEOUT_MS;
				while (!done) {
					struct ibv_wc wc; int n = ibv_poll_cq(cq, 1, &wc);
					require(n >= 0 && milliseconds() < deadline, "echo poll/deadline");
					if (n && wc.status == IBV_WC_SUCCESS && wc.opcode == IBV_WC_SEND) done = 1;
					require(n == 0 || wc.status == IBV_WC_SUCCESS, "echo WC status");
				}
			} else {
				verb(ibv_post_recv(qp, &rw, &rbad), "ping post receive");
				int64_t start = microseconds();
				verb(ibv_post_send(qp, &sw, &sbad), "ping post send");
				bool got_echo = false, sent = false;
				int64_t deadline = milliseconds() + TIMEOUT_MS;
				while (!(got_echo && sent)) {
					struct ibv_wc wc[2]; int n = ibv_poll_cq(cq, 2, wc);
					require(n >= 0 && milliseconds() < deadline, "ping poll/deadline");
					for (int i = 0; i < n; i++) {
						require(wc[i].status == IBV_WC_SUCCESS, "ping WC status");
						if (wc[i].opcode == IBV_WC_RECV) got_echo = true; else sent = true;
					}
				}
				int64_t rtt = microseconds() - start;
				total += rtt; if (rtt < best) best = rtt; if (rtt > worst) worst = rtt;
			}
		}
		barrier(fd, 2); /* both finished */
		if (!listening)
			printf("PINGPONG %u iters x 256 B: avg %.1f us, min %lld us, max %lld us (round-trip)\n",
			       ping_iters, (double)total / ping_iters, (long long)best, (long long)worst);
		goto done;
	}
	if (send_iters) { /* Verified bidirectional SEND/RECV, window 4 x 2 MiB. */
		/* Unlike the bench mode, every iteration's payload is distinct and
		 * checked byte-for-byte, so a frame replayed from an earlier iteration
		 * into a later receive is caught, not counted as throughput. */
		struct ibv_sge rs[WINDOW], ss[WINDOW];
		struct ibv_recv_wr rw[WINDOW] = {}, *rbad = NULL;
		struct ibv_send_wr sw[WINDOW] = {}, *sbad = NULL;
		struct counters pre = stats(argv[2]);
		unsigned long long verified = 0;
		int64_t t0 = milliseconds();
		for (unsigned it = 1; it <= send_iters; it++) {
			unsigned sends = 0, recvs = 0, next_recv = 0;
			int64_t deadline;
			for (unsigned i = 0; i < WINDOW; i++) {
				if (!gpu_kernels)
					for (unsigned j = 0; j < xfer_len; j++) htx[i*CHUNK+xfer_off+j] = pattern(local.nonce, it, i, j);
				if (!gpu) memset(rx+i*CHUNK, 0, CHUNK);
				rs[i] = (struct ibv_sge){ .addr = (uintptr_t)(rx+i*CHUNK+recv_off), .length = xfer_len, .lkey = rx_mr->lkey };
				rw[i] = (struct ibv_recv_wr){ .wr_id = it*16u+i, .sg_list = &rs[i], .num_sge = 1,
					.next = i+1 < WINDOW ? &rw[i+1] : NULL };
				ss[i] = (struct ibv_sge){ .addr = (uintptr_t)(tx+i*CHUNK+xfer_off), .length = xfer_len, .lkey = tx_mr->lkey };
				sw[i] = (struct ibv_send_wr){ .wr_id = it*16u+8+i, .sg_list = &ss[i], .num_sge = 1,
					.opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
					.next = i+1 < WINDOW ? &sw[i+1] : NULL };
			}
			buffers_publish(tx, htx, rx, local.nonce, it);
			verb(ibv_post_recv(qp, rw, &rbad), "post SEND window receives");
			atomic_thread_fence(memory_order_release);
			/* Both sides' receives are posted before either side sends. */
			barrier(fd, it);
			int64_t x0 = microseconds();
			verb(ibv_post_send(qp, sw, &sbad), "post SEND window");
			deadline = milliseconds() + TIMEOUT_MS;
			while (sends != WINDOW || recvs != WINDOW) {
				struct ibv_wc wc[8]; int n = ibv_poll_cq(cq, 8, wc);
				require(n >= 0 && milliseconds() < deadline, "SEND poll/deadline");
				for (int k = 0; k < n; k++) {
					unsigned slot = wc[k].wr_id % 16;
					if (wc[k].status != IBV_WC_SUCCESS || wc[k].wr_id/16 != it ||
					    wc[k].qp_num != qp->qp_num) {
						fprintf(stderr, "FAIL detail: iter=%u status=%d (%s) opcode=%d "
							"wr_id=%llu (iter=%llu slot=%u) qp_num=%u want=%u\n",
							it, wc[k].status, ibv_wc_status_str(wc[k].status), wc[k].opcode,
							(unsigned long long)wc[k].wr_id,
							(unsigned long long)(wc[k].wr_id/16), slot,
							wc[k].qp_num, qp->qp_num);
						fail("SEND WC status/identity");
					}
					if (wc[k].opcode == IBV_WC_RECV) {
						require(slot == next_recv++ && wc[k].byte_len == xfer_len, "ordered full-length RECV");
						recvs++;
					} else {
						require(wc[k].opcode == IBV_WC_SEND && slot >= 8 && slot < 8+WINDOW, "SEND WC opcode/slot");
						sends++;
					}
				}
			}
			xfer_us += microseconds() - x0;
			atomic_thread_fence(memory_order_acquire);
			if (gpu_kernels) gpu_kernel_verify(rx, peer.nonce, it);
			else buffer_collect(hrx, rx);
			if (!gpu_kernels) {
				unsigned bad_slots = 0;

				for (unsigned i = 0; i < WINDOW; i++) {
					if (!slot_verified(hrx, peer.nonce, local.nonce, it, i, recv_off))
						bad_slots++;
				}
				if (bad_slots)
					fail("SEND payload verification");
			}
			verified += (unsigned long long)WINDOW * xfer_len;
		}
		int64_t elapsed = milliseconds() - t0;
		gpu_kernel_summary(send_iters, argv[3]);
		struct counters post = stats(argv[2]);
		require(post.bad == pre.bad && post.send - pre.send == verified &&
			post.write == pre.write && post.read == pre.read,
			"native counters prove SEND placements only");
		barrier(fd, 2); /* both finished */
		printf("PASS native SEND/RECV verified: %u iters x %u x %u B, wall %lld ms, %.2f MiB/s, %llu bytes placed and verified byte-for-byte (%s)\n",
		       send_iters, WINDOW, xfer_len, (long long)elapsed,
		       (double)verified / (1048576.0 * elapsed / 1000.0), verified, argv[3]);
		printf("%s buffers: transfer-only %.2f MiB/s per direction (post to both directions complete; pattern generation, staging and verification excluded) (%s)\n",
		       gpu ? "GPU" : "CPU", (double)verified / (1048576.0 * (double)xfer_us / 1e6), argv[3]);
		if (linger_ms) {
			/* Idle with the connection up: let a responder whose last
			 * READ_ACK was lost probe for it, and this side answer. */
			struct timespec ts = { .tv_sec = linger_ms / 1000,
					       .tv_nsec = (long)(linger_ms % 1000) * 1000000 };

			printf("linger: idle %u ms with the connection up (%s)\n", linger_ms, argv[3]);
			fflush(stdout);
			require(!nanosleep(&ts, NULL), "linger");
		}
		goto done;
	}
	if (rdma_iters) { /* Verified bidirectional RDMA WRITE or READ, window 4 x 2 MiB. */
		struct ibv_sge ss[WINDOW];
		struct ibv_send_wr sw[WINDOW] = {}, *sbad = NULL;
		/* The local SGE is always the LOCAL_WRITE-only tx pool: a WRITE reads it
		 * and a READ lands in it. The rd pool is the only remotely accessible
		 * buffer, so it is the peer's WRITE target and the peer's READ source.
		 * `filled` is what this side publishes for the peer to transfer;
		 * `landed` is the buffer this side verifies afterwards. */
		unsigned char *filled = rdma_read ? rd : tx;
		unsigned char *landed = rdma_read ? tx : rd;
		unsigned char *hfilled = rdma_read ? hrd : htx;
		unsigned char *hlanded = rdma_read ? htx : hrd;
		struct counters pre = stats(argv[2]);
		unsigned long long verified = 0;
		int64_t t0 = milliseconds();
		for (unsigned it = 1; it <= rdma_iters; it++) {
			unsigned done = 0, completed = 0, next = 0;
			int64_t deadline;
			for (unsigned i = 0; i < WINDOW; i++) {
				if (!gpu_kernels)
					for (unsigned j = 0; j < xfer_len; j++) hfilled[i*CHUNK+xfer_off+j] = pattern(local.nonce, it, i, j);
				if (!gpu) memset(landed+i*CHUNK, 0, CHUNK);
			}
			buffers_publish(filled, hfilled, landed, local.nonce, it);
			atomic_thread_fence(memory_order_release);
			/* Both sides must have published their source and cleared their
			 * landing buffer before either operation can reach the peer. */
			barrier(fd, it);
			for (unsigned i = 0; i < WINDOW; i++) {
				ss[i] = (struct ibv_sge){ .addr = (uintptr_t)(tx+i*CHUNK+xfer_off), .length = xfer_len, .lkey = tx_mr->lkey };
				sw[i] = (struct ibv_send_wr){ .wr_id = it*16u+i, .sg_list = &ss[i], .num_sge = 1,
					.opcode = rdma_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE,
					.send_flags = IBV_SEND_SIGNALED,
					.next = i+1 < WINDOW ? &sw[i+1] : NULL,
					.wr = { .rdma = { .remote_addr = peer.raddr + i*CHUNK + xfer_off, .rkey = peer.rkey } } };
			}
			int64_t x0 = microseconds();
			verb(ibv_post_send(qp, sw, &sbad), rdma_read ? "post RDMA READ window" : "post RDMA WRITE window");
			deadline = milliseconds() + TIMEOUT_MS;
			while (done != WINDOW) {
				struct ibv_wc wc[8]; int n = ibv_poll_cq(cq, 8, wc);
				require(n >= 0 && milliseconds() < deadline, "RDMA poll/deadline");
				for (int k = 0; k < n; k++) {
					unsigned slot = wc[k].wr_id % 16;
					if (wc[k].status != IBV_WC_SUCCESS || wc[k].wr_id/16 != it ||
					    wc[k].qp_num != qp->qp_num) {
						/* A bare "WC status/identity" failure hides the one number that
						 * distinguishes a remote access refusal from retry-exceeded from a
						 * wr_id bookkeeping bug, so print the completion before failing. */
						fprintf(stderr, "FAIL detail: iter=%u status=%d (%s) opcode=%d "
							"wr_id=%llu (iter=%llu slot=%u) qp_num=%u want=%u\n",
						it, wc[k].status, ibv_wc_status_str(wc[k].status), wc[k].opcode,
						(unsigned long long)wc[k].wr_id,
						(unsigned long long)(wc[k].wr_id/16), slot,
						wc[k].qp_num, qp->qp_num);
						fail("RDMA WC status/identity");
					}
					require(wc[k].opcode == (rdma_read ? IBV_WC_RDMA_READ : IBV_WC_RDMA_WRITE), "RDMA WC opcode");
					require(slot < WINDOW && !(completed & (1u<<slot)) && slot == next++, "ordered unique RDMA completion");
					completed |= 1u<<slot; done++;
				}
			}
			/* A READ completion orders only this side's landing buffer; a remote
			 * WRITE into it is ordered by this barrier instead. Timing to the
			 * barrier covers both directions, so both sides measure the same. */
			barrier(fd, 1000 + it);
			xfer_us += microseconds() - x0;
			atomic_thread_fence(memory_order_acquire);
			if (gpu_kernels) gpu_kernel_verify(landed, peer.nonce, it);
			else buffer_collect(hlanded, landed);
			if (!gpu_kernels) {
				unsigned bad_slots = 0;

				for (unsigned i = 0; i < WINDOW; i++) {
					if (!slot_verified(hlanded, peer.nonce, local.nonce, it, i, xfer_off))
						bad_slots++;
				}
				if (bad_slots)
					fail("RDMA payload verification");
			}
			verified += (unsigned long long)WINDOW * xfer_len;
		}
		int64_t elapsed = milliseconds() - t0;
		gpu_kernel_summary(rdma_iters, argv[3]);
		struct counters post = stats(argv[2]);
		require(post.bad == pre.bad && post.tx > pre.tx && post.rx > pre.rx,
			"no rejected frames during RDMA verification");
		if (rdma_read)
			require(post.read - pre.read == verified && post.write == pre.write &&
				post.send == pre.send, "native counters prove READ placements only");
		else
			require(post.write - pre.write == verified && post.read == pre.read &&
				post.send == pre.send, "native counters prove WRITE placements only");
		printf("PASS native RDMA %s: %u iters x %u x %u B, wall %lld ms, %.2f MiB/s, %llu bytes placed and verified byte-for-byte (%s)\n",
		       rdma_read ? "READ" : "WRITE", rdma_iters, WINDOW, xfer_len, (long long)elapsed,
		       (double)verified / (1048576.0 * elapsed / 1000.0), verified, argv[3]);
		printf("%s buffers: transfer-only %.2f MiB/s per direction (post to both directions complete; pattern generation, staging and verification excluded) (%s)\n",
		       gpu ? "GPU" : "CPU", (double)verified / (1048576.0 * (double)xfer_us / 1e6), argv[3]);
		if (linger_ms) {
			/* Idle with the connection up: let a responder whose last
			 * READ_ACK was lost probe for it, and this side answer. */
			struct timespec ts = { .tv_sec = linger_ms / 1000,
					       .tv_nsec = (long)(linger_ms % 1000) * 1000000 };

			printf("linger: idle %u ms with the connection up (%s)\n", linger_ms, argv[3]);
			fflush(stdout);
			require(!nanosleep(&ts, NULL), "linger");
		}
		goto done;
	}
	if (bench_iters) { /* Timed window-4 x 2MiB bidirectional loop; no verification. */
		struct ibv_sge rs[WINDOW], ss[WINDOW];
		struct ibv_recv_wr rw[WINDOW] = {}, *rbad = NULL;
		struct ibv_send_wr sw[WINDOW] = {}, *sbad = NULL;
		for (unsigned i = 0; i < WINDOW; i++) {
			for (unsigned j = 0; j < 4096; j++) tx[i*CHUNK+j] = pattern(local.nonce, 1, i, j);
			rs[i] = (struct ibv_sge){ .addr = (uintptr_t)(rx+i*CHUNK), .length = CHUNK, .lkey = rx_mr->lkey };
			ss[i] = (struct ibv_sge){ .addr = (uintptr_t)(tx+i*CHUNK), .length = CHUNK, .lkey = tx_mr->lkey };
			rw[i] = (struct ibv_recv_wr){ .wr_id = i, .sg_list = &rs[i], .num_sge = 1,
				.next = i+1 < WINDOW ? &rw[i+1] : NULL };
			sw[i] = (struct ibv_send_wr){ .wr_id = 8+i, .sg_list = &ss[i], .num_sge = 1,
				.opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
				.next = i+1 < WINDOW ? &sw[i+1] : NULL };
		}
		barrier(fd, 1); /* align start */
		int64_t t0 = milliseconds();
		for (unsigned it = 0; it < bench_iters; it++) {
			verb(ibv_post_recv(qp, rw, &rbad), "bench post receive");
			verb(ibv_post_send(qp, sw, &sbad), "bench post send");
			unsigned done = 0; int64_t deadline = milliseconds() + TIMEOUT_MS;
			while (done != 2*WINDOW) {
				struct ibv_wc wc[8]; int n = ibv_poll_cq(cq, 8, wc);
				require(n >= 0 && milliseconds() < deadline, "bench poll/deadline");
				for (int i = 0; i < n; i++)
					require(wc[i].status == IBV_WC_SUCCESS, "bench WC status"), done++;
			}
		}
		int64_t elapsed = milliseconds() - t0;
		barrier(fd, 2); /* both finished */
		double bytes = (double)bench_iters * WINDOW * CHUNK; /* per direction */
		printf("BENCH %s: %u iters x %u x %u B, wall %lld ms, %.2f MiB/s, %.3f Gbps (per direction, bidirectional)\n",
		       argv[3], bench_iters, WINDOW, CHUNK, (long long)elapsed,
		       bytes / (1048576.0 * elapsed / 1000.0), bytes * 8.0 / (1e9 * elapsed / 1000.0));
		goto done;
	}
	/* Rejected WRs must not consume a queue slot or create a CQE. */
	{
		struct ibv_sge sg = { .addr = (uintptr_t)tx, .length = CHUNK+1, .lkey = tx_mr->lkey };
		struct ibv_send_wr w = { .wr_id = 999, .sg_list = &sg, .num_sge = 1,
			.opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED }, *bad = NULL;
		struct ibv_wc wc;
		require(ibv_post_send(qp, &w, &bad) == EINVAL && bad == &w, "oversized SEND rejection");
		sg.length = 1; sg.lkey = 0; bad = NULL;
		require(ibv_post_send(qp, &w, &bad) == EACCES && bad == &w, "invalid lkey rejection");
		sg.lkey = tx_mr->lkey; w.num_sge = 2; bad = NULL;
		require(ibv_post_send(qp, &w, &bad) == EINVAL && bad == &w, "multiple SGE rejection");
		w.num_sge = 1; w.send_flags |= IBV_SEND_INLINE; bad = NULL;
		require(ibv_post_send(qp, &w, &bad) == EOPNOTSUPP && bad == &w, "inline rejection");
		require(!ibv_poll_cq(cq, 1, &wc), "rejected WRs have no completion");
	}
	before = stats(argv[2]);
	for (unsigned window = 1; window <= 3; window++) {
		unsigned lengths[WINDOW] = { 0, 1, 3984, CHUNK };
		struct ibv_sge rs[WINDOW], ss[WINDOW];
		struct ibv_recv_wr rw[WINDOW] = {}, *rbad = NULL;
		struct ibv_send_wr sw[WINDOW] = {}, *sbad = NULL;
		unsigned sent = 0, received = 0, next_send = 0, next_recv = 0;
		int64_t deadline;
		if (window > 1) for (unsigned i = 0; i < WINDOW; i++) lengths[i] = CHUNK;
		for (unsigned i = 0; i < WINDOW; i++) {
			for (unsigned j = 0; j < lengths[i]; j++) tx[i*CHUNK+j] = pattern(local.nonce, window, i, j);
			memset(rx+i*CHUNK, 0, lengths[i]); expected += lengths[i];
			rs[i] = (struct ibv_sge){ .addr = (uintptr_t)(rx+i*CHUNK), .length = lengths[i], .lkey = rx_mr->lkey };
			ss[i] = (struct ibv_sge){ .addr = (uintptr_t)(tx+i*CHUNK), .length = lengths[i], .lkey = tx_mr->lkey };
			rw[i] = (struct ibv_recv_wr){ .wr_id = window*16+i, .sg_list = &rs[i], .num_sge = 1,
				.next = i+1 < WINDOW ? &rw[i+1] : NULL };
			sw[i] = (struct ibv_send_wr){ .wr_id = window*16+8+i, .sg_list = &ss[i], .num_sge = 1,
				.opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
				.next = i+1 < WINDOW ? &sw[i+1] : NULL };
		}
		verb(ibv_post_recv(qp, rw, &rbad), "post receive window");
		/* This must fail without unpinning or invalidating the live MR. */
		require(ibv_dereg_mr(rx_mr) == EBUSY, "deregister posted receive MR must return EBUSY");
		barrier(fd, window);
		verb(ibv_post_send(qp, sw, &sbad), "post signaled SEND window");
		deadline = milliseconds() + TIMEOUT_MS;
		while (sent != 15 || received != 15) {
			struct ibv_wc wc[8]; int n = ibv_poll_cq(cq, 8, wc);
			require(n >= 0 && milliseconds() < deadline, "CQ poll/deadline");
			for (int i = 0; i < n; i++) {
				unsigned slot = wc[i].wr_id % 16;
				require(wc[i].status == IBV_WC_SUCCESS && wc[i].wr_id/16 == window && wc[i].qp_num == qp->qp_num, "WC status/identity");
				if (slot < WINDOW && wc[i].opcode == IBV_WC_RECV) {
					require(!(received & (1u<<slot)) && slot == next_recv++ && wc[i].byte_len == lengths[slot], "ordered unique receive length");
					received |= 1u<<slot;
				} else {
					require(slot >= 8 && slot < 12 && wc[i].opcode == IBV_WC_SEND, "SEND WC"); slot -= 8;
					require(!(sent & (1u<<slot)) && slot == next_send++, "ordered unique SEND"); sent |= 1u<<slot;
				}
			}
		}
		atomic_thread_fence(memory_order_acquire);
		for (unsigned i = 0; i < WINDOW; i++) for (unsigned j = 0; j < lengths[i]; j++)
			require(rx[i*CHUNK+j] == pattern(peer.nonce, window, i, j), "full payload verification");
		barrier(fd, window+100);
	}
	after = stats(argv[2]);
	require(after.tx > before.tx && after.rx > before.rx && after.bad == before.bad &&
		after.send - before.send == expected && after.write == before.write && after.read == before.read, "native counters prove SEND placements, no payload fallback");
	/* Explicit ERR must flush posted receives in order and release their MRs. */
	for (unsigned i = 0; i < WINDOW; i++) {
		struct ibv_sge s = { .addr = (uintptr_t)rx, .length = 1, .lkey = rx_mr->lkey };
		struct ibv_recv_wr w = { .wr_id = 1000+i, .sg_list = &s, .num_sge = 1 }, *bad;
		verb(ibv_post_recv(qp, &w, &bad), "post flush WQE");
	}
	memset(&attr, 0, sizeof(attr)); attr.qp_state = IBV_QPS_ERR;
	verb(ibv_modify_qp(qp, &attr, IBV_QP_STATE), "ERR");
	{
		struct ibv_wc wc[8]; int n = ibv_poll_cq(cq, 8, wc);
		require(n == 4, "four synchronous flush completions");
		for (int i = 0; i < n; i++) require(wc[i].status == IBV_WC_WR_FLUSH_ERR && wc[i].wr_id == 1000u+(unsigned)i, "ordered error flush");
	}
	done:
	verb(ibv_destroy_qp(qp), "synchronous QP destroy");
	verb(ibv_dereg_mr(tx_mr), "TX deregistration"); verb(ibv_dereg_mr(rx_mr), "RX deregistration");
	if (rd_mr) verb(ibv_dereg_mr(rd_mr), "RDMA deregistration");
	buffer_free(tx, htx); buffer_free(rx, hrx); if (rd) buffer_free(rd, hrd);
	verb(ibv_destroy_cq(cq), "CQ destroy"); verb(ibv_dealloc_pd(pd), "PD free");
	verb(ibv_close_device(ctx), "context close"); close(fd);
	if (!bench_iters && !ping_iters && !rdma_iters && !send_iters)
		printf("PASS native CPU SEND/RECV: %llu bytes received, window4 x 2MiB setup, ERR flush; not complete RC qualification\n", expected);
	return 0;
}
