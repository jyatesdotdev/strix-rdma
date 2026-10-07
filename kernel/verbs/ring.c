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
static bool sn_slot_peek(struct sn_device *d, struct sn_slot *s)
{
	unsigned long flags;
	bool done;

	spin_lock_irqsave(&d->ring_lock, flags);
	done = s->done;
	spin_unlock_irqrestore(&d->ring_lock, flags);
	return done;
}
/* Lock-free hint for the idle spin: any completed ring work pending? */
bool sn_ring_pending(struct sn_device *d)
{
	if (!READ_ONCE(d->rings_started))
		return false;
	return sn_slot_peek(d, &d->rx[d->rx_tail % SN_RING_SIZE]) ||
	       sn_slot_peek(d, &d->tx[d->tx_tail % SN_RING_SIZE]);
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
		tb_ring_start(d->data_tx_ring);
		tb_ring_start(d->data_rx_ring);
	}
	ret = tb_xdomain_try_enable_native_paths(xd, d->out_hop2,
		d->data_tx_ring->hop, d->in_hop2, d->data_rx_ring->hop);
	if (ret)
		return ret;	/* EAGAIN retries next pass; rings stay allocated. */
	d->zdata = true;
	d->caps = SN_CAPS_V1 | SN_CAP_ZDATA;
	return 0;
}
static void sn_data_rings_stop(struct sn_device *d)
{
	lockdep_assert_held(&d->lock);
	if (d->data_tx_ring) tb_ring_stop(d->data_tx_ring);
	if (d->data_rx_ring) tb_ring_stop(d->data_rx_ring);
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
	int ret, i;

	/* A previous nonblocking teardown may still own the exact path tuple. */
	if (d->tx_ring || d->rx_ring) {
		ret = sn_rings_stop(d);
		if (ret) return ret;
	}
	d->tx_ring = tb_ring_alloc_tx(xd->tb->nhi, -1, SN_RING_SIZE,
				     RING_FLAG_FRAME | RING_FLAG_E2E);
	if (!d->tx_ring)
		return -ENOMEM;
	d->rx_ring = tb_ring_alloc_rx(xd->tb->nhi, -1, SN_RING_SIZE,
		RING_FLAG_FRAME | RING_FLAG_E2E, d->tx_ring->hop,
		BIT(1), BIT(2), NULL, NULL);
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
int sn_rings_stop(struct sn_device *d)
{
	struct tb_xdomain *xd = tb_service_parent(d->service);
	int ret;

	d->ready.paths_active = d->ready.rings_started = d->ready.rx_primed = 0;
	/* No peer close packet. tb_ring_stop disables local DMA and synchronously
	 * joins cancellation callbacks. Only then may staging memory be freed. */
	if (d->data_tx_ring) tb_ring_stop(d->data_tx_ring);
	if (d->data_rx_ring) tb_ring_stop(d->data_rx_ring);
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
/* Sender: queue header-less payload frames for the active SQ op from its MR.
 * Frames never span a physical page; the burst address must be page-aligned
 * (checked by the engine's eligibility). Returns 0 when the whole message is
 * queued, -EAGAIN when the data TX ring is full. */
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q)
{
	struct sn_wqe *w = &q->sq[q->sq_head];
	int ret;

	lockdep_assert_held(&d->lock);
	if (!d->zdata || d->dead || d->reset_rings)
		return -ENOLINK;
	ret = sn_mr_ensure_mapped(d, w->mr);
	if (ret)
		return ret;
	dma_sync_sg_for_device(w->mr->map_dev, w->mr->umem->sgt_append.sgt.sgl,
		w->mr->umem->sgt_append.sgt.orig_nents, DMA_TO_DEVICE);
	while (d->data_tx_head != d->data_tx_tail &&
	       sn_slot_done(d, &d->data_tx[d->data_tx_tail % SN_RING_SIZE])) {
		if (d->data_tx[d->data_tx_tail % SN_RING_SIZE].canceled)
			return -EIO;
		d->data_tx_tail++;
	}
	while (q->zc_offset < w->length) {
		struct sn_slot *s;
		dma_addr_t dma;
		u32 max, size;

		if (d->data_tx_head - d->data_tx_tail >= SN_RING_SIZE - 1)
			return -EAGAIN;
		dma = sn_mr_dma(w->mr, w->address + q->zc_offset, &max);
		if (!dma || !max)
			return -EFAULT;
		size = min3(max, (u32)SN_FRAME, w->length - q->zc_offset);
		s = &d->data_tx[d->data_tx_head % SN_RING_SIZE];
		s->frame.buffer = NULL;
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
		q->zc_offset += size;
	}
	return 0;
}
/* Post more of the expected burst's MR pages to the data RX ring. */
static int sn_zdata_repost(struct sn_device *d, struct sn_qp *q)
{
	struct sn_wqe *w = &q->rq[q->rq_head];

	lockdep_assert_held(&d->lock);
	while (d->zdata_posted < q->zc_total &&
	       d->data_rx_head - d->data_rx_tail < SN_RING_SIZE - 1) {
		struct sn_slot *s;
		dma_addr_t dma;
		u32 max, size;
		int ret;

		dma = sn_mr_dma(w->mr, w->address + d->zdata_posted, &max);
		if (!dma || !max)
			return -EFAULT;
		size = min(max, min((u32)SN_FRAME, q->zc_total - d->zdata_posted));
		s = &d->data_rx[d->data_rx_head % SN_RING_SIZE];
		s->frame.buffer = NULL;
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
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q, struct sn_wqe *w)
{
	int ret;

	lockdep_assert_held(&d->lock);
	ret = sn_mr_ensure_mapped(d, w->mr);
	if (ret)
		return ret;
	dma_sync_sg_for_device(w->mr->map_dev, w->mr->umem->sgt_append.sgt.sgl,
		w->mr->umem->sgt_append.sgt.orig_nents, DMA_FROM_DEVICE);
	d->zdata_posted = 0;
	return sn_zdata_repost(d, q);
}
/* A retried burst restarts from offset zero: flush the data RX ring (partial
 * postings from the previous attempt) and repost from the beginning. */
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q)
{
	lockdep_assert_held(&d->lock);
	tb_ring_stop(d->data_rx_ring);
	tb_ring_start(d->data_rx_ring);
	/* tb_ring_stop joined the cancellation callbacks synchronously; drain
	 * them without reposting so head/tail stay consistent. */
	while (d->data_rx_tail != d->data_rx_head) {
		struct sn_slot *s = &d->data_rx[d->data_rx_tail % SN_RING_SIZE];
		if (!sn_slot_done(d, s)) break;
		d->data_rx_tail++;
	}
	d->zdata_posted = 0;
	return sn_zdata_repost(d, q);
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
		if (d->qp && d->qp->zc_expect) {
			/* Sync the burst pages for the CPU just before the completing
			 * byte count makes the receive WQE visible. */
			if (d->qp->zc_received + size == d->qp->zc_total) {
				struct sn_wqe *w = &d->qp->rq[d->qp->rq_head];

				if (w->mr && w->mr->map_nents)
					dma_sync_sg_for_cpu(w->mr->map_dev,
						w->mr->umem->sgt_append.sgt.sgl,
						w->mr->umem->sgt_append.sgt.orig_nents,
						DMA_FROM_DEVICE);
			}
			sn_engine_zc_bytes(d->qp, size);
		}
	}
	if (d->qp && d->qp->zc_expect)
		sn_zdata_repost(d, d->qp);
}
