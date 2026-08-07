// SPDX-License-Identifier: MIT
/*
 * pingpong - latency/bandwidth test for USB4STREAM /dev/tbstreamX devices.
 *
 * The stream device is a byte stream (pipe semantics): short reads and
 * writes are allowed, read() returns 0 on peer close, framing is up to
 * the application. Both sides must agree on message size via -s.
 *
 * Modes:
 *   ping   measuring side: write msg, read echo, record round-trip time
 *   pong   echo side: read msg, write it back, forever (until EOF)
 *   tx     bandwidth send: write -n messages, then close
 *   rx     bandwidth drain: read until EOF, report MB/s
 *   zping  like ping but over the zero-copy slot interface (Linux only)
 *   zpong  like pong but over the zero-copy slot interface (Linux only)
 *   ztx    zero-copy bandwidth send (Linux only)
 *   zrx    zero-copy bandwidth drain (Linux only)
 *
 * Works on any pipe-like fd, so two FIFOs or a socketpair wrapper can
 * smoke-test it without Thunderbolt hardware (rw modes only).
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "thunderbolt-stream.h"
#endif

static const char *usage_str =
    "usage: pingpong <ping|pong|tx|rx|zping|zpong|ztx|zrx> [options]\n"
    "  -d DEV    device path (default /dev/tbstream0)\n"
    "  -s SIZE   message size, k/m suffixes ok (default 64k)\n"
    "  -n N      iterations (ping/tx/zping/ztx; default 1000)\n"
    "  -w N      warmup iterations, excluded from stats (default 50)\n"
    "  -V        verify echoed/incoming payload pattern\n"
    "  -c        CSV output: mode,size,iters,min,p50,p90,p95,p99,max,mean_us,mbps\n";

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

static int full_write(int fd, const unsigned char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = write(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("write");
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

/* Returns 0 on success, 1 on EOF before any byte, -1 on error/short EOF. */
static int full_read(int fd, unsigned char *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		ssize_t n = read(fd, buf + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("read");
			return -1;
		}
		if (n == 0) {
			if (off == 0)
				return 1;
			fprintf(stderr, "EOF mid-message (%zu/%zu bytes)\n",
				off, len);
			return -1;
		}
		off += (size_t)n;
	}
	return 0;
}

static void fill_pattern(unsigned char *buf, size_t len, uint64_t seq)
{
	memcpy(buf, &seq, sizeof(seq) < len ? sizeof(seq) : len);
	for (size_t i = sizeof(seq); i < len; i++)
		buf[i] = (unsigned char)(seq + i);
}

static int check_pattern(const unsigned char *buf, size_t len, uint64_t seq)
{
	unsigned char prefix[sizeof(seq)];
	size_t prefix_len = sizeof(seq) < len ? sizeof(seq) : len;

	memcpy(prefix, &seq, sizeof(prefix));
	for (size_t i = 0; i < prefix_len; i++) {
		if (buf[i] != prefix[i]) {
			fprintf(stderr,
				"verify: corrupt sequence byte at %zu (seq %" PRIu64 ")\n",
				i, seq);
			return -1;
		}
	}
	for (size_t i = sizeof(seq); i < len; i++) {
		if (buf[i] != (unsigned char)(seq + i)) {
			fprintf(stderr,
				"verify: corrupt byte at %zu (seq %" PRIu64 ")\n",
				i, seq);
			return -1;
		}
	}
	return 0;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static double pct(const double *sorted, size_t n, double p)
{
	size_t idx = (size_t)(p / 100.0 * (double)(n - 1) + 0.5);
	return sorted[idx];
}

static size_t parse_size(const char *s)
{
	char *end;

	errno = 0;
	double v = strtod(s, &end);
	if (end == s || errno == ERANGE) {
		fprintf(stderr, "bad size: %s\n", s);
		exit(2);
	}
	if ((*end == 'k' || *end == 'K') && end[1] == '\0')
		v *= 1024;
	else if ((*end == 'm' || *end == 'M') && end[1] == '\0')
		v *= 1024 * 1024;
	else if (*end != '\0') {
		fprintf(stderr, "bad size: %s\n", s);
		exit(2);
	}
	if (!isfinite(v) || v < 1 || v > (double)SIZE_MAX) {
		fprintf(stderr, "bad size: %s\n", s);
		exit(2);
	}
	return (size_t)v;
}

static long parse_count(const char *s, const char *name, long min)
{
	char *end;
	long value;

	errno = 0;
	value = strtol(s, &end, 10);
	if (end == s || *end || errno == ERANGE || value < min) {
		fprintf(stderr, "bad %s: %s\n", name, s);
		exit(2);
	}
	return value;
}

#ifdef __linux__

/* Zero-copy slot interface state */
struct zc {
	int fd;
	struct tbstream_zc_info info;
	unsigned char *tx;	/* TX frame pool */
	unsigned char *rx;	/* RX frame pool */
	uint32_t tx_next;	/* next TX pool index we will fill */
	uint32_t tx_inflight;	/* frames submitted, TX_DONE not yet seen */
};

static int zc_open(struct zc *z, const char *dev)
{
	unsigned char *base;
	size_t len;

	z->fd = open(dev, O_RDWR);
	if (z->fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return -1;
	}
	if (ioctl(z->fd, TBSTREAM_ZC_ENABLE)) {
		perror("TBSTREAM_ZC_ENABLE");
		return -1;
	}
	if (ioctl(z->fd, TBSTREAM_ZC_GET_INFO, &z->info)) {
		perror("TBSTREAM_ZC_GET_INFO");
		return -1;
	}
	len = 2ull * z->info.ring_size * z->info.frame_size;
	base = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, z->fd, 0);
	if (base == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	z->tx = base + z->info.tx_pool_offset;
	z->rx = base + z->info.rx_pool_offset;
	z->tx_next = 0;
	z->tx_inflight = 0;
	fprintf(stderr, "zc: ring_size %u frame_size %u\n",
		z->info.ring_size, z->info.frame_size);
	return 0;
}

/*
 * Reap events, retiring TX_DONE bookkeeping. If rx_ev is non-NULL,
 * block until an EV_RX arrives and store it there. Returns 0 on
 * success, 1 on peer close, -1 on error.
 */
static int zc_reap(struct zc *z, struct tbstream_zc_event *rx_ev)
{
	struct tbstream_zc_event ev;
	struct tbstream_zc_reap reap = {
		.max = 1,
		.events = (uint64_t)(uintptr_t)&ev,
	};

	for (;;) {
		int n = ioctl(z->fd, TBSTREAM_ZC_REAP, &reap);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("TBSTREAM_ZC_REAP");
			return -1;
		}
		if (n != 1) {
			fprintf(stderr, "zc: reap returned %d events, expected 1\n", n);
			return -1;
		}
		switch (ev.type) {
		case TBSTREAM_ZC_EV_TX_DONE:
			if (ev.nframes > z->tx_inflight) {
				fprintf(stderr, "zc: invalid TX completion (%u > %u)\n",
					ev.nframes, z->tx_inflight);
				return -1;
			}
			z->tx_inflight -= ev.nframes;
			break;
		case TBSTREAM_ZC_EV_RX:
			if (!rx_ev) {
				fprintf(stderr, "unexpected RX event\n");
				return -1;
			}
			*rx_ev = ev;
			return 0;
		case TBSTREAM_ZC_EV_CLOSE:
			return 1;
		default:
			fprintf(stderr, "zc: unknown event type %u\n", ev.type);
			return -1;
		}
		if (!rx_ev)
			return 0;
	}
}

static int zc_submit(struct zc *z, uint32_t nframes, uint32_t last_len)
{
	struct tbstream_zc_tx tx = { .nframes = nframes, .last_len = last_len };

	/* make sure the frames we are about to use are retired */
	while (z->info.ring_size - 1 - z->tx_inflight < nframes) {
		if (zc_reap(z, NULL))
			return -1;
	}
	if (ioctl(z->fd, TBSTREAM_ZC_SUBMIT_TX, &tx)) {
		perror("TBSTREAM_ZC_SUBMIT_TX");
		return -1;
	}
	if (tx.first != z->tx_next) {
		fprintf(stderr, "zc: tx cursor mismatch (%u != %u)\n",
			tx.first, z->tx_next);
		return -1;
	}
	z->tx_inflight += nframes;
	z->tx_next = (z->tx_next + nframes) % z->info.ring_size;
	return 0;
}

static int zc_post_rx(struct zc *z, uint32_t nframes)
{
	struct tbstream_zc_rx rx = {
		.nframes = nframes,
		.flags = TBSTREAM_ZC_RX_F_INTERRUPT_BOUNDARIES,
	};

	if (!ioctl(z->fd, TBSTREAM_ZC_POST_RX_FLAGS, &rx))
		return 0;
	if (errno != ENOTTY) {
		perror("TBSTREAM_ZC_POST_RX_FLAGS");
		return -1;
	}
	if (ioctl(z->fd, TBSTREAM_ZC_POST_RX, &nframes)) {
		perror("TBSTREAM_ZC_POST_RX");
		return -1;
	}
	return 0;
}

static int zc_run(const char *mode, const char *dev, size_t size, long iters,
		  long warmup, int verify, int csv)
{
	uint32_t fsz, k, last_len;
	double *lat = NULL;
	struct zc z;

	if (zc_open(&z, dev))
		return 1;

	fsz = z.info.frame_size;
	k = (size + fsz - 1) / fsz;
	last_len = size - (size_t)(k - 1) * fsz;
	if (!k || k >= z.info.ring_size) {
		fprintf(stderr, "size needs 1..%u frames\n", z.info.ring_size - 1);
		return 1;
	}

	if (strcmp(mode, "ztx") == 0) {
		double t0 = now_us();

		for (long it = 0; it < iters; it++) {
			for (uint32_t i = 0; i < k; i++) {
				uint32_t idx = (z.tx_next + i) % z.info.ring_size;
				unsigned char *page = z.tx + (size_t)idx * fsz;
				size_t len = i == k - 1 ? last_len : fsz;

				if (verify)
					fill_pattern(page, len, (uint64_t)it + i);
				else if (i == 0)
					memcpy(page, &it, sizeof(it) < len ? sizeof(it) : len);
			}
			if (zc_submit(&z, k, last_len))
				return 1;
		}
		while (z.tx_inflight) {
			if (zc_reap(&z, NULL))
				return 1;
		}

		double elapsed = now_us() - t0;
		double mbps = (double)size * (double)iters / elapsed;

		if (csv)
			printf("ztx,%zu,%ld,,,,,,,,%.1f\n", size, iters, mbps);
		else
			printf("ztx: %ld messages of %zu bytes, %.1f MB/s\n",
			       iters, size, mbps);
		return 0;
	}

	if (strcmp(mode, "zrx") == 0) {
		uint64_t total = 0, msgs = 0;
		double t0 = 0;

		for (;;) {
			struct tbstream_zc_event ev;
			int r = zc_reap(&z, &ev);

			if (r == 1)
				break;
			if (r < 0)
				return 1;
			if (ev.bytes != size || ev.nframes != k) {
				fprintf(stderr,
					"zrx: unexpected message geometry (%u bytes, %u frames)\n",
					ev.bytes, ev.nframes);
				return 1;
			}
			if (!msgs)
				t0 = now_us();
			if (verify) {
				for (uint32_t i = 0; i < ev.nframes; i++) {
					uint32_t idx = (ev.first + i) % z.info.ring_size;
					size_t len = i == ev.nframes - 1 ? last_len : fsz;

					if (check_pattern(z.rx + (size_t)idx * fsz,
							  len, msgs + i))
						return 1;
				}
			}
			if (zc_post_rx(&z, ev.nframes)) {
				return 1;
			}
			msgs++;
			total += ev.bytes;
		}

		double elapsed = msgs ? now_us() - t0 : 0;
		double mbps = elapsed > 0 ? (double)total / elapsed : 0;

		if (csv)
			printf("zrx,%zu,%" PRIu64 ",,,,,,,,%.1f\n",
			       size, msgs, mbps);
		else
			printf("zrx: %" PRIu64 " messages, %" PRIu64
			       " bytes, %.1f MB/s\n", msgs, total, mbps);
		return 0;
	}

	if (strcmp(mode, "zpong") == 0) {
		uint64_t msgs = 0;

		for (;;) {
			struct tbstream_zc_event ev;
			int r = zc_reap(&z, &ev);

			if (r == 1)
				break;
			if (r < 0)
				return 1;
			for (uint32_t i = 0; i < ev.nframes; i++) {
				uint32_t src = (ev.first + i) % z.info.ring_size;
				uint32_t dst = (z.tx_next + i) % z.info.ring_size;

				memcpy(z.tx + (size_t)dst * fsz,
				       z.rx + (size_t)src * fsz, fsz);
			}
			if (zc_submit(&z, ev.nframes, ev.bytes - (ev.nframes - 1) * fsz))
				return 1;
			if (zc_post_rx(&z, ev.nframes)) {
				return 1;
			}
			msgs++;
		}
		fprintf(stderr, "zpong: echoed %" PRIu64 " messages\n", msgs);
		return 0;
	}

	lat = calloc((size_t)iters, sizeof(double));
	if (!lat) {
		perror("calloc");
		return 1;
	}

	for (long it = -warmup; it < iters; it++) {
		struct tbstream_zc_event ev;
		uint64_t seq = (uint64_t)(it + warmup);
		double t0, rtt;
		int r;

		for (uint32_t i = 0; i < k; i++) {
			uint32_t idx = (z.tx_next + i) % z.info.ring_size;
			unsigned char *page = z.tx + (size_t)idx * fsz;
			size_t len = i == k - 1 ? last_len : fsz;

			if (verify)
				fill_pattern(page, len, seq + i);
			else if (i == 0)
				memcpy(page, &seq, sizeof(seq));
		}

		t0 = now_us();
		if (zc_submit(&z, k, last_len))
			return 1;
		r = zc_reap(&z, &ev);
		if (r) {
			fprintf(stderr, "peer closed mid-run\n");
			return 1;
		}
		rtt = now_us() - t0;

		if (ev.bytes != size) {
			fprintf(stderr, "short echo: %u != %zu\n", ev.bytes, size);
			return 1;
		}
		if (verify) {
			for (uint32_t i = 0; i < ev.nframes; i++) {
				uint32_t idx = (ev.first + i) % z.info.ring_size;
				size_t len = i == ev.nframes - 1 ? last_len : fsz;

				if (check_pattern(z.rx + (size_t)idx * fsz, len,
						  seq + i))
					return 1;
			}
		}
		if (zc_post_rx(&z, ev.nframes)) {
			return 1;
		}
		if (it >= 0)
			lat[it] = rtt;
	}

	double sum = 0;
	for (long i = 0; i < iters; i++)
		sum += lat[i];
	qsort(lat, (size_t)iters, sizeof(double), cmp_double);
	double mean = sum / (double)iters;
	double mbps = 2.0 * (double)size * (double)iters / sum;

	if (csv) {
		printf("zping,%zu,%ld,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
		       size, iters, lat[0], pct(lat, (size_t)iters, 50),
		       pct(lat, (size_t)iters, 90), pct(lat, (size_t)iters, 95),
		       pct(lat, (size_t)iters, 99), lat[iters - 1], mean, mbps);
	} else {
		printf("zping: %ld round trips of %zu bytes each way\n",
		       iters, size);
		printf("  rtt us: min %.1f  p50 %.1f  p90 %.1f  p95 %.1f  p99 %.1f  max %.1f  mean %.1f\n",
		       lat[0], pct(lat, (size_t)iters, 50),
		       pct(lat, (size_t)iters, 90), pct(lat, (size_t)iters, 95),
		       pct(lat, (size_t)iters, 99), lat[iters - 1], mean);
		printf("  approx one-way p50: %.1f us, throughput: %.1f MB/s\n",
		       pct(lat, (size_t)iters, 50) / 2.0, mbps);
	}
	return 0;
}

#else /* !__linux__ */

static int zc_run(const char *mode, const char *dev, size_t size, long iters,
		  long warmup, int verify, int csv)
{
	(void)mode; (void)dev; (void)size; (void)iters;
	(void)warmup; (void)verify; (void)csv;
	fprintf(stderr, "zero-copy modes are Linux-only\n");
	return 2;
}

#endif /* __linux__ */

int main(int argc, char **argv)
{
	const char *dev = "/dev/tbstream0";
	size_t size = 64 * 1024;
	long iters = 1000, warmup = 50;
	int verify = 0, csv = 0;
	const char *mode;
	int opt;

	if (argc < 2) {
		fputs(usage_str, stderr);
		return 2;
	}
	mode = argv[1];
	optind = 2;
	while ((opt = getopt(argc, argv, "d:s:n:w:Vc")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 's': size = parse_size(optarg); break;
		case 'n': iters = parse_count(optarg, "iteration count", 1); break;
		case 'w': warmup = parse_count(optarg, "warmup count", 0); break;
		case 'V': verify = 1; break;
		case 'c': csv = 1; break;
		default: fputs(usage_str, stderr); return 2;
		}
	}

	if (strcmp(mode, "zping") == 0 || strcmp(mode, "zpong") == 0 ||
	    strcmp(mode, "ztx") == 0 || strcmp(mode, "zrx") == 0)
		return zc_run(mode, dev, size, iters, warmup, verify, csv);
	if (strcmp(mode, "ping") != 0 && strcmp(mode, "pong") != 0 &&
	    strcmp(mode, "tx") != 0 && strcmp(mode, "rx") != 0) {
		fputs(usage_str, stderr);
		return 2;
	}

	int fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}

	unsigned char *buf;
	if (posix_memalign((void **)&buf, 4096, size)) {
		perror("posix_memalign");
		return 1;
	}
	memset(buf, 0xa5, size);

	if (strcmp(mode, "pong") == 0) {
		uint64_t msgs = 0;
		for (;;) {
			int r = full_read(fd, buf, size);
			if (r == 1)
				break; /* peer closed */
			if (r < 0)
				return 1;
			if (full_write(fd, buf, size))
				return 1;
			msgs++;
		}
		fprintf(stderr, "pong: echoed %" PRIu64 " messages of %zu bytes\n",
			msgs, size);
		return 0;
	}

	if (strcmp(mode, "rx") == 0) {
		uint64_t total = 0, msgs = 0;
		double t0 = 0;
		for (;;) {
			int r = full_read(fd, buf, size);
			if (r == 1)
				break;
			if (r < 0)
				return 1;
			if (!msgs)
				t0 = now_us(); /* clock starts at first message */
			if (verify && check_pattern(buf, size, msgs))
				return 1;
			msgs++;
			total += size;
		}
		double el = now_us() - t0;
		double mbps = el > 0 ? (double)total / el : 0; /* bytes/us == MB/s */
		if (csv)
			printf("rx,%zu,%" PRIu64 ",,,,,,,,%.1f\n", size, msgs, mbps);
		else
			printf("rx: %" PRIu64 " messages, %" PRIu64 " bytes, %.1f MB/s\n",
			       msgs, total, mbps);
		return 0;
	}

	if (strcmp(mode, "tx") == 0) {
		double t0 = now_us();
		for (long i = 0; i < iters; i++) {
			if (verify)
				fill_pattern(buf, size, (uint64_t)i);
			if (full_write(fd, buf, size))
				return 1;
		}
		close(fd); /* sends CLOSE, rx side sees EOF */
		double el = now_us() - t0;
		double mbps = (double)size * (double)iters / el;
		if (csv)
			printf("tx,%zu,%ld,,,,,,,,%.1f\n", size, iters, mbps);
		else
			printf("tx: %ld messages of %zu bytes, %.1f MB/s\n",
			       iters, size, mbps);
		return 0;
	}

	double *lat = calloc((size_t)iters, sizeof(double));
	if (!lat) {
		perror("calloc");
		return 1;
	}

	for (long i = -warmup; i < iters; i++) {
		uint64_t seq = (uint64_t)(i + warmup);
		if (verify)
			fill_pattern(buf, size, seq);
		double t0 = now_us();
		if (full_write(fd, buf, size))
			return 1;
		int r = full_read(fd, buf, size);
		if (r) {
			fprintf(stderr, "peer closed mid-run\n");
			return 1;
		}
		double rtt = now_us() - t0;
		if (verify && check_pattern(buf, size, seq))
			return 1;
		if (i >= 0)
			lat[i] = rtt;
	}

	double sum = 0;
	for (long i = 0; i < iters; i++)
		sum += lat[i];
	qsort(lat, (size_t)iters, sizeof(double), cmp_double);
	double mean = sum / (double)iters;
	/* 2x size on the wire per round trip */
	double mbps = 2.0 * (double)size * (double)iters / sum;

	if (csv) {
		printf("ping,%zu,%ld,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f\n",
		       size, iters, lat[0], pct(lat, (size_t)iters, 50),
		       pct(lat, (size_t)iters, 90), pct(lat, (size_t)iters, 95),
		       pct(lat, (size_t)iters, 99), lat[iters - 1], mean, mbps);
	} else {
		printf("ping: %ld round trips of %zu bytes each way\n", iters, size);
		printf("  rtt us: min %.1f  p50 %.1f  p90 %.1f  p95 %.1f  p99 %.1f  max %.1f  mean %.1f\n",
		       lat[0], pct(lat, (size_t)iters, 50),
		       pct(lat, (size_t)iters, 90), pct(lat, (size_t)iters, 95),
		       pct(lat, (size_t)iters, 99), lat[iters - 1], mean);
		printf("  approx one-way p50: %.1f us, throughput: %.1f MB/s\n",
		       pct(lat, (size_t)iters, 50) / 2.0, mbps);
	}
	return 0;
}
