// SPDX-License-Identifier: GPL-2.0-only
#include <linux/etherdevice.h>
#include <linux/delay.h>
#include <linux/random.h>
#include <linux/pci.h>
#include <linux/sched.h>
#include <linux/rtnetlink.h>
#include <net/net_namespace.h>
#include <rdma/ib_user_verbs.h>
#include "strix_nhi.h"
#include "native_platform.h"

static bool enable;
module_param(enable, bool, 0400);
MODULE_PARM_DESC(enable, "Explicitly enable experimental native NHI software verbs");

/* Poller scheduling: drain this many passes per wake before yielding the
 * lock to uverbs, and busy-poll up to this many microseconds while cold
 * before sleeping. The 1 ms sleep keeps the maintenance tick (refresh,
 * rendezvous publish, handshake) that the old workqueue backstop provided.
 * The spin window is a runtime-tunable so the latency/CPU knee can be
 * measured on hardware without a rebuild or reload. */
#define SN_PASS_BUDGET	8
#define SN_IDLE_SPIN_US	128

static unsigned int idle_spin_us = SN_IDLE_SPIN_US;
module_param(idle_spin_us, uint, 0644);
MODULE_PARM_DESC(idle_spin_us,
	"Microseconds the poller busy-polls while cold before sleeping (default 128)");

/* Defined here, consumed by sn_data_rings_start(). See strix_nhi.h for the
 * throughput/latency trade this dial controls. */
unsigned int sn_data_throttle_ns = SN_DATA_THROTTLE_NS;
module_param_named(data_throttle_ns, sn_data_throttle_ns, uint, 0444);
MODULE_PARM_DESC(data_throttle_ns,
	"Data ring interrupt moderation in ns, 0 to disable (default 128000; applied at ring start)");

/* Zero-copy RDMA WRITE, on by default; zc_write=0 is the kill-switch that puts
 * eligible WRITEs back on the staged control-ring path.
 *
 * This was briefly opt-in because a retry could corrupt payload: restarting a
 * burst re-sends the descriptor, the peer zeroes zc_received for it, and frames
 * from the superseded attempt that were still queued on the data TX ring then got
 * counted into the new one -- overshooting the byte count, skipping the for-CPU
 * sync that keyed on exact equality, and completing a buffer assembled from two
 * attempts as IBV_WC_SUCCESS.
 *
 * Two changes make it safe. The retry path now waits for the data TX ring to
 * drain before restarting a burst (sn_data_tx_drained), bounded by the normal
 * retry budget, so no stale frame can outlive the reset; and an overshooting
 * byte count fails the QP instead of completing. Validated on hardware over
 * 16.8 GiB per direction verified byte-for-byte at 449 MiB/s, bad_frames 0.
 * See kernel/verbs/README.md. */
bool sn_zc_write = true;
module_param_named(zc_write, sn_zc_write, bool, 0444);
MODULE_PARM_DESC(zc_write,
	"Let eligible RDMA WRITEs take the zero-copy data ring (default on; 0 forces the staged WRITE path)");

/* Zero-copy RDMA READ, opt-in and off by default until it has been validated on
 * hardware the way zc_write was.
 *
 * READ is the last verb on the staged control-ring path, where a 2 MiB reply is
 * pushed through as ~512 individually headered fragments and reassembled into
 * read_incoming: measured 62.6 MiB/s against 456 MiB/s for a zero-copy WRITE.
 * The zero-copy path bursts the reply header-less straight from the source MR
 * into the landing zone the requester armed before sending the request, so it
 * needs no SN_ZC_READY round trip.
 *
 * It carries the two guards that made zero-copy WRITE safe: the responder --
 * which is the burst sender here, so its response retry is the exposed path --
 * waits for the data TX ring to drain before re-bursting, bounded by the retry
 * budget, and an overshooting byte count fails the QP. One deliberate difference
 * from the staged path: a retried READ re-reads the live source MR instead of
 * replaying read_snapshot, so it may observe newer data. See
 * kernel/verbs/README.md. */
bool sn_zc_read = false;
module_param_named(zc_read, sn_zc_read, bool, 0444);
MODULE_PARM_DESC(zc_read,
	"Let eligible RDMA READs take the zero-copy data ring (default off; 1 opts in, 0 keeps the staged READ path)");
/* Fault injection for diagnosing zero-copy READ recovery: every Nth zero-copy
 * READ this host requests has its retransmit window expired the moment its reply
 * is within 32 frames of landing, so the requester falls back to a staged
 * SN_READ while the tail of the responder's burst is still live -- the
 * precondition of every recovery failure seen on hardware. Writable at runtime so
 * a capture needs no reload; 0 (the default) never faults. */
unsigned int sn_zc_read_fault;
module_param_named(zc_read_fault, sn_zc_read_fault, uint, 0644);
MODULE_PARM_DESC(zc_read_fault,
	"Diagnostic: force every Nth zero-copy READ to time out near its tail (default 0 = off)");

static const uuid_t sn_uuid = UUID_INIT(0x9dfdfb88, 0xeaa4, 0x4d26,
				      0x98, 0x92, 0x59, 0xf2, 0x08, 0x45, 0x51, 0x01);
static struct tb_property_dir *sn_directory;

/* Wake the resident poller. Safe from any context (ring callbacks run in the
 * NHI's ring work, uverbs calls run in process context) and a no-op once the
 * thread has been stopped, so it needs no teardown ordering of its own. */
void sn_schedule(struct sn_device *d)
{
	if (READ_ONCE(d->registered) && !READ_ONCE(d->dead)) {
		/* Diagnostic; may race across callers, so the count is a hint only. */
		d->sched_calls++;
		WRITE_ONCE(d->poll_kick, true);
		wake_up(&d->poll_wait);
	}
}
static void sn_gid_from_uuid(union ib_gid *gid, const uuid_t *uuid)
{
	memset(gid, 0, sizeof(*gid));
	gid->raw[0] = 0xfe; gid->raw[1] = 0x80;
	memcpy(gid->raw + 8, uuid->b + 8, 8);
}
/* Static rendezvous: one immediate per service incarnation, published once.
 * The dynamic session handshake is wire-carried (HELLO/BIND), so the property
 * channel only ever carries this single inline VALUE, the class of property
 * that round-trips reliably. */
int sn_publish(struct sn_device *d)
{
	struct tb_service *svc = d->service;
	struct tb_property_dir *dir, *old;
	int ret;

	dir = tb_property_create_dir(&sn_uuid);
	if (!dir) return -ENOMEM;
	ret = tb_property_add_immediate(dir, "rxhop", d->in_hop);
	if (ret) { tb_property_free_dir(dir); return ret; }
	if (d->in_hop2 >= 0) {
		ret = tb_property_add_immediate(dir, "rxhop2", d->in_hop2);
		if (ret) { tb_property_free_dir(dir); return ret; }
	}
	mutex_lock(&svc->lock);
	old = svc->local_properties;
	svc->local_properties = dir;
	mutex_unlock(&svc->lock);
	tb_property_free_dir(old);
	return tb_service_try_native_properties_changed(svc);
}
static int sn_remote_rxhop(struct sn_device *d, const char *key, u64 *hop)
{
	struct tb_service *svc = d->service;
	struct tb_property *p;
	int ret = -EAGAIN;

	mutex_lock(&svc->lock);
	if (svc->remote_properties && uuid_equal(svc->remote_properties->uuid, &sn_uuid)) {
		p = tb_property_find(svc->remote_properties, key, TB_PROPERTY_TYPE_VALUE);
		if (p) { *hop = p->value.immediate; ret = 0; }
	}
	mutex_unlock(&svc->lock);
	return ret;
}
static int sn_refresh(struct sn_device *d)
{
	struct tb_xdomain *xd = tb_service_parent(d->service);
	u64 hop;
	int ret;

	if (d->out_hop < 0) {
		ret = sn_remote_rxhop(d, "rxhop", &hop);
		if (ret) return ret;
		if (hop < 8 || hop > xd->remote_max_hopid) return -EPROTO;
		ret = tb_xdomain_alloc_out_hopid(xd, hop);
		if (ret < 0) return ret;
		if (ret != hop) { tb_xdomain_release_out_hopid(xd, ret); return -EBUSY; }
		d->out_hop = ret;
	}
	if (d->out_hop2 < 0 && d->in_hop2 >= 0) {
		ret = sn_remote_rxhop(d, "rxhop2", &hop);
		if (!ret && hop >= 8 && hop <= xd->remote_max_hopid) {
			ret = tb_xdomain_alloc_out_hopid(xd, hop);
			if (ret >= 0) {
				if (ret != hop) { tb_xdomain_release_out_hopid(xd, ret); return -EBUSY; }
				d->out_hop2 = ret;
			}
		}
	}
	if (!d->rings_started && d->ready.carrier)
		return sn_rings_start(d);
	/* Retry a pending data-tuple enable after an earlier busy core. */
	if (d->rings_started && !d->zdata && !d->zc_unavail && d->data_tx_ring) {
		ret = sn_data_rings_start(d);
		if (ret == -ENOMEM)
			d->zc_unavail = true;
		else if (ret && ret != -EAGAIN && ret != -ENODEV)
			return ret;
	}
	return 0;
}
/* One maintenance + drain pass. Returns true while the link is still busy, so
 * the poller re-passes immediately instead of sleeping. */
static bool sn_pass(struct sn_device *d)
{
	bool active, was_active, progress;
	unsigned long long rx0, tx0;
	int ret, budget;

	mutex_lock(&d->lock);
	if (d->dead) { mutex_unlock(&d->lock); return false; }
	was_active = sn_ready(&d->ready);
	/* TB-IP is optional: without it the wire handshake alone proves link and
	 * peer liveness (the XDomain's existence means the link enumerated). */
	d->ready.carrier = d->netdev ? netif_running(d->netdev) && netif_carrier_ok(d->netdev) : true;
	ret = 0;
	/* Latch even a down/up event entirely between worker passes. Do not
	 * overwrite the loss with current carrier and reuse the old DMA paths. */
	if (d->reset_rings) {
		ret = sn_rings_stop(d);
		if (!ret) d->reset_rings = false;
	}
	if (!ret) ret = sn_refresh(d);
	if (ret) {
		d->ready.peer_ready = 0;
		if (d->qp && d->qp->bound) sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	}
	if ((ret != -EAGAIN && ret) || d->net_gone ||
	    smp_load_acquire(&tb_service_parent(d->service)->native_dma_stopping)) {
		d->dead = true;
		if (d->qp) sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	}
	/* Static rendezvous is a one-shot immediate; retry until the core takes it. */
	if (!d->static_published) {
		ret = sn_publish(d);
		if (!ret) d->static_published = true;
		else if (ret != -EAGAIN)
			pr_notice_ratelimited("strix_nhi: static rendezvous publish failed: %d\n", ret);
	}
	active = !d->dead && sn_ready(&d->ready);
	if ((!d->ready.carrier || d->dead) && d->tx_ring)
		sn_rings_stop(d);
	/* Drain-until-idle with a bounded budget, then release the lock so uverbs
	 * (poll_cq etc.) gets in during sustained bursts. Bursts reschedule at
	 * zero delay; idle work falls through to the bounded spin below. */
	budget = SN_PASS_BUDGET;
	do {
		rx0 = d->rx_frames; tx0 = d->tx_frames;
		sn_ring_receive(d);
		sn_data_receive(d);
		sn_engine_progress(d);
		progress = d->rx_frames != rx0 || d->tx_frames != tx0;
	} while (progress && --budget);
	mutex_unlock(&d->lock);
	if (was_active != active && d->registered) {
		struct ib_event event = {.device = &d->ib,
			.event = active ? IB_EVENT_PORT_ACTIVE : IB_EVENT_PORT_ERR};
		event.element.port_num = 1; ib_dispatch_event(&event);
	}
	if (READ_ONCE(d->dead)) return false;
	return progress;
}
/* Resident poller (NAPI-shaped, but in process context so the pass may keep
 * taking d->lock for the CQ and control-ring work).
 *
 * Receive latency was bounded by workqueue dispatch: the NHI interrupt queued
 * ring_work, whose frame callback queued this driver's work item, so every
 * frame paid two scheduling hops. Measured against a 36 us hardware floor
 * that produced a 0.8-6.4 ms ping-pong tail, and under sustained bursts the
 * ms-scale drain gaps filled the 64-slot data ring and stalled the pipeline.
 *
 * A thread that never leaves the CPU while the link is busy removes both hops.
 * It spins on the lockless pending check while cold (cheap: no mutex), and
 * otherwise sleeps up to 1 ms -- the same maintenance tick the old backstop
 * gave -- waking immediately when a ring callback or a uverbs call kicks it. */
static int sn_thread(void *arg)
{
	struct sn_device *d = arg;
	unsigned int idle = 0;
	bool maintenance = true;

	while (!kthread_should_stop()) {
		bool kick = READ_ONCE(d->poll_kick);
		bool pending = kick || sn_ring_pending(d) ||
			       READ_ONCE(d->control_count);

		if (pending || maintenance) {
			/* Consume the latch only when this pass acts on it. Clearing it
			 * just before sleeping instead discards a kick that raced with
			 * the cold spin -- wake_up was a no-op because we were not yet
			 * queued -- and the frame then waits for the 1 ms tick. A kick
			 * arriving during the pass simply re-arms the next one. */
			if (kick)
				WRITE_ONCE(d->poll_kick, false);
			maintenance = false;
			idle = 0;
			d->poll_passes++;
			if (sn_pass(d)) {
				/* Still busy: yield the CPU to uverbs between passes
				 * (d->lock is not held here) and go again. */
				cond_resched();
				continue;
			}
			/* No progress: fall through to the bounded cold path so a
			 * pending hint the pass cannot act on does not pin the CPU. */
		}
		/* Cold: hold the CPU briefly so an imminent frame (a ping-pong
		 * reply lands tens of microseconds away) is caught without a
		 * sleep/wake round trip, then sleep on the 1 ms maintenance tick. */
		if (++idle < READ_ONCE(idle_spin_us)) {
			cpu_relax();
			/* Yield regularly. Without this the spin monopolises the CPU and
			 * starves the NHI's ring work that sets the slot-done flags this
			 * loop is waiting for, so spinning longer made latency far worse
			 * (measured: 1.3 ms at a 128 us window vs 17.6 ms at 20 ms). */
			if (!(idle & 31))
				cond_resched();
			udelay(1);
			continue;
		}
		d->poll_sleeps++;
		wait_event_timeout(d->poll_wait,
			READ_ONCE(d->poll_kick) || sn_ring_pending(d) ||
			READ_ONCE(d->control_count) ||
			READ_ONCE(d->dead) || kthread_should_stop(),
			msecs_to_jiffies(1));
		/* Attribute the wake: the latch is only set by sn_schedule, so a
		 * set latch means a kick reached us rather than the 1 ms tick. */
		if (READ_ONCE(d->poll_kick))
			d->poll_wake_kick++;
		else
			d->poll_wake_tick++;
		idle = 0;
		maintenance = true;
	}
	return 0;
}
static struct net_device *sn_find_netdev(struct tb_service *svc)
{
	struct net_device *netdev, *found = NULL;
	struct tb_xdomain *xd = tb_service_parent(svc);

	rtnl_lock();
	for_each_netdev(&init_net, netdev) {
		struct device *parent = netdev->dev.parent;
		struct tb_service *network;

		if (!parent) continue;
		network = tb_to_service(parent);
		if (!network || strcmp(network->key, "network") || network->prtcid != 1 ||
		    tb_service_parent(network) != xd || !parent->driver ||
		    strcmp(parent->driver->name, "thunderbolt-net") || netdev->type != ARPHRD_ETHER)
			continue;
		if (found) { dev_put(found); found = NULL; break; }
		dev_hold(netdev); found = netdev;
	}
	rtnl_unlock();
	return found;
}
static int sn_net_event(struct notifier_block *nb, unsigned long event, void *ptr)
{
	struct sn_device *d = container_of(nb, struct sn_device, net_notifier);
	struct net_device *netdev = netdev_notifier_info_to_dev(ptr);

	/* d->netdev is set before this notifier is registered, then changes only
	 * here (rtnl-serialized, written under d->lock for the worker). */
	if (!netdev || netdev != d->netdev) return NOTIFY_DONE;
	/* thunderbolt-net unregistration waits in netdev_wait_allrefs for every
	 * reference, and the RDMA core never drops a port netdev on its own for
	 * NETDEV_UNREGISTER. Release both now; the device stays dead until the
	 * service is reprobed and never re-associates another netdev. */
	if (event == NETDEV_UNREGISTER) ib_device_set_netdev(&d->ib, NULL, 1);
	mutex_lock(&d->lock);
	if (event == NETDEV_UNREGISTER || event == NETDEV_DOWN || !netif_carrier_ok(netdev)) {
		if (d->ready.carrier) {
			do { d->ready.local_epoch = get_random_u64(); } while (!d->ready.local_epoch);
			d->ready.local_echo = d->ready.peer_echo = 0;
			d->ready.peer_ready = 0;
			d->next_hello = 0;
		}
		d->ready.carrier = 0;
		d->reset_rings = true;
		if (event == NETDEV_UNREGISTER) { d->net_gone = true; d->netdev = NULL; }
		d->control_count = 0;
		if (d->qp) sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	}
	mutex_unlock(&d->lock);
	if (event == NETDEV_UNREGISTER) dev_put(netdev);
	sn_schedule(d);
	return NOTIFY_DONE;
}
static int sn_probe(struct tb_service *svc, const struct tb_service_id *id)
{
	struct sn_device *d;
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct net_device *netdev;
	int ret;

	if (!sn_supported_nhi(to_pci_dev(xd->tb->nhi->dev)->vendor,
	    to_pci_dev(xd->tb->nhi->dev)->device))
		return -EOPNOTSUPP;
	netdev = sn_find_netdev(svc);
	/* TB-IP is optional: absent it the wire handshake alone proves link and
	 * peer liveness, and the zero-copy data plane gets its ring HopID. */
	d = ib_alloc_device(sn_device, ib);
	if (!d) { dev_put(netdev); return -ENOMEM; }
	d->service = svc; d->netdev = netdev;
	d->in_hop = d->out_hop = -1;
	d->in_hop2 = d->out_hop2 = -1;
	d->caps = SN_CAPS_V1;
	mutex_init(&d->lock); spin_lock_init(&d->ring_lock);
	init_waitqueue_head(&d->poll_wait);
	INIT_LIST_HEAD(&d->mrs);
	do { d->ready.local_epoch = get_random_u64(); } while (!d->ready.local_epoch);
	sn_gid_from_uuid(&d->gid, xd->local_uuid);
	sn_gid_from_uuid(&d->peer_gid, xd->remote_uuid);
	/* Leave the low HopIDs to thunderbolt-net's login (it requires 8 and may
	 * take more). TB-IP re-login fails while we hold its HopID, and our rings
	 * gate on its carrier. */
	ret = tb_xdomain_alloc_in_hopid(xd, 10);
	if (ret < 0) goto free;
	d->in_hop = ret;
	/* A second HopID for the zero-copy data plane; absence is not fatal
	 * (staged-only when thunderbolt-net holds the low HopIDs). */
	ret = tb_xdomain_alloc_in_hopid(xd, d->in_hop + 1);
	if (ret >= 0)
		d->in_hop2 = ret;
	d->ib.node_type = RDMA_NODE_IB_CA;
	d->ib.phys_port_cnt = 1; d->ib.num_comp_vectors = 1;
	memcpy(&d->ib.node_guid, d->gid.raw + 8, 8);
	strscpy(d->ib.node_desc, "Strix native NHI software verbs (experimental)", sizeof(d->ib.node_desc));
	d->ib.dev.parent = &svc->dev;
	/* Keep the core's default write commands (PD, MR, QP, query, ...). */
	d->ib.uverbs_cmd_mask |= BIT_ULL(IB_USER_VERBS_CMD_POST_SEND) |
		BIT_ULL(IB_USER_VERBS_CMD_POST_RECV) | BIT_ULL(IB_USER_VERBS_CMD_POLL_CQ) |
		BIT_ULL(IB_USER_VERBS_CMD_REQ_NOTIFY_CQ);
	ib_set_device_ops(&d->ib, &sn_verbs_ops);
	if (netdev) {
		ret = ib_device_set_netdev(&d->ib, netdev, 1);
		if (ret) goto hop;
		/* After set_netdev: an unregistration that already raced past the
		 * lookup is rebroadcast by netdev_wait_allrefs_any while references
		 * remain. */
		d->net_notifier.notifier_call = sn_net_event;
		ret = register_netdevice_notifier(&d->net_notifier);
		if (ret) goto hop;
	}
	ret = ib_register_device(&d->ib, "strix_nhi%d", NULL);
	if (ret) goto notifier;
	d->registered = true;
	tb_service_set_drvdata(svc, d);
	/* Start the poller last: it drives the handshake, so everything it
	 * touches (service, device, readiness state) must already be live. */
	d->thread = kthread_run(sn_thread, d, "strix_nhi/%s", dev_name(&svc->dev));
	if (IS_ERR(d->thread)) {
		ret = PTR_ERR(d->thread);
		d->thread = NULL;
		d->registered = false;
		ib_unregister_device(&d->ib);
		goto notifier;
	}
	return 0;
notifier:
	if (netdev) unregister_netdevice_notifier(&d->net_notifier);
hop:
	tb_xdomain_release_in_hopid(xd, d->in_hop);
	if (d->in_hop2 >= 0) tb_xdomain_release_in_hopid(xd, d->in_hop2);
free:
	dev_put(d->netdev);
	ib_dealloc_device(&d->ib);
	return ret;
}
static void sn_remove(struct tb_service *svc)
{
	struct sn_device *d = tb_service_get_drvdata(svc);
	struct tb_xdomain *xd = tb_service_parent(svc);
	struct tb_property_dir *old;
	int ret;

	mutex_lock(&d->lock);
	d->dead = true; d->ready.peer_ready = 0;
	if (d->qp) sn_qp_error(d->qp, IB_WC_GENERAL_ERR);
	mutex_unlock(&d->lock);
	if (d->netdev) unregister_netdevice_notifier(&d->net_notifier);
	/* Stop the poller before tearing down the rings it drains. kthread_stop
	 * must NOT run under d->lock: the thread takes it for its final pass,
	 * observes d->dead (set above, lock released) and exits on its own. */
	if (d->thread) {
		wake_up(&d->poll_wait);
		kthread_stop(d->thread);
		d->thread = NULL;
	}
	/* No object lock or workqueue is held while waiting for local core
	 * ownership. Core-removal callbacks observe its release-published
	 * cleanup marker immediately. Ordinary unbind retries in this thread,
	 * never on a workqueue that core teardown might be draining. */
	while ((ret = sn_rings_stop(d))) {
		/* Only EAGAIN is possible after software-CM admission. Never
		 * turn an unexpected core error into unsafe ownership release. */
		WARN_ON_ONCE(ret != -EAGAIN);
		msleep(1);
	}
	ib_unregister_device(&d->ib);
	/* Ring callbacks, the notifier and uverbs commands that raced with the
	 * stop may still call sn_schedule; that only wakes an empty waitqueue
	 * now, so the poller cannot outlive d. */
	mutex_lock(&svc->lock);
	old = svc->local_properties; svc->local_properties = NULL;
	mutex_unlock(&svc->lock);
	tb_property_free_dir(old);
	while (tb_service_try_native_properties_changed(svc) == -EAGAIN)
		msleep(1);
	if (d->out_hop >= 0) tb_xdomain_release_out_hopid(xd, d->out_hop);
	if (d->out_hop2 >= 0) tb_xdomain_release_out_hopid(xd, d->out_hop2);
	tb_xdomain_release_in_hopid(xd, d->in_hop);
	if (d->in_hop2 >= 0) tb_xdomain_release_in_hopid(xd, d->in_hop2);
	if (d->data_tx_ring) tb_ring_free(d->data_tx_ring);
	if (d->data_rx_ring) tb_ring_free(d->data_rx_ring);
	dev_put(d->netdev); /* NULL after NETDEV_UNREGISTER released it */
	ib_dealloc_device(&d->ib);
}
/* An active experimental service cannot yet be safely suspended/resumed.
 * Reject system suspend rather than silently leaving active DMA paths. */
static int sn_suspend(struct device *dev)
{
	return -EBUSY;
}
static const struct dev_pm_ops sn_pm_ops = {
	.suspend = sn_suspend, .freeze = sn_suspend, .poweroff = sn_suspend,
};
static const struct tb_service_id sn_ids[] = {{ TB_SERVICE(SN_SERVICE, 1) }, {}};
/* Intentionally no module-device alias: never auto-load from a peer advert. */
static struct tb_service_driver sn_driver = {
	.driver = {.owner = THIS_MODULE, .name = "strix_nhi", .pm = &sn_pm_ops},
	.probe = sn_probe, .remove = sn_remove, .id_table = sn_ids,
};
static int __init sn_init(void)
{
	int ret;

	if (!enable) return -EACCES;
	sn_directory = tb_property_create_dir(&sn_uuid);
	if (!sn_directory) return -ENOMEM;
	ret = tb_property_add_immediate(sn_directory, "prtcid", 1);
	if (!ret) ret = tb_property_add_immediate(sn_directory, "prtcvers", SN_VERSION);
	if (!ret) ret = tb_property_add_immediate(sn_directory, "prtcrevs", 0);
	if (!ret) ret = tb_property_add_immediate(sn_directory, "prtcstns", 0);
	if (ret) goto free;
	ret = tb_register_property_dir(SN_SERVICE, sn_directory);
	if (ret) goto free;
	ret = tb_register_service_driver(&sn_driver);
	if (!ret) return 0;
	tb_unregister_property_dir(SN_SERVICE, sn_directory);
free:
	tb_property_free_dir(sn_directory);
	return ret;
}
static void __exit sn_exit(void)
{
	tb_unregister_service_driver(&sn_driver);
	tb_unregister_property_dir(SN_SERVICE, sn_directory);
	tb_property_free_dir(sn_directory);
}
module_init(sn_init);
module_exit(sn_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Opt-in native NHI CPU-copy software verbs; not RoCE");
