// SPDX-License-Identifier: GPL-2.0-only
#include "strix_nhi.h"

/* zc_send_fault modes (sn_qp.zc_send_fault); 0 is no fault. */
#define SN_FAULT_READY_LOST	1
#define SN_FAULT_READY_LATE	2
/* sn_stall_snapshot() status for a zero-copy ACK/retransmit timeout. */
#define SN_SNAP_TIMEOUT		(-1)

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
	d->peer_zc = false;
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
	d->peer_zc = !!(h->total & SN_CAP_ZDATA);
	r.opcode = SN_HELLO_ACK; r.src_epoch = d->ready.local_epoch;
	r.dst_epoch = h->src_epoch; r.total = d->caps;
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
	d->peer_zc = !!(h->total & SN_CAP_ZDATA);
	if (d->ready.local_echo != h->src_epoch) {
		d->ready.local_echo = h->src_epoch;
		r.opcode = SN_HELLO_ACK; r.src_epoch = d->ready.local_epoch;
		r.dst_epoch = h->src_epoch; r.total = d->caps;
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
/* Large SENDs, WRITEs and READs go header-less on the data ring when the peer
 * advertised SN_CAP_ZDATA; everything else uses the staged control-ring path.
 * A header-less frame never spans a page, so the local source must be frame
 * aligned; a WRITE must also name a frame-aligned remote destination, because
 * the responder posts that MR's pages directly, and a READ a frame-aligned
 * remote source, because the responder bursts from it directly. An unaligned
 * WRITE or READ stays on the staged path rather than being rejected. */
static bool sn_zc_eligible(struct sn_device *d, struct sn_wqe *w)
{
	if (!d->zdata || !d->peer_zc || w->length <= SN_ZC_THRESHOLD ||
	    (w->address & (SN_FRAME - 1)))
		return false;
	if (w->opcode == SN_SEND)
		return true;
	/* READ honours the zc_read kill-switch, latched into d->zc_read when the data
	 * plane comes up (default on). The local address checked
	 * above is the landing zone the requester arms itself; the remote address is the
	 * source the responder bursts from, so it must be frame aligned for the same
	 * reason a WRITE's destination must be. */
	if (w->opcode == SN_READ)
		return d->zc_read && !(w->remote_address & (SN_FRAME - 1));
	/* WRITE honours the zc_write kill-switch, latched into d->zc_write when the
	 * data plane comes up. A replayed SEND is harmless because it lands in a
	 * receive WQE the peer already consumed; a replayed WRITE lands in live user
	 * memory, so the retry path waits for the data TX ring to drain before
	 * restarting a burst and an overshooting byte count fails the QP. With those
	 * two guards the placement is verified byte-for-byte on hardware. */
	return w->opcode == SN_WRITE && d->zc_write &&
	       !(w->remote_address & (SN_FRAME - 1));
}
/* Returns nonzero after a full control queue failed the QP; the flush has
 * already retired every SQ/RQ WQE, so callers must not touch them again. */
static int sn_ack(struct sn_qp *q, u32 opcode, u64 sequence, u32 status)
{
	struct sn_device *d = sn_dev(q->ib.device);
	struct sn_header h = sn_out_header(q, opcode);
	unsigned int i;

	if (opcode == SN_ACK || opcode == SN_READ_ACK) {
		/* ack_drop: lose every Nth ACK/READ_ACK as if on the wire. One read:
		 * the parameter is writable at runtime. */
		unsigned int every = READ_ONCE(sn_ack_drop);

		if (every && !(++d->ack_seen % every)) {
			d->ack_dropped++;
			return 0;
		}
	}
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
	q->zc_state = SN_ZC_NONE; q->zc_offset = 0;
	q->zc_expect = false; q->zc_total = q->zc_received = 0;
	q->zc_mr = NULL; q->zc_address = 0;
	q->zc_read_armed = false; q->zc_held = false;
	q->zc_read_staged = false; q->response_fallback = false;
	q->zc_read_reply_ready = false;
	q->zc_send_fault = 0; q->zc_ready_held = false; q->send_staged = false;
	memset(&q->receive, 0, sizeof(q->receive));
	memset(&q->read_receive, 0, sizeof(q->read_receive));
	/* Reclaim the data rings so this aborted burst cannot leave posted RX
	 * buffers or stale TX payload behind to deadlock later bursts. */
	sn_data_abort(sn_dev(q->ib.device));
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
	q->zc_state = SN_ZC_NONE; q->zc_offset = 0;
	q->progress_deadline = ktime_get_ns() + 30000000000ULL;
	if (ret) sn_qp_error(q, IB_WC_GENERAL_ERR);
}
/* True while a zero-copy READ response owns the data TX ring: from the moment
 * the request is accepted until the READ_ACK, or the next operation that
 * implicitly acknowledges it, releases the retained source MR. */
static bool sn_response_zc(const struct sn_qp *q)
{
	return q->response_active && q->read_request.opcode == SN_ZC_READ;
}
/* May a zero-copy response burst start queueing now? Never while an SQ burst is
 * still queueing and never on top of frames still queued: the peer's inbound
 * byte stream is header-less, so two bursts interleaved there could not be
 * separated, and frames from a superseded attempt cannot be recalled. This is
 * the guard zero-copy WRITE needed retrofitted; a response starts behind it. */
static bool sn_response_may_burst(struct sn_device *d, struct sn_qp *q)
{
	return q->zc_state != SN_ZC_SENDING && sn_data_tx_drained(d);
}
/* Release the source MR a zero-copy READ response retains. The staged path
 * snapshots into read_snapshot and drops the reference at once; the zero-copy
 * path bursts from the MR itself, so it is held until the READ_ACK proves the
 * peer stopped reading it. Both the ACK and the next operation release it, since
 * a serial peer's next operation implicitly acknowledges the previous response. */
static void sn_response_release(struct sn_qp *q)
{
	if (q->read_request.opcode == SN_ZC_READ) {
		sn_mr_put(q->remote_mr);
		q->remote_mr = NULL;
	}
}
/* The receiver refused this zero-copy SEND (SN_STAGED): its receive buffer is not
 * frame aligned. Resend the same operation, same sequence, on the staged path.
 * Only a SEND still waiting for ZC_READY can be redirected -- nothing has been
 * burst -- so a repeated or late refusal is ignored. */
static void sn_zc_send_fallback(struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];

	if (w->opcode != SN_SEND || q->send_staged || q->zc_state != SN_ZC_WAIT)
		return;
	if (sn_mr_copy(w->mr, w->address, q->outgoing, w->length, false)) {
		sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
	}
	q->send_staged = true;
	sn_dev(q->ib.device)->zc_send_fallback++;
	q->zc_state = SN_ZC_NONE;
	q->outgoing_sent = false; q->outgoing_offset = 0;
}
static void sn_receive_ack(struct sn_qp *q, const struct sn_header *h)
{
	if (h->opcode == SN_READ_ACK) {
		if (q->response_active && h->ack_sequence == q->read_request.sequence) {
			q->response_active = false;
			sn_response_release(q);
		}
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
	} else if (h->status == SN_STAGED) {
		sn_zc_send_fallback(q);
	} else if (h->status != SN_OK) {
		sn_qp_error(q, h->status == SN_ACCESS ? IB_WC_REM_ACCESS_ERR : IB_WC_REM_INV_REQ_ERR);
	} else if (q->sq[q->sq_head].opcode != SN_READ) {
		sn_send_complete(q);
	}
}
/* Complete the active READ from its fully assembled staged reply. A zero-copy
 * READ that fell back gets here only once its landing zone has also filled: the
 * zone is then disarmed with every posted data RX buffer consumed, so nothing is
 * left to reclaim. The data RX ring is never restarted here. Hardware round nine
 * showed that a one-sided restart starves the peer's data TX of E2E credits: its
 * next burst sits in the TX ring against posted RX buffers and is never sent, so
 * the next zero-copy READ fails. */
static void sn_read_reply_finish(struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];

	if (sn_mr_copy(w->mr, w->address, q->read_incoming, w->length, true)) {
		sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
	}
	if (q->zc_read_armed) {
		q->zc_expect = false; q->zc_read_armed = false; q->zc_read_staged = false;
		q->zc_mr = NULL; q->zc_address = 0;
	}
	q->zc_read_reply_ready = false;
	sn_dev(q->ib.device)->read_bytes += w->length;
	q->last_read = q->next_sequence; q->last_read_length = w->length;
	q->have_last_read = true;
	if (sn_ack(q, SN_READ_ACK, q->next_sequence, SN_OK)) return;
	sn_send_complete(q);
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
	if (q->zc_read_armed && q->zc_received < q->zc_total) {
		/* The staged reply of a zero-copy READ that fell back is complete, but its
		 * abandoned burst has not finished landing. The responder posts that burst
		 * in full and drains it before serving this reply, so the rest is already on
		 * its way; sn_engine_zc_bytes() finishes the READ when the last byte lands. */
		q->zc_read_reply_ready = true;
		return;
	}
	sn_read_reply_finish(q);
}
static void sn_request_error(struct sn_qp *q, const struct sn_header *h, u32 status)
{
	/* Retain the rejection, just like placement, so retry cannot change it. */
	if (!q->receive.active) sn_fragment_commit(&q->receive, h);
	sn_operation_commit(&q->receive, status);
	sn_ack(q, SN_ACK, h->sequence, status);
	sn_mr_put(q->remote_mr); q->remote_mr = NULL;
}
/* A duplicate READ request means the requester never saw the response, so burst
 * it again -- exactly what the staged path does for a duplicate SN_READ. Unlike
 * the staged path this re-reads the live source MR instead of a snapshot, so a
 * retry may observe newer data: RC guarantees per-operation consistency, not a
 * snapshot held across retransmits. The MR is retained until the ACK, so it only
 * needs re-resolving when an ACK crossed this retransmit. */
/* Serve a zero-copy READ through the staged path instead: snapshot the source
 * MR, drop the retained reference exactly as the staged path always does, and
 * re-arm the response as SN_READ. The staged reply regains the snapshot
 * guarantee for any further retries. */
static int sn_response_to_staged(struct sn_device *d, struct sn_qp *q)
{
	const struct sn_header *r = &q->read_request;
	int ret;

	if (!q->remote_mr)
		q->remote_mr = sn_mr_get(q, r->rkey, r->address, r->total,
					 IB_ACCESS_REMOTE_READ);
	if (!q->remote_mr)
		return -EFAULT;
	ret = sn_mr_copy(q->remote_mr, r->address, q->read_snapshot, r->total, false);
	sn_mr_put(q->remote_mr); q->remote_mr = NULL;
	if (ret)
		return ret;
	q->read_request.opcode = SN_READ;
	q->response_fallback = false;
	q->response_active = true; q->response_sent = false;
	q->response_offset = q->response_retries = 0;
	q->response_deadline = ktime_get_ns() + 30000000000ULL;
	d->zc_read_fallback_served++;
	return 0;
}
static void sn_read_response_rearm(struct sn_device *d, struct sn_qp *q,
				  const struct sn_header *h)
{
	if (!q->remote_mr)
		q->remote_mr = sn_mr_get(q, h->rkey, h->address, h->total,
					   IB_ACCESS_REMOTE_READ);
	if (!q->remote_mr) {
		/* The source is gone. Reachable only when an ACK already completed the
		 * operation, so the requester is not waiting for a burst. */
		sn_ack(q, SN_ACK, h->sequence, SN_OK);
		return;
	}
	/* A duplicate descriptor means the requester timed out and reset its landing
	 * zone. Whether this side owes a fresh burst depends on whether the previous
	 * response has actually left the ring:
	 *
	 *  - Frames still queued: leave the response alone. The requester zeroed
	 *    zc_received when it retransmitted, so those frames are counted from zero
	 *    and complete the message. Re-bursting as well would put two full messages
	 *    into one landing zone and overshoot it -- and the requester cannot guard
	 *    against that itself, because the frames sit in OUR ring and it has no
	 *    visibility into it. This is the READ-side analogue of the WRITE replay
	 *    race with the roles reversed, which is why the WRITE drain barrier did not
	 *    cover it.
	 *  - Ring drained: the frames were delivered and the READ_ACK was lost, so the
	 *    requester is waiting for data that will never arrive. Re-burst from zero.
	 */
	if (!q->response_active || (q->response_sent && sn_data_tx_drained(d))) {
		d->zc_read_rearm++;
		q->response_active = true; q->response_sent = false;
		q->response_offset = 0;
		q->response_deadline = ktime_get_ns() + 30000000000ULL;
	} else {
		/* Split so a captured failure says WHERE the response is stuck:
		 * still being posted (the burst itself cannot get onto the data TX
		 * ring), or fully posted but the ring never drains (the NHI cannot
		 * transmit what is queued -- e.g. the peer has no RX buffer posted). */
		d->zc_read_dup_inflight++;
		if (!q->response_sent) d->zc_read_dup_posting++;
		else d->zc_read_dup_undrained++;
	}
}
/* Receiver: a zero-copy burst descriptor arrived on the control ring. The
 * payload of an SN_ZC_SEND lands header-less in the matched receive WQE's MR and
 * an SN_ZC_WRITE's in the requester-addressed remote MR; only accounting happens
 * here. An SN_ZC_READ is the reverse direction: it names a source MR this side
 * bursts from, so it commits at once and answers like a staged SN_READ. */
static void sn_receive_zc(struct sn_qp *q, const struct sn_header *h)
{
	struct sn_device *d = sn_dev(q->ib.device);
	u64 target_address = 0;
	struct sn_mr *target = NULL;
	struct sn_wqe *w;

	if (q->receive.have_replay && h->sequence == q->receive.replay.sequence) {
		if (!sn_same_operation(&q->receive.replay, h)) {
			sn_ack(q, SN_ACK, h->sequence, SN_PROTOCOL);
			sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
		}
		if (h->opcode == SN_ZC_READ && q->receive.replay_status == SN_OK) {
			sn_read_response_rearm(d, q, h);
			return;
		}
		sn_ack(q, SN_ACK, h->sequence, q->receive.replay_status);
		return;
	}
	if (h->sequence != q->receive.expected) {
		/* A silent drop: no ACK, no error. Stale sequences are benign late
		 * retransmits; an AHEAD sequence means this side lost track of a commit
		 * and will ignore every retransmit of that descriptor until the requester
		 * exhausts its retries. Count both so the two are distinguishable. */
		if (h->sequence > q->receive.expected) d->zc_seq_ahead++;
		else d->zc_seq_stale++;
		return;
	}
	if (q->receive.active) {
		/* Duplicate descriptor for the receive in progress: the sender's
		 * ZC_READY was lost or late, or it is probing for an ACK. Never restart
		 * the data RX ring here -- a one-sided restart starves the sender's data
		 * TX of E2E credits, so its burst is never sent (hardware round eleven).
		 * Nothing needs restarting: a sender bursts each operation exactly once,
		 * and only after a ZC_READY that found it waiting. With nothing landed
		 * the posted landing zone is still exactly right for that burst, so
		 * answer ZC_READY again; once bytes are landing the burst is live and
		 * completes on its own, and the ACK goes out when it does. */
		if (!sn_same_operation(&q->receive.operation, h) || !q->zc_expect) {
			sn_ack(q, SN_ACK, h->sequence, SN_PROTOCOL);
			sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
		}
		d->zc_desc_dup++;
		if (!q->zc_received)
			sn_ack(q, SN_ZC_READY, h->sequence, 0);
		return;
	}
	if (q->zc_expect && h->opcode != SN_ZC_READ) {
		/* Only one INBOUND burst at a time: the data RX landing zone is owned by
		 * the response to our own zero-copy READ, and header-less frames from two
		 * bursts could not be told apart there. Hold the request and let
		 * sn_engine_progress re-drive it the moment the response lands; the peer's
		 * own descriptor retransmit is the backstop.
		 *
		 * SN_ZC_READ is exempt and must be: answering it bursts OUT on the data TX
		 * ring and lands nothing inbound, so it does not contend for the landing
		 * zone at all. The TX ring already serialises a response burst against our
		 * own SQ burst (see the SN_ZC_SENDING case). Holding a READ request here
		 * deadlocks simultaneous bidirectional READ -- each side arms its landing
		 * zone for its own response, then holds the peer's request until that
		 * response lands, and neither ever answers. Observed on hardware as
		 * IBV_WC_RETRY_EXC_ERR on iteration 1 with zc_bursts 0 on both hosts:
		 * descriptors exchanged, no response, no bad_frames. */
		q->zc_held = true; q->zc_held_header = *h;
		return;
	}
	/* A new operation implicitly acknowledges the previous READ response, so a
	 * source MR retained for a zero-copy response is released here as well as on
	 * the READ_ACK -- before anything below reassigns q->remote_mr. */
	sn_response_release(q);
	if (h->opcode == SN_ZC_READ) {
		/* Zero-copy READ: commit at once and answer with a burst from the named
		 * source MR, so unlike the staged path the reference is retained until
		 * the READ_ACK. max_dest_rd_atomic and REMOTE_READ are enforced exactly
		 * as for a staged SN_READ; frame alignment is the requester's job
		 * (sn_zc_eligible) and is refused here so a non-conforming peer cannot
		 * make us burst unaligned frames. */
		if (!(q->attr.qp_access_flags & IB_ACCESS_REMOTE_READ) ||
		    !q->attr.max_dest_rd_atomic ||
		    (h->address & (SN_FRAME - 1))) {
			sn_request_error(q, h, SN_ACCESS); return;
		}
		q->remote_mr = sn_mr_get(q, h->rkey, h->address, h->total,
					 IB_ACCESS_REMOTE_READ);
		if (!q->remote_mr) { sn_request_error(q, h, SN_ACCESS); return; }
		q->read_request = *h;
		q->response_active = true; q->response_sent = false;
		q->response_offset = q->response_retries = 0;
		q->response_deadline = ktime_get_ns() + 30000000000ULL;
		sn_fragment_commit(&q->receive, h);
		sn_operation_commit(&q->receive, SN_OK);
		return;
	}
	if (h->opcode == SN_ZC_WRITE) {
		/* A WRITE names its own landing zone, so it consumes no receive WQE
		 * and can never be RNR. Frame alignment is the requester's job
		 * (sn_zc_eligible); refuse it here so a non-conforming peer cannot
		 * make us post unaligned burst pages. Rejection keeps the retained
		 * status, exactly like the staged WRITE path. */
		if (!(q->attr.qp_access_flags & IB_ACCESS_REMOTE_WRITE) ||
		    (h->address & (SN_FRAME - 1))) {
			sn_request_error(q, h, SN_ACCESS); return;
		}
		target = sn_mr_get(q, h->rkey, h->address, h->total,
				   IB_ACCESS_REMOTE_WRITE);
		if (!target) { sn_request_error(q, h, SN_ACCESS); return; }
		/* Owned until the burst lands or the QP is reset; the data ring may
		 * still be writing into it, so it must outlive this call. */
		q->remote_mr = target;
		target_address = h->address;
	} else {
		if (!q->rq_count) { d->rnr_sent++; sn_ack(q, SN_ACK, h->sequence, SN_RNR); return; }
		w = &q->rq[q->rq_head];
		if (w->address & (SN_FRAME - 1) && h->total <= w->length) {
			/* Legal in verbs, but header-less frames cannot land in a receive
			 * buffer that is not frame aligned, and only the receiver knows
			 * where its buffer is. Ask the sender to resend this operation on
			 * the staged path; nothing is committed and the receive stays
			 * posted for it. A repeated descriptor is refused again. */
			d->zc_send_refused++;
			sn_ack(q, SN_ACK, h->sequence, SN_STAGED);
			return;
		}
		if (h->total > w->length) {
			sn_complete(q, w, true, IB_WC_LOC_LEN_ERR, 0);
			sn_mr_put(w->mr); w->mr = NULL;
			q->rq_head = (q->rq_head + 1) % SN_QUEUE_DEPTH; q->rq_count--;
			sn_request_error(q, h, SN_LENGTH);
			sn_qp_error(q, IB_WC_REM_INV_REQ_ERR); return;
		}
		target = w->mr;
		target_address = w->address;
	}
	q->assembly_deadline = ktime_get_ns() + 30000000000ULL;
	q->response_active = false;
	q->receive.operation = *h; q->receive.received = 0; q->receive.active = 1;
	q->zc_expect = true; q->zc_total = h->total; q->zc_received = 0;
	q->zc_mr = target; q->zc_address = target_address;
	if (sn_data_recv_start(d, q)) {
		q->receive.active = 0; q->zc_expect = false;
		sn_qp_error(q, IB_WC_GENERAL_ERR); return;
	}
	sn_ack(q, SN_ZC_READY, h->sequence, 0);
}
/* Sender: the receiver posted its MR pages; the burst may start. */
static void sn_receive_zc_ready(struct sn_qp *q, const struct sn_header *h)
{
	if (q->zc_state != SN_ZC_WAIT || h->ack_sequence != q->next_sequence) return;
	if (q->zc_send_fault) {
		/* zc_send_fault: this READY is lost, or held and applied right after
		 * the descriptor retransmit (the late case). */
		sn_dev(q->ib.device)->zc_send_fault_injected++;
		q->zc_ready_held = q->zc_send_fault == SN_FAULT_READY_LATE;
		q->zc_send_fault = 0;
		return;
	}
	q->zc_state = SN_ZC_SENDING;
}
/* Data-ring drain hook: header-less payload bytes landed for the expected
 * burst. Called by the platform receive path under d->lock. */
void sn_engine_zc_bytes(struct sn_qp *q, u32 bytes)
{
	struct sn_device *d = sn_dev(q->ib.device);
	bool read = q->zc_read_armed;
	bool write = q->receive.operation.opcode == SN_ZC_WRITE;
	struct sn_wqe *w;
	int ret;

	if (!q->zc_expect) return;
	q->zc_received += bytes;
	if (q->zc_received > q->zc_total) { sn_qp_error(q, IB_WC_GENERAL_ERR); return; }
	if (q->zc_received < q->zc_total) {
		/* Bytes still landing are progress. A READ response is delivered by the
		 * peer's burst rather than by a frame we can time against, so measure the
		 * retransmit window from the last frame: without this a burst slower than
		 * one window would be restarted on top of frames still in flight. */
		if (read) {
			/* After a fallback too: the abandoned burst must land in full
			 * before the zone can be disarmed, so its frames are progress. */
			if (q->zc_read_fault && !q->zc_read_staged &&
			    q->zc_total - q->zc_received <= 32 * SN_FRAME) {
				/* zc_read_fault: time out now, with the tail of the burst
				 * still queued in the responder's data TX ring. */
				q->zc_read_fault = false;
				d->zc_read_fault_injected++;
				q->deadline = 0;
			} else {
				q->deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
			}
		}
		return;
	}
	/* The burst landed. Drop the landing zone first: either completion path can
	 * re-enter the data ring, and a retired WQE or MR must never be posted
	 * again. A WRITE placement retires the requester's remote MR and consumes
	 * no receive WQE, so it produces no receive CQE. */
	q->zc_mr = NULL; q->zc_address = 0;
	if (read && q->zc_read_staged) {
		/* Late frames of the abandoned zero-copy attempt finished landing. The
		 * staged reply owns completion; keep the zone armed so any further frame
		 * trips the overshoot check rather than completing anything. If the
		 * staged reply is already assembled, the READ completes now. */
		if (q->zc_read_reply_ready)
			sn_read_reply_finish(q);
		return;
	}
	if (read) {
		/* The landing zone is the READ WQE's own MR, synced for the CPU by the
		 * receive path just before the completing byte count, so the SQ operation
		 * completes here. The READ_ACK is what releases the responder's retained
		 * source MR; q->receive never saw this operation, so there is nothing to
		 * commit and no receive CQE. */
		q->zc_expect = false; q->zc_read_armed = false;
		d->read_bytes += q->zc_total;
		d->zc_bursts++;
		if (sn_ack(q, SN_READ_ACK, q->next_sequence, SN_OK)) return;
		sn_send_complete(q);
		return;
	}
	if (write) {
		sn_mr_put(q->remote_mr); q->remote_mr = NULL;
		d->write_bytes += q->zc_total;
	} else {
		w = &q->rq[q->rq_head];
		ret = sn_complete(q, w, true, IB_WC_SUCCESS, q->zc_total);
		sn_mr_put(w->mr); w->mr = NULL;
		q->rq_head = (q->rq_head + 1) % SN_QUEUE_DEPTH; q->rq_count--;
		if (ret) {
			sn_request_error(q, &q->receive.operation, SN_ACCESS);
			sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
		}
		d->send_bytes += q->zc_total;
	}
	d->zc_bursts++;
	sn_operation_commit(&q->receive, SN_OK);
	q->zc_expect = false;
	sn_ack(q, SN_ACK, q->receive.replay.sequence, SN_OK);
}
static void sn_receive_request(struct sn_qp *q, const struct sn_header *h, const u8 *payload)
{
	enum sn_fragment_result result = sn_fragment(&q->receive, h);

	if (h->opcode == SN_READ && result == SN_FRAGMENT_DROP)
		sn_dev(q->ib.device)->zc_read_req_drop++;
	struct sn_wqe *w;
	int ret;

	if (result == SN_FRAGMENT_REPLAY) {
		if (h->opcode == SN_READ && q->receive.replay_status == SN_OK) {
			if (q->read_request.opcode == SN_ZC_READ) {
				/* The requester abandoned the zero-copy reply and fell back to a
				 * staged SN_READ for the same operation. The response block
				 * switches it to staged once any queued burst has drained. */
				q->response_fallback = true; q->response_active = true;
				return;
			}
			sn_dev(q->ib.device)->zc_fb_reserve++;
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
		sn_response_release(q);
		if (h->opcode == SN_SEND) {
			if (!q->rq_count) { sn_dev(q->ib.device)->rnr_sent++; sn_ack(q, SN_ACK, h->sequence, SN_RNR); return; }
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
	case SN_ZC_READY: sn_receive_zc_ready(q, &h); break;
	case SN_ZC_SEND: case SN_ZC_WRITE: case SN_ZC_READ: sn_receive_zc(q, &h); break;
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
/* The zero-copy burst descriptor for an SQ SEND or WRITE. A ZC SEND lands in a
 * buffer the peer posted, so it names no remote MR; a ZC WRITE keeps address and
 * rkey so the responder places the header-less payload in the requester's MR. */
static struct sn_header sn_zc_descriptor(struct sn_qp *q, const struct sn_wqe *w)
{
	struct sn_header h = sn_out_header(q, w->opcode == SN_WRITE ? SN_ZC_WRITE : SN_ZC_SEND);

	h.sequence = q->next_sequence; h.total = w->length;
	if (h.opcode == SN_ZC_WRITE) { h.address = w->remote_address; h.rkey = w->rkey; }
	return h;
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
		h.dst_epoch = d->ready.peer_epoch; h.total = d->caps;
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
	/* An inbound burst descriptor held because the data RX landing zone was busy
	 * with our own zero-copy READ response: re-drive it as soon as the zone frees,
	 * instead of making the peer retransmit and spend its retry budget. */
	if (q->zc_held && !q->zc_expect) {
		h = q->zc_held_header;
		q->zc_held = false;
		sn_receive_zc(q, &h);
	}
	if (q->response_active) {
		if (q->response_fallback) {
			/* The requester keeps its landing zone armed until every byte of
			 * the zero-copy reply has landed, because the only other way to
			 * reclaim posted data RX buffers is a one-sided ring restart, which
			 * starves this side's TX of E2E credits (hardware round nine). So the
			 * reply is always posted in full -- started if it never was, finished
			 * if it was mid-post, by the posting path below -- and drained before
			 * the staged reply goes out. Until then the zero-copy deadline below
			 * keeps running. */
			if (!q->response_sent)
				d->zc_fb_wait_midpost++;
			else if (!sn_data_tx_drained(d)) {
				d->zc_fb_wait_drain++;
				sn_stall_snapshot(d, q, 0);
			}
			else if (sn_response_to_staged(d, q)) {
				sn_qp_error(q, IB_WC_GENERAL_ERR); return;
			}
		}
		if (q->response_sent && now >= q->response_deadline) {
			/* No READ_ACK: send the whole response again. A zero-copy response
			 * re-bursts from the live source MR, so it must not restart while
			 * frames from the superseded attempt are still queued or an SQ burst
			 * is still queueing -- the peer counts everything that arrives into
			 * one header-less byte stream. Defer instead, bounded by the same
			 * retry budget as the SQ path, so a ring that never drains fails the
			 * QP rather than deferring forever. */
			/* A zero-copy response is never re-burst on this side's own timer.
			 * Recovery is requester-driven: the requester keeps its landing zone
			 * armed and asks again with a staged SN_READ, so a second burst here
			 * would overshoot that zone. Wait, bounded by the retry budget. */
			bool defer = sn_response_zc(q);

			if (!sn_retry_take(&q->response_retries, q->attr.retry_cnt)) {
				d->zc_resp_retry_exc++;
				sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
			}
			if (defer) {
				q->response_deadline = now + sn_ack_timeout_ns(q->attr.timeout);
			} else {
				q->response_sent = false; q->response_offset = 0;
				q->response_deadline = now + 30000000000ULL;
			}
		}
		if (!q->response_sent) {
			if (sn_response_zc(q) && !sn_response_may_burst(d, q)) {
				/* A response that cannot even start within its budget fails the
				 * QP rather than deferring forever. */
				if (now >= q->response_deadline) {
					d->zc_resp_start_exc++;
					sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
				}
			} else {
				if (sn_response_zc(q)) {
					ret = sn_data_response_burst(d, q);
				} else {
					h = sn_out_header(q, SN_READ_REPLY);
					h.sequence = q->read_request.sequence;
					h.total = q->read_request.total;
					ret = sn_fragments_send(d, &h, q->read_snapshot,
								&q->response_offset, true);
				}
				if (!ret) {
					q->response_sent = true;
					q->response_deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
				} else if (ret != -EAGAIN || now >= q->response_deadline) {
					sn_qp_error(q, IB_WC_GENERAL_ERR); return;
				}
			}
		}
	}
	if (!q->sq_count) return;
	w = &q->sq[q->sq_head];
	if (!q->active) {
		if (q->next_sequence == U64_MAX) { sn_qp_error(q, IB_WC_GENERAL_ERR); return; }
		if (w->opcode != SN_READ && !sn_zc_eligible(d, w) && sn_mr_copy(w->mr, w->address, q->outgoing, w->length, false)) {
			sn_qp_error(q, IB_WC_LOC_PROT_ERR); return;
		}
		q->active = true; q->outgoing_sent = false;
		q->outgoing_offset = q->retries = q->rnr_retries = 0;
		q->zc_send_fault = 0; q->zc_ready_held = false; q->send_staged = false;
		if (w->opcode != SN_READ && sn_zc_eligible(d, w)) {
			/* One read: the parameter is writable at runtime. */
			unsigned int every = READ_ONCE(sn_zc_send_fault);

			d->zc_send_ops++;
			if (every && !(d->zc_send_ops % every))
				q->zc_send_fault = (d->zc_send_ops / every) % 2 ?
					SN_FAULT_READY_LOST : SN_FAULT_READY_LATE;
		}
		memset(&q->read_receive, 0, sizeof(q->read_receive));
		q->read_receive.expected = q->next_sequence;
	}
	if (q->outgoing_sent) {
		if (now < q->deadline) return;
		/* Restarting a zero-copy burst re-sends the descriptor and reposts payload
		 * from offset zero, and the peer zeroes zc_received when that duplicate
		 * descriptor lands. Frames from the previous attempt still queued on the
		 * data TX ring cannot be recalled, so they arrive after the reset and get
		 * counted into the new attempt: the byte count overshoots, the
		 * exact-equality for-CPU sync is skipped, and a buffer assembled from two
		 * attempts completes as IBV_WC_SUCCESS. Wait for the ring to drain first.
		 * Bounded by the normal retry budget, so a ring that never drains fails
		 * the QP instead of deferring forever. A descriptor retransmit from
		 * SN_ZC_WAIT needs no such barrier -- nothing has been posted yet. */
		if (q->zc_state == SN_ZC_SENT && !sn_data_tx_drained(d)) {
			if (!sn_retry_take(&q->retries, q->attr.retry_cnt)) {
				sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
			}
			q->deadline = now + sn_ack_timeout_ns(q->attr.timeout);
			return;
		}
		if (q->zc_state == SN_ZC_SENT) {
			/* The burst has drained, and E2E only transmits into posted
			 * receive buffers, so every frame of it reached the peer: it is
			 * never sent again. Only the ACK is missing, so re-send the
			 * descriptor as a probe and stay SENT, where a ZC_READY is ignored.
			 * The peer answers with the retained ACK once the burst has been
			 * counted, or ignores the probe while it is still counting. A burst
			 * the peer never counts fails with RETRY_EXC rather than re-bursting
			 * into a landing zone that would need a ring restart. */
			struct sn_header p = sn_zc_descriptor(q, w);

			sn_stall_snapshot(d, q, SN_SNAP_TIMEOUT);
			if (!sn_retry_take(&q->retries, q->attr.retry_cnt)) {
				sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
			}
			ret = sn_ring_send(d, &p, NULL, false);
			if (ret && ret != -EAGAIN) { sn_qp_error(q, IB_WC_GENERAL_ERR); return; }
			d->zc_retries++; d->zc_ack_probe++;
			q->deadline = now + sn_ack_timeout_ns(q->attr.timeout);
			return;
		}
		if (!q->rnr_wait && !sn_retry_take(&q->retries, q->attr.retry_cnt)) {
			sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
		}
		q->rnr_wait = q->outgoing_sent = false;
		q->outgoing_offset = 0;
		/* Counts ZC send retries of BOTH kinds, since this path is shared:
		 * RNR (peer had no receive armed; min_rnr_timer=12 -> 320 us) and
		 * ACK timeout (attr.timeout=14 -> 67 ms). Do not read it as a stall
		 * count -- a run can show thousands of these at wire rate, dominated
		 * by cheap 320 us RNR retries. */
		if (q->zc_state != SN_ZC_NONE) d->zc_retries++;
		q->zc_state = SN_ZC_NONE;
		if (q->zc_read_armed) {
			/* Counted here because zc_retries above cannot see it: a zero-copy READ
			 * requester queues no payload, so its zc_state stays SN_ZC_NONE. */
			d->zc_read_retx++;
			sn_stall_snapshot(d, q, SN_SNAP_TIMEOUT);
			/* Never stop/start the data RX ring here. Hardware showed that doing so
			 * under a live peer burst strands that burst in the responder's TX ring
			 * for the rest of the boot, and then zero-copy SEND fails too. Keep the
			 * landing zone armed and complete the same operation through a staged
			 * SN_READ; the zone is reclaimed only once that reply proves no burst is
			 * still live. */
			if (!q->zc_read_staged) {
				q->zc_read_staged = true;
				d->zc_read_fallback++;
			}
		}
	}
	if (q->zc_state == SN_ZC_WAIT) {
		if (now < q->deadline) return;
		sn_stall_snapshot(d, q, SN_SNAP_TIMEOUT);
		if (!sn_retry_take(&q->retries, q->attr.retry_cnt)) {
			sn_qp_error(q, IB_WC_RETRY_EXC_ERR); return;
		}
		q->zc_state = SN_ZC_NONE;
	}
	h = sn_out_header(q, w->opcode);
	h.sequence = q->next_sequence; h.total = w->length;
	h.address = w->remote_address; h.rkey = w->rkey;
	if (sn_zc_eligible(d, w) && !(w->opcode == SN_READ && q->zc_read_staged) &&
	    !(w->opcode == SN_SEND && q->send_staged)) {
		switch (q->zc_state) {
		case SN_ZC_NONE:
			if (w->opcode == SN_READ) {
				/* A READ requester already knows its landing zone -- the local MR
				 * it posted -- so it arms its own data RX ring before the
				 * descriptor goes out and needs no SN_ZC_READY round trip. Only
				 * one inbound burst at a time, so wait while a peer-initiated
				 * burst is still landing. zc_state stays NONE: this side queues
				 * no payload, so the retransmit path below is the whole state
				 * machine and q->zc_read_armed marks the landing zone as ours. */
				if (!q->zc_read_armed) {
					unsigned int every;

					if (q->zc_expect) { ret = 0; break; }
					q->zc_mr = w->mr; q->zc_address = w->address;
					q->zc_total = w->length; q->zc_received = 0;
					q->zc_expect = true; q->zc_read_armed = true;
					/* One read: the parameter is writable at runtime. */
					every = READ_ONCE(sn_zc_read_fault);
					d->zc_read_ops++;
					q->zc_read_fault = every && !(d->zc_read_ops % every);
					if (sn_data_recv_start(d, q)) {
						q->zc_expect = q->zc_read_armed = false;
						q->zc_mr = NULL; q->zc_address = 0;
						sn_qp_error(q, IB_WC_GENERAL_ERR); return;
					}
				}
				/* The descriptor names the responder's source MR; the reply
				 * itself is header-less payload on the data ring. */
				h.opcode = SN_ZC_READ;
				ret = sn_ring_send(d, &h, NULL, false);
				if (!ret) {
					q->outgoing_sent = true;
					q->deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
				}
				break;
			}
			h = sn_zc_descriptor(q, w);
			ret = sn_ring_send(d, &h, NULL, false);
			if (!ret) {
				q->zc_state = SN_ZC_WAIT; q->zc_offset = 0;
				q->deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
				if (q->zc_ready_held) {
					/* zc_send_fault: the first ZC_READY arrives only now,
					 * after the retransmit is on the wire. */
					q->zc_ready_held = false;
					q->zc_state = SN_ZC_SENDING;
				}
			}
			break;
		case SN_ZC_SENDING:
			/* One outbound burst at a time: a zero-copy READ response owns the
			 * data TX ring until its READ_ACK. A burst that has already started
			 * queueing always finishes, so neither side can wait on the other. */
			if (!q->zc_offset && sn_response_zc(q)) { ret = 0; break; }
			ret = sn_data_send_burst(d, q);
			if (!ret) {
				q->zc_state = SN_ZC_SENT; q->outgoing_sent = true;
				q->deadline = ktime_get_ns() + sn_ack_timeout_ns(q->attr.timeout);
			}
			break;
		default:
			ret = 0; break;
		}
		if (ret && ret != -EAGAIN) sn_qp_error(q, IB_WC_GENERAL_ERR);
		goto watchdog;
	}
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
