// SPDX-License-Identifier: GPL-2.0-only
#include <linux/random.h>
#include <linux/vmalloc.h>
#include <rdma/uverbs_ioctl.h>
#include "strix_nhi.h"

static void sn_event(struct ib_cq *cq, enum ib_event_type type)
{
	struct ib_event event = {.device = cq->device, .event = type};

	event.element.cq = cq;
	if (cq->event_handler) cq->event_handler(&event, cq->cq_context);
}
/* A CQE never points at the real QP: ib_uverbs_poll_cq dereferences
 * wc->qp->qp_num after poll_cq has dropped d->lock, and a concurrent
 * DESTROY_QP may already have freed the QP. These device-owned identities
 * live until ib_dealloc_device, after uverbs has drained. Alternating slots
 * mean only a poll stalled across two destroy/create cycles can report a
 * newer QPN; that race is cosmetic, never a use-after-free. */
static struct ib_qp *sn_wc_qp(struct sn_device *d, u32 qp_num)
{
	return &d->wc_qp[qp_num & 1];
}
int sn_complete(struct sn_qp *q, struct sn_wqe *w, bool receive,
		enum ib_wc_status status, u32 length)
{
	struct sn_cq *cq = sn_cq(receive ? q->ib.recv_cq : q->ib.send_cq);
	struct ib_wc *wc;
	bool notify;

	lockdep_assert_held(&sn_dev(q->ib.device)->lock);
	if (!receive && status == IB_WC_SUCCESS && !q->all_signaled &&
	    !(w->flags & IB_SEND_SIGNALED))
		return 0;
	if (cq->failed) return -ENOSPC;
	if (cq->count == cq->ib.cqe) {
		cq->failed = true;
		sn_event(&cq->ib, IB_EVENT_CQ_ERR);
		return -ENOSPC;
	}
	wc = &cq->entries[(cq->head + cq->count) % cq->ib.cqe];
	memset(wc, 0, sizeof(*wc));
	wc->wr_id = w->id; wc->status = status;
	wc->qp = sn_wc_qp(sn_dev(q->ib.device), q->ib.qp_num);
	WRITE_ONCE(wc->qp->qp_num, q->ib.qp_num);
	wc->opcode = receive ? IB_WC_RECV : w->opcode == SN_READ ? IB_WC_RDMA_READ :
		     w->opcode == SN_WRITE ? IB_WC_RDMA_WRITE : IB_WC_SEND;
	wc->byte_len = length;
	cq->count++;
	/* d->lock orders completed placement before CQ publication/polling. */
	notify = cq->notify == IB_CQ_NEXT_COMP ||
		 (cq->notify && status != IB_WC_SUCCESS);
	if (notify) {
		cq->notify = 0;
		if (cq->ib.comp_handler) cq->ib.comp_handler(&cq->ib, cq->ib.cq_context);
	}
	return 0;
}
static void sn_flush_qp(struct sn_qp *q, enum ib_wc_status status, bool notify)
{
	struct sn_device *d = sn_dev(q->ib.device);
	struct ib_event event = {.device = &d->ib, .event = IB_EVENT_QP_FATAL};
	bool first = true, changed = q->attr.qp_state != IB_QPS_ERR;

	lockdep_assert_held(&d->lock);
	q->attr.qp_state = IB_QPS_ERR;
	q->bound = false;
	while (q->sq_count) {
		struct sn_wqe *w = &q->sq[q->sq_head];
		sn_complete(q, w, false, first ? status : IB_WC_WR_FLUSH_ERR, 0);
		sn_mr_put(w->mr); w->mr = NULL;
		q->sq_head = (q->sq_head + 1) % SN_QUEUE_DEPTH; q->sq_count--;
		first = false;
	}
	while (q->rq_count) {
		struct sn_wqe *w = &q->rq[q->rq_head];
		sn_complete(q, w, true, IB_WC_WR_FLUSH_ERR, 0);
		sn_mr_put(w->mr); w->mr = NULL;
		q->rq_head = (q->rq_head + 1) % SN_QUEUE_DEPTH; q->rq_count--;
	}
	sn_engine_reset(q);
	if (changed && notify && !q->destroying && q->ib.event_handler) {
		event.element.qp = &q->ib;
		q->ib.event_handler(&event, q->ib.qp_context);
	}
}
void sn_qp_error(struct sn_qp *q, enum ib_wc_status status)
{
	sn_flush_qp(q, status, true);
}
static int sn_query_device(struct ib_device *ib, struct ib_device_attr *a,
			   struct ib_udata *udata)
{
	memset(a, 0, sizeof(*a));
	a->max_mr_size = SN_MR_BYTES; a->page_size_cap = PAGE_SIZE;
	a->max_qp = 1; a->max_qp_wr = SN_QUEUE_DEPTH;
	a->max_send_sge = a->max_recv_sge = 1;
	a->max_cq = 2; a->max_cqe = SN_MAX_CQE;
	a->max_mr = SN_MAX_MRS; a->max_pd = 1;
	a->max_qp_rd_atom = a->max_qp_init_rd_atom = a->max_res_rd_atom = 1;
	a->atomic_cap = IB_ATOMIC_NONE;
	a->sys_image_guid = ib->node_guid;
	return 0;
}
static int sn_query_port(struct ib_device *ib, u32 port, struct ib_port_attr *a)
{
	struct sn_device *d = sn_dev(ib);

	if (port != 1) return -EINVAL;
	memset(a, 0, sizeof(*a));
	mutex_lock(&d->lock);
	a->state = !d->dead && !d->reset_rings && sn_ready(&d->ready) &&
		!smp_load_acquire(&tb_service_parent(d->service)->native_dma_stopping) &&
		!READ_ONCE(tb_service_parent(d->service)->is_unplugged) ? IB_PORT_ACTIVE : IB_PORT_DOWN;
	a->phys_state = a->state == IB_PORT_ACTIVE ? IB_PORT_PHYS_STATE_LINK_UP : IB_PORT_PHYS_STATE_DISABLED;
	a->max_mtu = a->active_mtu = IB_MTU_4096;
	a->gid_tbl_len = a->pkey_tbl_len = 1;
	a->max_msg_sz = SN_MESSAGE;
	/* No protocol capability bits, negotiated speed, width or IB link claims. */
	mutex_unlock(&d->lock);
	return 0;
}
static int sn_immutable(struct ib_device *ib, u32 port, struct ib_port_immutable *a)
{
	if (port != 1) return -EINVAL;
	a->pkey_tbl_len = a->gid_tbl_len = 1;
	a->core_cap_flags = 0; a->max_mad_size = 0;
	return 0;
}
static enum rdma_link_layer sn_link_layer(struct ib_device *ib, u32 port)
{
	return port == 1 ? IB_LINK_LAYER_ETHERNET : IB_LINK_LAYER_UNSPECIFIED;
}
static int sn_query_gid(struct ib_device *ib, u32 port, int index, union ib_gid *gid)
{
	if (port != 1 || index) return -EINVAL;
	*gid = sn_dev(ib)->gid;
	return 0;
}
static int sn_query_pkey(struct ib_device *ib, u32 port, u16 index, u16 *pkey)
{
	if (port != 1 || index) return -EINVAL;
	*pkey = 0xffff;
	return 0;
}
static int sn_alloc_context(struct ib_ucontext *ctx, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(ctx->device);
	int ret = 0;

	if (udata->inlen || udata->outlen) return -EOPNOTSUPP;
	mutex_lock(&d->lock);
	if (d->dead) ret = -ENODEV;
	else if (d->have_context) ret = -EBUSY;
	else d->have_context = true;
	mutex_unlock(&d->lock);
	return ret;
}
static void sn_dealloc_context(struct ib_ucontext *ctx)
{
	struct sn_device *d = sn_dev(ctx->device);
	mutex_lock(&d->lock); d->have_context = false; mutex_unlock(&d->lock);
}
static void sn_disassociate_context(struct ib_ucontext *ctx)
{
	struct sn_device *d = sn_dev(ctx->device);

	/* There are no user mappings. The uverbs core destroys the objects after
	 * excluding commands; join the copying worker and flush before unpinning. */
	mutex_lock(&d->lock);
	if (d->qp) sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	mutex_unlock(&d->lock);
}
static int sn_alloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(pd->device);
	int ret = 0;

	if (!udata || udata->inlen || udata->outlen) return -EOPNOTSUPP;
	mutex_lock(&d->lock);
	if (d->dead) ret = -ENODEV;
	else if (d->have_pd) ret = -ENOMEM;
	else d->have_pd = true;
	mutex_unlock(&d->lock);
	return ret;
}
static int sn_dealloc_pd(struct ib_pd *pd, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(pd->device);
	mutex_lock(&d->lock); d->have_pd = false; mutex_unlock(&d->lock);
	return 0;
}
static int sn_create_cq(struct ib_cq *ib, const struct ib_cq_init_attr *a,
			struct uverbs_attr_bundle *attrs)
{
	struct ib_udata *udata = &attrs->driver_udata;
	struct sn_device *d = sn_dev(ib->device);
	int ret = 0;

	if (!udata || a->flags || udata->inlen || udata->outlen) return -EOPNOTSUPP;
	if (a->cqe < 1 || a->cqe > SN_MAX_CQE || a->comp_vector) return -EINVAL;
	mutex_lock(&d->lock);
	if (d->dead) ret = -ENODEV;
	else if (d->cq_count == 2) ret = -ENOMEM;
	else { d->cq_count++; ib->cqe = a->cqe; }
	mutex_unlock(&d->lock);
	return ret;
}
static int sn_destroy_cq(struct ib_cq *ib, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(ib->device);
	mutex_lock(&d->lock); d->cq_count--; mutex_unlock(&d->lock);
	return 0;
}
static int sn_poll_cq(struct ib_cq *ib, int n, struct ib_wc *wc)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_cq *cq = sn_cq(ib);
	int i = 0;

	if (n < 0) return -EINVAL;
	mutex_lock(&d->lock);
	while (i < n && cq->count) {
		wc[i++] = cq->entries[cq->head];
		cq->head = (cq->head + 1) % ib->cqe; cq->count--;
	}
	if (!i && cq->failed) i = -EIO;
	mutex_unlock(&d->lock);
	return i;
}
static int sn_notify_cq(struct ib_cq *ib, enum ib_cq_notify_flags flags)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_cq *cq = sn_cq(ib);
	int notify = flags & IB_CQ_SOLICITED_MASK, ret;

	if ((flags & ~(IB_CQ_SOLICITED_MASK | IB_CQ_REPORT_MISSED_EVENTS)) ||
	    (notify != IB_CQ_NEXT_COMP && notify != IB_CQ_SOLICITED)) return -EINVAL;
	mutex_lock(&d->lock);
	if (cq->failed) ret = -EIO;
	else {
		/* Re-arming with SOLICITED cannot weaken an outstanding NEXT arm. */
		if (cq->notify != IB_CQ_NEXT_COMP) cq->notify = notify;
		ret = !!((flags & IB_CQ_REPORT_MISSED_EVENTS) && cq->count);
	}
	mutex_unlock(&d->lock);
	return ret;
}
static int sn_create_qp(struct ib_qp *ib, struct ib_qp_init_attr *a, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_qp *q = sn_qp(ib);
	int ret = 0;

	if (!udata || udata->inlen || udata->outlen || a->qp_type != IB_QPT_RC ||
	    a->srq || a->create_flags || a->cap.max_inline_data) return -EOPNOTSUPP;
	if (!a->cap.max_send_wr || a->cap.max_send_wr > SN_QUEUE_DEPTH ||
	    !a->cap.max_recv_wr || a->cap.max_recv_wr > SN_QUEUE_DEPTH ||
	    a->cap.max_send_sge != 1 || a->cap.max_recv_sge != 1 ||
	    !a->send_cq || !a->recv_cq) return -EINVAL;
	q->outgoing = kvzalloc(SN_MESSAGE, GFP_KERNEL);
	q->incoming = kvzalloc(SN_MESSAGE, GFP_KERNEL);
	q->read_snapshot = kvzalloc(SN_MESSAGE, GFP_KERNEL);
	q->read_incoming = kvzalloc(SN_MESSAGE, GFP_KERNEL);
	if (!q->outgoing || !q->incoming || !q->read_snapshot || !q->read_incoming) {
		ret = -ENOMEM; goto free;
	}
	mutex_lock(&d->lock);
	if (d->dead) ret = -ENODEV;
	else if (d->qp) ret = -ENOMEM;
	else if (d->qpn_serial == SN_PSN_MAX) ret = -EOVERFLOW;
	if (!ret) {
		ib->qp_num = ++d->qpn_serial;
		q->binding.local_qpn = ib->qp_num;
		do { q->binding.local_generation = get_random_u64(); } while (!q->binding.local_generation);
		q->attr.qp_state = IB_QPS_RESET;
		q->init = *a;
		q->all_signaled = a->sq_sig_type == IB_SIGNAL_ALL_WR;
		d->qp = q;
	}
	mutex_unlock(&d->lock);
	if (!ret) { sn_schedule(d); return 0; }
free:
	kvfree(q->outgoing); kvfree(q->incoming);
	kvfree(q->read_snapshot); kvfree(q->read_incoming);
	return ret;
}
static void sn_cq_remove_qp(struct sn_cq *cq, struct ib_qp *qp)
{
	struct ib_qp *identity = sn_wc_qp(sn_dev(cq->ib.device), qp->qp_num);
	unsigned int i, count = cq->count, kept = 0;

	/* Only one QP exists at a time, so the identity selects its CQEs. */
	for (i = 0; i < count; i++) {
		struct ib_wc wc = cq->entries[(cq->head + i) % cq->ib.cqe];
		if (wc.qp != identity) cq->entries[(cq->head + kept++) % cq->ib.cqe] = wc;
	}
	cq->count = kept;
}
static int sn_destroy_qp(struct ib_qp *ib, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_qp *q = sn_qp(ib);

	mutex_lock(&d->lock);
	q->destroying = true;
	d->qp = NULL;
	sn_qp_error(q, IB_WC_WR_FLUSH_ERR);
	d->control_count = 0;
	sn_cq_remove_qp(sn_cq(ib->send_cq), ib);
	if (ib->recv_cq != ib->send_cq) sn_cq_remove_qp(sn_cq(ib->recv_cq), ib);
	/* Worker copies also hold d->lock. DMA slots contain copies only, never
	 * QP or MR pointers. The service rings remain primed across QP lifetime. */
	mutex_unlock(&d->lock);
	kvfree(q->outgoing); kvfree(q->incoming);
	kvfree(q->read_snapshot); kvfree(q->read_incoming);
	sn_schedule(d);
	return 0;
}
static int sn_modify_qp(struct ib_qp *ib, struct ib_qp_attr *a, int mask, struct ib_udata *udata)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_qp *q = sn_qp(ib);
	const struct ib_global_route *grh = rdma_ah_read_grh(&a->ah_attr);
	int required = IB_QP_STATE, ret = -EINVAL;

	mutex_lock(&d->lock);
	if (d->dead) { ret = -ENODEV; goto out; }
	if (!(mask & IB_QP_STATE) || !ib_modify_qp_is_ok(q->attr.qp_state, a->qp_state, IB_QPT_RC, mask)) goto out;
	if (a->qp_state == IB_QPS_ERR || a->qp_state == IB_QPS_RESET) {
		if (mask != required) goto out;
		sn_flush_qp(q, IB_WC_WR_FLUSH_ERR, false);
		if (a->qp_state == IB_QPS_RESET) {
			memset(&q->attr, 0, sizeof(q->attr));
			q->attr.qp_state = IB_QPS_RESET;
			do { q->binding.local_generation = get_random_u64(); } while (!q->binding.local_generation);
			q->binding.peer_generation = 0;
			d->control_count = 0;
		}
		ret = 0; goto out;
	}
	if (q->attr.qp_state == IB_QPS_RESET && a->qp_state == IB_QPS_INIT) {
		required |= IB_QP_PKEY_INDEX | IB_QP_PORT | IB_QP_ACCESS_FLAGS;
		if (mask != required || a->port_num != 1 || a->pkey_index ||
		    (a->qp_access_flags & ~SN_ACCESS_FLAGS)) goto out;
		q->attr.port_num = 1; q->attr.qp_access_flags = a->qp_access_flags;
	} else if (q->attr.qp_state == IB_QPS_INIT && a->qp_state == IB_QPS_RTR) {
		required |= IB_QP_AV | IB_QP_PATH_MTU | IB_QP_DEST_QPN | IB_QP_RQ_PSN |
			    IB_QP_MAX_DEST_RD_ATOMIC | IB_QP_MIN_RNR_TIMER;
		if (mask != required || a->path_mtu != IB_MTU_4096 || !a->dest_qp_num ||
		    a->dest_qp_num > SN_PSN_MAX || a->rq_psn > SN_PSN_MAX ||
		    a->max_dest_rd_atomic > 1 || a->min_rnr_timer != 12 ||
		    rdma_ah_get_port_num(&a->ah_attr) != 1 ||
		    rdma_ah_get_ah_flags(&a->ah_attr) != IB_AH_GRH ||
		    grh->sgid_index || grh->flow_label || grh->traffic_class || grh->hop_limit != 64 ||
		    rdma_ah_get_sl(&a->ah_attr) || rdma_ah_get_static_rate(&a->ah_attr) ||
		    memcmp(&grh->dgid, &d->peer_gid, sizeof(d->peer_gid))) goto out;
		q->attr.path_mtu = a->path_mtu; q->attr.dest_qp_num = a->dest_qp_num;
		q->attr.rq_psn = a->rq_psn; q->attr.max_dest_rd_atomic = a->max_dest_rd_atomic;
		q->attr.min_rnr_timer = a->min_rnr_timer;
		q->attr.ah_attr = a->ah_attr;
		/* Core releases this borrowed cache reference after modify_qp. */
		q->attr.ah_attr.grh.sgid_attr = NULL;
		q->binding.peer_qpn = a->dest_qp_num;
		q->receive.expected = a->rq_psn;
	} else if (q->attr.qp_state == IB_QPS_RTR && a->qp_state == IB_QPS_RTS) {
		required |= IB_QP_SQ_PSN | IB_QP_TIMEOUT | IB_QP_RETRY_CNT |
			    IB_QP_RNR_RETRY | IB_QP_MAX_QP_RD_ATOMIC;
		if (mask != required || a->sq_psn > SN_PSN_MAX || a->timeout != 14 ||
		    a->retry_cnt > 6 || a->rnr_retry > 6 || a->max_rd_atomic > 1) goto out;
		q->attr.sq_psn = a->sq_psn; q->attr.timeout = a->timeout;
		q->attr.retry_cnt = a->retry_cnt; q->attr.rnr_retry = a->rnr_retry;
		q->attr.max_rd_atomic = a->max_rd_atomic;
		q->next_sequence = a->sq_psn;
	} else { ret = -EOPNOTSUPP; goto out; }
	q->attr.qp_state = a->qp_state;
	ret = 0;
out:
	mutex_unlock(&d->lock);
	if (!ret) sn_schedule(d);
	return ret;
}
static int sn_query_qp(struct ib_qp *ib, struct ib_qp_attr *a, int mask, struct ib_qp_init_attr *init)
{
	struct sn_device *d = sn_dev(ib->device);
	mutex_lock(&d->lock);
	*a = sn_qp(ib)->attr; *init = sn_qp(ib)->init;
	a->cur_qp_state = a->qp_state;
	mutex_unlock(&d->lock);
	return 0;
}
static int sn_post_send(struct ib_qp *ib, const struct ib_send_wr *wr, const struct ib_send_wr **bad)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_qp *q = sn_qp(ib);
	int ret = 0;

	mutex_lock(&d->lock);
	for (; wr; wr = wr->next) {
		struct sn_wqe w = {0};
		const struct ib_sge *s = wr->sg_list;
		if (d->dead || q->destroying || q->attr.qp_state != IB_QPS_RTS) { ret = -EINVAL; break; }
		if (wr->opcode != IB_WR_SEND && wr->opcode != IB_WR_RDMA_WRITE && wr->opcode != IB_WR_RDMA_READ) { ret = -EOPNOTSUPP; break; }
		if (wr->send_flags & ~(IB_SEND_SIGNALED | IB_SEND_FENCE)) { ret = -EOPNOTSUPP; break; }
		if (wr->opcode == IB_WR_RDMA_READ && !q->attr.max_rd_atomic) { ret = -EINVAL; break; }
		if (wr->num_sge != 1 || !s || s->length > SN_MESSAGE) { ret = -EINVAL; break; }
		if (q->sq_count == q->init.cap.max_send_wr) { ret = -ENOMEM; break; }
		w.mr = sn_mr_get(q, s->lkey, s->addr, s->length, wr->opcode == IB_WR_RDMA_READ ? IB_ACCESS_LOCAL_WRITE : 0);
		if (!w.mr) { ret = -EACCES; break; }
		w.id = wr->wr_id; w.address = s->addr; w.length = s->length;
		w.flags = wr->send_flags;
		w.opcode = wr->opcode == IB_WR_SEND ? SN_SEND : wr->opcode == IB_WR_RDMA_READ ? SN_READ : SN_WRITE;
		if (w.opcode != SN_SEND) {
			w.remote_address = rdma_wr(wr)->remote_addr; w.rkey = rdma_wr(wr)->rkey;
			if (!w.rkey || w.remote_address > U64_MAX - w.length) {
				sn_mr_put(w.mr); ret = -EINVAL; break;
			}
		}
		q->sq[(q->sq_head + q->sq_count) % SN_QUEUE_DEPTH] = w;
		if (!q->sq_count) q->progress_deadline = ktime_get_ns() + 30000000000ULL;
		q->sq_count++;
	}
	if (ret) *bad = wr;
	mutex_unlock(&d->lock);
	sn_schedule(d);
	return ret;
}
static int sn_post_recv(struct ib_qp *ib, const struct ib_recv_wr *wr, const struct ib_recv_wr **bad)
{
	struct sn_device *d = sn_dev(ib->device);
	struct sn_qp *q = sn_qp(ib);
	int ret = 0;

	mutex_lock(&d->lock);
	for (; wr; wr = wr->next) {
		struct sn_wqe w = {0};
		const struct ib_sge *s = wr->sg_list;
		if (d->dead || q->destroying || q->attr.qp_state == IB_QPS_RESET || q->attr.qp_state == IB_QPS_ERR) { ret = -EINVAL; break; }
		if (wr->num_sge != 1 || !s || s->length > SN_MESSAGE) { ret = -EINVAL; break; }
		if (q->rq_count == q->init.cap.max_recv_wr) { ret = -ENOMEM; break; }
		w.mr = sn_mr_get(q, s->lkey, s->addr, s->length, IB_ACCESS_LOCAL_WRITE);
		if (!w.mr) { ret = -EACCES; break; }
		w.id = wr->wr_id; w.address = s->addr; w.length = s->length;
		q->rq[(q->rq_head + q->rq_count) % SN_QUEUE_DEPTH] = w;
		q->rq_count++;
	}
	if (ret) *bad = wr;
	mutex_unlock(&d->lock);
	sn_schedule(d);
	return ret;
}
static ssize_t native_stats_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct sn_device *d = sn_dev(container_of(dev, struct ib_device, dev));
	ssize_t n;

	mutex_lock(&d->lock);
	n = sysfs_emit(buf, "tx_frames %llu\nrx_frames %llu\nbad_frames %llu\nplaced_send_bytes %llu\nplaced_write_bytes %llu\nplaced_read_bytes %llu\n",
		d->tx_frames, d->rx_frames, d->bad_frames, d->send_bytes,
		d->write_bytes, d->read_bytes);
	mutex_unlock(&d->lock);
	return n;
}
static DEVICE_ATTR_RO(native_stats);
static struct attribute *sn_attributes[] = { &dev_attr_native_stats.attr, NULL };
static const struct attribute_group sn_device_group = { .attrs = sn_attributes };
const struct ib_device_ops sn_verbs_ops = {
	.owner = THIS_MODULE, .driver_id = RDMA_DRIVER_UNKNOWN,
	.uverbs_abi_ver = SN_ABI, .device_group = &sn_device_group,
	.query_device = sn_query_device, .query_port = sn_query_port,
	.get_port_immutable = sn_immutable, .get_link_layer = sn_link_layer,
	.query_gid = sn_query_gid, .query_pkey = sn_query_pkey,
	.alloc_ucontext = sn_alloc_context, .dealloc_ucontext = sn_dealloc_context,
	.disassociate_ucontext = sn_disassociate_context,
	.alloc_pd = sn_alloc_pd, .dealloc_pd = sn_dealloc_pd,
	.create_user_cq = sn_create_cq, .destroy_cq = sn_destroy_cq,
	.poll_cq = sn_poll_cq, .req_notify_cq = sn_notify_cq,
	.create_qp = sn_create_qp, .destroy_qp = sn_destroy_qp,
	.modify_qp = sn_modify_qp, .query_qp = sn_query_qp,
	.post_send = sn_post_send, .post_recv = sn_post_recv,
	.reg_user_mr = sn_reg_user_mr, .dereg_mr = sn_dereg_mr,
	.size_ib_cq = sizeof(struct sn_cq), .size_ib_pd = sizeof(struct ib_pd),
	.size_ib_qp = sizeof(struct sn_qp), .size_ib_ucontext = sizeof(struct ib_ucontext),
};
