// SPDX-License-Identifier: GPL-2.0-only
#include <linux/dma-mapping.h>
#include "strix_nhi.h"

static void sn_ring_callback(struct tb_ring *ring, struct ring_frame *frame, bool canceled)
{
	struct sn_slot *s = container_of(frame, struct sn_slot, frame);
	struct sn_device *d = s->device;
	unsigned long flags;

	spin_lock_irqsave(&d->ring_lock, flags);
	s->canceled = canceled;
	s->done = true;
	spin_unlock_irqrestore(&d->ring_lock, flags);
	sn_schedule(d);
}
static bool sn_slot_done(struct sn_device *d, struct sn_slot *s)
{
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&d->ring_lock, flags);
	done = s->done;
	if (done)
		s->done = false;
	spin_unlock_irqrestore(&d->ring_lock, flags);
	return done;
}
/* Advisory scheduling hint only, so it must be lock-free. The poller peeks
 * roughly once per microsecond while cold; taking ring_lock that often
 * contends with the ring callback that sets the flag, and measurably slows
 * the receive path (a 2 ms spin window cost ~85% of throughput). A stale or
 * torn read costs at most one extra spin iteration or one callback-driven
 * wake, so correctness does not depend on it -- the drain re-reads under the
 * lock in sn_slot_done. */
static bool sn_slot_peek(struct sn_slot *s)
{
	return READ_ONCE(s->done);
}
/* Lock-free hint for the idle spin: any completed ring work pending? */
bool sn_ring_pending(struct sn_device *d)
{
	if (!READ_ONCE(d->rings_started))
		return false;
	/* Only RX completions are latency-critical pending work. A completed TX
	 * slot is bookkeeping: the send path reaps it before it needs the space.
	 * Treating it as pending would pin the poller between sends. */
	if (sn_slot_peek(&d->rx[d->rx_tail % SN_RING_SIZE]))
		return true;
	/* The zero-copy burst path lands on the second ring pair; the poller
	 * must see it too or it sleeps mid-burst and pays a wake-up per frame.
	 * Both are hints: a stale read only costs one callback-driven wake. */
	if (READ_ONCE(d->zdata) && d->data_rx_tail != d->data_rx_head &&
	    sn_slot_peek(&d->data_rx[d->data_rx_tail % SN_RING_SIZE]))
		return true;
	return false;
}

/* ---- Zero-copy data plane: header-less payload frames over a second ring
 * pair, DMA'd directly from/to the application's registered-MR pages. */

static void sn_data_slots_init(struct sn_device *d, struct sn_slot *slots)
{
	int i;

	for (i = 0; i < SN_RING_SIZE; i++) {
		slots[i].device = d;
		slots[i].frame.callback = sn_ring_callback;
		slots[i].buffer = NULL;	/* frames point at MR pages, not coherent buffers */
	}
}
/* Allocate and start the data ring pair and enable its path tuple (slot 1).
 * Returns -EAGAIN for a busy core (retry next pass), -ENOMEM when no second
 * ring HopID exists (thunderbolt-net present: stay staged-only), 0 on success. */
int sn_data_rings_start(struct sn_device *d)
{
	struct tb_xdomain *xd = tb_service_parent(d->service);
	int ret;

	lockdep_assert_held(&d->lock);
	if (d->zdata)
		return 0;
	if (d->out_hop2 < 0 || d->in_hop2 < 0)
		return -ENOMEM;
	if (!d->data_tx_ring) {
		d->data_tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, SN_RING_SIZE,
						 RING_FLAG_FRAME | RING_FLAG_E2E);
		if (!d->data_tx_ring)
			return -ENOMEM;
		d->data_rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, SN_RING_SIZE,
			RING_FLAG_FRAME | RING_FLAG_E2E, d->data_tx_ring->hop,
			BIT(1), BIT(2), NULL, NULL);
		if (!d->data_rx_ring) {
			tb_ring_free(d->data_tx_ring); d->data_tx_ring = NULL;
			return -ENOMEM;
		}
		sn_data_slots_init(d, d->data_tx);
		sn_data_slots_init(d, d->data_rx);
		/* Moderate data-ring interrupts before they go live
		 * (tb_ring_throttling requires a stopped ring). Without this each
		 * 4 KiB frame raises one interrupt: measured 1,229,852 RX
		 * interrupts for 1,228,800 frames received. Batching them is safe
		 * because the drain consumes every pending frame per pass (37 frames
		 * accumulate per 128 us at wire speed, well inside the 64-slot ring).
		 * The control ring stays unthrottled -- it carries the acks and
		 * descriptors whose latency the 36 us ping-pong floor depends on.
		 * The window is the data_throttle_ns module parameter; 0 leaves both
		 * data rings unmoderated (lowest latency, per-frame interrupts). */
		if (sn_data_throttle_ns &&
		    (tb_ring_throttling(d->data_tx_ring, sn_data_throttle_ns) ||
		     tb_ring_throttling(d->data_rx_ring, sn_data_throttle_ns)))
			dev_warn(&d->service->dev,
				 "strix_nhi: data ring interrupt throttling unavailable\n");
		tb_ring_start(d->data_tx_ring);
		tb_ring_start(d->data_rx_ring);
		d->data_rings_started = true;
	}
	ret = tb_xdomain_try_enable_native_paths(xd, d->out_hop2,
		d->data_tx_ring->hop, d->in_hop2, d->data_rx_ring->hop);
	/* EBUSY means an old single-tuple core; stay staged-only. */
	if (ret == -EBUSY)
		return -ENOMEM;
	if (ret)
		return ret;	/* EAGAIN retries next pass; rings stay allocated. */
	d->zdata = true;
	d->zc_write = sn_zc_write;
	d->zc_read = sn_zc_read;
	d->caps = SN_CAPS_V1 | SN_CAP_ZDATA;
	return 0;
}
static int sn_slots_alloc(struct sn_device *d, struct tb_ring *r, struct sn_slot *slots)
{
	int i;

	for (i = 0; i < SN_RING_SIZE; i++) {
		struct sn_slot *s = &slots[i];

		s->device = d;
		s->frame.callback = sn_ring_callback;
		s->buffer = dma_alloc_coherent(tb_ring_dma_device(r), SN_FRAME,
				&s->frame.buffer_phy, GFP_KERNEL);
		if (!s->buffer)
			return -ENOMEM;
	}
	return 0;
}
static void sn_slots_free(struct tb_ring *r, struct sn_slot *slots)
{
	int i;

	if (!r)
		return;
	for (i = 0; i < SN_RING_SIZE; i++) {
		struct sn_slot *s = &slots[i];

		if (s->buffer)
			dma_free_coherent(tb_ring_dma_device(r), SN_FRAME,
					  s->buffer, s->frame.buffer_phy);
		s->buffer = NULL;
		s->done = false;
	}
}
int sn_rings_start(struct sn_device *d)
{
	struct tb_xdomain *xd = tb_service_parent(d->service);
	unsigned int flags;
	int ret, i;

	/* A previous nonblocking teardown may still own the exact path tuple. */
	if (d->tx_ring || d->rx_ring) {
		ret = sn_rings_stop(d);
		if (ret) return ret;
	}
	/* ctl_e2e=0 drops E2E from this pair only, leaving one E2E pair per host
	 * (rounds twelve and seventeen). The hop is still passed: the core only
	 * consults it when the flag is set. */
	flags = RING_FLAG_FRAME | (sn_ctl_e2e ? RING_FLAG_E2E : 0);
	d->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, SN_RING_SIZE, flags);
	if (!d->tx_ring)
		return -ENOMEM;
	d->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, SN_RING_SIZE, flags,
		d->tx_ring->hop, BIT(1), BIT(2), NULL, NULL);
	if (!d->rx_ring) {
		ret = -ENOMEM;
		goto fail;
	}
	ret = sn_slots_alloc(d, d->tx_ring, d->tx);
	if (ret) goto fail;
	ret = sn_slots_alloc(d, d->rx_ring, d->rx);
	if (ret) goto fail;
	tb_ring_start(d->tx_ring);
	tb_ring_start(d->rx_ring);
	d->rings_started = true;
	for (i = 0; i < SN_RING_SIZE; i++) {
		ret = tb_ring_rx(d->rx_ring, &d->rx[i].frame);
		if (ret) goto fail;
	}
	d->rx_primed = true;
	ret = tb_xdomain_try_enable_native_paths(xd, d->out_hop, d->tx_ring->hop,
				     d->in_hop, d->rx_ring->hop);
	if (ret) goto fail;
	d->paths_active = true;
	d->ready.rx_primed = d->ready.rings_started = d->ready.paths_active = 1;
	/* The zero-copy data plane starts with the control rings so the HELLO
	 * capabilities advertise the actual local state from the first frame.
	 * Missing second HopID/tuple slot keeps the driver staged-only. */
	if (!d->zc_unavail && d->out_hop2 >= 0 && d->in_hop2 >= 0) {
		ret = sn_data_rings_start(d);
		if (ret == -ENOMEM)
			d->zc_unavail = true;
		else if (ret && ret != -EAGAIN && ret != -ENODEV)
			goto fail;
	}
	return 0;
fail:
	sn_rings_stop(d);
	return ret;
}
/* Wire-version mismatch from a peer: the session is refused frame by frame, so
 * this is the only place it becomes visible. Warn on the first frame. */
void sn_peer_version_mismatch(struct sn_device *d, unsigned int version)
{
	if (d->version_mismatch == 1)
		dev_warn(&d->service->dev,
			 "strix_nhi: peer speaks wire version %u, this build speaks %u; refusing the session\n",
			 version, SN_VERSION);
}
int sn_rings_stop(struct sn_device *d)
{
	struct tb_xdomain *xd = tb_service_parent(d->service);
	int ret;

	d->ready.paths_active = d->ready.rings_started = d->ready.rx_primed = 0;
	/* No peer close packet. tb_ring_stop disables local DMA and synchronously
	 * joins cancellation callbacks. Only then may staging memory be freed. */
	/* Stop each pair only while it runs: this is retried on EAGAIN below (from
	 * sn_remove and from every poller pass after carrier loss), and a hot
	 * unplug makes that the normal case because core removal holds the lock
	 * the disable trylocks. Re-stopping a stopped ring trips a core WARN. */
	if (d->data_rings_started) {
		if (d->data_tx_ring) tb_ring_stop(d->data_tx_ring);
		if (d->data_rx_ring) tb_ring_stop(d->data_rx_ring);
		d->data_rings_started = false;
	}
	if (d->rings_started) {
		tb_ring_stop(d->tx_ring);
		tb_ring_stop(d->rx_ring);
		d->rings_started = false;
	}
	if (d->paths_active) {
		ret = tb_xdomain_try_disable_native_paths(xd);
		/* Core owns the exact admitted tuple. EAGAIN retains EVERY ring,
		 * buffer and HopID; ENODEV proves core cleanup already completed. */
		if (ret && ret != -ENODEV) return ret;
		d->paths_active = false;
	}
	if (d->data_tx_ring) { tb_ring_free(d->data_tx_ring); d->data_tx_ring = NULL; }
	if (d->data_rx_ring) { tb_ring_free(d->data_rx_ring); d->data_rx_ring = NULL; }
	d->zdata = false;
	d->zc_write = false;
	d->zc_read = false;
	d->caps = SN_CAPS_V1;
	d->data_tx_head = d->data_tx_tail = d->data_rx_head = d->data_rx_tail = 0;
	d->rx_primed = false;
	sn_slots_free(d->tx_ring, d->tx);
	sn_slots_free(d->rx_ring, d->rx);
	if (d->tx_ring) tb_ring_free(d->tx_ring);
	if (d->rx_ring) tb_ring_free(d->rx_ring);
	d->tx_ring = d->rx_ring = NULL;
	d->tx_head = d->tx_tail = d->rx_tail = 0;
	return 0;
}
int sn_ring_send(struct sn_device *d, struct sn_header *h, const void *payload, bool control)
{
	struct sn_slot *s;
	bool hs = h->opcode >= SN_HELLO && h->opcode <= SN_BIND_ACK;
	unsigned int limit = SN_RING_SIZE - 1;
	int ret;

	lockdep_assert_held(&d->lock);
	if (d->dead || d->reset_rings ||
	    smp_load_acquire(&tb_service_parent(d->service)->native_dma_stopping) ||
	    READ_ONCE(tb_service_parent(d->service)->is_unplugged))
		return -ENOLINK;
	/* Handshake frames only need local paths; everything else needs the
	 * fully established reciprocal session. */
	if (hs ? !sn_local_ready(&d->ready) : !sn_ready(&d->ready))
		return -ENOLINK;
	if (!control) limit -= SN_CONTROL_RESERVE;
	while (d->tx_head != d->tx_tail &&
	       sn_slot_done(d, &d->tx[d->tx_tail % SN_RING_SIZE])) {
		if (d->tx[d->tx_tail % SN_RING_SIZE].canceled)
			return -EIO;
		d->tx_tail++;
	}
	if (d->tx_head - d->tx_tail >= limit)
		return -EAGAIN;
	s = &d->tx[d->tx_head % SN_RING_SIZE];
	if (h->length) memcpy(s->buffer + SN_HEADER, payload, h->length);
	if (sn_encode(s->buffer, SN_FRAME, h))
		return -EINVAL;
	s->frame.size = (SN_HEADER + h->length) & 0xfff;
	s->frame.flags = 0;
	/* Match tbnet's proven framing: sof=FRAME_START(1), eof=FRAME_END(2). */
	s->frame.sof = 1; s->frame.eof = 2;
	s->canceled = false;
	dma_wmb();
	ret = tb_ring_tx(d->tx_ring, &s->frame);
	if (!ret) { d->tx_head++; d->tx_frames++; }
	return ret;
}
void sn_ring_receive(struct sn_device *d)
{
	int i;

	lockdep_assert_held(&d->lock);
	if (!d->rings_started || d->dead)
		return;
	for (i = 0; i < SN_RING_SIZE; i++) {
		struct sn_slot *s = &d->rx[d->rx_tail % SN_RING_SIZE];

		if (!sn_slot_done(d, s)) break;
		d->rx_tail++;
		if (s->canceled) continue;
		dma_rmb();
		d->rx_frames++;
		if (!(s->frame.flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN)))
			sn_engine_receive(d, s->buffer, tb_ring_frame_size(&s->frame));
		s->frame.size = 0;
		s->frame.flags = 0;
		if (tb_ring_rx(d->rx_ring, &s->frame) && d->qp)
			sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	}
}
/* Queue header-less payload frames for one burst: the source MR, the address
 * inside it, the total bytes, and the cursor to advance. Frames never span a
 * physical page; the burst address must be page-aligned (checked by the
 * engine's eligibility). Returns 0 when the whole message is queued, -EAGAIN
 * when the data TX ring is full. The cursor belongs to the caller because not
 * every burst has an SQ WQE: a READ response bursts from the source MR the
 * request named and tracks its own offset. */
static int sn_data_burst(struct sn_device *d, struct sn_mr *mr, u64 address,
			 u32 total, u32 *offset)
{
	int ret;

	lockdep_assert_held(&d->lock);
	if (!d->zdata || d->dead || d->reset_rings)
		return -ENOLINK;
	ret = sn_mr_ensure_mapped(d, mr);
	if (ret)
		return ret;
	/* A GPU MR's mapping belongs to its exporter; never sync it here. */
	if (!mr->dmabuf)
		dma_sync_sg_for_device(mr->map_dev, mr->umem->sgt_append.sgt.sgl,
			mr->umem->sgt_append.sgt.orig_nents, DMA_TO_DEVICE);
	while (d->data_tx_head != d->data_tx_tail &&
	       sn_slot_done(d, &d->data_tx[d->data_tx_tail % SN_RING_SIZE])) {
		if (d->data_tx[d->data_tx_tail % SN_RING_SIZE].canceled)
			return -EIO;
		d->data_tx_tail++;
	}
	while (*offset < total) {
		struct sn_slot *s;
		dma_addr_t dma;
		u32 max, size;

		if (d->data_tx_head - d->data_tx_tail >= SN_RING_SIZE - 1)
			return -EAGAIN;
		dma = sn_mr_dma(mr, address + *offset, &max);
		if (!dma || !max)
			return -EFAULT;
		size = min3(max, (u32)SN_FRAME, total - *offset);
		s = &d->data_tx[d->data_tx_head % SN_RING_SIZE];
		s->frame.buffer_phy = dma;
		s->frame.size = size & 0xfff;
		s->frame.flags = 0;
		s->frame.sof = 1;
		s->frame.eof = 2;
		s->canceled = false;
		ret = tb_ring_tx(d->data_tx_ring, &s->frame);
		if (ret)
			return ret;
		d->data_tx_head++;
		d->tx_frames++;
		*offset += size;
	}
	return 0;
}
/* Sender: queue the active SQ operation's payload from its own MR. */
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];

	return sn_data_burst(d, w->mr, w->address, w->length, &q->zc_offset);
}
/* Responder: queue a zero-copy READ response straight from the source MR the
 * request named, which is retained until the READ_ACK. There is no SQ WQE for
 * it, so the cursor is the response's own. */
int sn_data_response_burst(struct sn_device *d, struct sn_qp *q)
{
	if (!q->remote_mr)
		return -EFAULT;
	return sn_data_burst(d, q->remote_mr, q->read_request.address,
		q->read_request.total, &q->response_offset);
}
/* Post more of the expected burst's landing-zone pages to the data RX ring.
 * The zone is the matched receive WQE's MR for a SEND burst, the
 * requester-addressed remote MR for a WRITE burst and the requester's own MR for
 * a READ response; q->zc_mr names it and is cleared the moment the expected byte
 * count lands. */
static int sn_zdata_repost(struct sn_device *d, struct sn_qp *q)
{
	lockdep_assert_held(&d->lock);
	if (!q->zc_mr)
		return 0;
	while (d->zdata_posted < q->zc_total &&
	       d->data_rx_head - d->data_rx_tail < SN_RING_SIZE - 1) {
		struct sn_slot *s;
		dma_addr_t dma;
		u32 max, size;
		int ret;

		dma = sn_mr_dma(q->zc_mr, q->zc_address + d->zdata_posted, &max);
		if (!dma || !max)
			return -EFAULT;
		size = min(max, min((u32)SN_FRAME, q->zc_total - d->zdata_posted));
		s = &d->data_rx[d->data_rx_head % SN_RING_SIZE];
		s->frame.buffer_phy = dma;
		s->frame.size = 0;
		s->frame.flags = 0;
		s->canceled = false;
		ret = tb_ring_rx(d->data_rx_ring, &s->frame);
		if (ret)
			return ret;
		d->data_rx_head++;
		d->zdata_posted += size;
	}
	return 0;
}
/* Harvest completed data TX slots and report whether the ring is now empty.
 * A canceled slot is deliberately not harvested here, so it reads as not drained
 * and the caller's bounded retry budget fails the QP instead of spinning. */
bool sn_data_tx_drained(struct sn_device *d)
{
	lockdep_assert_held(&d->lock);
	while (d->data_tx_head != d->data_tx_tail &&
	       sn_slot_done(d, &d->data_tx[d->data_tx_tail % SN_RING_SIZE])) {
		if (d->data_tx[d->data_tx_tail % SN_RING_SIZE].canceled)
			break;
		d->data_tx_tail++;
	}
	return d->data_tx_head == d->data_tx_tail;
}
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q)
{
	int ret;

	lockdep_assert_held(&d->lock);
	if (!q->zc_mr)
		return -EINVAL;
	ret = sn_mr_ensure_mapped(d, q->zc_mr);
	if (ret)
		return ret;
	if (!q->zc_mr->dmabuf)
		dma_sync_sg_for_device(q->zc_mr->map_dev, q->zc_mr->umem->sgt_append.sgt.sgl,
			q->zc_mr->umem->sgt_append.sgt.orig_nents, DMA_FROM_DEVICE);
	d->zdata_posted = 0;
	return sn_zdata_repost(d, q);
}
/* Abort any in-progress zero-copy burst: stop+start both data rings (stop
 * synchronously joins the cancellation callbacks), drain every completed /
 * canceled slot so head/tail resync, and drop any slot that was queued but
 * never transmitted or filled. Without this an aborted burst leaves posted
 * RX buffers (and stale TX payload) in the rings, which then fill and
 * deadlock every subsequent burst. */
void sn_data_abort(struct sn_device *d)
{
	lockdep_assert_held(&d->lock);
	if (!d->zdata || !d->data_rx_ring || !d->data_tx_ring)
		return;
	d->zc_aborts++;
	tb_ring_stop(d->data_tx_ring);
	tb_ring_start(d->data_tx_ring);
	tb_ring_stop(d->data_rx_ring);
	tb_ring_start(d->data_rx_ring);
	while (d->data_tx_tail != d->data_tx_head &&
	       sn_slot_done(d, &d->data_tx[d->data_tx_tail % SN_RING_SIZE]))
		d->data_tx_tail++;
	while (d->data_rx_tail != d->data_rx_head &&
	       sn_slot_done(d, &d->data_rx[d->data_rx_tail % SN_RING_SIZE]))
		d->data_rx_tail++;
	d->data_tx_head = d->data_tx_tail;
	d->data_rx_head = d->data_rx_tail;
	d->zdata_posted = 0;
}
void sn_stall_snapshot(struct sn_device *d, struct sn_qp *q, int status)
{
	struct sn_stall_snap *s = status < 0 ? &d->snap_tmo :
				  status ? &d->snap_err : &d->snap_drain;
	u64 now = ktime_get_ns(), count = s->count + 1;
	bool rings;

	lockdep_assert_held(&d->lock);
	if (!status && s->count && now - s->at_ns < NSEC_PER_MSEC)
		return;
	memset(s, 0, sizeof(*s));
	s->count = count; s->at_ns = now; s->status = status;
	s->since_land_ns = d->last_land_ns ? now - d->last_land_ns : 0;
	s->max_pass_gap_ns = d->max_pass_gap_ns;
	s->data_tx_head = d->data_tx_head; s->data_tx_tail = d->data_tx_tail;
	s->data_rx_head = d->data_rx_head; s->data_rx_tail = d->data_rx_tail;
	s->zdata_posted = d->zdata_posted;
	if (q) {
		s->qp = true;
		s->zc_received = q->zc_received; s->zc_total = q->zc_total;
		s->zc_expect = q->zc_expect; s->zc_state = q->zc_state;
		s->zc_read_staged = q->zc_read_staged;
		s->zc_read_armed = q->zc_read_armed;
		s->response_active = q->response_active;
		s->response_sent = q->response_sent;
		s->response_fallback = q->response_fallback;
		s->response_offset = q->response_offset;
	}
	rings = d->zdata && !d->dead && d->data_tx_ring && d->data_rx_ring;
	s->tx_ret = rings ? tb_ring_get_stats(d->data_tx_ring, &s->tx) : -ENODEV;
	s->rx_ret = rings ? tb_ring_get_stats(d->data_rx_ring, &s->rx) : -ENODEV;
}
void sn_data_receive(struct sn_device *d)
{
	int i;

	lockdep_assert_held(&d->lock);
	if (!d->zdata || d->dead)
		return;
	for (i = 0; i < SN_RING_SIZE; i++) {
		struct sn_slot *s = &d->data_rx[d->data_rx_tail % SN_RING_SIZE];
		u32 size;

		if (!sn_slot_done(d, s)) break;
		d->data_rx_tail++;
		if (s->canceled) continue;
		dma_rmb();
		size = tb_ring_frame_size(&s->frame);
		if (s->frame.flags & (RING_DESC_CRC_ERROR | RING_DESC_BUFFER_OVERRUN)) {
			/* E2E makes this unreachable; byte accounting cannot recover. */
			if (d->qp)
				sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
			continue;
		}
		d->rx_frames++;
		d->last_land_ns = ktime_get_ns();
		if (d->qp && d->qp->zc_expect) {
			if (d->qp->zc_received + size > d->qp->zc_total) {
				/* Overshoot: more bytes than the burst holds. A sender bursts
				 * each operation once and a receiver never restarts its ring, so
				 * this needs a peer that broke the protocol -- for example one
				 * running an older module that still re-bursts on retry -- and
				 * the byte stream then describes parts of two attempts.
				 * Completing would hand the caller a buffer assembled from
				 * both -- and because the sync
				 * below keys on exact equality, it would also hand back pages
				 * never synced for the CPU. Fail the QP loudly instead of
				 * corrupting silently. */
				sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
				continue;
			}
			/* Sync the burst pages for the CPU just before the completing
			 * byte count makes the placement visible. */
			if (d->qp->zc_received + size == d->qp->zc_total) {
				struct sn_mr *m = d->qp->zc_mr;

				if (m && m->map_nents && !m->dmabuf)
					dma_sync_sg_for_cpu(m->map_dev,
						m->umem->sgt_append.sgt.sgl,
						m->umem->sgt_append.sgt.orig_nents,
						DMA_FROM_DEVICE);
			}
			sn_engine_zc_bytes(d->qp, size);
		}
	}
	if (d->qp && d->qp->zc_expect)
		sn_zdata_repost(d, d->qp);
}
