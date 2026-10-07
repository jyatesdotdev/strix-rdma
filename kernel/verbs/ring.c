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
