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
	int native_tx_path, native_tx_ring, native_rx_path, native_rx_ring;
	bool native_dma_active, native_dma_stopping, native_dma_removed;
};
struct tb_service { struct tb_xdomain *xd; };
static struct tb_xdomain *tb_service_parent(struct tb_service *svc) { return svc->xd; }
static unsigned int activations, releases, notifications;
static void tb_service_properties_changed(struct tb_service *svc)
{
	assert(svc->xd->tb->lock.held && !svc->xd->native_dma_stopping);
	notifications++;
}
static bool tuple_present;
static int activation_error;
#define lockdep_assert_held(p) assert((p)->held)
static bool mutex_trylock(struct mutex *m)
{
	m->attempts++;
	if (m->held) return false;
	m->held = true;
	return true;
}
static void mutex_unlock(struct mutex *m) { assert(m->held); m->held = false; }
static int __tb_approve_xdomain_paths(struct tb *tb, struct tb_xdomain *xd,
		int tx_path, int tx_ring, int rx_path, int rx_ring)
{
	(void)xd;
	assert(tb->lock.held && tx_path == 9 && tx_ring == 2 && rx_path == 10 && rx_ring == 3);
	if (activation_error) return activation_error;
	assert(!tuple_present); tuple_present = true; activations++;
	return 0;
}
static void __tb_disconnect_xdomain_paths(struct tb *tb, struct tb_xdomain *xd,
		int tx_path, int tx_ring, int rx_path, int rx_ring)
{
	(void)xd;
	assert(tb->lock.held && tx_path == 9 && tx_ring == 2 && rx_path == 10 && rx_ring == 3);
	if (tuple_present) { tuple_present = false; releases++; }
}
#include "native-core-impl.inc"
int main(void)
{
	struct tb tb = {.cm_ops = &tb_cm_ops};
	struct tb_xdomain xd = {.tb = &tb};
	unsigned int attempts;
	struct tb_service svc = {.xd = &xd};

	puts("1..6");
	tb.cm_ops = &firmware_ops;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EOPNOTSUPP);
	assert(!tb.lock.attempts && !activations);
	tb.cm_ops = &tb_cm_ops;
	tb.lock.held = true;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EAGAIN);
	assert(!xd.native_dma_active && !activations);
	tb.lock.held = false;
	xd.is_unplugged = true;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -ENODEV);
	xd.is_unplugged = false;
	activation_error = -EIO;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EIO);
	assert(!xd.native_dma_active && !activations);
	activation_error = 0;
	puts("ok 1 - actual core rejects firmware, busy lock, unplug and failed activation without ownership");
	assert(!tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3));
	assert(xd.native_dma_active && activations == 1);
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EBUSY);
	tb.lock.held = true;
	assert(tb_xdomain_try_disable_native_paths(&xd) == -EAGAIN);
	assert(xd.native_dma_active && tuple_present && !releases);
	tb.lock.held = false;
	assert(!tb_xdomain_try_disable_native_paths(&xd));
	assert(!xd.native_dma_active && releases == 1 && !xd.native_dma_removed);
	puts("ok 2 - EAGAIN retains exact tuple; later local release permits carrier restart");
	assert(!tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3));
	/* Model removal owning tb->lock while a worker attempts activation/stop. */
	tb.lock.held = true;
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -EAGAIN);
	tb_xdomain_remove_native_paths(&xd);
	assert(xd.native_dma_removed && xd.native_dma_stopping && !xd.native_dma_active);
	assert(releases == 2 && !tuple_present);
	attempts = tb.lock.attempts;
	assert(tb_xdomain_try_disable_native_paths(&xd) == -ENODEV);
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -ENODEV);
	assert(tb_service_try_native_properties_changed(&svc) == -ENODEV);
	assert(tb.lock.attempts == attempts && tb.lock.held && !notifications);
	puts("ok 3 - removal retires before callback; enable/disable do not try to take removing core lock");
	tb_xdomain_remove_native_paths(&xd);
	assert(releases == 2);
	tb.lock.held = false;
	xd = (struct tb_xdomain){.tb = &tb};
	assert(!tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3));
	/* Earlier generic invalid-tunnel teardown removed the tuple, while the
	 * service still retained ring/HopID reservations. No raw tunnel pointer. */
	tuple_present = false;
	tb.lock.held = true;
	tb_xdomain_remove_native_paths(&xd);
	assert(xd.native_dma_removed && releases == 2);
	puts("ok 4 - prior core tunnel cleanup and repeated retirement are idempotent");
	tb.lock.held = false;
	xd = (struct tb_xdomain){.tb = &tb, .native_dma_stopping = true};
	assert(tb_xdomain_try_enable_native_paths(&xd, 9, 2, 10, 3) == -ENODEV);
	assert(!tuple_present && !tb.lock.held);
	puts("ok 5 - published admission stop rejects reactivation before completed removal");
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
	puts("ok 6 - notification EAGAIN retries; removal prevents post-cancellation queueing");
	return 0;
}
