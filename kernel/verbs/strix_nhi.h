/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef STRIX_NHI_H
#define STRIX_NHI_H
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/thunderbolt.h>
#include <linux/workqueue.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_umem.h>
#include "native_order.h"

#define SN_ABI 1
#define SN_RING_SIZE 64
/* Data ring interrupt moderation, matching thunderbolt_net's proven
 * TBNET_THROTTLING. At ~1.1 GiB/s a 4 KiB data frame otherwise raises one
 * interrupt each: measured 6,042,981 and 6,043,218 interrupts on the two data
 * rings against ~41k on the control rings. The control ring is deliberately
 * left unthrottled so ack latency stays at the ~36 us ping-pong floor. */
#define SN_DATA_THROTTLE_NS 128000
#define SN_CONTROL_RESERVE 8

/* Data-ring interrupt moderation, in ns; 0 disables it. Bulk throughput wants
 * long batching -- 128 us took the link from bimodal 18-1313 MiB/s to a
 * consistent 1182-1238 MiB/s by removing the per-frame interrupt storm -- but a
 * latency-sensitive sender moving small batches pays up to one window per batch.
 * Runtime-tunable (read-only: tb_ring_throttling requires a stopped ring, so a
 * new value takes effect on the next module load) so the knee can be measured
 * on hardware without a rebuild. Defined in main.c, consumed in ring.c. */
extern unsigned int sn_data_throttle_ns;

/* Opt-in zero-copy RDMA WRITE, defined in main.c and latched into
 * sn_device::zc_write by ring.c when the data plane comes up. */
extern bool sn_zc_write;
/* Opt-in zero-copy RDMA READ, same shape and latched the same way. */
extern bool sn_zc_read;
extern unsigned int sn_zc_read_fault;
#define SN_SERVICE "strixv1"
#define SN_CONTROL_BYTES 112
/* Handshake retransmission intervals; loss is expected, peers retransmit. */
#define SN_HELLO_INTERVAL_NS 100000000ULL
#define SN_BIND_INTERVAL_NS 100000000ULL
#define SN_ACCESS_FLAGS (IB_ACCESS_LOCAL_WRITE | IB_ACCESS_REMOTE_WRITE | IB_ACCESS_REMOTE_READ)

struct sn_device;
struct sn_mr {
	struct ib_mr ib;
	struct ib_umem *umem;
	struct list_head entry;
	u64 base, size, pinned;
	u32 access;
	unsigned int users;
	/* Scatterlist DMA-mapped for the NHI device at registration (zero-copy
	 * data-ring frames point directly at these addresses). 0 when unmapped. */
	int map_nents;
	struct device *map_dev;
	/* Sequential DMA-lookup cursor, valid only while map_nents != 0 and
	 * mutated only under d->lock. Bursts walk an MR forward one frame at a
	 * time, so resuming from the last position makes each lookup O(1)
	 * amortized instead of O(map_nents). Without it the per-frame walk was
	 * quadratic over a burst, and because dma_map_sg's merged segment count
	 * depends on allocation contiguity, throughput came out bimodal:
	 * ~1100 MiB/s on a freshly allocated contiguous MR vs 45-150 MiB/s on a
	 * fragmented one with ~2048 segments. */
	struct scatterlist *cur_sg;
	unsigned int cur_idx;
	u64 cur_off;
};
struct sn_cq {
	struct ib_cq ib;
	struct ib_wc entries[SN_MAX_CQE];
	unsigned int head, count, notify;
	bool failed;
};
struct sn_wqe {
	struct sn_mr *mr;
	u64 id, address, remote_address;
	u32 length, rkey, opcode, flags;
};
struct sn_qp {
	struct ib_qp ib;
	struct ib_qp_attr attr;
	struct ib_qp_init_attr init;
	struct sn_binding binding;
	struct sn_wqe sq[SN_QUEUE_DEPTH], rq[SN_QUEUE_DEPTH];
	unsigned int sq_head, sq_count, rq_head, rq_count;
	u8 *outgoing, *incoming, *read_snapshot, *read_incoming;
	struct sn_assembly receive, read_receive;
	struct sn_header read_request;
	struct sn_mr *remote_mr;
	u64 next_sequence, last_read, deadline, progress_deadline;
	u64 response_deadline, assembly_deadline, credit_revision;
	u32 last_credit;
	u32 outgoing_offset, response_offset, last_read_length;
	unsigned int retries, rnr_retries, response_retries;
	bool active, outgoing_sent, response_active, response_sent, have_last_read, rnr_wait;
	bool bound, all_signaled, destroying, credit_sent;
	/* Next BIND retransmission while unbound (ktime ns). */
	u64 next_bind;
	/* Zero-copy burst state: the sender walks NONE -> WAIT -> SENDING -> SENT;
	 * the receiver tracks the expected header-less payload by byte count. */
	unsigned int zc_state;
	u32 zc_offset;
	bool zc_expect;
	u32 zc_total, zc_received;
	/* Landing zone of the burst this side is receiving: the matched receive
	 * WQE's MR for SN_ZC_SEND, the requester-addressed remote MR for
	 * SN_ZC_WRITE, and for SN_ZC_READ the local MR of this side's own READ WQE,
	 * which the requester arms before the descriptor goes out. Set by
	 * sn_receive_zc (or the READ arm) before sn_data_recv_start and cleared the
	 * moment the expected byte count lands. Holds no reference of its own:
	 * the owner is the receive WQE, q->remote_mr or the SQ WQE, which all
	 * outlive it. */
	struct sn_mr *zc_mr;
	u64 zc_address;
	/* True while the armed landing zone belongs to this side's own zero-copy
	 * READ rather than to a peer-initiated SEND/WRITE burst: the completion is
	 * an SQ WQE and an SN_READ_ACK, not a receive WQE and an SN_ACK. */
	bool zc_read_armed;
	/* Requester: this zero-copy READ timed out and is being completed by a
	 * staged SN_READ. Responder: a staged SN_READ replaced the zero-copy
	 * response for the same operation and is waiting to be served. */
	bool zc_read_staged, response_fallback;
	/* This zero-copy READ was selected by zc_read_fault; cleared once injected. */
	bool zc_read_fault;
	/* Inbound burst descriptor held because the landing zone was busy with our
	 * own zero-copy READ response; re-driven by sn_engine_progress as soon as it
	 * frees. Header-less frames from two bursts could not be told apart, so one
	 * inbound burst at a time is the invariant this preserves. */
	bool zc_held;
	struct sn_header zc_held_header;
};
/* Ring and QP state frozen where a zero-copy stall is detected, before the QP
 * error flushes it: the responder's latest wait for its data TX ring to drain
 * under a READ fallback, and the latest QP error other than a flush on either
 * side. tx/rx are the NHI's own view of the data rings (tb_ring_get_stats):
 * hw_posted/hw_completed say whether the hardware still owns descriptors.
 * Diagnostics only; read through the stall_snap attribute. */
struct sn_stall_snap {
	u64 count, at_ns;
	int status, tx_ret, rx_ret;
	u32 data_tx_head, data_tx_tail, data_rx_head, data_rx_tail, zdata_posted;
	u32 zc_received, zc_total, response_offset;
	int zc_expect;
	unsigned int zc_state;
	bool qp, zc_read_staged, zc_read_armed, response_active, response_sent, response_fallback;
	struct tb_ring_stats tx, rx;
};
struct sn_slot {
	struct ring_frame frame;
	struct sn_device *device;
	void *buffer;
	bool done, canceled;
};
struct sn_device {
	struct ib_device ib;
	struct tb_service *service;
	struct net_device *netdev;
	struct notifier_block net_notifier;
	/* Serializes uverbs objects, worker copies, admission, and property state.
	 * Ring callbacks only set slot flags under ring_lock; never take lock. */
	struct mutex lock;
	spinlock_t ring_lock;
	/* Resident poller: replaces the delayed_work whose dispatch latency
	 * (ring_work then the work item) dominated receive latency.
	 * poll_kick is the explicit wake latch: wait_event re-tests its
	 * condition on every wake and sleeps again when it is false, so a
	 * bare wake_up can be lost when the ring peek has not yet observed
	 * the frame. Every sn_schedule sets the latch first. */
	struct task_struct *thread;
	wait_queue_head_t poll_wait;
	bool poll_kick;
	struct tb_ring *tx_ring, *rx_ring;
	struct sn_slot tx[SN_RING_SIZE], rx[SN_RING_SIZE];
	unsigned int tx_head, tx_tail, rx_tail;
	/* Zero-copy data ring pair (header-less payload frames landing directly
	 * in MR pages). Present only when a second ring HopID is available
	 * (thunderbolt-net absent); otherwise the driver is staged-only. */
	struct tb_ring *data_tx_ring, *data_rx_ring;
	struct sn_slot data_tx[SN_RING_SIZE], data_rx[SN_RING_SIZE];
	unsigned int data_tx_head, data_tx_tail, data_rx_head, data_rx_tail;
	u32 zdata_posted;	/* bytes of the expected burst posted to data_rx_ring */
	bool zdata;
	/* Latched from the zc_write module parameter when the data plane comes up.
	 * A per-device field rather than a global read in protocol.c so the offline
	 * engine suite (which compiles protocol.c against stubs and never links
	 * main.c) needs no new symbol. */
	bool zc_write;
	/* Latched from the zc_read module parameter the same way; default off. */
	bool zc_read;
	int in_hop, out_hop;
	/* Zero-copy data-plane HopIDs (second path tuple); -1 when unavailable. */
	int in_hop2, out_hop2;
	bool zc_unavail;
	bool paths_active, rings_started, rx_primed, dead, registered, net_gone;
	bool reset_rings, static_published;
	bool have_context, have_pd;
	unsigned int cq_count, mr_count;
	u64 pinned, key_serial, qpn_serial;
	u64 tx_frames, rx_frames, bad_frames, send_bytes, write_bytes, read_bytes;
	u64 zc_bursts, zc_aborts, zc_retries;
	/* Poller diagnostics: written only by the poller thread. */
	u64 poll_passes, poll_sleeps, poll_wake_kick, poll_wake_tick;
	u64 sched_calls, rnr_sent;
	/* Zero-copy READ observability. zc_retries cannot see READ retransmits: a
	 * READ requester queues no payload, so its zc_state stays SN_ZC_NONE and the
	 * zc_retries increment in the retry path never fires. These count what it
	 * misses, plus the two silent-drop outcomes of an unexpected sequence. */
	u64 zc_read_retx, zc_read_dup_inflight, zc_read_rearm, zc_seq_ahead, zc_seq_stale;
	/* zc_read_dup_inflight = posting + undrained. */
	u64 zc_read_dup_posting, zc_read_dup_undrained;
	u64 zc_read_fallback, zc_read_fallback_served;
	/* Responder-side exits of the zero-copy READ fallback path (diagnostic). */
	u64 zc_fb_wait_midpost, zc_fb_wait_drain, zc_fb_reserve, zc_read_req_drop, zc_resp_retry_exc, zc_resp_start_exc;
	/* Zero-copy READs requested, and timeouts forced on them by zc_read_fault. */
	u64 zc_read_ops, zc_read_fault_injected;
	struct sn_stall_snap snap_drain, snap_err;
	struct list_head mrs;
	struct sn_qp *qp;
	/* CQE identities; see sn_wc_qp(). Only qp_num is ever set or read. */
	struct ib_qp wc_qp[2];
	struct sn_readiness ready;
	union ib_gid gid, peer_gid;
	/* Control frames have priority over data; coalescing is by ACK identity. */
	struct sn_header control[16];
	unsigned int control_head, control_count;
	/* Consecutive invalid remote directory snapshots; see sn_snapshot_check(). */
	unsigned int invalid_snapshots;
	/* Next allowed renotification while not ready; see sn_pass(). */
	unsigned long renotify_after;
	/* Next HELLO retransmission (ktime ns); session handshake is timer-driven. */
	u64 next_hello;
	/* Peer advertised SN_CAP_ZDATA in its HELLO caps. */
	bool peer_zc;
	/* Our HELLO caps: SN_CAPS_V1 plus SN_CAP_ZDATA when the data rings are up. */
	u32 caps;
};
static inline struct sn_device *sn_dev(struct ib_device *ib)
{
	return container_of(ib, struct sn_device, ib);
}
static inline struct sn_qp *sn_qp(struct ib_qp *ib)
{
	return container_of(ib, struct sn_qp, ib);
}
static inline struct sn_cq *sn_cq(struct ib_cq *ib)
{
	return container_of(ib, struct sn_cq, ib);
}
extern const struct ib_device_ops sn_verbs_ops;
struct ib_mr *sn_reg_user_mr(struct ib_pd *, u64, u64, u64, int,
			   struct ib_dmah *, struct ib_udata *);
int sn_dereg_mr(struct ib_mr *, struct ib_udata *);
void sn_schedule(struct sn_device *d);
int sn_publish(struct sn_device *d);
void sn_qp_error(struct sn_qp *q, enum ib_wc_status status);
int sn_complete(struct sn_qp *q, struct sn_wqe *w, bool receive,
		enum ib_wc_status status, u32 length);
struct sn_mr *sn_mr_get(struct sn_qp *q, u32 key, u64 address, u32 length, u32 access);
void sn_mr_put(struct sn_mr *m);
int sn_mr_copy(struct sn_mr *m, u64 address, void *buffer, u32 length, bool to_mr);
int sn_ring_send(struct sn_device *d, struct sn_header *h, const void *payload, bool control);
int sn_rings_start(struct sn_device *d);
int sn_rings_stop(struct sn_device *d);
void sn_ring_receive(struct sn_device *d);
bool sn_ring_pending(struct sn_device *d);
void sn_data_receive(struct sn_device *d);
dma_addr_t sn_mr_dma(struct sn_mr *m, u64 address, u32 *max_len);

/* Zero-copy data-plane platform hooks (kernel: ring.c; tests: modeled).
 * sn_data_send_burst queues header-less payload frames for the active SQ op
 * from its MR, returning 0 when the whole message is queued, -EAGAIN when
 * the data ring is full. sn_data_response_burst does the same for a zero-copy
 * READ response, which has no SQ WQE and bursts from the source MR the request
 * named. sn_data_recv_start posts the q->zc_mr pages at q->zc_address for the
 * expected burst; sn_data_recv_restart re-posts them from offset zero for a
 * retried burst. */
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q);
int sn_data_response_burst(struct sn_device *d, struct sn_qp *q);
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q);
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q);
/* Abort an in-progress burst and reclaim both data rings (called from
 * sn_engine_reset so an aborted burst cannot poison the rings). */
void sn_data_abort(struct sn_device *d);
/* Freeze the ring and QP state for diagnosis: status 0 records a drain wait
 * (at most once per millisecond), anything else a QP error with that status. */
void sn_stall_snapshot(struct sn_device *d, struct sn_qp *q, int status);
/* True once every burst frame this host posted has been consumed by the NHI.
 * The zero-copy retry path must not restart a burst while frames are still
 * queued: they cannot be recalled, and the peer counts them into the new attempt
 * after it zeroes zc_received for the retransmitted descriptor. E2E neither loses
 * nor reorders frames, so once every posted slot is done, nothing more from the
 * superseded attempt can still arrive. */
bool sn_data_tx_drained(struct sn_device *d);
void sn_engine_zc_bytes(struct sn_qp *q, u32 bytes);
int sn_data_rings_start(struct sn_device *d);
int sn_mr_ensure_mapped(struct sn_device *d, struct sn_mr *m);
void sn_engine_receive(struct sn_device *d, const void *frame, size_t bytes);
void sn_engine_progress(struct sn_device *d);
void sn_engine_reset(struct sn_qp *q);
#endif
