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
/* Data rings are far larger than the control rings: the burst pipeline is
 * latency-bound (ring_bytes / worker drain latency), and 64 x 4096 B (256 KiB)
 * caps sustained throughput at ~250 MiB/s. 1024 x 4096 B (4 MiB) hides the
 * ~1 ms workqueue latency well above the wire rate. */
#define SN_DATA_RING_SIZE 1024
#define SN_CONTROL_RESERVE 8
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
	struct delayed_work work;
	struct tb_ring *tx_ring, *rx_ring;
	struct sn_slot tx[SN_RING_SIZE], rx[SN_RING_SIZE];
	unsigned int tx_head, tx_tail, rx_tail;
	/* Zero-copy data ring pair (header-less payload frames landing directly
	 * in MR pages). Present only when a second ring HopID is available
	 * (thunderbolt-net absent); otherwise the driver is staged-only. */
	struct tb_ring *data_tx_ring, *data_rx_ring;
	struct sn_slot *data_tx, *data_rx;
	unsigned int data_tx_head, data_tx_tail, data_rx_head, data_rx_tail;
	u32 zdata_posted;	/* bytes of the expected burst posted to data_rx_ring */
	bool zdata;
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
	/* Next allowed renotification while not ready; see sn_work(). */
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
 * the data ring is full. sn_data_recv_start posts the matched receive WQE's
 * MR pages for the expected burst; sn_data_recv_restart re-posts them from
 * offset zero for a retried burst. */
int sn_data_send_burst(struct sn_device *d, struct sn_qp *q);
int sn_data_recv_start(struct sn_device *d, struct sn_qp *q, struct sn_wqe *w);
int sn_data_recv_restart(struct sn_device *d, struct sn_qp *q);
void sn_engine_zc_bytes(struct sn_qp *q, u32 bytes);
int sn_data_rings_start(struct sn_device *d);
int sn_mr_ensure_mapped(struct sn_device *d, struct sn_mr *m);
void sn_engine_receive(struct sn_device *d, const void *frame, size_t bytes);
void sn_engine_progress(struct sn_device *d);
void sn_engine_reset(struct sn_qp *q);
#endif
