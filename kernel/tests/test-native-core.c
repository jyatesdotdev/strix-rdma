/* SPDX-License-Identifier: GPL-2.0-only */
/* Execute the actual prepared core API bodies with deterministic lock/path
 * adapters. This tests ownership branches, NOT real kernel concurrency. */
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <stdio.h>
#define EXPORT_SYMBOL_GPL(name)
#define smp_load_acquire(p) __atomic_load_n(p, __ATOMIC_ACQUIRE)
#define smp_store_release(p, v) __atomic_store_n(p, v, __ATOMIC_RELEASE)
struct mutex { bool held; unsigned int attempts; };
struct tb_cm_ops { int kind; };
static const struct tb_cm_ops tb_cm_ops = {1}, firmware_ops = {2};
struct tb { const struct tb_cm_ops *cm_ops; struct mutex lock; };
struct tb_xdomain {
	struct tb *tb;
	bool is_unplugged;
	int native_tx_path[2], native_tx_ring[2], native_rx_path[2], native_rx_ring[2];
	unsigned int native_dma_count;
	bool native_dma_stopping, native_dma_removed;
};
struct tb_service { struct tb_xdomain *xd; };
static struct tb_xdomain *tb_service_parent(struct tb_service *svc) { return svc->xd; }
static unsigned int activations, releases, notifications;
static void tb_service_properties_changed(struct tb_service *svc)
{
	assert(svc->xd->tb->lock.held && !svc->xd->native_dma_stopping);
	notifications++;
}
static bool tuple_present[2];
static int activation_error;
#define lockdep_assert_held(p) assert((p)->held)
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
static bool mutex_trylock(struct mutex *m)
{
	m->attempts++;
	if (m->held) return false;
	m->held = true;
	return true;
}
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static int tuple_index(int tx_path, int tx_ring, int rx_path, int rx_ring)
{
	if (tx_path == 9 && tx_ring == 2 && rx_path == 10 && rx_ring == 3) return 0;
	if (tx_path == 11 && tx_ring == 1 && rx_path == 12 && rx_ring == 1) return 1;
	return -1;
}
static int __tb_approve_xdomain_paths(struct tb *tb, struct tb_xdomain *xd,
		int tx_path, int tx_ring, int rx_path, int rx_ring)
{
	int i = tuple_index(tx_path, tx_ring, rx_path, rx_ring);
	(void)xd;
	assert(tb->lock.held && i >= 0 && !tuple_present[i]);
	if (activation_error) return activation_error;
	tuple_present[i] = true; activations++;
	return 0;
}
static void __tb_disconnect_xdomain_paths(struct tb *tb, struct tb_xdomain *xd,
		int tx_path, int tx_ring, int rx_path, int rx_ring)
{
	int i = tuple_index(tx_path, tx_ring, rx_path, rx_ring);
	(void)xd;
	assert(tb->lock.held && i >= 0);
	if (tuple_present[i]) { tuple_present[i] = false; releases++; }
}
#include "native-core-impl.inc"
int main(void)
{
	struct tb tb = {.cm_ops = &tb_cm_ops};
	struct tb_xdomain xd = {.tb = &tb};
	unsigned int attempts;
	struct tb_service svc = {.xd = &xd};

	puts("1..7");
	tb.cm_ops = &firmware_ops;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EOPNOTSUPP);
	assert(!tb.lock.attempts && !activations);
	tb.cm_ops = &tb_cm_ops;
	tb.lock.held = true;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EAGAIN);
	assert(!xd.native_dma_count && !activations);
	tb.lock.held = false;
	xd.is_unplugged = true;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -ENODEV);
	xd.is_unplugged = false;
	activation_error = -EIO;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EIO);
	assert(!xd.native_dma_count && !activations);
	activation_error = 0;
	puts("ok 1 - actual core rejects firmware, busy lock, unplug and failed activation without ownership");
	assert(!tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3));
	assert(xd.native_dma_count == 1 && activations == 1);
	assert(!tb_xdomain_try_enable_native_paths(&xd, 11, 1, 12, 1));
	assert(xd.native_dma_count == 2 && activations == 2);
	assert(xd.native_tx_path[0] == 9 && xd.native_tx_ring[1] == 1 && xd.native_rx_path[1] == 12);
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EBUSY);
	assert(xd.native_dma_count == 2 && activations == 2);
	puts("ok 2 - two exact tuples admitted (control + zero-copy data); a third is refused");
	tb.lock.held = true;
	assert(tb_xdomain_try_disable_native_paths(&xd) == -EAGAIN);
	assert(xd.native_dma_count == 2 && tuple_present[0] && tuple_present[1] && !releases);
	tb.lock.held = false;
	assert(!tb_xdomain_try_disable_native_paths(&xd));
	assert(!xd.native_dma_count && releases == 2 && !xd.native_dma_removed);
	puts("ok 3 - EAGAIN retains both tuples; later local release permits carrier restart");
	assert(!tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3));
	assert(xd.native_dma_count == 1);
	assert(!tb_xdomain_try_enable_native_paths(&xd, 11, 1, 12, 1));
	assert(xd.native_dma_count == 2);
	/* Model removal owning tb->lock while a worker attempts activation/stop. */
	tb.lock.held = true;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EAGAIN);
	tb_xdomain_remove_native_paths(&xd);
	assert(xd.native_dma_removed && xd.native_dma_stopping && !xd.native_dma_count);
	assert(releases == 4 && !tuple_present[0] && !tuple_present[1]);
	attempts = tb.lock.attempts;
	assert(tb_xdomain_try_disable_native_paths(&xd) == -ENODEV);
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -ENODEV);
	assert(tb_service_try_native_properties_changed(&svc) == -ENODEV);
	assert(tb.lock.attempts == attempts && tb.lock.held && !notifications);
	puts("ok 4 - removal retires both tuples before callback; enable/disable do not take removing core lock");
	tb_xdomain_remove_native_paths(&xd);
	assert(releases == 4);
	tb.lock.held = false;
	xd = (struct tb_xdomain){.tb = &tb};
	assert(!tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3));
	assert(xd.native_dma_count == 1);
	/* Earlier generic invalid-tunnel teardown removed the tuple, while the
	 * service still retained ring/HopID reservations. No raw tunnel pointer. */
	tuple_present[0] = false;
	tb.lock.held = true;
	tb_xdomain_remove_native_paths(&xd);
	assert(xd.native_dma_removed && releases == 4);
	puts("ok 5 - prior core tunnel cleanup and repeated retirement are idempotent");
	tb.lock.held = false;
	xd = (struct tb_xdomain){.tb = &tb, .native_dma_stopping = true};
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -ENODEV);
	assert(!tuple_present[0] && !tb.lock.held);
	puts("ok 6 - published admission stop rejects reactivation before completed removal");
	xd.native_dma_stopping = false;
	tb.lock.held = true;
	assert(tb_service_try_native_properties_changed(&svc) == -EAGAIN);
	assert(!notifications);
	tb.lock.held = false;
	assert(!tb_service_try_native_properties_changed(&svc));
	assert(notifications == 1 && !tb.lock.held);
	tb.lock.held = true;
	tb_xdomain_remove_native_paths(&xd);
	assert(tb_service_try_native_properties_changed(&svc) == -ENODEV);
	assert(notifications == 1);
	puts("ok 7 - notification EAGAIN retries; removal prevents post-cancellation queueing");
	return 0;
}
