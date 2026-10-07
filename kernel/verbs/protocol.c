// SPDX-License-Identifier: GPL-2.0-only
#include "strix_nhi.h"

/* Session handshake is wire-carried over the native rings. Static discovery
 * uses only immediate service properties (rxhop); epochs/generations in the
 * frames themselves reject stale sessions. HELLO/HELLO_ACK establish the
 * reciprocal readiness echoes; BIND/BIND_ACK pair the QP exactly like the
 * retired property snapshot did, with the generation echo rejecting stale
 * pairings. Handshake frames are best-effort: timers retransmit them. */
static void sn_session_reset(struct sn_device *d)
{
	struct sn_qp *q = d->qp;

	d->ready.peer_epoch = 0;
	d->ready.peer_ready = 0;
	d->ready.local_echo = 0;
	d->ready.peer_echo = 0;
	d->control_count = 0;
	if (q && q->bound) sn_qp_error(q, IB_WC_GENERAL_ERR);
}
static void sn_control_queue(struct sn_device *d, const struct sn_header *h)
{
	unsigned int i;

	/* One pending entry per handshake opcode; a newer echo REPLACES a stale
	 * one. ACKs keep their identity coalescing in sn_ack(). Overflow just
	 * drops: timers retry. */
	for (i = 0; i < d->control_count; i++) {
		struct sn_header *old = &d->control[(d->control_head + i) % ARRAY_SIZE(d->control)];
		if (old->opcode == h->opcode) { *old = *h; return; }
	}
	if (d->control_count == ARRAY_SIZE(d->control)) return;
	d->control[(d->control_head + d->control_count++) % ARRAY_SIZE(d->control)] = *h;
}
static void sn_receive_hello(struct sn_device *d, const struct sn_header *h)
{
	struct sn_header r = {0};

	if (!sn_local_ready(&d->ready)) return;
	if (d->ready.peer_epoch && d->ready.peer_epoch != h->src_epoch)
		sn_session_reset(d);
	d->ready.peer_epoch = h->src_epoch;
	d->ready.local_echo = h->src_epoch;
	r.opcode = SN_HELLO_ACK; r.src_epoch = d->ready.local_epoch;
	r.dst_epoch = h->src_epoch; r.total = SN_CAPS_V1;
	sn_control_queue(d, &r);
}
static void sn_receive_hello_ack(struct sn_device *d, const struct sn_header *h)
{
	struct sn_header r = {0};

	if (!sn_local_ready(&d->ready)) return;
	if (h->dst_epoch != d->ready.local_epoch) return;
	/* A valid echo proves the peer's current epoch. Adopt it and refresh our
	 * own echo so a session where only one side reset still converges. */
	if (d->ready.peer_epoch != h->src_epoch) {
		if (d->ready.peer_epoch && d->ready.peer_echo) sn_session_reset(d);
		d->ready.peer_epoch = h->src_epoch;
	}
	if (d->ready.local_echo != h->src_epoch) {
		d->ready.local_echo = h->src_epoch;
		r.opcode = SN_HELLO_ACK; r.src_epoch = d->ready.local_epoch;
		r.dst_epoch = h->src_epoch; r.total = SN_CAPS_V1;
		sn_control_queue(d, &r);
	}
	d->ready.peer_echo = d->ready.local_epoch;
	d->ready.peer_ready = 1;
}
static void sn_receive_bind(struct sn_device *d, const struct sn_header *h, bool ack)
{
	struct sn_qp *q = d->qp;
	struct sn_header r = {0};

	if (!q || q->destroying || q->attr.qp_state != IB_QPS_RTS ||
	    !sn_ready(&d->ready)) return;
	if (h->src_epoch != d->ready.peer_epoch ||
	    h->dst_epoch != d->ready.local_epoch) return;
	/* Cross-check the pairing exactly like the retired property snapshot. */
	if (h->dst_qpn != q->ib.qp_num || h->src_qpn != q->attr.dest_qp_num ||
	    h->sequence != q->attr.rq_psn || h->ack_sequence != q->attr.sq_psn)
		return;
	if (q->bound && q->binding.peer_generation != h->src_generation) {
		/* The peer recreated its QP; never silently re-pair. */
		d->control_count = 0;
		sn_qp_error(q, IB_WC_GENERAL_ERR);
		return;
	}
	if (ack) {
		/* Only an echo of our current generation may complete the pair. */
		if (h->dst_generation != q->binding.local_generation) return;
	} else {
		r.opcode = SN_BIND_ACK; r.src_qpn = q->ib.qp_num;
		r.dst_qpn = q->attr.dest_qp_num;
		r.src_generation = q->binding.local_generation;
		r.dst_generation = h->src_generation;
		r.src_epoch = d->ready.local_epoch; r.dst_epoch = h->src_epoch;
		r.sequence = q->attr.sq_psn; r.ack_sequence = q->attr.rq_psn;
		sn_control_queue(d, &r);
	}
	if (!q->bound) {
		q->binding.local_epoch = d->ready.local_epoch;
		q->binding.peer_epoch = d->ready.peer_epoch;
		q->binding.peer_generation = h->src_generation;
		q->binding.peer_qpn = h->src_qpn;
		q->bound = true;
	}
}

static struct sn_header sn_out_header(struct sn_qp *q, u32 opcode)
{
	struct sn_header h = {0};

	h.opcode = opcode;
	h.src_qpn = q->binding.local_qpn; h.dst_qpn = q->binding.peer_qpn;
	h.src_generation = q->binding.local_generation; h.dst_generation = q->binding.peer_generation;
	h.src_epoch = q->binding.local_epoch; h.dst_epoch = q->binding.peer_epoch;
	return h;
}
/* Returns nonzero after a full control queue failed the QP; the flush has
 * already retired every SQ/RQ WQE, so callers must not touch them again. */
static int sn_ack(struct sn_qp *q, u32 opcode, u64 sequence, u32 status)
{
	struct sn_device *d = sn_dev(q->ib.device);
	struct sn_header h = sn_out_header(q, opcode);
	unsigned int i;

	h.ack_sequence = sequence; h.status = status;
	for (i = 0; i < d->control_count; i++) {
		struct sn_header *old = &d->control[(d->control_head + i) % ARRAY_SIZE(d->control)];
		if (old->opcode == opcode && old->ack_sequence == sequence) {
			*old = h;
			return 0;
		}
	}
	if (d->control_count == ARRAY_SIZE(d->control)) {
		sn_qp_error(q, IB_WC_GENERAL_ERR);
		return -ENOSPC;
	}
	d->control[(d->control_head + d->control_count++) % ARRAY_SIZE(d->control)] = h;
	return 0;
}
void sn_engine_reset(struct sn_qp *q)
{
	sn_mr_put(q->remote_mr); q->remote_mr = NULL;
	q->active = q->response_active = q->outgoing_sent = q->response_sent = false;
	q->have_last_read = q->rnr_wait = q->credit_sent = false;
	memset(&q->receive, 0, sizeof(q->receive));
	memset(&q->read_receive, 0, sizeof(q->read_receive));
}
static void sn_send_complete(struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];
	int ret;

	if (WARN_ON_ONCE(!q->sq_count || q->attr.qp_state != IB_QPS_RTS))
		return;
	ret = sn_complete(q, w, false, IB_WC_SUCCESS, w->opcode == SN_READ ? w->length : 0);
	sn_mr_put(w->mr); w->mr = NULL;
	q->sq_head = (q->sq_head + 1) % SN_QUEUE_DEPTH; q->sq_count--;
	q->next_sequence++;
	q->active = q->outgoing_sent = q->rnr_wait = false;
	q->progress_deadline = ktime_get_ns() + 30000000000ULL;
	if (ret) sn_qp_error(q, IB_WC_GENERAL_ERR);
}
static void sn_receive_ack(struct sn_qp *q, const struct sn_header *h)
{
	if (h->opcode == SN_READ_ACK) {
		if (q->response_active && h->ack_sequence == q->read_request.sequence)
			q->response_active = false;
		return;
	}
	if (!q->active || !q->sq_count || h->ack_sequence != q->next_sequence)
		return;
	if (h->status == SN_RNR) {
		if (q->sq[q->sq_head].opcode != SN_SEND || q->rnr_wait) return;
		if (!sn_retry_take(&q->rnr_retries, q->attr.rnr_retry)) {
			sn_qp_error(q, IB_WC_RNR_RETRY_EXC_ERR); return;
		}
		q->rnr_wait = true;
		q->outgoing_sent = true;
		q->deadline = ktime_get_ns() + sn_rnr_timeout_ns(q->attr.min_rnr_timer);
	} else if (h->status != SN_OK) {
		sn_qp_error(q, h->status == SN_ACCESS ? IB_WC_REM_ACCESS_ERR : IB_WC_REM_INV_REQ_ERR);
	} else if (q->sq[q->sq_head].opcode != SN_READ) {
		sn_send_complete(q);
	}
}
static void sn_receive_read_reply(struct sn_qp *q, const struct sn_header *h, const u8 *payload)
{
	struct sn_wqe *w;
	enum sn_fragment_result result;

	if (q->have_last_read && h->sequence == q->last_read && h->total == q->last_read_length) {
		sn_ack(q, SN_READ_ACK, h->sequence, SN_OK);
		return;
	}
	if (!q->active || !q->sq_count || h->sequence != q->next_sequence) return;
	w = &q->sq[q->sq_head];
	if (w->opcode != SN_READ || h->total != w->length) {
		sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
	}
	result = sn_fragment(&q->read_receive, h);
	if (result == SN_FRAGMENT_INVALID) {
		sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
	}
	if (result != SN_FRAGMENT_START && result != SN_FRAGMENT_APPEND) return;
	memcpy(q->read_incoming + h->offset, payload, h->length);
	sn_fragment_commit(&q->read_receive, h);
	if (q->read_receive.received != h->total) return;
	if (sn_mr_copy(w->mr, w->address, q->read_incoming, w->length, true)) {
		sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
	}
	sn_dev(q->ib.device)->read_bytes += h->total;
	q->last_read = h->sequence; q->last_read_length = h->total; q->have_last_read = true;
	if (sn_ack(q, SN_READ_ACK, h->sequence, SN_OK)) return;
	sn_send_complete(q);
}
static void sn_request_error(struct sn_qp *q, const struct sn_header *h, u32 status)
{
	/* Retain the rejection, just like placement, so retry cannot change it. */
	if (!q->receive.active) sn_fragment_commit(&q->receive, h);
	sn_operation_commit(&q->receive, status);
	sn_ack(q, SN_ACK, h->sequence, status);
	sn_mr_put(q->remote_mr); q->remote_mr = NULL;
}
static void sn_receive_request(struct sn_qp *q, const struct sn_header *h, const u8 *payload)
{
	enum sn_fragment_result result = sn_fragment(&q->receive, h);
	struct sn_wqe *w;
	int ret;

	if (result == SN_FRAGMENT_REPLAY) {
		if (h->opcode == SN_READ && q->receive.replay_status == SN_OK) {
			/* Never resample a live MR for a duplicate READ. */
			if (!q->response_active || q->response_sent) {
				q->response_active = true; q->response_sent = false;
				q->response_offset = 0;
			}
		} else sn_ack(q, SN_ACK, h->sequence, q->receive.replay_status);
		return;
	}
	if (result == SN_FRAGMENT_INVALID) {
		sn_ack(q, SN_ACK, h->sequence, SN_PROTOCOL);
		sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
	}
	if (result != SN_FRAGMENT_START && result != SN_FRAGMENT_APPEND) return;
	if (result == SN_FRAGMENT_START) {
		q->assembly_deadline = ktime_get_ns() + 30000000000ULL;
		/* A serial peer's next operation implicitly acknowledges its previous
		 * READ response, allowing the single retained snapshot to advance. */
		q->response_active = false;
		if (h->opcode == SN_SEND) {
			if (!q->rq_count) { sn_ack(q, SN_ACK, h->sequence, SN_RNR); return; }
			w = &q->rq[q->rq_head];
			if (h->total > w->length) {
				sn_complete(q, w, true, IB_WC_LOC_LEN_ERR, 0);
				sn_mr_put(w->mr); w->mr = NULL;
				q->rq_head = (q->rq_head + 1) % SN_QUEUE_DEPTH; q->rq_count--;
				sn_request_error(q, h, SN_LENGTH);
				sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
			}
		} else {
			u32 access = h->opcode == SN_READ ? IB_ACCESS_REMOTE_READ : IB_ACCESS_REMOTE_WRITE;
			if (!(q->attr.qp_access_flags & access) ||
			    (h->opcode == SN_READ && !q->attr.max_dest_rd_atomic)) {
				sn_request_error(q, h, SN_ACCESS); return;
			}
			q->remote_mr = sn_mr_get(q, h->rkey, h->address, h->total, access);
			if (!q->remote_mr) { sn_request_error(q, h, SN_ACCESS); return; }
		}
		if (h->opcode == SN_READ) {
			ret = sn_mr_copy(q->remote_mr, h->address, q->read_snapshot, h->total, false);
			sn_mr_put(q->remote_mr); q->remote_mr = NULL;
			if (ret) { sn_request_error(q, h, SN_ACCESS); return; }
			q->read_request = *h;
			q->response_active = true; q->response_sent = false;
			q->response_offset = q->response_retries = 0;
			q->response_deadline = ktime_get_ns() + 30000000000ULL;
			sn_fragment_commit(&q->receive, h);
			sn_operation_commit(&q->receive, SN_OK);
			return;
		}
	}
	memcpy(q->incoming + h->offset, payload, h->length);
	sn_fragment_commit(&q->receive, h);
	if (q->receive.received != h->total) return;
	if (h->opcode == SN_SEND) {
		w = &q->rq[q->rq_head];
		ret = sn_mr_copy(w->mr, w->address, q->incoming, h->total, true);
		if (!ret) ret = sn_complete(q, w, true, IB_WC_SUCCESS, h->total);
		sn_mr_put(w->mr); w->mr = NULL;
		q->rq_head = (q->rq_head + 1) % SN_QUEUE_DEPTH; q->rq_count--;
	} else {
		ret = sn_mr_copy(q->remote_mr, h->address, q->incoming, h->total, true);
		sn_mr_put(q->remote_mr); q->remote_mr = NULL;
	}
	if (ret) {
		sn_request_error(q, h, SN_ACCESS);
		sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
	}
	if (h->opcode == SN_SEND) sn_dev(q->ib.device)->send_bytes += h->total;
	else sn_dev(q->ib.device)->write_bytes += h->total;
	sn_operation_commit(&q->receive, SN_OK);
	sn_ack(q, SN_ACK, h->sequence, SN_OK);
}
void sn_engine_receive(struct sn_device *d, const void *frame, size_t bytes)
{
	struct sn_qp *q = d->qp;
	struct sn_header h;

	if (sn_decode(&h, frame, bytes)) {
		d->bad_frames++;
		return;
	}
	switch (h.opcode) {
	case SN_HELLO: sn_receive_hello(d, &h); return;
	case SN_HELLO_ACK: sn_receive_hello_ack(d, &h); return;
	case SN_BIND: sn_receive_bind(d, &h, false); return;
	case SN_BIND_ACK: sn_receive_bind(d, &h, true); return;
	}
	if (!q || !q->bound || q->attr.qp_state != IB_QPS_RTS || !sn_ready(&d->ready)) return;
	if (!sn_admit(&q->binding, &h)) {
		d->bad_frames++;
		return;
	}
	switch (h.opcode) {
	case SN_ACK: case SN_READ_ACK: sn_receive_ack(q, &h); break;
	case SN_READ_REPLY: sn_receive_read_reply(q, &h, frame + SN_HEADER); break;
	case SN_SEND: case SN_WRITE: case SN_READ:
		sn_receive_request(q, &h, frame + SN_HEADER); break;
	case SN_CREDIT:
		/* Credits are only hints; RQ ownership is checked on actual SEND. */
		break;
	}
}
static int sn_fragments_send(struct sn_device *d, struct sn_header *h, const u8 *data,
			     u32 *offset, bool reply)
{
	int ret;

	do {
		h->offset = *offset;
		h->length = min_t(u32, h->total - *offset, SN_PAYLOAD);
		ret = sn_ring_send(d, h, data + *offset, reply);
		if (ret) return ret;
		*offset += h->length;
	} while (*offset < h->total);
	return 0;
}
void sn_engine_progress(struct sn_device *d)
{
	struct sn_qp *q = d->qp;
	struct sn_header h;
	struct sn_wqe *w;
	u64 now = ktime_get_ns();
	int ret;

	/* Retransmit HELLO until the reciprocal echoes complete the session. */
	if (!d->dead && sn_local_ready(&d->ready) && !sn_ready(&d->ready) &&
	    now >= d->next_hello) {
		h = (struct sn_header){0};
		h.opcode = SN_HELLO; h.src_epoch = d->ready.local_epoch;
		h.dst_epoch = d->ready.peer_epoch; h.total = SN_CAPS_V1;
		sn_ring_send(d, &h, NULL, true);
		d->next_hello = now + SN_HELLO_INTERVAL_NS;
	}
	/* Handshake control (HELLO_ACK/BIND) must drain before full readiness;
	 * data ACKs that cannot send yet stay queued. */
	while (d->control_count) {
		h = d->control[d->control_head];
		ret = sn_ring_send(d, &h, NULL, true);
		if (ret) break;
		d->control_head = (d->control_head + 1) % ARRAY_SIZE(d->control);
		d->control_count--;
	}
	if (!sn_ready(&d->ready) || d->dead) goto watchdog;
	/* Retransmit BIND while the QP waits to pair; sn_ready is required. */
	if (q && !q->bound && !q->destroying && q->attr.qp_state == IB_QPS_RTS &&
	    now >= q->next_bind) {
		h = (struct sn_header){0};
		h.opcode = SN_BIND; h.src_qpn = q->ib.qp_num;
		h.dst_qpn = q->attr.dest_qp_num;
		h.src_generation = q->binding.local_generation;
		h.src_epoch = d->ready.local_epoch; h.dst_epoch = d->ready.peer_epoch;
		h.sequence = q->attr.sq_psn; h.ack_sequence = q->attr.rq_psn;
		sn_control_queue(d, &h);
		q->next_bind = now + SN_BIND_INTERVAL_NS;
	}
	if (!q || !q->bound || q->attr.qp_state != IB_QPS_RTS) goto watchdog;
	if (q->receive.active && now >= q->assembly_deadline) {
		sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
	}
	if (!q->credit_sent || q->last_credit != q->rq_count) {
		if (q->credit_revision == U64_MAX) {
			sn_qp_error(q, IB_WC_GENERAL_ERR); return;
		}
		h = sn_out_header(q, SN_CREDIT);
		h.sequence = q->credit_revision + 1; h.total = q->rq_count;
		if (!sn_ring_send(d, &h, NULL, true)) {
			q->credit_revision++; q->last_credit = q->rq_count;
			q->credit_sent = true;
		}
	}
	if (q->response_active) {
		if (q->response_sent && now >= q->response_deadline) {
			if (!sn_retry_take(&q->response_retries, q->attr.retry_cnt)) {
				sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
			}
			q->response_sent = false; q->response_offset = 0;
			q->response_deadline = now + 30000000000ULL;
		}
		if (!q->response_sent) {
			h = sn_out_header(q, SN_READ_REPLY);
			h.sequence = q->read_request.sequence; h.total = q->read_request.total;
			ret = sn_fragments_send(d, &h, q->read_snapshot, &q->response_offset, true);
			if (!ret) {
				q->response_sent = true;
				q->response_deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
			} else if (ret != -EAGAIN || now >= q->response_deadline) {
				sn_qp_error(q, IB_WC_GENERAL_ERR); return;
			}
		}
	}
	if (!q->sq_count) return;
	w = &q->sq[q->sq_head];
	if (!q->active) {
		if (q->next_sequence == U64_MAX) { sn_qp_error(q, IB_WC_GENERAL_ERR); return; }
		if (w->opcode != SN_READ && sn_mr_copy(w->mr, w->address, q->outgoing, w->length, false)) {
			sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
		}
		q->active = true; q->outgoing_sent = false;
		q->outgoing_offset = q->retries = q->rnr_retries = 0;
		memset(&q->read_receive, 0, sizeof(q->read_receive));
		q->read_receive.expected = q->next_sequence;
	}
	if (q->outgoing_sent) {
		if (now < q->deadline) return;
		if (!q->rnr_wait && !sn_retry_take(&q->retries, q->attr.retry_cnt)) {
			sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
		}
		q->rnr_wait = q->outgoing_sent = false;
		q->outgoing_offset = 0;
	}
	h = sn_out_header(q, w->opcode);
	h.sequence = q->next_sequence; h.total = w->length;
	h.address = w->remote_address; h.rkey = w->rkey;
	if (w->opcode == SN_READ) ret = sn_ring_send(d, &h, NULL, false);
	else ret = sn_fragments_send(d, &h, q->outgoing, &q->outgoing_offset, false);
	if (!ret) {
		q->outgoing_sent = true;
		q->deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
	} else if (ret != -EAGAIN) sn_qp_error(q, IB_WC_GENERAL_ERR);
watchdog:
	if (q && ((q->sq_count && now >= q->progress_deadline) ||
	    (q->receive.active && now >= q->assembly_deadline) ||
	    (q->response_active && !sn_ready(&d->ready) && now >= q->response_deadline)))
		sn_qp_error(q, IB_WC_RETRY_EXC_ERR);
}
