// SPDX-License-Identifier: GPL-2.0-only
/* Ordinary libibverbs client. TCP carries only 64-byte metadata and barriers;
 * every tested payload byte goes through registered native SEND/RECV, RDMA
 * WRITE or RDMA READ. */
#define _POSIX_C_SOURCE 200809L
#include <infiniband/verbs.h>
#include <arpa/inet.h>
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
static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", what, errno, strerror(errno));
	exit(1); /* Kernel context close, never manual unpin of outstanding memory. */
}
static void require(bool ok, const char *what) { if (!ok) fail(what); }
static void verb(int ret, const char *what) { if (ret) { errno = ret; fail(what); } }
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
int main(int argc, char **argv)
{
	struct ibv_device **devices; struct ibv_context *ctx = NULL;
	struct ibv_device_attr dev; struct ibv_port_attr port;
	struct ibv_pd *pd; struct ibv_cq *cq; struct ibv_qp *qp;
	struct ibv_mr *tx_mr, *rx_mr, *rd_mr = NULL; unsigned char *tx, *rx, *rd = NULL;
	struct endpoint local = {}, peer; struct counters before, after;
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr init = { .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = WINDOW, .max_recv_wr = WINDOW, .max_send_sge = 1, .max_recv_sge = 1 } };
	unsigned char mine[META_SIZE], theirs[META_SIZE];
	unsigned long long expected = 0;
	unsigned bench_iters = 0, ping_iters = 0, rdma_iters = 0, send_iters = 0;
	int count, fd; bool listening, rdma_read = false;
	if (argc == 2 && !strcmp(argv[1], "--selftest")) { selftest(); return 0; }
	if ((argc < 6 || argc > 8) || strcmp(argv[1], "--live") ||
	    (strcmp(argv[3], "listen") && strcmp(argv[3], "connect"))) {
		fprintf(stderr, "usage: %s --selftest\n       %s --live strix_nhiN listen|connect explicit-IPv4 TCP-port [bench-iterations | ping [iters] | send [iters] | write [iters] | read [iters]]\n", argv[0], argv[0]);
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
	verb(posix_memalign((void **)&tx, 4096, POOL), "TX allocation");
	verb(posix_memalign((void **)&rx, 4096, POOL), "RX allocation");
	tx_mr = ibv_reg_mr(pd, tx, POOL, IBV_ACCESS_LOCAL_WRITE);
	rx_mr = ibv_reg_mr(pd, rx, POOL, IBV_ACCESS_LOCAL_WRITE);
	require(tx_mr && rx_mr, "two LOCAL_WRITE-only 8MiB MRs / memlock");
	if (rdma_iters) {
		/* The RDMA target is the only remotely accessible buffer. It exists
		 * solely for the WRITE/READ modes so the SEND modes keep proving that
		 * remote access is not granted by default. */
		verb(posix_memalign((void **)&rd, RDMA_ALIGN, POOL), "RDMA allocation");
		rd_mr = ibv_reg_mr(pd, rd, POOL, IBV_ACCESS_LOCAL_WRITE |
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
				for (unsigned j = 0; j < CHUNK; j++) tx[i*CHUNK+j] = pattern(local.nonce, it, i, j);
				memset(rx+i*CHUNK, 0, CHUNK);
				rs[i] = (struct ibv_sge){ .addr = (uintptr_t)(rx+i*CHUNK), .length = CHUNK, .lkey = rx_mr->lkey };
				rw[i] = (struct ibv_recv_wr){ .wr_id = it*16u+i, .sg_list = &rs[i], .num_sge = 1,
					.next = i+1 < WINDOW ? &rw[i+1] : NULL };
				ss[i] = (struct ibv_sge){ .addr = (uintptr_t)(tx+i*CHUNK), .length = CHUNK, .lkey = tx_mr->lkey };
				sw[i] = (struct ibv_send_wr){ .wr_id = it*16u+8+i, .sg_list = &ss[i], .num_sge = 1,
					.opcode = IBV_WR_SEND, .send_flags = IBV_SEND_SIGNALED,
					.next = i+1 < WINDOW ? &sw[i+1] : NULL };
			}
			verb(ibv_post_recv(qp, rw, &rbad), "post SEND window receives");
			atomic_thread_fence(memory_order_release);
			/* Both sides' receives are posted before either side sends. */
			barrier(fd, it);
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
						require(slot == next_recv++ && wc[k].byte_len == CHUNK, "ordered full-length RECV");
						recvs++;
					} else {
						require(wc[k].opcode == IBV_WC_SEND && slot >= 8 && slot < 8+WINDOW, "SEND WC opcode/slot");
						sends++;
					}
				}
			}
			atomic_thread_fence(memory_order_acquire);
			{
				unsigned bad_slots = 0;

				for (unsigned i = 0; i < WINDOW; i++) {
					unsigned j;

					for (j = 0; j < CHUNK; j++)
						if (rx[i*CHUNK+j] != pattern(peer.nonce, it, i, j))
							break;
					if (j < CHUNK) {
						report_mismatch(rx, peer.nonce, local.nonce, it, i, CHUNK, WINDOW);
						bad_slots++;
					}
				}
				if (bad_slots)
					fail("SEND payload verification");
			}
			verified += (unsigned long long)WINDOW * CHUNK;
		}
		int64_t elapsed = milliseconds() - t0;
		struct counters post = stats(argv[2]);
		require(post.bad == pre.bad && post.send - pre.send == verified &&
			post.write == pre.write && post.read == pre.read,
			"native counters prove SEND placements only");
		barrier(fd, 2); /* both finished */
		printf("PASS native SEND/RECV verified: %u iters x %u x %u B, wall %lld ms, %.2f MiB/s, %llu bytes placed and verified byte-for-byte (%s)\n",
		       send_iters, WINDOW, CHUNK, (long long)elapsed,
		       (double)verified / (1048576.0 * elapsed / 1000.0), verified, argv[3]);
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
		struct counters pre = stats(argv[2]);
		unsigned long long verified = 0;
		int64_t t0 = milliseconds();
		for (unsigned it = 1; it <= rdma_iters; it++) {
			unsigned done = 0, completed = 0, next = 0;
			int64_t deadline;
			for (unsigned i = 0; i < WINDOW; i++) {
				for (unsigned j = 0; j < CHUNK; j++) filled[i*CHUNK+j] = pattern(local.nonce, it, i, j);
				memset(landed+i*CHUNK, 0, CHUNK);
			}
			atomic_thread_fence(memory_order_release);
			/* Both sides must have published their source and cleared their
			 * landing buffer before either operation can reach the peer. */
			barrier(fd, it);
			for (unsigned i = 0; i < WINDOW; i++) {
				ss[i] = (struct ibv_sge){ .addr = (uintptr_t)(tx+i*CHUNK), .length = CHUNK, .lkey = tx_mr->lkey };
				sw[i] = (struct ibv_send_wr){ .wr_id = it*16u+i, .sg_list = &ss[i], .num_sge = 1,
					.opcode = rdma_read ? IBV_WR_RDMA_READ : IBV_WR_RDMA_WRITE,
					.send_flags = IBV_SEND_SIGNALED,
					.next = i+1 < WINDOW ? &sw[i+1] : NULL,
					.wr = { .rdma = { .remote_addr = peer.raddr + i*CHUNK, .rkey = peer.rkey } } };
			}
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
			 * WRITE into it is ordered by this barrier instead. */
			barrier(fd, 1000 + it);
			atomic_thread_fence(memory_order_acquire);
			{
				unsigned bad_slots = 0;

				for (unsigned i = 0; i < WINDOW; i++) {
					unsigned j;

					for (j = 0; j < CHUNK; j++)
						if (landed[i*CHUNK+j] != pattern(peer.nonce, it, i, j))
							break;
					if (j < CHUNK) {
						report_mismatch(landed, peer.nonce, local.nonce, it, i, CHUNK, WINDOW);
						bad_slots++;
					}
				}
				if (bad_slots)
					fail("RDMA payload verification");
			}
			verified += (unsigned long long)WINDOW * CHUNK;
		}
		int64_t elapsed = milliseconds() - t0;
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
		       rdma_read ? "READ" : "WRITE", rdma_iters, WINDOW, CHUNK, (long long)elapsed,
		       (double)verified / (1048576.0 * elapsed / 1000.0), verified, argv[3]);
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
	free(tx); free(rx); free(rd); verb(ibv_destroy_cq(cq), "CQ destroy"); verb(ibv_dealloc_pd(pd), "PD free");
	verb(ibv_close_device(ctx), "context close"); close(fd);
	if (!bench_iters && !ping_iters && !rdma_iters && !send_iters)
		printf("PASS native CPU SEND/RECV: %llu bytes received, window4 x 2MiB setup, ERR flush; not complete RC qualification\n", expected);
	return 0;
}
