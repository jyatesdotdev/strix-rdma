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
#define READ_ONCE(x) (x)
/* cq->lock: a held/not-held flag, like the d->lock fake; never nested. */
#define spin_lock_init(l) (*(l) = 0)
#define spin_lock_irqsave(l, f) do { (void)(f); assert(!*(l)); *(l) = 1; } while (0)
#define spin_unlock_irqrestore(l, f) do { (void)(f); assert(*(l) == 1); *(l) = 0; } while (0)
#define kfree free
#define IB_QPS_RESET 0
#define IB_QPS_RTS 3
#define IB_QPS_ERR 6
#define IB_SEND_FENCE 1
#define IB_SEND_SIGNALED 2
#define IB_ACCESS_LOCAL_WRITE 1
#define IB_ACCESS_REMOTE_WRITE 2
#define IB_ACCESS_REMOTE_READ 4
#define SN_ACCESS_FLAGS (IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ)
#define PAGE_SIZE 4096ULL
#define PAGE_MASK (~(PAGE_SIZE - 1))
#define PAGE_ALIGN(x) (((x) + PAGE_SIZE - 1) & PAGE_MASK)
#define ULONG_MAX UINT64_MAX
#define U32_MAX UINT32_MAX
#define ERR_PTR(e) ((void *)(intptr_t)(e))
#define PTR_ERR(p) ((int)(intptr_t)(p))
#define IS_ERR(p) ((uintptr_t)(p) >= (uintptr_t)-4095)
#define kzalloc(n, f) calloc(1, (n))
#define GFP_KERNEL 0
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
struct ib_pd { struct ib_device *device; };
struct ib_udata { int dummy; };
struct ib_event;
struct ib_cq { struct ib_device *device; int cqe; void *cq_context;
	void (*event_handler)(struct ib_event *, void *);
	void (*comp_handler)(struct ib_cq *, void *); };
struct ib_qp { u32 qp_num; struct ib_device *device; struct ib_pd *pd; struct ib_cq *send_cq, *recv_cq;
	void *qp_context; void (*event_handler)(struct ib_event *, void *); };
struct ib_mr { struct ib_device *device; struct ib_pd *pd; u32 lkey, rkey; u64 iova, length; };
struct ib_dmah; struct uverbs_attr_bundle;
enum ib_wc_status { IB_WC_SUCCESS, IB_WC_GENERAL_ERR, IB_WC_WR_FLUSH_ERR };
enum ib_event_type { IB_EVENT_CQ_ERR, IB_EVENT_QP_FATAL };
enum ib_cq_notify_flags { UNUSED_NOTIFY_ENUM };
struct ib_event { struct ib_device *device; enum ib_event_type event;
	union { struct ib_cq *cq; struct ib_qp *qp; } element; };
struct ib_wc { u64 wr_id; enum ib_wc_status status; struct ib_qp *qp; unsigned opcode, byte_len; };
struct device { int dummy; };
struct scatterlist { int dummy; };
enum dma_data_direction { DMA_BIDIRECTIONAL, DMA_TO_DEVICE, DMA_FROM_DEVICE };
struct fake_umem { struct { struct { struct scatterlist *sgl; int nents, orig_nents; } sgt; } sgt_append; };
static unsigned dma_unmaps, vunmaps;
static void dma_unmap_sg(struct device *dev, struct scatterlist *sg, int n, enum dma_data_direction dir)
{ (void)dev; (void)sg; (void)n; (void)dir; dma_unmaps++; }
/* GPU-memory (DMA-BUF) MR: only what sn_mr_release() touches. */
struct iosys_map { void *vaddr; };
struct dma_buf { int dummy; };
struct dma_buf_attachment { struct dma_buf *dmabuf; };
struct ib_umem_dmabuf { struct fake_umem umem; struct dma_buf_attachment *attach; };
static void dma_buf_vunmap_unlocked(struct dma_buf *b, struct iosys_map *map)
{ assert(b && map && map->vaddr); map->vaddr = NULL; vunmaps++; }
/* Modeled import: records the device the attachment targets, can fail, and can
 * mark the device dead while the lock is dropped (a hot unplug mid-import). */
struct tb_nhi { struct device *dev; };
struct tb { struct tb_nhi *nhi; };
struct tb_xdomain { struct tb *tb; };
struct tb_service { struct tb_xdomain *xd; };
static struct tb_xdomain *tb_service_parent(struct tb_service *s) { return s->xd; }
static struct dma_buf import_buf;
static struct dma_buf_attachment import_att = { .dmabuf = &import_buf };
static struct ib_umem_dmabuf import_umem = { .attach = &import_att };
static struct device *import_dev;
static int import_err, vmap_err, imports;
static bool *import_kills;
static char vmap_backing[16];
static struct ib_umem_dmabuf *ib_umem_dmabuf_get_pinned_with_dma_device(struct ib_device *ib,
	struct device *dev, unsigned long offset, size_t size, int fd, int access)
{
	(void)ib; (void)offset; (void)size; (void)fd; (void)access;
	imports++; import_dev = dev;
	if (import_kills) *import_kills = true;
	if (import_err) return ERR_PTR(import_err);
	import_umem.umem.sgt_append.sgt.nents = 7;
	return &import_umem;
}
static int dma_buf_vmap_unlocked(struct dma_buf *b, struct iosys_map *map)
{ assert(b == &import_buf); if (vmap_err) return vmap_err; map->vaddr = vmap_backing; return 0; }
struct sn_mr { struct ib_mr ib; struct list_head entry; u64 base, size, pinned;
	u32 access, users; int map_nents; struct device *map_dev; struct fake_umem *umem;
	struct ib_umem_dmabuf *dmabuf; struct iosys_map vmap;
	struct scatterlist *cur_sg; unsigned cur_idx; u64 cur_off; };
struct sn_wqe { struct sn_mr *mr; u64 id, address, remote_address; u32 length, flags, opcode, rkey; };
struct sn_cq { struct ib_cq ib; int lock; struct ib_wc entries[SN_MAX_CQE]; unsigned head, count, notify; bool failed; };
struct sn_qp { struct ib_qp ib; struct { int qp_state; unsigned max_rd_atomic; } attr;
	struct { struct { unsigned max_send_wr, max_recv_wr; } cap; } init;
	struct sn_wqe sq[4], rq[4]; unsigned sq_head, sq_count, rq_head, rq_count;
	bool all_signaled, bound, destroying; u64 progress_deadline; };
struct sn_device { struct ib_device ib; int lock; bool dead;
	struct list_head mrs; unsigned mr_count; u64 pinned, key_serial; struct ib_qp wc_qp[2];
	struct tb_service *service; };
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
static void sn_stall_snapshot(struct sn_device *d, struct sn_qp *q, int status) { (void)d; (void)q; (void)status; }
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
/* sn_dereg_mr() through sn_mr_release(): a CPU MR mapped for the NHI is
 * unmapped; a GPU (DMA-BUF) MR is never dma-unmapped here -- its exporter owns
 * that mapping -- but its CPU mapping is dropped; both release the umem once. */
static void mr_release_kinds(void)
{
	static struct dma_buf buf;
	static struct dma_buf_attachment att = { .dmabuf = &buf };
	struct ib_umem_dmabuf *gpu;
	char backing[8];

	setup();
	mr->map_nents = 3; dma_unmaps = vunmaps = 0;
	cleanup();
	assert(dma_unmaps == 1 && !vunmaps);
	setup();
	gpu = calloc(1, sizeof(*gpu)); assert(gpu);
	gpu->attach = &att;
	free(mr->umem); mr->umem = &gpu->umem; mr->dmabuf = gpu;
	mr->map_nents = 5; mr->vmap.vaddr = backing; dma_unmaps = vunmaps = 0;
	assert(!sn_dereg_mr(&mr->ib, NULL));
	assert(unpins == 1 && !dma_unmaps && vunmaps == 1 && !d.mr_count && !d.pinned);
	free(gpu);
	puts("ok 5 - MR release: CPU MR dma-unmapped, GPU (DMA-BUF) MR vunmapped and never dma-unmapped");
}
/* sn_reg_user_mr_dmabuf(): validation happens before any import; the import
 * targets the NHI's DMA device; the slot and quota reserved across the unlocked
 * import are returned on every failure (import, vmap, or the device dying
 * meanwhile, which also releases what was imported); success publishes the MR
 * with the exporter's segment count as its NHI mapping. */
static void dmabuf_registration(void)
{
	static struct device nhi_dev;
	static struct tb_nhi nhi = { .dev = &nhi_dev };
	static struct tb tb = { .nhi = &nhi };
	static struct tb_xdomain xd = { .tb = &tb };
	static struct tb_service svc = { .xd = &xd };
	const int rw = IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ;
	struct ib_mr *ib;
	struct sn_mr *g;

	setup();
	pd.device = &d.ib; d.service = &svc; d.key_serial = 40; imports = 0;
	/* Rejected before any import. */
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 100, 4096, 0x10000, 3, rw, NULL, NULL)) == -EINVAL);
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, 0x10010, 3, rw, NULL, NULL)) == -EINVAL);
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 0, 0x10000, 3, rw, NULL, NULL)) == -EINVAL);
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, SN_MR_BYTES + 4096, 0x10000, 3, rw, NULL, NULL)) == -EINVAL);
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, 0x10000, 3,
		IB_ACCESS_REMOTE_WRITE, NULL, NULL)) == -EINVAL);
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, 0x10000, 3, 0x100, NULL, NULL)) == -EOPNOTSUPP);
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, UINT64_MAX & PAGE_MASK, 3, rw, NULL, NULL)) == -EINVAL);
	assert(!imports && d.mr_count == 1 && d.pinned == SN_MR_BYTES && !d.lock);
	/* Import, vmap and mid-import death all return the reservation. */
	import_err = -EBADF;
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, 0x10000, 3, rw, NULL, NULL)) == -EBADF);
	import_err = 0; vmap_err = -ENOMEM; unpins = 0;
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, 0x10000, 3, rw, NULL, NULL)) == -ENOMEM);
	assert(unpins == 1);
	vmap_err = 0; unpins = vunmaps = 0; import_kills = &d.dead;
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, 4096, 0x10000, 3, rw, NULL, NULL)) == -ENODEV);
	assert(unpins == 1 && vunmaps == 1);
	import_kills = NULL; d.dead = false;
	assert(imports == 3 && d.mr_count == 1 && d.pinned == SN_MR_BYTES && !d.lock && d.key_serial == 40);
	/* Success: NHI device, exporter mapping, accounting, distinct key. */
	ib = sn_reg_user_mr_dmabuf(&pd, 0x200000, 3 * 4096 + 1, 0x7f0000000000ULL, 3, rw, NULL, NULL);
	assert(!IS_ERR(ib) && import_dev == &nhi_dev);
	g = container_of(ib, struct sn_mr, ib);
	assert(g->dmabuf == &import_umem && g->umem == &import_umem.umem && g->vmap.vaddr == vmap_backing);
	assert(g->map_dev == &nhi_dev && g->map_nents == 7 && g->base == 0x7f0000000000ULL);
	assert(ib->lkey == 41 && ib->rkey == 41 && g->size == 3 * 4096 + 1 && g->pinned == 4 * 4096);
	assert(d.mr_count == 2 && d.pinned == SN_MR_BYTES + 4 * 4096 && !d.lock);
	/* Quota: three more 8 MiB MRs do not fit the 32 MiB aggregate. */
	assert(!IS_ERR(sn_reg_user_mr_dmabuf(&pd, 0, SN_MR_BYTES, 0x100000000ULL, 4, rw, NULL, NULL)));
	assert(!IS_ERR(sn_reg_user_mr_dmabuf(&pd, 0, SN_MR_BYTES, 0x200000000ULL, 5, rw, NULL, NULL)));
	assert(PTR_ERR(sn_reg_user_mr_dmabuf(&pd, 0, SN_MR_BYTES, 0x300000000ULL, 6, rw, NULL, NULL)) == -ENOMEM);
	assert(d.mr_count == 4 && !d.lock);
	puts("ok 6 - GPU (DMA-BUF) MR registration: validation first, NHI attach device, reservation returned on every failure");
}
int main(void)
{
	puts("1..6"); post_and_lifetime(); keys_and_sizes(); cq_order_notify_overflow();
	cqe_identity_outlives_qp(); mr_release_kinds(); dmabuf_registration(); return 0;
}
