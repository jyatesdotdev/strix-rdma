// SPDX-License-Identifier: GPL-2.0-only
/* Standard uverbs command provider. Reliability and memory ownership reside
 * in the kernel; no local TX event is synthesized into a verbs completion. */
#include <infiniband/driver.h>
#include <infiniband/kern-abi.h>
#include <pthread.h>
#include <errno.h>
#include <stdlib.h>
#include "identity.h"

struct strix_context { struct verbs_context verbs; };
struct strix_qp {
	struct ibv_qp ibv;
	pthread_mutex_t send_lock, recv_lock;
};

static int strix_query_device(struct ibv_context *ctx,
	const struct ibv_query_device_ex_input *input,
	struct ibv_device_attr_ex *attr, size_t size)
{
	struct ib_uverbs_ex_query_device_resp resp = {};
	size_t resp_size = sizeof(resp);
	int ret = ibv_cmd_query_device_any(ctx, input, attr, size, &resp, &resp_size);

	if (!ret)
		snprintf(attr->orig_attr.fw_ver, sizeof(attr->orig_attr.fw_ver),
			 "%llu", (unsigned long long)resp.base.fw_ver);
	return ret;
}
static int strix_query_port(struct ibv_context *ctx, uint8_t port,
	struct ibv_port_attr *attr)
{
	struct ibv_query_port cmd = {};
	return ibv_cmd_query_port(ctx, port, attr, &cmd, sizeof(cmd));
}
static struct ibv_pd *strix_alloc_pd(struct ibv_context *ctx)
{
	struct ibv_alloc_pd cmd = {};
	struct ib_uverbs_alloc_pd_resp resp = {};
	struct ibv_pd *pd = calloc(1, sizeof(*pd));
	int ret;

	if (!pd) return NULL;
	ret = ibv_cmd_alloc_pd(ctx, pd, &cmd, sizeof(cmd), &resp, sizeof(resp));
	if (ret) { free(pd); errno = ret; return NULL; }
	return pd;
}
static int strix_dealloc_pd(struct ibv_pd *pd)
{
	int ret = ibv_cmd_dealloc_pd(pd);
	if (!ret) free(pd);
	return ret;
}
static struct ibv_mr *strix_reg_mr(struct ibv_pd *pd, void *addr, size_t length,
	uint64_t iova, int access)
{
	struct ibv_reg_mr cmd = {};
	struct ib_uverbs_reg_mr_resp resp = {};
	struct verbs_mr *mr = calloc(1, sizeof(*mr));
	int ret;

	if (!mr) return NULL;
	ret = ibv_cmd_reg_mr(pd, addr, length, iova, access, mr,
		&cmd, sizeof(cmd), &resp, sizeof(resp));
	if (ret) { free(mr); errno = ret; return NULL; }
	return &mr->ibv_mr;
}
static int strix_dereg_mr(struct verbs_mr *mr)
{
	int ret = ibv_cmd_dereg_mr(mr);
	/* In particular EBUSY retains the object and pinned memory. */
	if (!ret) free(mr);
	return ret;
}
static struct ibv_cq *strix_create_cq(struct ibv_context *ctx, int entries,
	struct ibv_comp_channel *channel, int vector)
{
	struct ibv_create_cq cmd = {};
	struct ib_uverbs_create_cq_resp resp = {};
	struct ibv_cq *cq = calloc(1, sizeof(*cq));
	int ret;

	if (!cq) return NULL;
	ret = ibv_cmd_create_cq(ctx, entries, channel, vector, cq,
		&cmd, sizeof(cmd), &resp, sizeof(resp));
	if (ret) { free(cq); errno = ret; return NULL; }
	return cq;
}
static int strix_destroy_cq(struct ibv_cq *cq)
{
	int ret = ibv_cmd_destroy_cq(cq);
	if (!ret) free(cq);
	return ret;
}
static int strix_poll_cq(struct ibv_cq *cq, int entries, struct ibv_wc *wc)
{
	if (entries < 0 || (entries && !wc)) { errno = EINVAL; return -1; }
	if (!entries) return 0;
	/* Bound rdma-core's response allocation independently of caller input. */
	return ibv_cmd_poll_cq(cq, entries > 64 ? 64 : entries, wc);
}
static struct ibv_qp *strix_create_qp(struct ibv_pd *pd, struct ibv_qp_init_attr *attr)
{
	struct ibv_create_qp cmd = {};
	struct ib_uverbs_create_qp_resp resp = {};
	struct strix_qp *qp = calloc(1, sizeof(*qp));
	int ret;

	if (!qp) return NULL;
	ret = pthread_mutex_init(&qp->send_lock, NULL);
	if (ret) goto fail;
	ret = pthread_mutex_init(&qp->recv_lock, NULL);
	if (ret) goto send_lock;
	ret = ibv_cmd_create_qp(pd, &qp->ibv, attr,
		&cmd, sizeof(cmd), &resp, sizeof(resp));
	if (!ret) return &qp->ibv;
	pthread_mutex_destroy(&qp->recv_lock);
send_lock:
	pthread_mutex_destroy(&qp->send_lock);
fail:
	free(qp); errno = ret; return NULL;
}
static int strix_destroy_qp(struct ibv_qp *ibv)
{
	struct strix_qp *qp = container_of(ibv, struct strix_qp, ibv);
	int ret = ibv_cmd_destroy_qp(ibv);

	if (ret) return ret;
	/* As with other verbs objects the application excludes concurrent destroy. */
	pthread_mutex_destroy(&qp->send_lock);
	pthread_mutex_destroy(&qp->recv_lock);
	free(qp);
	return 0;
}
static int strix_modify_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr, int mask)
{
	struct ibv_modify_qp cmd = {};
	return ibv_cmd_modify_qp(qp, attr, mask, &cmd, sizeof(cmd));
}
static int strix_query_qp(struct ibv_qp *qp, struct ibv_qp_attr *attr,
	int mask, struct ibv_qp_init_attr *init)
{
	struct ibv_query_qp cmd = {};
	return ibv_cmd_query_qp(qp, attr, mask, init, &cmd, sizeof(cmd));
}
static int strix_post_send(struct ibv_qp *ibv, struct ibv_send_wr *wr,
	struct ibv_send_wr **bad)
{
	struct strix_qp *qp = container_of(ibv, struct strix_qp, ibv);
	int ret = 0;

	if (!bad) return EINVAL;
	*bad = NULL;
	pthread_mutex_lock(&qp->send_lock);
	for (; wr; wr = wr->next) {
		struct ibv_send_wr one = *wr, *cmd_bad = NULL;
		/* One bounded command per WR avoids rdma-core's unbounded alloca
		 * and preserves the accepted prefix even on malformed suffixes. */
		if (wr->num_sge != 1 || !wr->sg_list) { ret = EINVAL; break; }
		one.next = NULL;
		ret = ibv_cmd_post_send(ibv, &one, &cmd_bad);
		if (ret) break;
	}
	if (ret) *bad = wr;
	pthread_mutex_unlock(&qp->send_lock);
	return ret;
}
static int strix_post_recv(struct ibv_qp *ibv, struct ibv_recv_wr *wr,
	struct ibv_recv_wr **bad)
{
	struct strix_qp *qp = container_of(ibv, struct strix_qp, ibv);
	int ret = 0;

	if (!bad) return EINVAL;
	*bad = NULL;
	pthread_mutex_lock(&qp->recv_lock);
	for (; wr; wr = wr->next) {
		struct ibv_recv_wr one = *wr, *cmd_bad = NULL;
		if (wr->num_sge != 1 || !wr->sg_list) { ret = EINVAL; break; }
		one.next = NULL;
		ret = ibv_cmd_post_recv(ibv, &one, &cmd_bad);
		if (ret) break;
	}
	if (ret) *bad = wr;
	pthread_mutex_unlock(&qp->recv_lock);
	return ret;
}
static void strix_free_context(struct ibv_context *ibv)
{
	struct strix_context *ctx = container_of(ibv, struct strix_context, verbs.context);
	verbs_uninit_context(&ctx->verbs);
	free(ctx);
}
static const struct verbs_context_ops strix_context_ops = {
	.query_device_ex = strix_query_device, .query_port = strix_query_port,
	.alloc_pd = strix_alloc_pd, .dealloc_pd = strix_dealloc_pd,
	.reg_mr = strix_reg_mr, .dereg_mr = strix_dereg_mr,
	.create_cq = strix_create_cq, .destroy_cq = strix_destroy_cq,
	.poll_cq = strix_poll_cq, .req_notify_cq = ibv_cmd_req_notify_cq,
	.create_qp = strix_create_qp, .destroy_qp = strix_destroy_qp,
	.modify_qp = strix_modify_qp, .query_qp = strix_query_qp,
	.post_send = strix_post_send, .post_recv = strix_post_recv,
	.free_context = strix_free_context,
};
static struct verbs_context *strix_alloc_context(struct ibv_device *dev,
	int fd, void *private_data)
{
	struct strix_context *ctx;
	struct ibv_get_context cmd = {};
	struct ib_uverbs_get_context_resp resp = {};
	int ret;

	(void)private_data;
	ctx = verbs_init_and_alloc_context(dev, fd, ctx, verbs, RDMA_DRIVER_UNKNOWN);
	if (!ctx) return NULL;
	ret = ibv_cmd_get_context(&ctx->verbs, &cmd, sizeof(cmd), NULL, &resp, sizeof(resp));
	if (ret) {
		verbs_uninit_context(&ctx->verbs); free(ctx); errno = ret; return NULL;
	}
	verbs_set_ops(&ctx->verbs, &strix_context_ops);
	return &ctx->verbs;
}
static bool strix_match_device(struct verbs_sysfs_dev *dev)
{
	return strix_identity(dev->ibdev_name, dev->abi_ver, dev->driver_id, dev->ibdev_path);
}
static struct verbs_device *strix_alloc_device(struct verbs_sysfs_dev *dev)
{
	(void)dev;
	return calloc(1, sizeof(struct verbs_device));
}
static void strix_free_device(struct verbs_device *dev) { free(dev); }
/* v62's global driver-ID pass dereferences every provider's table even when
 * it has a custom match callback. An empty sentinel table is mandatory: do
 * not claim UNKNOWN (or any other driver's ID) to get through that pass. */
static const struct verbs_match_ent strix_match_table[] = { {} };
static const struct verbs_device_ops strix_device_ops = {
	.name = "strix_nhi", .match_min_abi_version = 1, .match_max_abi_version = 1,
	.match_table = strix_match_table,
	.match_device = strix_match_device, .alloc_device = strix_alloc_device,
	.uninit_device = strix_free_device, .alloc_context = strix_alloc_context,
};
PROVIDER_DRIVER(strix_nhi, strix_device_ops);
