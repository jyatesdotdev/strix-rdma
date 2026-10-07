// SPDX-License-Identifier: GPL-2.0-only
/* Ordinary libibverbs client. TCP carries only 48-byte metadata and barriers;
 * every tested payload byte goes through registered native SEND/RECV. */
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
#define META_SIZE 48u
#define TIMEOUT_MS 30000
struct endpoint { uint64_t nonce; uint32_t qpn, psn; union ibv_gid gid; };
struct counters { unsigned long long tx, rx, bad, send, write, read; };
static void fail(const char *what)
{
	fprintf(stderr, "FAIL: %s (errno=%d: %s)\n", what, errno, strerror(errno));
	exit(1); /* Kernel context close, never manual unpin of outstanding memory. */
}
static void require(bool ok, const char *what) { if (!ok) fail(what); }
static void verb(int ret, const char *what) { if (ret) { errno = ret; fail(what); } }
static int64_t milliseconds(void)
{
	struct timespec ts;
	require(!clock_gettime(CLOCK_MONOTONIC, &ts), "clock_gettime");
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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
	memset(out, 0, META_SIZE); put32(out, 0x53564d31); put32(out+4, 1);
	put32(out+8, e->nonce >> 32); put32(out+12, e->nonce);
	put32(out+16, e->qpn); put32(out+20, e->psn); put32(out+24, IBV_MTU_4096);
	memcpy(out+32, e->gid.raw, 16);
}
static bool metadata_decode(struct endpoint *e, const unsigned char in[META_SIZE])
{
	union ibv_gid zero = {};
	if (get32(in) != 0x53564d31 || get32(in+4) != 1 || get32(in+28) ||
	    get32(in+24) != IBV_MTU_4096 || !get32(in+16) || get32(in+16) > 0xffffff ||
	    get32(in+20) > 0xffffff || !memcmp(in+32, &zero, 16)) return false;
	e->nonce = (uint64_t)get32(in+8)<<32 | get32(in+12);
	e->qpn = get32(in+16); e->psn = get32(in+20); memcpy(e->gid.raw, in+32, 16);
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
	struct endpoint e = { .nonce = UINT64_C(0x12345678abcdef01), .qpn = 17, .psn = 0xffffff }, d;
	unsigned char bytes[META_SIZE];
	e.gid.raw[0] = 0xfe; e.gid.raw[1] = 0x80; e.gid.raw[15] = 3;
	metadata_encode(bytes, &e); require(metadata_decode(&d, bytes), "metadata decode");
	require(d.nonce == e.nonce && d.qpn == e.qpn && d.psn == e.psn && !memcmp(&d.gid, &e.gid, 16), "metadata round trip");
	bytes[28] = 1; require(!metadata_decode(&d, bytes), "reserved rejection");
	metadata_encode(bytes, &e); bytes[20] = 1; require(!metadata_decode(&d, bytes), "PSN rejection");
	metadata_encode(bytes, &e); put32(bytes+16, 0); require(!metadata_decode(&d, bytes), "QPN rejection");
	metadata_encode(bytes, &e); memset(bytes+32, 0, 16); require(!metadata_decode(&d, bytes), "zero GID rejection");
	require(pattern(e.nonce, 1, 0, 0) != pattern(e.nonce, 1, 1, 0), "distinct slots");
	puts("ok - smoke metadata codec/selftest only; no device opened");
}
int main(int argc, char **argv)
{
	struct ibv_device **devices; struct ibv_context *ctx = NULL;
	struct ibv_device_attr dev; struct ibv_port_attr port;
	struct ibv_pd *pd; struct ibv_cq *cq; struct ibv_qp *qp;
	struct ibv_mr *tx_mr, *rx_mr; unsigned char *tx, *rx;
	struct endpoint local = {}, peer; struct counters before, after;
	struct ibv_qp_attr attr = {};
	struct ibv_qp_init_attr init = { .qp_type = IBV_QPT_RC,
		.cap = { .max_send_wr = WINDOW, .max_recv_wr = WINDOW, .max_send_sge = 1, .max_recv_sge = 1 } };
	unsigned char mine[META_SIZE], theirs[META_SIZE];
	unsigned long long expected = 0;
	int count, fd; bool listening;
	if (argc == 2 && !strcmp(argv[1], "--selftest")) { selftest(); return 0; }
	if (argc != 6 || strcmp(argv[1], "--live") ||
	    (strcmp(argv[3], "listen") && strcmp(argv[3], "connect"))) {
		fprintf(stderr, "usage: %s --selftest\n       %s --live strix_nhiN listen|connect explicit-IPv4 TCP-port\n", argv[0], argv[0]);
		return 2;
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
	require(getrandom(&local.nonce, sizeof(local.nonce), 0) == sizeof(local.nonce) && local.nonce, "nonce");
	require(getrandom(&local.psn, sizeof(local.psn), 0) == sizeof(local.psn), "PSN");
	local.psn &= 0xffffff; local.qpn = qp->qp_num;
	attr.qp_state = IBV_QPS_INIT; attr.port_num = 1; attr.pkey_index = 0; attr.qp_access_flags = 0;
	verb(ibv_modify_qp(qp, &attr, IBV_QP_STATE|IBV_QP_PKEY_INDEX|IBV_QP_PORT|IBV_QP_ACCESS_FLAGS), "INIT");
	fd = control_socket(listening, argv[4], argv[5]);
	metadata_encode(mine, &local);
	control_io(fd, mine, sizeof(mine), true, milliseconds()+TIMEOUT_MS);
	control_io(fd, theirs, sizeof(theirs), false, milliseconds()+TIMEOUT_MS);
	require(metadata_decode(&peer, theirs) && peer.nonce != local.nonce, "peer metadata");
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
	verb(ibv_destroy_qp(qp), "synchronous QP destroy");
	verb(ibv_dereg_mr(tx_mr), "TX deregistration"); verb(ibv_dereg_mr(rx_mr), "RX deregistration");
	free(tx); free(rx); verb(ibv_destroy_cq(cq), "CQ destroy"); verb(ibv_dealloc_pd(pd), "PD free");
	verb(ibv_close_device(ctx), "context close"); close(fd);
	printf("PASS native CPU SEND/RECV: %llu bytes received, DS4-shaped setup/window4, ERR flush; not full DS4 or complete RC qualification\n", expected);
	return 0;
}
