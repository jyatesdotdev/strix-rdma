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
	u64 zc_bursts, zc_timeouts, zc_aborts;
	bool check_ids;
	u32 caps;
	/* Zero-copy model state: peer link, expected-burst landing zone, drop knob. */
	bool peer_zc, zdata, zc_drop;
	struct sn_device *zc_peer;
	struct sn_mr *zc_mr;
	u64 zc_addr;
	u32 zc_len, zc_landed;
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
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q, struct sn_wqe *w);
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q);
void sn_data_abort(struct sn_device *d);
#include "../verbs/protocol.c"
/* Modeled zero-copy data plane: the sender copies payload from the active SQ
 * WQE's MR straight into the peer's expected-burst landing zone and feeds the
 * engine's byte counter; zc_drop simulates a burst lost on the wire. */
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];
	struct sn_device *p = d->zc_peer;
	u32 off = q->zc_offset, n = w->length - off;

	if (d->zc_drop) { q->zc_offset = w->length; return 0; }
	assert(p && p->zc_mr && n <= p->zc_len - p->zc_landed);
	memcpy(p->zc_mr->data + (p->zc_addr - p->zc_mr->base) + p->zc_landed,
	       w->mr->data + (w->address - w->mr->base) + off, n);
	p->zc_landed += n;
	sn_engine_zc_bytes(p->qp, n);
	q->zc_offset = w->length;
	return 0;
}
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q, struct sn_wqe *w)
{
	d->zc_mr = w->mr; d->zc_addr = w->address;
	d->zc_len = q->zc_total; d->zc_landed = 0;
	return 0;
}
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q)
{
	(void)q;
	d->zc_landed = 0;
	return 0;
}
void sn_data_abort(struct sn_device *d) { (void)d; }
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
	q->attr.qp_access_flags = 0; /* DS4 INIT access flags */
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
	assert(!b->zc_mr);
	release(a); release(b);
	puts("ok 11 - no ZDATA capability: large SEND stays on the staged path");
}
int main(void)
{
	puts("1..11");
	test_send(); test_write_read(); test_failures(); test_gating_window();
	test_control_overflow(); test_handshake();
	test_zc(); test_zc_rnr(); test_zc_retry_ready(); test_zc_retry_burst();
	test_zc_fallback();
	assert(!warnings);
	return 0;
}
