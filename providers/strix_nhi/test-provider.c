// SPDX-License-Identifier: GPL-2.0-only
/* Actual provider, actual v62 headers/library; mocked command boundary only.
 * This does not exercise uverbs or kernel lifetimes. */
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>
#include "strix_nhi.c"

static int calls, fail_at, failure, polled;
static uint64_t ids[8];
int __wrap_ibv_cmd_post_send(struct ibv_qp *qp, struct ibv_send_wr *wr,
                            struct ibv_send_wr **bad)
{
	(void)qp;
	assert(!wr->next && wr->num_sge == 1);
	ids[calls++] = wr->wr_id;
	if (calls == fail_at) { *bad = wr; return failure; }
	return 0;
}
int __wrap_ibv_cmd_post_recv(struct ibv_qp *qp, struct ibv_recv_wr *wr,
                            struct ibv_recv_wr **bad)
{
	(void)qp;
	assert(!wr->next && wr->num_sge == 1);
	ids[calls++] = wr->wr_id;
	if (calls == fail_at) { *bad = wr; return failure; }
	return 0;
}
int __wrap_ibv_cmd_poll_cq(struct ibv_cq *cq, int n, struct ibv_wc *wc)
{
	(void)cq; (void)wc; polled = n;
	return failure ? -1 : n;
}
int __wrap_ibv_cmd_dereg_mr(struct verbs_mr *mr)
{
	assert(mr->ibv_mr.lkey == 37); return failure;
}
int __wrap_ibv_cmd_destroy_qp(struct ibv_qp *qp)
{
	assert(qp->qp_num == 17); return failure;
}
static void identity_test(const char *path)
{
	struct verbs_sysfs_dev dev = { .abi_ver = 1, .driver_id = RDMA_DRIVER_UNKNOWN };
	/* v62's global known-driver-ID pass unconditionally scans this table. */
	assert(strix_device_ops.match_table &&
	       strix_device_ops.match_table[0].kind == VERBS_MATCH_SENTINEL);
	snprintf(dev.ibdev_path, sizeof(dev.ibdev_path), "%s", path);
	snprintf(dev.ibdev_name, sizeof(dev.ibdev_name), "strix_nhi0");
	assert(strix_device_ops.match_device(&dev));
	dev.driver_id = RDMA_DRIVER_RXE;
	assert(!strix_device_ops.match_device(&dev));
	assert(strix_identity("strix_nhi0", 1, 0, path));
	assert(strix_identity("strix_nhi123", 1, 0, path));
	assert(!strix_identity("strix_nhi", 1, 0, path));
	assert(!strix_identity("strix_nhi0fake", 1, 0, path));
	assert(!strix_identity("rxe0", 1, 0, path));
	assert(!strix_identity("strix_nhi0", 2, 0, path));
	assert(!strix_identity("strix_nhi0", 1, RDMA_DRIVER_RXE, path));
	assert(!strix_identity("strix_nhi0", 1, 0, "/nonexistent"));
	puts("ok 1 - exact native name, ABI, UNKNOWN and real parent driver required");
}
static void post_test(void)
{
	struct strix_qp qp = {};
	struct ibv_sge sge = { .lkey = 13, .length = 2097152 };
	struct ibv_send_wr wr[3] = {}, *bad;
	struct ibv_recv_wr recv[3] = {}, *rbad;
	int i;

	assert(!pthread_mutex_init(&qp.send_lock, NULL));
	assert(!pthread_mutex_init(&qp.recv_lock, NULL));
	for (i = 0; i < 3; i++) {
		wr[i].wr_id = recv[i].wr_id = 40 + i;
		wr[i].sg_list = recv[i].sg_list = &sge;
		wr[i].num_sge = recv[i].num_sge = 1;
		wr[i].next = i < 2 ? &wr[i+1] : NULL;
		recv[i].next = i < 2 ? &recv[i+1] : NULL;
	}
	failure = EACCES; fail_at = 2; calls = 0;
	assert(strix_post_send(&qp.ibv, wr, &bad) == EACCES);
	assert(bad == &wr[1] && calls == 2 && ids[0] == 40 && ids[1] == 41);
	assert(wr[0].next == &wr[1] && wr[1].next == &wr[2]);
	calls = 0;
	assert(strix_post_recv(&qp.ibv, recv, &rbad) == EACCES);
	assert(rbad == &recv[1] && calls == 2);
	fail_at = 0; calls = 0; wr[1].num_sge = -1;
	assert(strix_post_send(&qp.ibv, wr, &bad) == EINVAL);
	assert(bad == &wr[1] && calls == 1);
	calls = 0; recv[1].num_sge = 2;
	assert(strix_post_recv(&qp.ibv, recv, &rbad) == EINVAL);
	assert(rbad == &recv[1] && calls == 1);
	wr[1].num_sge = recv[1].num_sge = 1;
	wr[1].sg_list = NULL; calls = 0;
	assert(strix_post_send(&qp.ibv, wr, &bad) == EINVAL && calls == 1);
	wr[1].sg_list = &sge; calls = 0;
	assert(!strix_post_send(&qp.ibv, wr, &bad) && !bad && calls == 3);
	assert(ids[0] == 40 && ids[1] == 41 && ids[2] == 42);
	calls = 0;
	assert(!strix_post_recv(&qp.ibv, recv, &rbad) && !rbad && calls == 3);
	assert(!strix_post_send(&qp.ibv, NULL, &bad) && !bad);
	pthread_mutex_destroy(&qp.send_lock); pthread_mutex_destroy(&qp.recv_lock);
	puts("ok 2 - bounded commands, partial prefixes, first bad WR, SGE validation and order");
}
static void lifetime_test(void)
{
	struct verbs_mr *mr = calloc(1, sizeof(*mr));
	struct strix_qp *qp = calloc(1, sizeof(*qp));
	struct ibv_wc wc[64];

	assert(mr && qp); mr->ibv_mr.lkey = 37; qp->ibv.qp_num = 17;
	assert(!pthread_mutex_init(&qp->send_lock, NULL));
	assert(!pthread_mutex_init(&qp->recv_lock, NULL));
	failure = EBUSY;
	assert(strix_dereg_mr(mr) == EBUSY && mr->ibv_mr.lkey == 37);
	assert(strix_destroy_qp(&qp->ibv) == EBUSY && qp->ibv.qp_num == 17);
	assert(!pthread_mutex_lock(&qp->send_lock));
	assert(!pthread_mutex_unlock(&qp->send_lock));
	failure = 0;
	assert(!strix_dereg_mr(mr)); assert(!strix_destroy_qp(&qp->ibv));
	polled = 0;
	assert(strix_poll_cq(NULL, -1, wc) == -1 && errno == EINVAL && !polled);
	assert(strix_poll_cq(NULL, 1, NULL) == -1 && errno == EINVAL);
	assert(!strix_poll_cq(NULL, 0, NULL));
	assert(strix_poll_cq(NULL, INT_MAX, wc) == 64 && polled == 64);
	failure = EIO;
	assert(strix_poll_cq(NULL, 8, wc) < 0);
	puts("ok 3 - failed teardown retains objects; poll bounds and errors are not completions");
}
int main(int argc, char **argv)
{
	assert(argc == 2);
	puts("1..3"); identity_test(argv[1]); post_test(); lifetime_test();
	return 0;
}
