/* SPDX-License-Identifier: GPL-2.0-only */
/* Executes the production protocol.c with an in-memory transport and explicit
 * MR/CQ/time adapters. This is NOT a kernel, uverbs, DMA or lifetime test. The
 * independent real target-kernel build remains mandatory. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "../verbs/native_order.h"
#define STRIX_NHI_H
#define SN_RING_SIZE 64
#define SN_CONTROL_RESERVE 8
#define SN_CONTROL_BYTES 112
#define SN_HELLO_INTERVAL_NS 100000000ULL
#define SN_BIND_INTERVAL_NS 100000000ULL
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define min_t(t, a, b) ((t)(a) < (t)(b) ? (t)(a) : (t)(b))
#define U64_MAX UINT64_MAX
#define IB_ACCESS_LOCAL_WRITE 1
#define IB_ACCESS_REMOTE_WRITE 2
#define IB_ACCESS_REMOTE_READ 4
#define IB_QPS_RTS 3
#define IB_QPS_ERR 6
#define IB_SEND_SIGNALED 2
#define lockdep_assert_held(x) ((void)(x))
static unsigned int warnings;
#define WARN_ON_ONCE(x) ((x) ? (warnings++, 1) : 0)
typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
enum ib_wc_status { IB_WC_SUCCESS, IB_WC_GENERAL_ERR, IB_WC_RNR_RETRY_EXC_ERR,
	IB_WC_REM_ACCESS_ERR, IB_WC_REM_INV_REQ_ERR, IB_WC_LOC_PROT_ERR,
	IB_WC_LOC_LEN_ERR, IB_WC_RETRY_EXC_ERR };
struct sn_device;
struct sn_mr { u8 *data; u64 base; u32 size, key, access, users; };
struct sn_wqe { struct sn_mr *mr; u64 id, address, remote_address; u32 length, rkey, opcode, flags; };
struct sn_qp {
	struct { struct sn_device *device; u32 qp_num; } ib;
	struct { u32 qp_state, qp_access_flags, max_dest_rd_atomic, min_rnr_timer,
		rnr_retry, retry_cnt, timeout, dest_qp_num, sq_psn, rq_psn; } attr;
	struct sn_binding binding;
	struct sn_wqe sq[SN_QUEUE_DEPTH], rq[SN_QUEUE_DEPTH];
	unsigned int sq_head, sq_count, rq_head, rq_count;
	u8 *outgoing, *incoming, *read_snapshot, *read_incoming;
	struct sn_assembly receive, read_receive;
	struct sn_header read_request;
	struct sn_mr *remote_mr;
	u64 next_sequence, last_read, deadline, progress_deadline;
	u64 response_deadline, assembly_deadline, credit_revision;
	u32 last_credit, outgoing_offset, response_offset, last_read_length;
	unsigned int retries, rnr_retries, response_retries;
	bool active, outgoing_sent, response_active, response_sent, have_last_read, rnr_wait;
	bool bound, credit_sent, destroying;
	u64 next_bind;
	unsigned int zc_state;
	u32 zc_offset;
	bool zc_expect;
	u32 zc_total, zc_received;
	struct sn_mr *zc_mr;
	u64 zc_address;
	bool zc_read_armed, zc_held, zc_read_staged, response_fallback, zc_read_fault;
	bool zc_read_reply_ready;
	struct sn_header zc_held_header;
};
struct packet { u8 data[SN_FRAME]; size_t bytes; };
union ib_gid { u8 raw[16]; };
struct sn_device {
	struct sn_qp *qp;
	struct sn_readiness ready;
	union ib_gid peer_gid;
	u64 next_hello;
	bool dead;
	struct sn_header control[16];
	unsigned int control_head, control_count;
	struct sn_mr mr[2];
	struct packet packets[SN_RING_SIZE];
	unsigned int head, count, sends, recvs, errors, placements, read_samples;
	enum ib_wc_status last_error;
	u64 bad_frames, send_bytes, write_bytes, read_bytes;
	u64 zc_bursts, zc_retries, zc_aborts, zc_tx_bursts;
	u64 zc_read_retx, zc_read_dup_inflight, zc_read_rearm, zc_seq_ahead, zc_seq_stale;
	u64 zc_read_dup_posting, zc_read_dup_undrained;
	u64 zc_read_fallback, zc_read_fallback_served;
	u64 zc_fb_wait_midpost, zc_fb_wait_drain, zc_fb_reserve, zc_read_req_drop, zc_resp_retry_exc, zc_resp_start_exc;
	u64 zc_read_ops, zc_read_fault_injected;
	/* Set when this side's queued burst was stranded by the peer stopping and
	 * restarting its data RX ring underneath it. Observed on hardware: the ring
	 * then never drains and its frames never land, for the rest of the boot. */
	bool zc_tx_stranded;
	/* Round nine (hardware): a one-sided restart of this side's data RX ring
	 * starves the peer's data TX of E2E credits, so nothing it posts afterwards
	 * is ever sent. Opt-in so the SEND/WRITE retry models stay as they were. */
	bool e2e_model, zc_tx_starved;
	unsigned int rx_restarts;
	u64 sched_calls, rnr_sent;
	bool check_ids;
	u32 caps;
	/* Zero-copy model state: peer link, expected-burst landing zone, drop knob.
	 * The landing zone mirrors what the real data ring posts from q->zc_mr. */
	bool peer_zc, zdata, zc_write, zc_read, zc_drop;
	/* Models a data TX ring whose posted burst frames have not been consumed,
	 * so sn_data_tx_drained() reports false and the retry path must defer. */
	bool zc_tx_stuck;
	/* Truncates a burst to this many bytes: the sender believes it queued the whole
	 * message while only part of it lands, which is what makes a receiver's
	 * restart-on-retransmit observable. 0 lands everything. */
	u32 zc_stall_after;
	/* In-flight model. With zc_defer set to a chunk size, sn_data_burst() queues
	 * that many bytes here instead of landing them, so a test can interleave a
	 * descriptor retransmit between posting and arrival. That overlap is the one
	 * thing the atomic model cannot represent, and it is what corrupted zero-copy
	 * WRITE on hardware: a real 2 MiB burst is 512 frames against a 64-slot ring,
	 * so posting is partial and delivery is asynchronous. Payload is copied at
	 * flush time, as the NHI DMAs from the MR when it transmits. 0 = atomic. */
	u32 zc_defer;
	struct {
		struct sn_device *dst;
		struct sn_mr *src;
		u64 src_addr;
		u32 len;
	} inflight[64];
	unsigned inflight_count;
	struct sn_device *zc_peer;
	struct sn_mr *zc_land_mr;
	u64 zc_land_addr;
	u32 zc_land_len, zc_land_landed;
};
static u64 now;
static u64 ktime_get_ns(void) { return now; }
static struct sn_device *sn_dev(struct sn_device *d) { return d; }
static void sn_qp_error(struct sn_qp *, enum ib_wc_status);
static void sn_engine_reset(struct sn_qp *);
static struct sn_mr *sn_mr_get(struct sn_qp *q, u32 key, u64 address, u32 length, u32 access)
{
	unsigned int i;
	for (i = 0; i < 2; i++) {
		struct sn_mr *m = &q->ib.device->mr[i];
		if (m->key == key && (m->access & access) == access && sn_range(m->base, m->size, address, length)) {
			m->users++; return m;
		}
	}
	return NULL;
}
static void sn_mr_put(struct sn_mr *m) { if (m) { assert(m->users); m->users--; } }
static int sn_mr_copy(struct sn_mr *m, u64 address, void *data, u32 length, bool to_mr)
{
	assert(m && m->users && sn_range(m->base, m->size, address, length));
	if (to_mr) memcpy(m->data + address - m->base, data, length);
	else memcpy(data, m->data + address - m->base, length);
	return 0;
}
static int sn_complete(struct sn_qp *q, struct sn_wqe *w, bool receive, enum ib_wc_status status, u32 length)
{
	struct sn_device *d = q->ib.device;
	(void)length;
	if (d->check_ids && !status)
		assert(w->id == (receive ? d->recvs : d->sends) + 1);
	if (status) { d->last_error = status; d->errors++; }
	else if (receive) d->recvs++;
	else d->sends++;
	return 0;
}
static int sn_ring_send(struct sn_device *d, struct sn_header *h, const void *payload, bool control)
{
	struct packet *p;
	unsigned int limit = SN_RING_SIZE - 1 - (control ? 0 : SN_CONTROL_RESERVE);
	bool hs = h->opcode >= SN_HELLO && h->opcode <= SN_BIND_ACK;
	if (hs ? !sn_local_ready(&d->ready) : !sn_ready(&d->ready)) return -ENOLINK;
	if (d->count >= limit) return -EAGAIN;
	p = &d->packets[(d->head + d->count) % SN_RING_SIZE];
	if (h->length) memcpy(p->data + SN_HEADER, payload, h->length);
	assert(!sn_encode(p->data, sizeof(p->data), h));
	p->bytes = SN_HEADER + h->length;
	d->count++;
	return 0; /* Local TX ownership is returned, never peer placement. */
}
/* Zero-copy data-plane models; defined after the engine include. */
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q);
int sn_data_response_burst(struct sn_device *d, struct sn_qp *q);
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q);
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q);
void sn_data_abort(struct sn_device *d);
void sn_stall_snapshot(struct sn_device *d, struct sn_qp *q, int status);
bool sn_data_tx_drained(struct sn_device *d);
unsigned int sn_zc_read_fault;
#define READ_ONCE(x) (*(volatile __typeof__(x) *)&(x))
#include "../verbs/protocol.c"
/* Modeled zero-copy data plane: one burst copies payload from an explicit source
 * MR straight into the peer's expected-burst landing zone and feeds the engine's
 * byte counter. zc_drop simulates a burst lost on the wire, and frames arriving
 * with no burst armed are dropped, exactly as a data RX ring with nothing posted
 * drops them. The SQ path bursts from the active WQE; a READ response has no WQE
 * and bursts from the source MR the request named. */
static int sn_data_burst(struct sn_device *d, struct sn_mr *mr, u64 address,
			 u32 total, u32 *offset)
{
	struct sn_device *p = d->zc_peer;
	u32 n = total - *offset;

	assert(p && mr && n <= total);
	d->zc_tx_bursts++;
	if (d->zc_drop) { *offset = total; return 0; }
	/* A starved TX ring accepts the frames and never sends them: the ring reads as
	 * drained until the first post after the starving restart, then never again. */
	if (d->zc_tx_starved || d->zc_tx_stranded) {
		d->zc_tx_stranded = true; *offset = total; return 0;
	}
	if (d->zc_stall_after && n > d->zc_stall_after) n = d->zc_stall_after;
	if (!p->qp->zc_expect) { *offset = total; return 0; }
	if (d->zc_defer) {
		unsigned idx = d->inflight_count;

		if (n > d->zc_defer) n = d->zc_defer;
		assert(idx < 64);
		d->inflight[idx].dst = p;
		d->inflight[idx].src = mr;
		d->inflight[idx].src_addr = address + *offset;
		d->inflight[idx].len = n;
		d->inflight_count = idx + 1;
		*offset += n;
		/* Mirrors ring.c: a partial post returns -EAGAIN and the engine stays in
		 * SN_ZC_SENDING to resume from zc_offset on a later pass. */
		return *offset >= total ? 0 : -EAGAIN;
	}
	assert(p->zc_land_mr && n <= p->zc_land_len - p->zc_land_landed);
	memcpy(p->zc_land_mr->data + (p->zc_land_addr - p->zc_land_mr->base) + p->zc_land_landed,
	       mr->data + (address - mr->base) + *offset, n);
	p->zc_land_landed += n;
	sn_engine_zc_bytes(p->qp, n);
	*offset = total;
	return 0;
}
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];

	return sn_data_burst(d, w->mr, w->address, w->length, &q->zc_offset);
}
int sn_data_response_burst(struct sn_device *d, struct sn_qp *q)
{
	return sn_data_burst(d, q->remote_mr, q->read_request.address,
			     q->read_request.total, &q->response_offset);
}
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q)
{
	assert(q->zc_mr);
	d->zc_land_mr = q->zc_mr; d->zc_land_addr = q->zc_address;
	d->zc_land_len = q->zc_total; d->zc_land_landed = 0;
	return 0;
}
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q)
{
	(void)q;
	d->rx_restarts++;
	/* Restarting the RX ring under a live peer burst strands it (round 4), and
	 * under the E2E model any one-sided restart starves the peer (round 9). */
	if (d->zc_peer && d->zc_peer->inflight_count) d->zc_peer->zc_tx_stranded = true;
	if (d->zc_peer && d->e2e_model) d->zc_peer->zc_tx_starved = true;
	d->zc_land_landed = 0;
	return 0;
}
void sn_data_abort(struct sn_device *d) { (void)d; }
void sn_stall_snapshot(struct sn_device *d, struct sn_qp *q, int status) { (void)d; (void)q; (void)status; }
/* Deliver everything a device has queued, in order, into each frame's destination
 * landing zone. A frame whose destination no longer expects a burst is dropped,
 * exactly as a data RX ring with nothing posted drops it. The landing-zone assert
 * is the overshoot the real ring.c harvest loop guards against: if it fires, a
 * superseded attempt's frames were counted into the new one. */
static void flush_inflight(struct sn_device *d)
{
	unsigned i;

	if (d->zc_tx_stranded) { d->inflight_count = 0; return; }

	for (i = 0; i < d->inflight_count; i++) {
		struct sn_device *p = d->inflight[i].dst;
		u32 n = d->inflight[i].len;

		if (!p->qp->zc_expect) continue;
		assert(p->zc_land_mr && n <= p->zc_land_len - p->zc_land_landed);
		memcpy(p->zc_land_mr->data + (p->zc_land_addr - p->zc_land_mr->base) + p->zc_land_landed,
		       d->inflight[i].src->data + (d->inflight[i].src_addr - d->inflight[i].src->base), n);
		p->zc_land_landed += n;
		sn_engine_zc_bytes(p->qp, n);
	}
	d->inflight_count = 0;
}
bool sn_data_tx_drained(struct sn_device *d) { return !d->zc_tx_stuck && !d->inflight_count && !d->zc_tx_stranded; }
static void sn_qp_error(struct sn_qp *q, enum ib_wc_status status)
{
	struct sn_device *d = q->ib.device;
	d->errors++; d->last_error = status;
	q->attr.qp_state = IB_QPS_ERR; q->bound = false;
	while (q->sq_count) {
		sn_mr_put(q->sq[q->sq_head].mr);
		q->sq_head = (q->sq_head + 1) % SN_QUEUE_DEPTH; q->sq_count--;
	}
	while (q->rq_count) {
		sn_mr_put(q->rq[q->rq_head].mr);
		q->rq_head = (q->rq_head + 1) % SN_QUEUE_DEPTH; q->rq_count--;
	}
	sn_engine_reset(q);
}
static struct sn_device *endpoint(u32 qpn, u32 peer_qpn, u64 sequence, u64 peer_sequence)
{
	struct sn_device *d = calloc(1, sizeof(*d));
	struct sn_qp *q = calloc(1, sizeof(*q));
	unsigned int i;
	assert(d && q);
	d->qp = q; q->ib.device = d;
	q->ib.qp_num = qpn;
	d->ready = (struct sn_readiness){qpn, peer_qpn, peer_qpn, qpn, 1, 1, 1, 1, 1};
	q->binding = (struct sn_binding){qpn, peer_qpn, qpn, peer_qpn, qpn, peer_qpn};
	q->attr.qp_state = IB_QPS_RTS; q->bound = true;
	q->attr.qp_access_flags = 7; q->attr.max_dest_rd_atomic = 1;
	q->attr.min_rnr_timer = 12; q->attr.rnr_retry = q->attr.retry_cnt = 3; q->attr.timeout = 14;
	q->attr.dest_qp_num = peer_qpn; q->attr.sq_psn = sequence; q->attr.rq_psn = peer_sequence;
	d->caps = SN_CAPS_V1;
	q->outgoing = calloc(1, SN_MESSAGE); q->incoming = calloc(1, SN_MESSAGE);
	q->read_snapshot = calloc(1, SN_MESSAGE); q->read_incoming = calloc(1, SN_MESSAGE);
	assert(q->outgoing && q->incoming && q->read_snapshot && q->read_incoming);
	q->next_sequence = sequence; q->receive.expected = peer_sequence;
	for (i = 0; i < 2; i++) {
		d->mr[i] = (struct sn_mr){calloc(1, SN_MR_BYTES), (u64)(i + 1) * SN_MR_BYTES,
			SN_MR_BYTES, i + 1, 7, 0};
		assert(d->mr[i].data);
	}
	return d;
}
static void release(struct sn_device *d)
{
	unsigned int i;
	sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	for (i = 0; i < 2; i++) { assert(!d->mr[i].users); free(d->mr[i].data); }
	free(d->qp->outgoing); free(d->qp->incoming); free(d->qp->read_snapshot);
	free(d->qp->read_incoming); free(d->qp); free(d);
}
static void post(struct sn_device *d, u32 opcode, u32 length)
{
	struct sn_qp *q = d->qp;
	struct sn_wqe *w;
	assert(q->sq_count < SN_QUEUE_DEPTH);
	w = &q->sq[(q->sq_head + q->sq_count++) % SN_QUEUE_DEPTH];
	*w = (struct sn_wqe){&d->mr[0], q->sq_count, d->mr[0].base + (q->sq_count - 1) * SN_MESSAGE,
		opcode == SN_SEND ? 0 : d->mr[1].base, length, opcode == SN_SEND ? 0 : 2, opcode, IB_SEND_SIGNALED};
	w->mr->users++;
	q->progress_deadline = now + 30000000000ULL;
}
static void receive(struct sn_device *d, u32 length)
{
	struct sn_qp *q = d->qp;
	struct sn_wqe *w;
	assert(q->rq_count < SN_QUEUE_DEPTH);
	w = &q->rq[(q->rq_head + q->rq_count++) % SN_QUEUE_DEPTH];
	*w = (struct sn_wqe){.mr = &d->mr[1], .id = q->rq_count,
		.address = d->mr[1].base + (q->rq_count - 1) * SN_MESSAGE, .length = length};
	w->mr->users++;
}
/* Fault injection acts on encoded production packets, after local completion. */
static void deliver(struct sn_device *from, struct sn_device *to, int *drop_opcode)
{
	while (from->count) {
		struct packet *p = &from->packets[from->head];
		if (drop_opcode && *drop_opcode == p->data[8]) *drop_opcode = 0;
		else sn_engine_receive(to, p->data, p->bytes);
		from->head = (from->head + 1) % SN_RING_SIZE; from->count--;
	}
}
static void step(struct sn_device *a, struct sn_device *b, int *drop_a, int *drop_b)
{
	now += 1000000;
	sn_engine_progress(a); sn_engine_progress(b);
	deliver(a, b, drop_a); deliver(b, a, drop_b);
}
static void test_send(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200), *b = endpoint(20, 10, 200, 100);
	unsigned int i;
	int drop_ack = SN_ACK;
	memset(a->mr[0].data, 0x5a, SN_MESSAGE);
	receive(b, SN_MESSAGE); post(a, SN_SEND, SN_MESSAGE);
	sn_engine_progress(a);
	assert(a->count && !a->sends && !b->recvs && a->mr[0].users == 1);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, &drop_ack);
	assert(a->sends == 1 && b->recvs == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(!a->mr[0].users && !b->mr[1].users);
	assert(b->send_bytes == SN_MESSAGE);
	release(a); release(b);
	puts("ok 1 - actual protocol SEND: local TX is not success; lost ACK replays once");
}
static void test_write_read(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200), *b = endpoint(20, 10, 200, 100);
	unsigned int i;
	int drop_ack = SN_ACK;
	bool changed = false;
	memset(a->mr[0].data, 0x37, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	for (i = 0; i < 1000 && !a->sends; i++) {
		step(a, b, NULL, &drop_ack);
		if (!changed && b->qp->receive.have_replay) {
			memset(b->mr[1].data, 0x68, SN_MESSAGE); changed = true;
		}
	}
	assert(changed && !a->errors && !b->errors && a->sends == 1 && !b->recvs);
	for (i = 0; i < SN_MESSAGE; i++) assert(b->mr[1].data[i] == 0x68);
	/* The READ must retain its first snapshot even when source memory changes
	 * and the first READ response fragment is lost. */
	changed = false; drop_ack = SN_READ_REPLY;
	post(a, SN_READ, SN_MESSAGE);
	for (i = 0; i < 1000 && a->sends < 2; i++) {
		step(a, b, NULL, &drop_ack);
		if (!changed && b->qp->read_request.opcode == SN_READ) {
			memset(b->mr[1].data, 0x91, SN_MESSAGE); changed = true;
		}
	}
	assert(a->sends == 2 && changed && !a->errors && !b->errors);
	for (i = 0; i < SN_MESSAGE; i++) assert(a->mr[0].data[i] == 0x68);
	for (i = 0; i < 5; i++) step(a, b, NULL, NULL);
	assert(!b->qp->response_active && a->sends == 2);
	assert(b->write_bytes == SN_MESSAGE && a->read_bytes == SN_MESSAGE);
	release(a); release(b);
	puts("ok 2 - actual WRITE replay does not rewrite; READ retry returns original snapshot");
}
static void test_failures(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200), *b = endpoint(20, 10, 200, 100);
	unsigned int i;
	post(a, SN_SEND, 1);
	for (i = 0; i < 100 && !a->errors; i++) step(a, b, NULL, NULL);
	assert(a->last_error == IB_WC_RNR_RETRY_EXC_ERR && !a->sends && !b->recvs);
	release(a); release(b);
	a = endpoint(10, 20, 100, 200); b = endpoint(20, 10, 200, 100);
	b->qp->attr.qp_access_flags = 0;
	post(a, SN_READ, 1);
	for (i = 0; i < 100 && !a->errors; i++) step(a, b, NULL, NULL);
	assert(a->last_error == IB_WC_REM_ACCESS_ERR && !a->sends);
	release(a); release(b);
	a = endpoint(10, 20, 100, 200); b = endpoint(20, 10, 200, 100);
	post(a, SN_SEND, 1); receive(b, 1);
	b->qp->binding.peer_generation++;
	for (i = 0; i < 1000 && !a->errors; i++) step(a, b, NULL, NULL);
	assert(a->last_error == IB_WC_RETRY_EXC_ERR && !a->sends && !b->recvs);
	release(a); release(b);
	puts("ok 3 - actual RNR exhaustion, disabled READ permission and stale-generation timeout");
}
static void test_gating_window(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200), *b = endpoint(20, 10, 200, 100);
	unsigned int i;
	a->check_ids = b->check_ids = true;
	for (i = 0; i < 4; i++) {
		memset(a->mr[0].data + i * SN_MESSAGE, 0x30 + i, SN_MESSAGE);
		memset(b->mr[0].data + i * SN_MESSAGE, 0x40 + i, SN_MESSAGE);
		receive(a, SN_MESSAGE); receive(b, SN_MESSAGE);
		post(a, SN_SEND, SN_MESSAGE); post(b, SN_SEND, SN_MESSAGE);
	}
	a->ready.paths_active = 0;
	sn_engine_progress(a); assert(!a->count);
	a->ready.paths_active = 1;
	for (i = 0; i < 1000 && (a->sends < 4 || b->sends < 4); i++) step(a, b, NULL, NULL);
	assert(a->sends == 4 && b->sends == 4 && a->recvs == 4 && b->recvs == 4);
	assert(!memcmp(a->mr[1].data, b->mr[0].data, SN_MR_BYTES));
	assert(!memcmp(b->mr[1].data, a->mr[0].data, SN_MR_BYTES));
	assert(!a->errors && !b->errors);
	release(a); release(b);
	puts("ok 4 - no TX before activated readiness; bidirectional four by 2 MiB queue progress");
}
/* Peer-scripted frames for the QP created by endpoint(10, 20, ...). */
static void inject(struct sn_device *to, struct sn_header h, const u8 *payload)
{
	u8 frame[SN_FRAME];
	h.src_qpn = 20; h.dst_qpn = 10; h.src_generation = 20; h.dst_generation = 10;
	h.src_epoch = 20; h.dst_epoch = 10;
	if (h.length) memcpy(frame + SN_HEADER, payload, h.length);
	assert(!sn_encode(frame, sizeof(frame), &h));
	sn_engine_receive(to, frame, SN_HEADER + h.length);
}
static void test_control_overflow(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200);
	struct sn_qp *q = a->qp;
	struct sn_header h;
	u8 byte = 0x42;
	unsigned int i;
	q->attr.qp_access_flags = 0; /* SEND-only INIT access flags */
	post(a, SN_READ, 1);
	sn_engine_progress(a);
	assert(q->active && q->sq_count == 1 && q->next_sequence == 100);
	a->count = 0;
	/* One receive batch, no progress drain: 16 distinct rejected WRITEs fill
	 * the control queue before the matching READ reply arrives. */
	for (i = 0; i < 16; i++) {
		memset(&h, 0, sizeof(h));
		h.opcode = SN_WRITE; h.sequence = 200 + i; h.total = h.length = 1; h.rkey = 99;
		inject(a, h, &byte);
	}
	assert(a->control_count == 16 && q->attr.qp_state == IB_QPS_RTS && !a->errors);
	memset(&h, 0, sizeof(h));
	h.opcode = SN_READ_REPLY; h.sequence = 100; h.total = h.length = 1;
	inject(a, h, &byte);
	assert(q->attr.qp_state == IB_QPS_ERR && a->errors == 1);
	assert(!q->sq_count && !a->sends && q->next_sequence == 100 && !warnings);
	assert(a->control_count == 16 && !a->mr[0].users);
	/* Defense in depth: completing a flushed SQ is refused, not underflowed. */
	sn_send_complete(q);
	assert(warnings == 1 && !q->sq_count && !a->sends && q->next_sequence == 100);
	warnings = 0;
	release(a);
	puts("ok 5 - full control queue fails the READ requester once; no post-flush completion or SQ underflow");
}
static void raw_inject(struct sn_device *to, struct sn_header h)
{
	u8 frame[SN_FRAME];
	assert(!sn_encode(frame, sizeof(frame), &h));
	sn_engine_receive(to, frame, SN_HEADER);
}
static void test_handshake(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200), *b = endpoint(20, 10, 200, 100);
	struct sn_header h;
	unsigned int i;

	/* Both sides start with local-only readiness: no session, QP unbound. */
	a->ready = (struct sn_readiness){7, 0, 0, 0, 1, 1, 1, 0, 1};
	b->ready = (struct sn_readiness){9, 0, 0, 0, 1, 1, 1, 0, 1};
	a->qp->bound = b->qp->bound = false;
	a->qp->binding.local_generation = 0xaa; b->qp->binding.local_generation = 0xbb;
	a->qp->binding.peer_generation = b->qp->binding.peer_generation = 0;
	a->qp->binding.peer_qpn = b->qp->binding.peer_qpn = 0;
	a->qp->binding.peer_epoch = b->qp->binding.peer_epoch = 0;

	/* No handshake traffic until local paths are up. */
	a->ready.paths_active = 0;
	sn_engine_progress(a);
	assert(!a->count);
	a->ready.paths_active = 1;

	/* The full exchange completes on its own: HELLO, ACKs, BIND, ACKs. */
	for (i = 0; i < 64 && !(sn_ready(&a->ready) && sn_ready(&b->ready) &&
	     a->qp->bound && b->qp->bound); i++) step(a, b, NULL, NULL);
	assert(sn_ready(&a->ready) && sn_ready(&b->ready));
	assert(a->qp->bound && b->qp->bound);
	assert(a->qp->binding.peer_epoch == 9 && b->qp->binding.peer_epoch == 7);
	assert(a->qp->binding.peer_generation == 0xbb &&
	       b->qp->binding.peer_generation == 0xaa);
	assert(a->qp->binding.peer_qpn == 20 && b->qp->binding.peer_qpn == 10);

	/* A stale BIND_ACK echo never re-pairs an already bound QP. */
	h = (struct sn_header){0};
	h.opcode = SN_BIND_ACK; h.src_qpn = 20; h.dst_qpn = 10;
	h.src_generation = 0xbb; h.dst_generation = 0xdead;
	h.src_epoch = 9; h.dst_epoch = 7; h.sequence = 200; h.ack_sequence = 100;
	raw_inject(a, h);
	assert(a->qp->binding.peer_generation == 0xbb && !a->errors);

	/* A peer restart (new epoch) resets reciprocal readiness and fails the
	 * bound QP; the new epoch is adopted so the session can re-form. */
	h = (struct sn_header){0};
	h.opcode = SN_HELLO; h.src_epoch = 0x55; h.total = SN_CAPS_V1;
	raw_inject(a, h);
	assert(!a->ready.peer_ready && !a->ready.peer_echo);
	assert(a->ready.peer_epoch == 0x55);
	assert(a->qp->attr.qp_state == IB_QPS_ERR);

	/* The session re-forms with the new epoch; the errored QP cannot re-pair. */
	for (i = 0; i < 200 && !sn_ready(&a->ready); i++) step(a, b, NULL, NULL);
	assert(sn_ready(&a->ready) && a->qp->attr.qp_state == IB_QPS_ERR);
	release(a); release(b);
	puts("ok 6 - wire handshake: HELLO readiness, BIND pairing, stale rejection, epoch reset");
}
static struct sn_device *zc_pair(struct sn_device **b)
{
	struct sn_device *a = endpoint(10, 20, 100, 200);
	*b = endpoint(20, 10, 200, 100);
	a->peer_zc = (*b)->peer_zc = true;
	a->zdata = (*b)->zdata = true;
	/* Zero-copy WRITE has a kill-switch in the driver (module param zc_write,
	 * latched into sn_device::zc_write when the data plane comes up). The suite
	 * exercises the enabled path; the switched-off case is asserted separately. */
	a->zc_write = (*b)->zc_write = true;
	/* Zero-copy READ has the same kill-switch (module param zc_read); the
	 * switched-off case is asserted too. */
	a->zc_read = (*b)->zc_read = true;
	a->caps = (*b)->caps = SN_CAPS_V2;
	a->zc_peer = *b; (*b)->zc_peer = a;
	return a;
}
static void test_zc(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[0].data, 0x5a, SN_MESSAGE);
	receive(b, SN_MESSAGE); post(a, SN_SEND, SN_MESSAGE);
	sn_engine_progress(a);
	assert(a->count && !a->sends && !b->recvs && a->mr[0].users == 1 && a->qp->zc_state == SN_ZC_WAIT);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && b->recvs == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(!a->mr[0].users && !b->mr[1].users);
	assert(b->send_bytes == SN_MESSAGE);
	release(a); release(b);
	puts("ok 7 - zero-copy burst: descriptor, ZC_READY, header-less payload, single ACK");
}
static void test_zc_rnr(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[0].data, 0x6b, SN_MESSAGE);
	post(a, SN_SEND, SN_MESSAGE);
	step(a, b, NULL, NULL); /* descriptor out; b queues RNR (no WQE yet) */
	receive(b, SN_MESSAGE);
	for (i = 0; i < 100 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && b->recvs == 1 && !a->errors && !b->errors);
	assert(a->qp->rnr_retries == 1);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	release(a); release(b);
	puts("ok 8 - zero-copy RNR: descriptor waits, burst follows the posted receive");
}
static void test_zc_retry_ready(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	int drop_ready = SN_ZC_READY;
	unsigned int i;

	memset(a->mr[0].data, 0x7c, SN_MESSAGE);
	receive(b, SN_MESSAGE); post(a, SN_SEND, SN_MESSAGE);
	for (i = 0; i < 2000 && !a->sends; i++) step(a, b, NULL, &drop_ready);
	assert(a->sends == 1 && b->recvs == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	release(a); release(b);
	puts("ok 9 - lost ZC_READY: descriptor retransmit, duplicate restart, completes");
}
static void test_zc_retry_burst(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[0].data, 0x8d, SN_MESSAGE);
	receive(b, SN_MESSAGE); post(a, SN_SEND, SN_MESSAGE);
	sn_engine_progress(a);
	deliver(a, b, NULL); deliver(b, a, NULL);
	a->zc_drop = true;
	sn_engine_progress(a);
	a->zc_drop = false;
	for (i = 0; i < 2000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && b->recvs == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(b->send_bytes == SN_MESSAGE);
	release(a); release(b);
	puts("ok 10 - lost burst: whole-message retry reposts from offset zero");
}
static void test_zc_fallback(void)
{
	struct sn_device *a = endpoint(10, 20, 100, 200), *b = endpoint(20, 10, 200, 100);
	unsigned int i;

	memset(a->mr[0].data, 0x5a, SN_MESSAGE);
	receive(b, SN_MESSAGE); post(a, SN_SEND, SN_MESSAGE);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && b->recvs == 1 && !a->errors && !b->errors);
	assert(!b->zc_land_mr);
	release(a); release(b);
	puts("ok 11 - no ZDATA capability: large SEND stays on the staged path");
}
static void test_zc_write(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	const struct packet *desc;
	struct sn_header got;
	unsigned int i;

	memset(a->mr[0].data, 0x5a, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	sn_engine_progress(a);
	assert(a->count && !a->sends && !b->recvs && a->mr[0].users == 1 && a->qp->zc_state == SN_ZC_WAIT);
	/* The descriptor must name the responder MR: unlike a ZC SEND it carries
	 * address+rkey, and it carries no payload of its own. It is the last frame
	 * queued by this pass, after the credit hint. */
	desc = &a->packets[(a->head + a->count - 1) % SN_RING_SIZE];
	assert(!sn_decode(&got, desc->data, desc->bytes));
	assert(got.opcode == SN_ZC_WRITE && got.total == SN_MESSAGE && got.sequence == 100 &&
	       !got.length && !got.offset && !got.status && !got.ack_sequence &&
	       got.rkey == 2 && got.address == a->mr[1].base);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !b->recvs && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	/* One landed burst, so exactly one ACK; the remote MR reference is retired
	 * and the receive queue was never touched. */
	assert(b->zc_bursts == 1 && b->write_bytes == SN_MESSAGE && !b->send_bytes);
	assert(!a->mr[0].users && !b->mr[1].users && !b->qp->rq_count && !b->qp->remote_mr);
	assert(b->qp->receive.have_replay && b->qp->receive.replay.opcode == SN_ZC_WRITE &&
	       b->qp->receive.replay_status == SN_OK);
	release(a); release(b);
	puts("ok 12 - zero-copy WRITE burst: descriptor rkey/address, ZC_READY, payload at the remote address, single ACK");
}
static void test_zc_write_retry_ready(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	int drop_ready = SN_ZC_READY;
	unsigned int i;

	memset(a->mr[0].data, 0x7c, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	for (i = 0; i < 2000 && !a->sends; i++) step(a, b, NULL, &drop_ready);
	assert(a->sends == 1 && !b->recvs && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(b->zc_bursts == 1 && b->write_bytes == SN_MESSAGE && !b->mr[1].users);
	release(a); release(b);
	puts("ok 13 - lost ZC_WRITE READY: descriptor retransmit, duplicate restart, completes");
}
static void test_zc_write_retry_burst(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[0].data, 0x8d, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	sn_engine_progress(a);
	deliver(a, b, NULL); deliver(b, a, NULL);
	a->zc_drop = true;
	sn_engine_progress(a);
	a->zc_drop = false;
	for (i = 0; i < 2000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !b->recvs && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(b->zc_bursts == 1 && b->write_bytes == SN_MESSAGE && !b->mr[1].users);
	release(a); release(b);
	puts("ok 14 - lost ZC_WRITE burst: whole-message retry reposts the remote MR from offset zero");
}
/* The retry that restarts a burst must not fire while burst frames are still
 * queued on the data TX ring. The peer zeroes zc_received when the retransmitted
 * descriptor lands, so frames from the superseded attempt would be counted into
 * the new one: the byte count overshoots and a buffer assembled from two attempts
 * completes as IBV_WC_SUCCESS. The guard defers instead, bounded by the retry
 * budget so a ring that never drains fails the QP rather than spinning. */
static void test_zc_retry_waits_for_drain(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[0].data, 0x9e, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	sn_engine_progress(a);
	deliver(a, b, NULL);
	/* sn_ack() queues into the control queue, which only reaches the packet ring
	 * on the responder's next progress -- so b must be stepped before its
	 * ZC_READY can be delivered. The step()-driven tests self-correct here. */
	sn_engine_progress(b);
	deliver(b, a, NULL);			/* ZC_READY -> requester goes SENDING */
	sn_engine_progress(a);			/* burst posted: requester now in SENT */
	sn_engine_progress(b);			/* responder counts bytes, retires the MR */
	/* step() would complete the whole exchange in one pass, so drive the states
	 * by hand to observe the transient SN_ZC_SENT the guard keys on. */
	assert(a->qp->zc_state == SN_ZC_SENT && a->qp->outgoing_sent);
	assert(b->write_bytes == SN_MESSAGE && b->zc_bursts == 1);

	/* Drop the completion ACK so the requester keeps waiting, and refuse to drain.
	 * The engine must keep deferring: no descriptor retransmit, no second burst,
	 * and the payload that already landed must never be counted twice. */
	{ int drop_ack = SN_ACK; deliver(b, a, &drop_ack); }
	a->zc_tx_stuck = true;
	for (i = 0; i < 500 && !a->errors; i++) {
		now += 1000000;
		sn_engine_progress(a);
		assert(b->write_bytes == SN_MESSAGE);
	}
	assert(a->errors == 1 && a->last_error == IB_WC_RETRY_EXC_ERR);
	assert(!a->sends && a->zc_retries == 0);
	release(a); release(b);
	puts("ok 17 - burst restart waits for the data TX ring to drain; a ring that never drains fails the QP");
}
static void test_zc_read(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	const struct packet *desc;
	struct sn_header got;
	unsigned int i;

	memset(b->mr[1].data, 0x5a, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	sn_engine_progress(a);
	/* A READ requester already knows its landing zone, so it arms its own data RX
	 * before the descriptor goes out: no SN_ZC_READY round trip, and no burst state
	 * of its own because this side queues no payload. */
	assert(a->zc_land_mr == &a->mr[0] && a->zc_land_addr == a->mr[0].base &&
	       a->zc_land_len == SN_MESSAGE && !a->zc_land_landed);
	assert(a->qp->zc_expect && a->qp->zc_read_armed && a->qp->zc_state == SN_ZC_NONE);
	assert(a->count && !a->sends && a->mr[0].users == 1 && !b->zc_tx_bursts);
	/* The descriptor names the responder's source MR, exactly as a staged SN_READ
	 * does, and carries no payload of its own. It is the last frame this pass
	 * queued, after the credit hint. */
	desc = &a->packets[(a->head + a->count - 1) % SN_RING_SIZE];
	assert(!sn_decode(&got, desc->data, desc->bytes));
	assert(got.opcode == SN_ZC_READ && got.total == SN_MESSAGE && got.sequence == 100 &&
	       !got.length && !got.offset && !got.status && !got.ack_sequence &&
	       got.rkey == 2 && got.address == b->mr[1].base);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !a->recvs && !b->recvs && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(a->zc_bursts == 1 && a->read_bytes == SN_MESSAGE && !a->write_bytes);
	/* The requester disarms its landing zone at the completing count. */
	assert(!a->qp->zc_expect && !a->qp->zc_read_armed && !a->qp->zc_mr);
	/* The READ_ACK is what releases the responder: until it lands the responder
	 * still owes a response and still holds the source MR. */
	assert(b->zc_tx_bursts == 1 && b->qp->response_active && b->qp->remote_mr == &b->mr[1]);
	for (i = 0; i < 5; i++) step(a, b, NULL, NULL);
	assert(!b->qp->remote_mr && !b->qp->response_active && !b->mr[1].users);
	assert(!a->mr[0].users && a->sends == 1 && !a->errors && !b->errors);
	release(a); release(b);
	puts("ok 18 - zero-copy READ: descriptor names the source, no ZC_READY, payload lands in the requester's MR");
}
static void test_zc_read_retry_burst(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(b->mr[1].data, 0x8d, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	sn_engine_progress(a);
	deliver(a, b, NULL);
	/* The responder commits the request at once and retains the source MR: it
	 * bursts from it, so unlike the staged path it cannot copy and drop. */
	assert(b->qp->response_active && !b->qp->response_sent &&
	       b->qp->remote_mr == &b->mr[1] && b->mr[1].users == 1);
	assert(b->qp->receive.have_replay && b->qp->receive.replay.opcode == SN_ZC_READ &&
	       b->qp->receive.replay_status == SN_OK);
	b->zc_drop = true;
	sn_engine_progress(b);
	b->zc_drop = false;
	assert(b->qp->response_sent && b->zc_tx_bursts == 1 && !a->zc_bursts);
	/* The requester never sees the completing count. A burst lost on the wire
	 * cannot happen on E2E hardware -- the data hop is lossless -- and the only
	 * way to reclaim the posted landing zone it leaves behind is a one-sided RX
	 * ring restart, which starves the peer's TX (hardware round nine). So the
	 * requester falls back, the responder serves the staged reply, never bursting
	 * again, and the READ then fails loudly: no completion, no ring restart, and
	 * the half-assembled zone is never handed back as success. */
	for (i = 0; i < 2000 && !a->sends && !a->errors; i++) step(a, b, NULL, NULL);
	assert(!a->sends && a->errors == 1 && a->last_error == IB_WC_RETRY_EXC_ERR);
	assert(!a->recvs && !b->recvs && !b->errors && !a->rx_restarts);
	assert(!a->zc_bursts && !a->read_bytes && b->zc_tx_bursts == 1);
	assert(a->zc_read_fallback == 1 && b->zc_read_fallback_served == 1);
	release(a); release(b);

	/* A response truncated on the wire: half of it landed. The old recovery flushed
	 * the landing zone and re-burst from zero, which needs the RX ring restarted
	 * under a possibly live burst. Nothing bursts again and nothing restarts: the
	 * half that never lands keeps the zone from being disarmed, so the READ fails
	 * loudly rather than reclaim the zone one-sided. */
	a = zc_pair(&b);
	memset(b->mr[1].data, 0x9e, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	sn_engine_progress(a);
	deliver(a, b, NULL);
	b->zc_stall_after = SN_MESSAGE / 2;
	sn_engine_progress(b);
	b->zc_stall_after = 0;
	assert(b->qp->response_sent && a->zc_land_landed == SN_MESSAGE / 2 && !a->sends);
	for (i = 0; i < 2000 && !a->sends && !a->errors; i++) step(a, b, NULL, NULL);
	assert(!a->sends && a->errors == 1 && a->last_error == IB_WC_RETRY_EXC_ERR);
	assert(!b->errors && !a->zc_bursts && !a->rx_restarts);
	assert(!a->read_bytes && b->zc_tx_bursts == 1);
	assert(a->zc_read_fallback == 1 && b->zc_read_fallback_served == 1);
	release(a); release(b);
	puts("ok 19 - lost zero-copy READ response: staged fallback, no second burst, no ring restart, fails loudly");
}
/* The response retry is the zero-copy READ's exposed replay path: the responder is
 * the burst sender, so it must not restart a burst while frames from the
 * superseded attempt are still queued. The requester cannot recall them and would
 * count them into the new attempt. Same guard as the SQ path, same bounded budget. */
static void test_zc_read_retry_waits_for_drain(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	int drop_ack = SN_READ_ACK;
	unsigned int i;

	memset(b->mr[1].data, 0x9e, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, &drop_ack, NULL);
	assert(a->sends == 1 && a->zc_bursts == 1 && !a->errors && !b->errors);
	/* The READ_ACK was dropped, so the responder still owes a response and still
	 * holds the source MR. Refuse to drain its data TX ring: it must defer the
	 * re-burst rather than queue it on top of the frames already posted. */
	assert(b->qp->response_active && b->qp->response_sent &&
	       b->qp->remote_mr == &b->mr[1] && b->zc_tx_bursts == 1);
	b->zc_tx_stuck = true;
	for (i = 0; i < 500 && !b->errors; i++) {
		now += 1000000;
		sn_engine_progress(b);
		assert(b->zc_tx_bursts == 1 && a->zc_bursts == 1 && a->read_bytes == SN_MESSAGE);
	}
	assert(b->errors == 1 && b->last_error == IB_WC_RETRY_EXC_ERR);
	assert(b->qp->response_retries == 3 && !b->qp->response_active && !b->qp->remote_mr);
	assert(!a->errors && a->sends == 1 && a->qp->attr.qp_state == IB_QPS_RTS);
	release(a); release(b);
	puts("ok 20 - READ response re-burst waits for the data TX ring to drain; a ring that never drains fails the QP");
}
static void test_zc_read_fallback(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	/* At the threshold a READ stays on the staged control-ring path, and so keeps
	 * the read_snapshot guarantee that a zero-copy retry gives up. */
	memset(b->mr[1].data, 0x5a, SN_ZC_THRESHOLD);
	post(a, SN_READ, SN_ZC_THRESHOLD);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!a->zc_land_mr && !a->zc_bursts && !b->zc_tx_bursts &&
	       a->read_bytes == SN_ZC_THRESHOLD);
	for (i = 0; i < SN_ZC_THRESHOLD; i++) assert(a->mr[0].data[i] == 0x5a);
	release(a); release(b);

	/* Above it, a frame-unaligned local landing zone still stays staged: the
	 * requester would have to post unaligned burst pages to its own data RX. */
	a = zc_pair(&b);
	memset(b->mr[1].data, 0x6b, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	a->qp->sq[a->qp->sq_head].address += SN_HEADER;
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!a->zc_land_mr && !a->zc_bursts && !b->zc_tx_bursts &&
	       a->read_bytes == SN_MESSAGE);
	for (i = 0; i < SN_MESSAGE; i++) assert(a->mr[0].data[SN_HEADER + i] == 0x6b);
	release(a); release(b);

	/* A frame-unaligned remote source stays staged too: the responder bursts from
	 * it directly, and a header-less frame never spans a page. */
	a = zc_pair(&b);
	memset(b->mr[1].data, 0x7c, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	a->qp->sq[a->qp->sq_head].remote_address += SN_HEADER;
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!a->zc_land_mr && !a->zc_bursts && !b->zc_tx_bursts &&
	       a->read_bytes == SN_MESSAGE);
	for (i = 0; i < SN_MESSAGE; i++)
		assert(a->mr[0].data[i] == b->mr[1].data[SN_HEADER + i]);
	release(a); release(b);

	/* Kill-switch off (zc_read=0): a READ that is otherwise
	 * fully eligible -- above the threshold, frame-aligned locally and remotely,
	 * peer advertising ZDATA -- still stays staged and never reaches the data
	 * ring. */
	a = zc_pair(&b);
	a->zc_read = b->zc_read = false;
	memset(b->mr[1].data, 0x8d, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!a->zc_land_mr && !a->zc_bursts && !b->zc_tx_bursts &&
	       a->read_bytes == SN_MESSAGE);
	for (i = 0; i < SN_MESSAGE; i++) assert(a->mr[0].data[i] == 0x8d);
	release(a); release(b);
	puts("ok 21 - zero-copy READ fallback: threshold, both alignments and the opt-in gate keep a READ staged");
}
static void test_zc_read_refusal(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	struct sn_header h;
	unsigned int i;

	/* Without REMOTE_READ the request is refused before anything is retained or
	 * burst; the responder stays in RTS and the requester fails remotely. */
	memset(b->mr[1].data, 0x5a, SN_MESSAGE);
	b->qp->attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE;
	post(a, SN_READ, SN_MESSAGE);
	for (i = 0; i < 100 && !a->errors; i++) step(a, b, NULL, NULL);
	assert(a->last_error == IB_WC_REM_ACCESS_ERR && !a->sends);
	assert(!b->qp->response_active && !b->qp->remote_mr && !b->zc_tx_bursts &&
	       !b->mr[1].users && b->qp->attr.qp_state == IB_QPS_RTS && !b->errors);
	release(a); release(b);

	/* max_dest_rd_atomic is enforced exactly as for a staged SN_READ. */
	a = zc_pair(&b);
	memset(b->mr[1].data, 0x6b, SN_MESSAGE);
	b->qp->attr.max_dest_rd_atomic = 0;
	post(a, SN_READ, SN_MESSAGE);
	for (i = 0; i < 100 && !a->errors; i++) step(a, b, NULL, NULL);
	assert(a->last_error == IB_WC_REM_ACCESS_ERR && !a->sends);
	assert(!b->qp->response_active && !b->qp->remote_mr && !b->zc_tx_bursts &&
	       !b->mr[1].users && b->qp->attr.qp_state == IB_QPS_RTS && !b->errors);
	release(a); release(b);

	/* Defense in depth: a non-conforming peer's unaligned ZC_READ descriptor is
	 * refused the same way, and the refusal survives a retry. */
	a = zc_pair(&b);
	h = (struct sn_header){0};
	h.opcode = SN_ZC_READ; h.sequence = 200; h.total = SN_MESSAGE;
	h.rkey = 2; h.address = a->mr[1].base + SN_HEADER;
	inject(a, h, NULL);
	assert(a->qp->receive.have_replay && a->qp->receive.replay_status == SN_ACCESS);
	assert(!a->qp->response_active && !a->qp->remote_mr && !a->zc_tx_bursts &&
	       !a->mr[1].users && !a->errors && a->qp->attr.qp_state == IB_QPS_RTS);
	inject(a, h, NULL);
	assert(a->qp->receive.have_replay && a->qp->receive.replay_status == SN_ACCESS);
	assert(!a->qp->response_active && !a->zc_tx_bursts && !a->errors &&
	       a->qp->attr.qp_state == IB_QPS_RTS);
	release(a); release(b);
	puts("ok 22 - zero-copy READ refusal: no REMOTE_READ, no max_dest_rd_atomic, an unaligned source");
}
/* One inbound burst at a time: the data RX landing zone is a single header-less
 * byte stream, so a peer-initiated burst descriptor that crosses our own READ
 * request must be held until the response lands, and the peer must not queue its
 * two bursts into each other. Without this the two payloads would be counted into
 * whichever zone was armed first. */
static void test_zc_read_serializes_inbound_bursts(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(b->mr[1].data, 0xa1, SN_MESSAGE);	/* source of a's READ */
	memset(b->mr[0].data, 0xb2, SN_MESSAGE);	/* source of b's WRITE */
	post(a, SN_READ, SN_MESSAGE);
	sn_engine_progress(a);			/* a arms its landing zone, descriptor queued */
	post(b, SN_WRITE, SN_MESSAGE);
	sn_engine_progress(b);			/* b queues a descriptor that crosses it */
	deliver(b, a, NULL);
	/* Held, not armed on top of the READ response that owns a's landing zone. */
	assert(a->qp->zc_held && a->qp->zc_read_armed && !a->qp->receive.active);
	assert(a->zc_land_mr == &a->mr[0] && !a->zc_land_landed && !a->zc_bursts);
	deliver(a, b, NULL);			/* b accepts the READ and retains its source */
	assert(b->qp->response_active && b->qp->remote_mr == &b->mr[1] && b->qp->zc_state == SN_ZC_WAIT);
	for (i = 0; i < 1000 && (a->sends < 1 || b->sends < 1); i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && b->sends == 1 && !a->recvs && !b->recvs);
	assert(!a->errors && !b->errors && !a->qp->zc_held);
	/* Both landed, each in its own zone: the READ in a->mr[0], the WRITE that was
	 * held behind it in a->mr[1]. Neither burst was interleaved with the other. */
	for (i = 0; i < SN_MESSAGE; i++) assert(a->mr[0].data[i] == 0xa1);
	for (i = 0; i < SN_MESSAGE; i++) assert(a->mr[1].data[i] == 0xb2);
	assert(a->zc_bursts == 2 && b->zc_tx_bursts == 2);
	assert(a->read_bytes == SN_MESSAGE && a->write_bytes == SN_MESSAGE);
	assert(!b->qp->remote_mr && !a->qp->remote_mr && !a->qp->response_active);
	release(a); release(b);
	puts("ok 23 - one inbound burst at a time: a crossing WRITE descriptor is held behind the READ response");
}
/* The staged path recovers a lost READ_ACK: the requester recognises the duplicate
 * SN_READ_REPLY and acks it again. A zero-copy response retransmit is payload only,
 * so a requester that already completed has nothing armed to catch it. If it has
 * also moved on to its own zero-copy operation, the two sides must not queue bursts
 * into each other: the SQ burst waits for the response to retire and the response
 * waits for the SQ burst to stop queueing, and the bounded retry budget fails the
 * QP instead of letting two payloads share one header-less byte stream. */
static void test_zc_read_lost_ack_is_fatal(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	int drop_ack = SN_READ_ACK;
	unsigned int i;

	memset(b->mr[1].data, 0xc3, SN_MESSAGE);
	memset(b->mr[0].data, 0xd4, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	sn_engine_progress(a);
	post(b, SN_WRITE, SN_MESSAGE);
	sn_engine_progress(b);
	deliver(b, a, NULL);
	deliver(a, b, NULL);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, &drop_ack, NULL);
	/* The READ itself completed; only its ACK was lost. It is still queued when the
	 * loop above exits, so this is the pass that flushes and drops it. */
	step(a, b, &drop_ack, NULL);
	assert(a->sends == 1 && a->zc_bursts == 1 && !a->errors && !b->errors);
	assert(b->qp->response_active && b->qp->remote_mr == &b->mr[1] && b->zc_tx_bursts == 1);
	/* a releases the held WRITE descriptor and arms for it; b never bursts into
	 * that zone while it still owes a response, so nothing lands there. */
	for (i = 0; i < 1000 && !b->errors; i++) step(a, b, NULL, NULL);
	assert(b->errors == 1 && b->last_error == IB_WC_RETRY_EXC_ERR);
	assert(b->zc_tx_bursts == 1 && b->qp->response_retries == 3);
	assert(!a->zc_land_landed && a->zc_bursts == 1 && !a->write_bytes && !a->errors);
	for (i = 0; i < SN_MESSAGE; i++) assert(!a->mr[1].data[i]);
	for (i = 0; i < SN_MESSAGE; i++) assert(a->mr[0].data[i] == 0xc3);
	release(a); release(b);
	puts("ok 24 - a lost READ_ACK is unrecoverable zero-copy: bursts never interleave, the responder fails instead");
}
static void test_zc_write_fallback(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	/* At the threshold a WRITE stays on the staged control-ring path. */
	memset(a->mr[0].data, 0x5a, SN_ZC_THRESHOLD);
	post(a, SN_WRITE, SN_ZC_THRESHOLD);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !b->recvs && !a->errors && !b->errors);
	assert(!b->zc_land_mr && !b->zc_bursts && b->write_bytes == SN_ZC_THRESHOLD);
	for (i = 0; i < SN_ZC_THRESHOLD; i++) assert(b->mr[1].data[i] == 0x5a);
	release(a); release(b);

	/* Above it, a frame-unaligned remote destination still stays staged. */
	a = zc_pair(&b);
	memset(a->mr[0].data, 0x6b, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	a->qp->sq[a->qp->sq_head].remote_address += SN_HEADER;
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !b->recvs && !a->errors && !b->errors);
	assert(!b->zc_land_mr && !b->zc_bursts && b->write_bytes == SN_MESSAGE);
	for (i = 0; i < SN_MESSAGE; i++) assert(b->mr[1].data[SN_HEADER + i] == 0x6b);
	release(a); release(b);
	/* Opt-in gate off, which is the shipped default: a WRITE that is otherwise
	 * fully eligible -- above the threshold, frame-aligned locally and remotely,
	 * peer advertising ZDATA -- still stays staged and never reaches the data
	 * ring. This is the assertion that keeps the replay-ordering bug unreachable
	 * by default. */
	a = zc_pair(&b);
	a->zc_write = b->zc_write = false;
	memset(a->mr[0].data, 0x7c, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	for (i = 0; i < 1000 && !a->sends; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !b->recvs && !a->errors && !b->errors);
	assert(!b->zc_land_mr && !b->zc_bursts && b->write_bytes == SN_MESSAGE);
	for (i = 0; i < SN_MESSAGE; i++) assert(b->mr[1].data[i] == 0x7c);
	release(a); release(b);
	puts("ok 15 - zero-copy WRITE fallback: threshold, remote alignment and the opt-in gate keep a WRITE staged");
}
static void test_zc_write_refusal(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	struct sn_header h;
	unsigned int i;

	/* Without REMOTE_WRITE the descriptor is refused before any burst page is
	 * posted; the responder stays in RTS and the requester fails remotely. */
	memset(a->mr[0].data, 0x5a, SN_MESSAGE);
	b->qp->attr.qp_access_flags = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_READ;
	post(a, SN_WRITE, SN_MESSAGE);
	for (i = 0; i < 100 && !a->errors; i++) step(a, b, NULL, NULL);
	assert(a->last_error == IB_WC_REM_ACCESS_ERR && !a->sends && !b->recvs);
	assert(!b->zc_land_mr && !b->zc_bursts && !b->write_bytes && !b->mr[1].users);
	assert(b->qp->attr.qp_state == IB_QPS_RTS && !b->errors);
	release(a); release(b);

	/* Defense in depth: a non-conforming peer's unaligned ZC_WRITE descriptor is
	 * refused the same way, and the refusal survives a retry. */
	a = zc_pair(&b);
	h = (struct sn_header){0};
	h.opcode = SN_ZC_WRITE; h.sequence = 200; h.total = SN_MESSAGE;
	h.rkey = 2; h.address = a->mr[1].base + SN_HEADER;
	inject(a, h, NULL);
	assert(a->qp->receive.have_replay && a->qp->receive.replay_status == SN_ACCESS);
	assert(!a->zc_land_mr && !a->zc_land_landed && !a->write_bytes && !a->errors);
	assert(a->qp->attr.qp_state == IB_QPS_RTS && !a->qp->rq_count && !a->qp->remote_mr);
	inject(a, h, NULL);
	assert(a->qp->receive.have_replay && a->qp->receive.replay_status == SN_ACCESS);
	assert(!a->zc_land_mr && !a->errors && a->qp->attr.qp_state == IB_QPS_RTS);
	release(a); release(b);
	puts("ok 16 - zero-copy WRITE refusal: no REMOTE_WRITE and an unaligned destination stay off the data ring");
}
/* Both sides post a zero-copy READ at once. Each arms its own data RX landing
 * zone for the response it is owed, so q->zc_expect is set on both. A responder
 * that held an inbound READ request until its own response landed would deadlock
 * here: neither side ever answers. Answering a READ bursts OUT on the data TX ring
 * and lands nothing inbound, so it must not be held. The unidirectional tests
 * cannot reach this; on hardware it appeared as IBV_WC_RETRY_EXC_ERR on iteration
 * 1 with zc_bursts 0 on both hosts -- descriptors exchanged, no response, no
 * bad_frames. */
static void test_zc_read_bidirectional(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[1].data, 0xa5, SN_MESSAGE);
	memset(b->mr[1].data, 0x5a, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	post(b, SN_READ, SN_MESSAGE);
	for (i = 0; i < 2000 && (a->sends < 1 || b->sends < 1); i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && b->sends == 1 && !a->errors && !b->errors);
	/* Each side landed the other's source bytes into its own local MR. */
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(!memcmp(b->mr[0].data, a->mr[1].data, SN_MESSAGE));
	assert(a->read_bytes == SN_MESSAGE && b->read_bytes == SN_MESSAGE);
	assert(a->zc_bursts == 1 && b->zc_bursts == 1);
	assert(a->zc_tx_bursts == 1 && b->zc_tx_bursts == 1);
	for (i = 0; i < 20; i++) step(a, b, NULL, NULL);
	assert(!a->qp->remote_mr && !b->qp->remote_mr);
	assert(!a->qp->response_active && !b->qp->response_active);
	assert(!a->errors && !b->errors);
	release(a); release(b);
	puts("ok 25 - simultaneous bidirectional zero-copy READ: neither side holds the other's request");
}
/* The overlap the atomic model could not represent, and that corrupted zero-copy
 * WRITE on hardware. A burst is posted in bounded chunks, so frames are still
 * queued in the data TX ring when the ACK deadline passes. Retransmitting the
 * descriptor at that moment makes the receiver zero zc_received for the duplicate
 * and then count the superseded attempt's frames into the new one, overshooting
 * the landing zone. The drain barrier must defer the restart instead. */
static void test_zc_burst_inflight_overlap(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(a->mr[0].data, 0xc3, SN_MESSAGE);
	post(a, SN_WRITE, SN_MESSAGE);
	/* 128 KiB per pass, so a 2 MiB burst queues as 16 chunks -- more than the
	 * 64-slot ring holds at once on real hardware, and none of it has landed. */
	a->zc_defer = SN_MESSAGE / 16;

	sn_engine_progress(a);			/* descriptor */
	deliver(a, b, NULL);
	sn_engine_progress(b);			/* flush ZC_READY into packets */
	deliver(b, a, NULL);
	for (i = 0; i < 100 && a->qp->zc_state != SN_ZC_SENT; i++) sn_engine_progress(a);
	/* Fully queued, nothing delivered: the exact state a real ring is in mid-burst,
	 * and the state the atomic model jumped straight over. */
	assert(a->qp->zc_state == SN_ZC_SENT && a->inflight_count == 16);
	assert(!b->zc_land_landed && !b->write_bytes);
	assert(!sn_data_tx_drained(a));

	/* No READ_ACK/ACK can exist yet because nothing has landed, so the deadline
	 * passes with frames still outstanding. retry_cnt is 3, so defer twice: the
	 * restart must be held off, not taken, and nothing may land or error. */
	for (i = 0; i < 2; i++) { now += 1000000000ULL; sn_engine_progress(a); }
	assert(a->qp->zc_state == SN_ZC_SENT && !a->errors && !b->errors);
	assert(!b->zc_land_landed && a->inflight_count == 16);

	/* Now the queued frames arrive. They must land exactly once into the still-armed
	 * zone and complete the message; the flush assert is the overshoot check. */
	flush_inflight(a);
	assert(sn_data_tx_drained(a));
	assert(b->zc_land_landed == SN_MESSAGE && b->write_bytes == SN_MESSAGE);
	sn_engine_progress(b);
	deliver(b, a, NULL);
	sn_engine_progress(a);
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(!b->mr[1].users && !a->mr[0].users);
	release(a); release(b);
	puts("ok 26 - in-flight burst frames survive an ACK timeout: the restart defers until the ring drains");
}
static void inject_header(struct sn_device *to, const struct sn_header *h)
{
	u8 buf[SN_FRAME];

	assert(!sn_encode(buf, sizeof(buf), h));
	sn_engine_receive(to, buf, SN_HEADER);
}
/* A zero-copy READ times out while its reply is still queued in the responder's
 * data TX ring. Restarting the requester's RX ring under that burst strands it
 * (hardware round 4: the ring never drains, the duplicate is ignored forever, the
 * requester exhausts its retries). The requester must instead keep its landing zone
 * armed and fall back to a staged SN_READ; the queued frames then drain, the
 * staged reply completes the READ exactly once and byte-for-byte. */
static void test_zc_read_timeout_staged_fallback(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(b->mr[1].data, 0x77, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	b->zc_defer = SN_MESSAGE;
	sn_engine_progress(a);
	deliver(a, b, NULL);
	for (i = 0; i < 100 && !b->qp->response_sent; i++) sn_engine_progress(b);
	assert(b->qp->response_sent && b->inflight_count == 1 && !a->zc_land_landed);
	now += 1000000000ULL;
	sn_engine_progress(a);			/* the zero-copy READ times out */
	for (i = 0; i < 400 && !a->sends && !a->errors; i++) {
		flush_inflight(b);
		step(a, b, NULL, NULL);
	}
	assert(!b->zc_tx_stranded);		/* no RX restart under a live burst */
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(a->zc_read_fallback == 1 && b->zc_read_fallback_served == 1);
	for (i = 0; i < 20; i++) step(a, b, NULL, NULL);
	assert(a->sends == 1 && !a->errors && !b->errors);	/* exactly one completion */
	release(a); release(b);
	puts("ok 27 - zero-copy READ timeout with the reply queued falls back to staged, no ring restart, one completion");
}
/* A staged SN_READ reusing the sequence of a committed SN_ZC_READ is the same
 * operation, not a protocol violation. sn_same_operation() compares opcode, so
 * today the responder rejects it with IB_WC_REM_INV_REQ_ERR. */
static void test_zc_read_cross_path_identity(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	const struct packet *desc;
	struct sn_header got;

	memset(b->mr[1].data, 0x3c, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	sn_engine_progress(a);
	desc = &a->packets[(a->head + a->count - 1) % SN_RING_SIZE];
	assert(!sn_decode(&got, desc->data, desc->bytes) && got.opcode == SN_ZC_READ);
	deliver(a, b, NULL);			/* b commits the zero-copy READ */
	assert(b->qp->response_active && b->qp->read_request.opcode == SN_ZC_READ);
	got.opcode = SN_READ;			/* the requester's staged fallback */
	inject_header(b, &got);
	assert(!b->errors && b->qp->response_fallback);
	/* Nothing had been queued. The requester's landing zone is still armed, so
	 * the zero-copy reply is posted in full first; only once it has drained is
	 * the same operation served staged. */
	sn_engine_progress(b);
	assert(!b->errors && b->qp->response_sent && b->zc_tx_bursts == 1);
	assert(b->qp->read_request.opcode == SN_ZC_READ && !b->zc_read_fallback_served);
	sn_engine_progress(b);
	assert(!b->errors);
	assert(b->qp->response_active && b->qp->read_request.opcode == SN_READ);
	assert(b->zc_read_fallback_served == 1);
	release(a); release(b);
	puts("ok 28 - a staged SN_READ under a committed SN_ZC_READ sequence is the same operation");
}
/* The zero-copy reply never lands at all -- lost on the wire every time, which E2E
 * hardware cannot do. Re-bursting cannot recover, and completing through the
 * staged reply would leave the posted landing zone to a one-sided RX ring restart,
 * which starves the peer's TX (hardware round nine). The READ fails loudly. */
static void test_zc_read_never_lands(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(b->mr[1].data, 0xe1, SN_MESSAGE);
	b->zc_drop = true;
	post(a, SN_READ, SN_MESSAGE);
	for (i = 0; i < 400 && !a->sends && !a->errors; i++) step(a, b, NULL, NULL);
	assert(!a->sends && a->errors == 1 && a->last_error == IB_WC_RETRY_EXC_ERR);
	assert(!b->errors && !a->rx_restarts && !a->read_bytes);
	assert(a->zc_read_fallback == 1 && b->zc_read_fallback_served == 1);
	release(a); release(b);
	puts("ok 29 - a zero-copy READ reply that never lands fails loudly: no completion, no ring restart");
}
/* A fallback that arrives while the zero-copy reply is still being POSTED. 7977ae2
 * truncated the reply there, which left posted landing-zone buffers that only a
 * one-sided RX ring restart could reclaim -- the restart that starves the peer's TX
 * (hardware round nine; the mid-post stalls of round six were that starvation,
 * not the finishing itself). The reply is posted and drained in full, the zone
 * fills exactly, and the staged reply then completes the READ once. */
static void test_zc_read_fallback_midpost_finishes(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	memset(b->mr[1].data, 0x5c, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	b->zc_defer = SN_MESSAGE / 16;
	sn_engine_progress(a);
	deliver(a, b, NULL);
	sn_engine_progress(b);			/* one chunk queued, then gated on drain */
	assert(b->qp->response_offset == SN_MESSAGE / 16 && !b->qp->response_sent);
	now += 100000000ULL;
	sn_engine_progress(a);			/* a times out and falls back */
	assert(a->zc_read_fallback == 1);
	deliver(a, b, NULL);			/* the staged SN_READ meets a mid-post reply */
	sn_engine_progress(b);
	/* Not truncated, and not served staged while the reply is incomplete. */
	assert(!b->qp->response_sent && b->qp->response_fallback);
	assert(!b->zc_read_fallback_served && b->zc_fb_wait_midpost);
	for (i = 0; i < 400 && !a->sends && !a->errors; i++) {
		flush_inflight(b);
		step(a, b, NULL, NULL);
	}
	assert(a->sends == 1 && !a->errors && !b->errors);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	assert(a->zc_land_landed == SN_MESSAGE && b->zc_read_fallback_served == 1);
	assert(!a->rx_restarts && !a->qp->zc_read_armed && !a->qp->zc_expect);
	release(a); release(b);
	puts("ok 30 - a fallback arriving mid-post finishes the zero-copy reply, the zone fills, then it is served staged");
}
/* zc_read_fault (diagnostic) forces every Nth zero-copy READ to time out with the
 * tail of its reply still live -- the precondition of every hardware stall. Only
 * the selected operation is faulted; it must still complete exactly once,
 * byte-for-byte, through the staged fallback. */
static void read_until(struct sn_device *a, struct sn_device *b, unsigned int sends)
{
	unsigned int i;

	for (i = 0; i < 400 && a->sends < sends && !a->errors; i++) {
		flush_inflight(b);
		step(a, b, NULL, NULL);
	}
	if (a->sends != sends || a->errors || b->errors)
		fprintf(stderr, "read_until(%u): sends=%u a.err=%u(last %d) b.err=%u fb=%llu served=%llu wait_drain=%llu stranded=%d\n", sends, a->sends, a->errors, a->last_error, b->errors, (unsigned long long)a->zc_read_fallback, (unsigned long long)b->zc_read_fallback_served, (unsigned long long)b->zc_fb_wait_drain, b->zc_tx_stranded);
	assert(a->sends == sends && !a->errors && !b->errors);
}
static void test_zc_read_fault_injection(void)
{
	struct sn_device *b, *a = zc_pair(&b);

	sn_zc_read_fault = 2;
	b->zc_defer = SN_MESSAGE / 16;
	memset(b->mr[1].data, 0x9e, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	read_until(a, b, 1);			/* first READ: not selected */
	assert(!a->zc_read_fault_injected && !a->zc_read_fallback);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	memset(b->mr[1].data, 0x4b, SN_MESSAGE);
	post(a, SN_READ, SN_MESSAGE);
	read_until(a, b, 2);			/* second READ: faulted near its tail */
	sn_zc_read_fault = 0;
	assert(a->zc_read_fault_injected == 1 && a->zc_read_fallback == 1);
	assert(b->zc_read_fallback_served == 1);
	assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
	release(a); release(b);
	puts("ok 31 - zc_read_fault forces a READ timeout with its tail live; the staged fallback completes it once");
}
/* Round nine on hardware: after a served fallback the requester's staged completion
 * restarted its data RX ring alone, and the responder's next burst then sat in its
 * TX ring -- 63 frames posted against 63 posted RX buffers, never sent -- until
 * both ends were restarted. Under e2e_model that restart starves the peer the same
 * way. The requester must never need it: the zero-copy READ after a fallback
 * completes, and no data RX ring is ever restarted. */
static void test_zc_read_fallback_keeps_ring_usable(void)
{
	struct sn_device *b, *a = zc_pair(&b);
	unsigned int i;

	a->e2e_model = b->e2e_model = true;
	sn_zc_read_fault = 2;
	b->zc_defer = SN_MESSAGE / 16;
	for (i = 1; i <= 3; i++) {
		memset(b->mr[1].data, 0x20 + i, SN_MESSAGE);
		post(a, SN_READ, SN_MESSAGE);
		read_until(a, b, i);	/* READ 2 is faulted near its tail */
		assert(!memcmp(a->mr[0].data, b->mr[1].data, SN_MESSAGE));
		if (i == 2) sn_zc_read_fault = 0;
	}
	assert(a->zc_read_fault_injected == 1 && a->zc_read_fallback == 1);
	assert(b->zc_read_fallback_served == 1);
	assert(!a->rx_restarts && !b->rx_restarts && !b->zc_tx_starved);
	release(a); release(b);
	puts("ok 32 - after a served fallback the next zero-copy READ completes: no data RX ring restart");
}
int main(void)
{
	puts("1..32");
	test_send(); test_write_read(); test_failures(); test_gating_window();
	test_control_overflow(); test_handshake();
	test_zc(); test_zc_rnr(); test_zc_retry_ready(); test_zc_retry_burst();
	test_zc_fallback();
	test_zc_write(); test_zc_write_retry_ready(); test_zc_write_retry_burst();
	test_zc_write_fallback(); test_zc_write_refusal();
	test_zc_retry_waits_for_drain();
	test_zc_read(); test_zc_read_retry_burst(); test_zc_read_retry_waits_for_drain();
	test_zc_read_fallback(); test_zc_read_refusal();
	test_zc_read_serializes_inbound_bursts();
	test_zc_read_lost_ack_is_fatal();
	test_zc_read_bidirectional();
	test_zc_burst_inflight_overlap();
	test_zc_read_timeout_staged_fallback();
	test_zc_read_cross_path_identity();
	test_zc_read_never_lands();
	test_zc_read_fallback_midpost_finishes();
	test_zc_read_fault_injection();
	test_zc_read_fallback_keeps_ring_usable();
	assert(!warnings);
	return 0;
}
