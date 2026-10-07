// SPDX-License-Identifier: GPL-2.0-only
/* Exact production CQ/post/MR/flush bodies, modeled types/locks/umem/scheduler.
 * Not a kernel ABI, page-pinning, uverbs, concurrency or DMA test. */
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../verbs/native_order.h"
typedef uint32_t u32;
typedef uint64_t u64;
#define U64_MAX UINT64_MAX
#define container_of(p, type, field) ((type *)((char *)(p) - offsetof(type, field)))
#define lockdep_assert_held(p) assert(*(p) == 1)
#define WARN_ON(v) assert(!(v))
#define WRITE_ONCE(x, v) ((x) = (v))
#define kfree free
#define IB_QPS_RESET 0
#define IB_QPS_RTS 3
#define IB_QPS_ERR 6
#define IB_SEND_FENCE 1
#define IB_SEND_SIGNALED 2
#define IB_ACCESS_LOCAL_WRITE 1
#define IB_ACCESS_REMOTE_READ 4
#define IB_CQ_SOLICITED 1
#define IB_CQ_NEXT_COMP 2
#define IB_CQ_SOLICITED_MASK 3
#define IB_CQ_REPORT_MISSED_EVENTS 4
#define IB_WR_SEND 0
#define IB_WR_RDMA_WRITE 1
#define IB_WR_RDMA_READ 2
#define IB_WC_SEND 0
#define IB_WC_RDMA_WRITE 1
#define IB_WC_RDMA_READ 2
#define IB_WC_RECV 3
struct list_head { struct list_head *next, *prev; };
static void list_add_tail(struct list_head *n, struct list_head *h)
{ n->prev = h->prev; n->next = h; h->prev->next = n; h->prev = n; }
static void list_del(struct list_head *n) { n->prev->next = n->next; n->next->prev = n->prev; }
#define list_for_each_entry(p, h, field) \
    for (struct list_head *node = (h)->next; node != (h) && ((p) = container_of(node, struct sn_mr, field)); node = node->next)
struct ib_device { int dummy; };
struct ib_pd { int dummy; };
struct ib_udata { int dummy; };
struct ib_event;
struct ib_cq { struct ib_device *device; int cqe; void *cq_context;
	void (*event_handler)(struct ib_event *, void *);
	void (*comp_handler)(struct ib_cq *, void *); };
struct ib_qp { u32 qp_num; struct ib_device *device; struct ib_pd *pd; struct ib_cq *send_cq, *recv_cq;
	void *qp_context; void (*event_handler)(struct ib_event *, void *); };
struct ib_mr { struct ib_device *device; struct ib_pd *pd; u32 lkey; };
enum ib_wc_status { IB_WC_SUCCESS, IB_WC_GENERAL_ERR, IB_WC_WR_FLUSH_ERR };
enum ib_event_type { IB_EVENT_CQ_ERR, IB_EVENT_QP_FATAL };
enum ib_cq_notify_flags { UNUSED_NOTIFY_ENUM };
struct ib_event { struct ib_device *device; enum ib_event_type event;
	union { struct ib_cq *cq; struct ib_qp *qp; } element; };
struct ib_wc { u64 wr_id; enum ib_wc_status status; struct ib_qp *qp; unsigned opcode, byte_len; };
struct device { int dummy; };
struct scatterlist { int dummy; };
enum dma_data_direction { DMA_BIDIRECTIONAL, DMA_TO_DEVICE, DMA_FROM_DEVICE };
struct fake_umem { struct { struct { struct scatterlist *sgl; int orig_nents; } sgt; } sgt_append; };
static void dma_unmap_sg(struct device *dev, struct scatterlist *sg, int n, enum dma_data_direction dir)
{ (void)dev; (void)sg; (void)n; (void)dir; }
struct sn_mr { struct ib_mr ib; struct list_head entry; u64 base, size, pinned;
	u32 access, users; int map_nents; struct device *map_dev; struct fake_umem *umem; };
struct sn_wqe { struct sn_mr *mr; u64 id, address, remote_address; u32 length, flags, opcode, rkey; };
struct sn_cq { struct ib_cq ib; struct ib_wc entries[SN_MAX_CQE]; unsigned head, count, notify; bool failed; };
struct sn_qp { struct ib_qp ib; struct { int qp_state; unsigned max_rd_atomic; } attr;
	struct { struct { unsigned max_send_wr, max_recv_wr; } cap; } init;
	struct sn_wqe sq[4], rq[4]; unsigned sq_head, sq_count, rq_head, rq_count;
	bool all_signaled, bound, destroying; u64 progress_deadline; };
struct sn_device { struct ib_device ib; int lock; bool dead;
	struct list_head mrs; unsigned mr_count; u64 pinned; struct ib_qp wc_qp[2]; };
struct ib_sge { u64 addr; u32 length, lkey; };
struct ib_send_wr { const struct ib_send_wr *next; u64 wr_id;
	int opcode, send_flags, num_sge; struct ib_sge *sg_list; };
struct ib_rdma_wr { struct ib_send_wr wr; u64 remote_addr; u32 rkey; };
struct ib_recv_wr { const struct ib_recv_wr *next; u64 wr_id; int num_sge; struct ib_sge *sg_list; };
#define rdma_wr(w) container_of(w, struct ib_rdma_wr, wr)
static struct sn_device *sn_dev(struct ib_device *p) { return container_of(p, struct sn_device, ib); }
static struct sn_qp *sn_qp(struct ib_qp *p) { return container_of(p, struct sn_qp, ib); }
static struct sn_cq *sn_cq(struct ib_cq *p) { return container_of(p, struct sn_cq, ib); }
static void mutex_lock(int *p) { assert(!*p); *p = 1; }
static void mutex_unlock(int *p) { assert(*p); *p = 0; }
static unsigned unpins, completions, events, scheduled;
static void ib_umem_release(struct fake_umem *p) { assert(p); unpins++; }
static void sn_schedule(struct sn_device *d) { assert(!d->lock); scheduled++; }
static void sn_engine_reset(struct sn_qp *q) { (void)q; }
static u64 ktime_get_ns(void) { return 100; }
#include "native-objects.inc"
static void on_completion(struct ib_cq *cq, void *context) { (void)cq; (void)context; completions++; }
static void on_event(struct ib_event *e, void *context) { (void)e; (void)context; events++; }
static struct sn_device d;
static struct sn_cq cq;
static struct sn_qp qp;
static struct ib_pd pd, other_pd;
static struct sn_mr *mr;
static void setup(void)
{
	memset(&d, 0, sizeof(d)); memset(&cq, 0, sizeof(cq)); memset(&qp, 0, sizeof(qp));
	d.mrs.next = d.mrs.prev = &d.mrs;
	cq.ib.device = &d.ib; cq.ib.cqe = 8; cq.ib.comp_handler = on_completion; cq.ib.event_handler = on_event;
	qp.ib.qp_num = 1; qp.ib.device = &d.ib; qp.ib.pd = &pd; qp.ib.send_cq = qp.ib.recv_cq = &cq.ib;
	qp.ib.event_handler = on_event; qp.attr.qp_state = IB_QPS_RTS; qp.attr.max_rd_atomic = 1;
	qp.init.cap.max_send_wr = qp.init.cap.max_recv_wr = 4;
	mr = calloc(1, sizeof(*mr)); assert(mr);
	mr->ib.device = &d.ib; mr->ib.pd = &pd; mr->ib.lkey = 17;
	mr->base = 1000; mr->size = SN_MR_BYTES; mr->pinned = SN_MR_BYTES;
	mr->access = IB_ACCESS_LOCAL_WRITE;
	mr->umem = calloc(1, sizeof(*mr->umem)); assert(mr->umem);
	list_add_tail(&mr->entry, &d.mrs); d.mr_count = 1; d.pinned = mr->pinned;
	unpins = completions = events = scheduled = 0;
}
static void cleanup(void)
{
	assert(!sn_dereg_mr(&mr->ib, NULL));
	assert(unpins == 1 && !d.mr_count && !d.pinned && d.mrs.next == &d.mrs);
}
static void post_and_lifetime(void)
{
	struct ib_sge sg = { .addr = 1000, .length = SN_MESSAGE, .lkey = 17 };
	struct ib_send_wr send[3] = {}, *bad_mut = NULL;
	const struct ib_send_wr *bad = bad_mut;
	struct ib_recv_wr recv[2] = {};
	const struct ib_recv_wr *rbad;
	struct ib_wc wc[8];
	setup();
	for (unsigned i = 0; i < 3; i++) {
		send[i] = (struct ib_send_wr){ .wr_id = i+1, .sg_list = &sg, .num_sge = 1,
			.next = i < 2 ? &send[i+1] : NULL, .opcode = IB_WR_SEND };
	}
	send[1].num_sge = 2;
	assert(sn_post_send(&qp.ib, send, &bad) == -EINVAL && bad == &send[1]);
	assert(qp.sq_count == 1 && mr->users == 1 && scheduled == 1);
	assert(sn_dereg_mr(&mr->ib, NULL) == -EBUSY && !unpins && d.mr_count == 1);
	recv[0] = (struct ib_recv_wr){ .wr_id = 101, .sg_list = &sg, .num_sge = 1, .next = &recv[1] };
	recv[1] = recv[0]; recv[1].next = NULL; recv[1].num_sge = 0;
	assert(sn_post_recv(&qp.ib, recv, &rbad) == -EINVAL && rbad == &recv[1]);
	assert(qp.rq_count == 1 && mr->users == 2);
	mutex_lock(&d.lock); sn_qp_error(&qp, IB_WC_GENERAL_ERR); mutex_unlock(&d.lock);
	assert(!mr->users && !qp.sq_count && !qp.rq_count && events == 1);
	assert(sn_poll_cq(&cq.ib, 8, wc) == 2);
	assert(wc[0].wr_id == 1 && wc[0].status == IB_WC_GENERAL_ERR); /* unsignaled error is visible */
	assert(wc[1].wr_id == 101 && wc[1].status == IB_WC_WR_FLUSH_ERR);
	cleanup();
	puts("ok 1 - real post prefix, SGE rejection, EBUSY until error flush, unsignaled error CQE");
}
static void keys_and_sizes(void)
{
	struct ib_sge sg = { .addr = 1000, .length = SN_MESSAGE, .lkey = 17 };
	struct ib_send_wr wr = { .sg_list = &sg, .num_sge = 1, .opcode = IB_WR_SEND };
	struct ib_rdma_wr rd = { .wr = { .sg_list = &sg, .num_sge = 1, .opcode = IB_WR_RDMA_READ }, .rkey = 18, .remote_addr = 2000 };
	const struct ib_send_wr *bad;
	setup();
	sg.length = SN_MESSAGE+1; assert(sn_post_send(&qp.ib, &wr, &bad) == -EINVAL && bad == &wr);
	sg.length = SN_MESSAGE; sg.lkey = 16; assert(sn_post_send(&qp.ib, &wr, &bad) == -EACCES);
	sg.lkey = 17; sg.addr = UINT64_MAX; assert(sn_post_send(&qp.ib, &wr, &bad) == -EACCES);
	sg.addr = 1000; mr->ib.pd = &other_pd; assert(sn_post_send(&qp.ib, &wr, &bad) == -EACCES);
	mr->ib.pd = &pd; mr->access = 0; assert(sn_post_send(&qp.ib, &rd.wr, &bad) == -EACCES);
	mr->access = IB_ACCESS_LOCAL_WRITE; rd.remote_addr = UINT64_MAX;
	assert(sn_post_send(&qp.ib, &rd.wr, &bad) == -EINVAL && !mr->users);
	rd.remote_addr = 2000; rd.rkey = 0; assert(sn_post_send(&qp.ib, &rd.wr, &bad) == -EINVAL && !mr->users);
	wr.send_flags = 4; assert(sn_post_send(&qp.ib, &wr, &bad) == -EOPNOTSUPP);
	wr.send_flags = 0; wr.opcode = 99; assert(sn_post_send(&qp.ib, &wr, &bad) == -EOPNOTSUPP);
	wr.opcode = IB_WR_SEND; sg.length = 0;
	for (unsigned i = 0; i < 4; i++) { wr.wr_id = i; assert(!sn_post_send(&qp.ib, &wr, &bad)); }
	assert(sn_post_send(&qp.ib, &wr, &bad) == -ENOMEM && mr->users == 4);
	mutex_lock(&d.lock); sn_qp_error(&qp, IB_WC_GENERAL_ERR); mutex_unlock(&d.lock);
	assert(!mr->users);
	cleanup();
	puts("ok 2 - real key/PD/range/local-write/remote-overflow/flags/size/queue validation");
}
static void cq_order_notify_overflow(void)
{
	struct sn_wqe w = { .id = 1, .opcode = SN_SEND };
	struct ib_wc wc[8];
	setup();
	assert(!sn_notify_cq(&cq.ib, IB_CQ_NEXT_COMP));
	assert(!sn_notify_cq(&cq.ib, IB_CQ_SOLICITED)); /* cannot weaken NEXT */
	mutex_lock(&d.lock);
	assert(!sn_complete(&qp, &w, false, IB_WC_SUCCESS, 1) && !cq.count); /* unsignaled */
	w.flags = IB_SEND_SIGNALED;
	assert(!sn_complete(&qp, &w, false, IB_WC_SUCCESS, 1));
	assert(completions == 1 && !cq.notify);
	w.id = 2; assert(!sn_complete(&qp, &w, true, IB_WC_SUCCESS, 2));
	mutex_unlock(&d.lock);
	assert(sn_notify_cq(&cq.ib, IB_CQ_NEXT_COMP|IB_CQ_REPORT_MISSED_EVENTS) == 1);
	assert(sn_poll_cq(&cq.ib, 8, wc) == 2 && wc[0].wr_id == 1 && wc[1].wr_id == 2);
	assert(wc[0].opcode == IB_WC_SEND && wc[1].opcode == IB_WC_RECV && wc[1].byte_len == 2);
	mutex_lock(&d.lock);
	for (unsigned i = 0; i < 8; i++) { w.id = 10+i; assert(!sn_complete(&qp, &w, true, IB_WC_SUCCESS, 0)); }
	assert(sn_complete(&qp, &w, true, IB_WC_SUCCESS, 0) == -ENOSPC && cq.failed && events == 1);
	mutex_unlock(&d.lock);
	assert(sn_notify_cq(&cq.ib, IB_CQ_NEXT_COMP) == -EIO);
	assert(sn_poll_cq(&cq.ib, 8, wc) == 8);
	for (unsigned i = 0; i < 8; i++) assert(wc[i].wr_id == 10+i);
	assert(sn_poll_cq(&cq.ib, 1, wc) == -EIO);
	/* QP destruction compacts that QP's unread CQEs. */
	cq.failed = false;
	mutex_lock(&d.lock); assert(!sn_complete(&qp, &w, true, IB_WC_SUCCESS, 0));
	sn_cq_remove_qp(&cq, &qp.ib); assert(!cq.count); mutex_unlock(&d.lock);
	cleanup();
	puts("ok 3 - real CQ ordering, one-shot notify/missed events, overflow preservation and destroyed-QP CQE compaction");
}
/* Models the uverbs POLL_CQ/DESTROY_QP interleaving: the core reads
 * wc->qp->qp_num after poll_cq drops d->lock, possibly after the QP is freed. */
static void cqe_identity_outlives_qp(void)
{
	struct sn_wqe w = { .id = 5, .opcode = SN_SEND, .flags = IB_SEND_SIGNALED };
	struct ib_wc wc[2];
	struct sn_qp *old = malloc(sizeof(*old));
	setup();
	assert(old);
	*old = qp; old->ib.qp_num = 7;
	mutex_lock(&d.lock);
	assert(!sn_complete(old, &w, false, IB_WC_SUCCESS, 0));
	w.id = 6; assert(!sn_complete(old, &w, true, IB_WC_SUCCESS, 0));
	mutex_unlock(&d.lock);
	assert(sn_poll_cq(&cq.ib, 1, wc) == 1 && wc[0].wr_id == 5);
	mutex_lock(&d.lock); sn_cq_remove_qp(&cq, &old->ib); assert(!cq.count); mutex_unlock(&d.lock);
	assert((char *)wc[0].qp + sizeof(*wc[0].qp) <= (char *)old ||
	       (char *)wc[0].qp >= (char *)(old + 1));
	memset(old, 0xa5, sizeof(*old)); free(old);
	assert(wc[0].qp->qp_num == 7);
	/* The next QP's completions do not relabel the in-flight CQE. */
	qp.ib.qp_num = 8; w.id = 9;
	mutex_lock(&d.lock); assert(!sn_complete(&qp, &w, false, IB_WC_SUCCESS, 0)); mutex_unlock(&d.lock);
	assert(sn_poll_cq(&cq.ib, 1, &wc[1]) == 1 && wc[1].wr_id == 9);
	assert(wc[1].qp->qp_num == 8 && wc[0].qp->qp_num == 7 && wc[1].qp != &qp.ib);
	cleanup();
	puts("ok 4 - CQE QP identity stays valid after the QP is destroyed and freed between poll and copy");
}
int main(void)
{
	puts("1..4"); post_and_lifetime(); keys_and_sizes(); cq_order_notify_overflow();
	cqe_identity_outlives_qp(); return 0;
}
