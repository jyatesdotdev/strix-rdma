#!/usr/bin/env bash
# Apply any missing zero-copy patches in an isolated worktree, then verify the
# DMA, HopID, mode-transition, diagnostic, and UAPI contracts the stack uses.
set -euo pipefail

TEST_DIR=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(unset CDPATH; cd -- "$TEST_DIR/../.." && pwd)
KERNEL_SRC=${1:-"$REPO_ROOT/linux"}

if [ ! -d "$KERNEL_SRC/.git" ] && [ ! -f "$KERNEL_SRC/.git" ]; then
	printf 'usage: %s [kernel-source-with-backport]\n' "$0" >&2
	exit 2
fi

TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/tbstream-zc-series.XXXXXX")
case "$TEST_TMP" in
	/*/tbstream-zc-series.*) ;;
	*) printf 'unsafe temporary path: %s\n' "$TEST_TMP" >&2; exit 1 ;;
esac
TEST_TREE="$TEST_TMP/linux"
WORKTREE_ADDED=0

cleanup() {
	if [ "$WORKTREE_ADDED" -eq 1 ]; then
		git -C "$KERNEL_SRC" worktree remove --force "$TEST_TREE" \
			>/dev/null 2>&1 || true
	fi
	case "$TEST_TMP" in
		/*/tbstream-zc-series.*) rm -rf -- "$TEST_TMP" ;;
	esac
}
trap cleanup EXIT HUP INT TERM

git -C "$KERNEL_SRC" worktree add --detach "$TEST_TREE" HEAD >/dev/null
WORKTREE_ADDED=1

patch_subject() {
	awk '
		function emit() {
			if (!emitted) {
				print subject
				emitted = 1
			}
		}
		/^Subject: / {
			subject = substr($0, 10)
			collecting = 1
			next
		}
		collecting && /^[ \t]/ {
			sub(/^[ \t]+/, " ")
			subject = subject $0
			next
		}
		collecting { emit(); exit }
		END { if (collecting) emit() }
	' "$1" | sed -E 's/^\[PATCH[^]]*\] //'
}

PATCH_SUBJECTS="$TEST_TMP/patch-subjects"
: >"$PATCH_SUBJECTS"
patch_number=0
for patch in "$REPO_ROOT"/kernel/zerocopy/*.patch; do
	patch_number=$((patch_number + 1))
	expected_prefix=$(printf '%04d-' "$patch_number")
	patch_name=${patch##*/}
	case "$patch_name" in
		"$expected_prefix"*.patch) ;;
		*)
			printf 'not ok - expected patch %d to start with %s, got %s\n' \
				"$patch_number" "$expected_prefix" "$patch_name" >&2
			exit 1
			;;
	esac

	subject=$(patch_subject "$patch")
	if [ -z "$subject" ]; then
		printf 'not ok - patch has no subject: %s\n' "$patch" >&2
		exit 1
	fi
	if grep -Fx "$subject" "$PATCH_SUBJECTS" >/dev/null; then
		printf 'not ok - duplicate patch subject: %s\n' "$subject" >&2
		exit 1
	fi
	printf '%s\n' "$subject" >>"$PATCH_SUBJECTS"

	if ! git -C "$TEST_TREE" log -64 --format=%s |
		grep -Fx "$subject" >/dev/null; then
		git -C "$TEST_TREE" -c user.name=test -c user.email=test@localhost \
			am "$patch" >/dev/null
	fi
done

STREAM="$TEST_TREE/drivers/thunderbolt/stream.c"
NET="$TEST_TREE/drivers/net/thunderbolt/main.c"
NHI="$TEST_TREE/drivers/thunderbolt/nhi.c"
TB_HEADER="$TEST_TREE/include/linux/thunderbolt.h"
KERNEL_UAPI="$TEST_TREE/include/uapi/linux/thunderbolt-stream.h"
USER_UAPI="$REPO_ROOT/tools/pingpong/thunderbolt-stream.h"
PASS_COUNT=0

fail() {
	printf 'not ok - %s\n' "$*" >&2
	exit 1
}

pass() {
	PASS_COUNT=$((PASS_COUNT + 1))
	printf 'ok %d - %s\n' "$PASS_COUNT" "$1"
}

body_between_file() {
	local file=$1
	local start=$2
	local end=$3
	awk -v start="$start" -v end="$end" '
		$0 ~ start { active = 1 }
		active && seen && $0 ~ end { exit }
		active { print; seen = 1 }
	' "$file"
}

body_between() {
	body_between_file "$STREAM" "$1" "$2"
}

must_contain() {
	local body=$1
	local needle=$2
	local label=$3
	grep -F "$needle" <<<"$body" >/dev/null || fail "$label: missing $needle"
}

must_not_contain() {
	local body=$1
	local needle=$2
	local label=$3
	if grep -F "$needle" <<<"$body" >/dev/null; then
		fail "$label: unexpectedly contains $needle"
	fi
}

line_of() {
	local body=$1
	local needle=$2
	awk -v needle="$needle" 'index($0, needle) { print NR; exit }' <<<"$body"
}

body_from() {
	local body=$1
	local needle=$2
	awk -v needle="$needle" '
		index($0, needle) { active = 1 }
		active { print }
	' <<<"$body"
}

must_order() {
	local body=$1
	local label=$2
	shift 2
	local previous=0
	local needle line

	for needle in "$@"; do
		line=$(line_of "$body" "$needle")
		[ -n "$line" ] || fail "$label: missing $needle"
		[ "$line" -gt "$previous" ] ||
			fail "$label: '$needle' is out of order"
		previous=$line
	done
}

pass 'zero-copy patch filenames and subjects are ordered and unique'

for subject in \
	'thunderbolt: stream: Synchronize zero-copy TX DMA ownership' \
	'thunderbolt: stream: Harden zero-copy mode transitions' \
	'thunderbolt: net: Track receive HopID ownership' \
	'thunderbolt: stream: Make HopID attachment transactional' \
	'thunderbolt: stream: Add zero-copy progress diagnostics' \
	'thunderbolt: Flush posted MSI-X interrupt clears' \
	'thunderbolt: stream: Prime Rx ring before enabling DMA paths'; do
	git -C "$TEST_TREE" log -64 --format=%s | grep -Fx "$subject" >/dev/null ||
		fail "series did not apply $subject"
done
pass 'the complete zero-copy patch series applies in an isolated worktree'

stream_start=$(body_between '^static int tbstream_dev_start' \
	'^static void tbstream_dev_stop')
must_order "$stream_start" 'partial transmit allocation cleanup' \
	'ret = tbstream_dev_alloc_tx_buffers(sdev);' \
	'goto err_free_tx_buffers;'
must_not_contain "$stream_start" 'goto err_free_tx;' \
	'partial transmit allocation cleanup'
must_order "$stream_start" 'receive-ring priming before DMA path enable' \
	'tb_ring_start(sdev->tx_ring.ring);' \
	'tb_ring_start(sdev->rx_ring.ring);' \
	'ret = tbstream_dev_alloc_rx_buffers(sdev);' \
	'ret = tb_xdomain_enable_paths(xd, sdev->out_hopid,'
start_unwind=$(body_from "$stream_start" 'err_stop:')
must_order "$start_unwind" 'receive-ring start failure cleanup' \
	'err_stop:' \
	'tb_ring_stop(sdev->rx_ring.ring);' \
	'tb_ring_stop(sdev->tx_ring.ring);' \
	'tbstream_ring_free(&sdev->rx_ring);' \
	'tb_ring_free(sdev->rx_ring.ring);' \
	'sdev->rx_ring.ring = NULL;' \
	'tbstream_ring_free(&sdev->tx_ring);' \
	'tb_ring_free(sdev->tx_ring.ring);' \
	'sdev->tx_ring.ring = NULL;'
ring_free=$(body_between '^static void tbstream_ring_free' \
	'^static inline bool tbstream_ring_available')
must_order "$ring_free" 'idempotent stream frame cleanup' \
	'if (!ring->frames)' \
	'dma_dev = tb_ring_dma_device(ring->ring);' \
	'kfree(ring->frames);' \
	'ring->frames = NULL;'
pass 'stream Rx rings are primed before paths and cleaned on start failure'

if ! cmp -s "$KERNEL_UAPI" "$USER_UAPI"; then
	diff -u "$KERNEL_UAPI" "$USER_UAPI" >&2 || true
	fail 'userspace thunderbolt-stream.h is not an exact mirror of the kernel UAPI'
fi
pass 'the userspace header exactly mirrors the fully patched kernel UAPI'

CC_BIN=${CC:-cc}
command -v "$CC_BIN" >/dev/null 2>&1 ||
	fail "C compiler not found: $CC_BIN"
for u64_alignment in 4 8; do
	"$CC_BIN" -std=c11 -Wall -Wextra -Werror -nostdinc \
		-I"$TEST_DIR/include" -I"$TEST_TREE/include/uapi" \
		-DTBSTREAM_TEST_U64_ALIGNMENT="$u64_alignment" \
		-fsyntax-only "$TEST_DIR/test-uapi-layout.c" ||
		fail "UAPI layout differs with __u64 alignment $u64_alignment"
done
pass 'diagnostic UAPI layout and ioctl values match across 32/64-bit alignment'

net_connected=$(body_between_file "$NET" \
	'^static void tbnet_connected_work' '^static void tbnet_login_work')
must_order "$net_connected" 'mismatched Rx HopID rollback' \
	'tb_xdomain_alloc_in_hopid(net->xd, net->remote_transmit_path);' \
	'if (ret != net->remote_transmit_path)' \
	'if (ret >= 0)' \
	'tb_xdomain_release_in_hopid(net->xd, ret);' \
	'net->rx_hopid = ret;'
pass 'a nonnegative mismatched Rx HopID is released before ownership publication'

net_release=$(body_between_file "$NET" \
	'^static void tbnet_release_rx_hopid' '^static void tbnet_tear_down')
must_order "$net_release" 'tracked Rx HopID release' \
	'if (!net->rx_hopid)' \
	'tb_xdomain_release_in_hopid(net->xd, net->rx_hopid);' \
	'net->rx_hopid = 0;'
net_teardown=$(body_between_file "$NET" \
	'^static void tbnet_tear_down' '^static int tbnet_handle_packet')
must_contain "$net_teardown" 'tbnet_release_rx_hopid(net);' \
	'tracked Rx HopID teardown'
must_contain "$net_connected" 'tbnet_release_rx_hopid(net);' \
	'tracked Rx HopID setup rollback'
if grep -F 'tb_xdomain_release_in_hopid(net->xd, net->remote_transmit_path)' \
	"$NET" >/dev/null; then
	fail 'tbnet still releases the requested rather than owned Rx HopID'
fi
pass 'setup rollback and teardown release only the tracked owned Rx HopID'

in_hopid_alloc=$(body_between \
	'^static int tbstream_dev_alloc_in_hopid' \
	'^static int tbstream_dev_alloc_out_hopid')
must_order "$in_hopid_alloc" 'input HopID replacement ownership' \
	'int old_hopid = sdev->in_hopid;' \
	'if (old_hopid > 0 && old_hopid == hopid)' \
	'return 0;'
in_hopid_replace=$(body_from "$in_hopid_alloc" \
	'ret = tb_xdomain_alloc_in_hopid(xd, hopid);')
must_order "$in_hopid_replace" 'transactional input HopID replacement' \
	'ret = tb_xdomain_alloc_in_hopid(xd, hopid);' \
	'if (ret < 0)' \
	'return ret;' \
	'if (hopid > 0 && hopid != ret)' \
	'tb_xdomain_release_in_hopid(xd, ret);' \
	'return -EBUSY;' \
	'if (old_hopid > 0)' \
	'tb_xdomain_release_in_hopid(xd, old_hopid);' \
	'sdev->in_hopid = ret;'
out_hopid_alloc=$(body_between \
	'^static int tbstream_dev_alloc_out_hopid' \
	'^static ssize_t$')
must_order "$out_hopid_alloc" 'output HopID replacement ownership' \
	'int old_hopid = sdev->out_hopid;' \
	'if (old_hopid > 0 && old_hopid == hopid)' \
	'return 0;'
out_hopid_replace=$(body_from "$out_hopid_alloc" \
	'ret = tb_xdomain_alloc_out_hopid(xd, hopid);')
must_order "$out_hopid_replace" 'transactional output HopID replacement' \
	'ret = tb_xdomain_alloc_out_hopid(xd, hopid);' \
	'if (ret < 0)' \
	'return ret;' \
	'if (hopid > 0 && hopid != ret)' \
	'tb_xdomain_release_out_hopid(xd, ret);' \
	'return -EBUSY;' \
	'if (old_hopid > 0)' \
	'tb_xdomain_release_out_hopid(xd, old_hopid);' \
	'sdev->out_hopid = ret;'
pass 'HopID replacements retain prior ownership until exact acquisition succeeds'

in_hopid_store=$(body_between \
	'^tbstream_dev_in_hopid_store' \
	'^CONFIGFS_ATTR.tbstream_dev_, in_hopid')
must_order "$in_hopid_store" 'input HopID recovery wake' \
	'ret = tbstream_dev_alloc_in_hopid(sdev, in_hopid);' \
	'ret = tbstream_dev_update_properties(sdev);' \
	'if (!ret)' \
	'wake_up_interruptible(&sdev->wait);'
out_hopid_store=$(body_between \
	'^tbstream_dev_out_hopid_store' \
	'^CONFIGFS_ATTR.tbstream_dev_, out_hopid')
must_order "$out_hopid_store" 'output HopID recovery wake' \
	'ret = tbstream_dev_alloc_out_hopid(sdev, out_hopid);' \
	'ret = tbstream_dev_update_properties(sdev);' \
	'if (!ret)' \
	'wake_up_interruptible(&sdev->wait);'
pass 'successful manual HopID recovery wakes blocked openers'

attach_body=$(body_between \
	'^tbstream_dev_attach_stream' \
	'^static void tbstream_dev_detach_stream')
must_order "$attach_body" 'transactional HopID attachment' \
	'in_hopid = sdev->in_hopid;' \
	'out_hopid = sdev->out_hopid;' \
	'service_get_hopids(stream->svc, name, &in_hopid, &out_hopid);' \
	'sdev->in_hopid = 0;' \
	'sdev->out_hopid = 0;' \
	'ret = tbstream_dev_alloc_in_hopid(sdev, in_hopid);' \
	'in_allocated = true;' \
	'ret = tbstream_dev_alloc_out_hopid(sdev, out_hopid);' \
	'out_allocated = true;' \
	'ret = service_update_properties(stream->svc, name, sdev->in_hopid,'
pass 'attachment publishes HopIDs only after both exact allocations succeed'

rollback_body=$(body_between '^err_release:' '^}')
must_order "$rollback_body" 'transactional HopID rollback' \
	'err_release:' \
	'if (out_allocated)' \
	'tb_xdomain_release_out_hopid(xd, sdev->out_hopid);' \
	'if (in_allocated)' \
	'tb_xdomain_release_in_hopid(xd, sdev->in_hopid);' \
	'sdev->in_hopid = 0;' \
	'sdev->out_hopid = 0;' \
	'service_update_properties(stream->svc, name, 0, 0);' \
	'mutex_unlock(&sdev->lock);' \
	'return ret;'
pass 'attachment rollback clears ownership and advertisement under one lock'

make_group_body=$(body_between \
	'^tbstream_dev_make_group' '^static void$')
must_order "$make_group_body" 'creation-time attach failure' \
	'mutex_lock(&sg->lock);' \
	'list_add_tail(&sdev->list, &sg->dev_list);' \
	'ret = tbstream_dev_attach_stream(sdev, sg);' \
	'if (ret) {' \
	'tbstream_dev_detach_stream(sdev);' \
	'list_del(&sdev->list);' \
	'mutex_unlock(&sg->lock);' \
	'config_group_put(&sdev->group);' \
	'return ERR_PTR(ret);'
reattach_body=$(body_between \
	'^static void tbstream_group_attach_stream' \
	'^static void tbstream_group_detach_stream')
must_order "$reattach_body" 'existing-device attach failure' \
	'ret = tbstream_dev_attach_stream(sdev, sg);' \
	'if (ret)' \
	'dev_warn(&stream->svc->dev,'
item_release_body=$(body_between \
	'^static void tbstream_dev_item_release' \
	'^static struct configfs_item_operations')
must_order "$item_release_body" 'pre-registration ConfigFS release' \
	'if (sdev->misc_registered)' \
	'misc_deregister(&sdev->misc);'
misc_register_body=$(body_from "$make_group_body" \
	'ret = misc_register(&sdev->misc);')
must_order "$misc_register_body" 'misc registration publication' \
	'ret = misc_register(&sdev->misc);' \
	'if (ret) {' \
	'mutex_lock(&sg->lock);' \
	'tbstream_dev_detach_stream(sdev);' \
	'list_del(&sdev->list);' \
	'mutex_unlock(&sg->lock);' \
	'return ERR_PTR(ret);' \
	'sdev->misc_registered = true;'
pass 'ConfigFS creation serializes attachment and guards pre-registration cleanup'

free_body=$(body_between '^static void tbstream_ring_free' '^static inline bool')
must_contain "$free_body" 'TB_MAX_FRAME_SIZE, dir);' 'mapping teardown'
if grep -F 'tb_ring_frame_size(&sf->frame), dir);' <<<"$free_body" >/dev/null; then
	fail 'mapping teardown still uses a mutable descriptor length'
fi
pass 'TX and RX mappings are unmapped with their original page size'

tx_callback=$(body_between '^tbstream_dev_tx_callback' '^static int tbstream_dev_alloc_tx_buffers')
tx_completion=$(body_from "$tx_callback" 'zc = READ_ONCE(sdev->zc);')
must_order "$tx_completion" 'TX completion ownership' \
	'zc = READ_ONCE(sdev->zc);' \
	'dma_sync_single_for_cpu' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'sdev->tx_ring.prod++;' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);' \
	'kfifo_put(&sdev->zc_events, ev)'
must_contain "$tx_completion" 'TB_MAX_FRAME_SIZE,' 'TX completion ownership'
pass 'TX completion returns the full slot before producer and event publication'

enable_body=$(body_between '^static int tbstream_dev_zc_enable' '^static int tbstream_dev_zc_get_info')
must_order "$enable_body" 'zero-copy enable ownership' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'sdev->tx_ring.prod - sdev->tx_ring.cons != size - 1' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);' \
	'kfifo_alloc(&sdev->zc_events' \
	'dma_sync_single_for_cpu' \
	'WRITE_ONCE(sdev->zc, true);'
must_order "$enable_body" 'zero-copy enable ownership' \
	'if (sf->page)' \
	'dma_sync_single_for_cpu' \
	'TB_MAX_FRAME_SIZE,' \
	'DMA_TO_DEVICE);'
pass 'enable requires idle TX and hands page-backed frames to CPU before publication'

must_order "$enable_body" 'zero-copy cursor transition' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'sdev->rx_ring.cons || sdev->rx_ring.prod' \
	'sdev->tx_ring.cons = 0;' \
	'sdev->tx_ring.prod = size - 1;' \
	'WRITE_ONCE(sdev->zc, true);'
must_contain "$enable_body" 'goto out_free_fifo;' 'zero-copy cursor transition'
must_order "$enable_body" 'aborted enable ownership rollback' \
	'dma_sync_single_for_cpu' \
	'sdev->rx_ring.cons || sdev->rx_ring.prod' \
	'dma_sync_single_for_device' \
	'goto out_free_fifo;'
pass 'enable rejects non-pristine RX and rebases the idle TX cursor atomically'

rx_callback=$(body_between '^tbstream_dev_rx_callback' '^static struct tbstream_frame \*')
rx_completion=$(body_from "$rx_callback" 'sf->completed = true;')
must_order "$rx_completion" 'RX transition serialization' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'sdev->rx_ring.prod++;' \
	'zc = READ_ONCE(sdev->zc);' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);'
pass 'RX completion and zero-copy publication share a transition lock'

consume_rx=$(body_between '^static int tbstream_dev_consume_rx' \
	'^static int tbstream_dev_alloc_rx_buffers')
must_order "$consume_rx" 'failed RX repost ownership rollback' \
	'sdev->rx_ring.cons++;' \
	'frame_offset = sf->offset;' \
	'frame_size = sf->frame.size;' \
	'sf->completed = false;' \
	'sf->offset = 0;' \
	'sf->frame.size = 0;' \
	'dma_sync_single_for_device' \
	'ret = tb_ring_rx(sdev->rx_ring.ring, &sf->frame);' \
	'if (ret)' \
	'dma_sync_single_for_cpu' \
	'sf->offset = frame_offset;' \
	'sf->frame.size = frame_size;' \
	'sf->completed = true;' \
	'sdev->rx_ring.cons--;'
pass 'failed RX repost restores frame offset, size, completion, and cursor'

read_body=$(body_between '^tbstream_dev_fops_read_iter' '^static ssize_t$')
must_contain "$read_body" 'if (sdev->zc || sdev->zc_tx_import || sdev->zc_rx_import) {' \
	'legacy read import/zero-copy entry gate'
[ "$(grep -Fc 'if (sdev->zc)' <<<"$read_body")" -ge 1 ] ||
	fail 'legacy read lacks post-lock zero-copy rechecks'
must_contain "$read_body" 'READ_ONCE(sdev->zc) ||' 'legacy read transition'
pass 'legacy reads reject imports and recheck zero-copy after wait-loop locking'

write_body=$(body_between '^tbstream_dev_fops_write_iter' '^static __poll_t$')
must_contain "$write_body" 'if (sdev->zc || sdev->zc_tx_import || sdev->zc_rx_import) {' \
	'legacy write import/zero-copy entry gate'
[ "$(grep -Fc 'if (sdev->zc)' <<<"$write_body")" -ge 1 ] ||
	fail 'legacy write lacks post-lock zero-copy rechecks'
must_contain "$write_body" 'READ_ONCE(sdev->zc) ||' 'legacy write transition'
pass 'legacy writes reject imports and recheck zero-copy after wait-loop locking'

submit_body=$(body_between '^static int tbstream_dev_zc_submit_tx' '^static int tbstream_dev_zc_post_rx')
must_order "$submit_body" 'TX submission ownership' \
	'tx.first = sdev->tx_ring.cons % size;' \
	'copy_to_user(arg, &tx, sizeof(tx))' \
	'dma_sync_single_for_device' \
	'tb_ring_tx(sdev->tx_ring.ring, &sf->frame);'
must_contain "$submit_body" 'TB_MAX_FRAME_SIZE, DMA_TO_DEVICE);' \
	'TX submission ownership'
pass 'submission gives the full slot to the NHI immediately before enqueue'

must_order "$submit_body" 'failed TX enqueue rollback' \
	'tb_ring_tx(sdev->tx_ring.ring, &sf->frame);' \
	'dma_sync_single_for_cpu' \
	'sdev->tx_ring.cons--;'
pass 'failed enqueue returns CPU ownership and restores the TX cursor'

must_order "$submit_body" 'partial TX failure latch' \
	'sdev->tx_ring.cons--;' \
	'if (i)' \
	'wake_error = tbstream_zc_fail_locked(' \
	'TBSTREAM_ZC_ERROR_TX_PARTIAL);' \
	'wake_up_interruptible_poll(&sdev->wait,'
must_contain "$submit_body" 'ret = -EIO;' 'partial TX failure latch'
post_body=$(body_between '^static int tbstream_dev_zc_post_rx' '^static int tbstream_dev_zc_reap')
must_contain "$post_body" 'if (tbstream_dev_zc_failed(sdev))' \
	'partial TX failure latch'
must_contain "$post_body" 'ret = -EIO;' 'partial TX failure latch'
reap_body=$(body_between '^static int tbstream_dev_zc_reap' '^static long$')
must_contain "$reap_body" 'tbstream_dev_zc_failed(sdev)' \
	'partial TX failure latch'
must_contain "$reap_body" 'ret = -EIO;' 'partial TX failure latch'
pass 'partial multi-frame enqueue failure becomes terminal for zero-copy I/O'

must_order "$reap_body" 'fault-safe event reap' \
	'kfifo_peek(&sdev->zc_events, &ev);' \
	'copy_to_user(&uevents[n], &ev, sizeof(ev))' \
	'kfifo_skip(&sdev->zc_events);'
must_contain "$reap_body" 'ret = n ? n : -EFAULT;' 'fault-safe event reap'
if grep -F 'kfifo_out_spinlocked' <<<"$reap_body" >/dev/null; then
	fail 'event reap still dequeues before copying to userspace'
fi
pass 'event reap preserves the FIFO head when copy_to_user faults'

diagnostic_uapi=$(body_between_file "$KERNEL_UAPI" \
	'^struct tbstream_zc_ring_stats' '^#define TBSTREAM_ZC_MAGIC')
aligned_u64_count=$(grep -Ec \
	'^[[:space:]]*__aligned_u64[[:space:]]' <<<"$diagnostic_uapi")
[ "$aligned_u64_count" -eq 40 ] ||
	fail "diagnostic UAPI has $aligned_u64_count aligned u64 fields, expected 40"
if grep -Eq '^[[:space:]]*__u64[[:space:]]' <<<"$diagnostic_uapi"; then
	fail 'diagnostic UAPI contains a compat-unsafe plain __u64 field'
fi
must_contain "$diagnostic_uapi" '__u32 last_error;' \
	'diagnostic terminal error ABI'
must_contain "$diagnostic_uapi" '#define TBSTREAM_ZC_ERROR_RX_CRC' \
	'diagnostic terminal error ABI'
must_contain "$diagnostic_uapi" '#define TBSTREAM_ZC_ERROR_RX_PARTIAL' \
	'diagnostic terminal error ABI'
must_contain "$diagnostic_uapi" '#define TBSTREAM_ZC_RING_F_HW_VALID' \
	'diagnostic hardware snapshot validity ABI'
pass 'diagnostic counters are compat-aligned and expose a stable terminal error'

ring_write=$(body_between_file "$NHI" \
	'^static void ring_write_descriptors' '^static void ring_work')
must_order "$ring_write" 'descriptor-post accounting' \
	'descriptor->flags = RING_DESC_POSTED;' \
	'ring->descriptors_posted++;' \
	'ring->head = (ring->head + 1) % ring->size;' \
	'ring_iowrite_prod(ring, ring->head);'
ring_progress=$(body_between_file "$NHI" \
	'^static void ring_work' '^int __tb_ring_enqueue')
must_order "$ring_progress" 'descriptor-completion accounting' \
	'spin_lock_irqsave(&ring->lock, flags);' \
	'ring->work_runs++;' \
	'if (!(ring->descriptors[ring->tail].flags' \
	'ring->descriptors_completed++;' \
	'ring->tail = (ring->tail + 1) % ring->size;' \
	'ring_write_descriptors(ring);' \
	'spin_unlock_irqrestore(&ring->lock, flags);'
[ "$(grep -Fc 'ring->interrupts++;' "$NHI")" -eq 2 ] ||
	fail 'MSI-X and shared-MSI paths do not both account ring interrupts'
pass 'ring posting, completion, work, and both interrupt paths are accounted'

msix_clear=$(body_between_file "$NHI" '^static void ring_clear_msix' \
	'^static irqreturn_t ring_msix')
must_order "$msix_clear" 'MSI-X clear completion flush' \
	'if (ring->nhi->quirks & QUIRK_AUTO_CLEAR_INT)' \
	'index = ring_interrupt_index(ring);' \
	'iowrite32(BIT(bit), ring->nhi->iobase + REG_RING_INT_CLEAR);' \
	'ioread32(ring->nhi->iobase + REG_RING_INTERRUPT_BASE +' \
	'index / 32 * 4);'
must_not_contain "$msix_clear" 'REG_RING_NOTIFY_BASE' \
	'MSI-X clear completion flush'
pass 'non-auto-clear MSI-X writes are flushed through a safe register read'

ring_stats=$(body_between_file "$NHI" \
	'^int tb_ring_get_stats' '^int tb_ring_kick')
must_order "$ring_stats" 'lock-consistent ring snapshot' \
	'if (!stats)' \
	'memset(stats, 0, sizeof(*stats));' \
	'pm_ret = pm_runtime_get_if_active(&ring->nhi->pdev->dev);' \
	'spin_lock_irqsave(&ring->nhi->lock, flags);' \
	'spin_lock(&ring->lock);' \
	'stats->descriptors_posted = ring->descriptors_posted;' \
	'stats->tail_flags = descriptor->flags;' \
	'dma_rmb();' \
	'stats->tail_length = descriptor->length;' \
	'if (pm_ret > 0 && ring->running && !ring->nhi->going_away)' \
	'indices = ioread32(ring_desc_base(ring) + 8);' \
	'stats->hw_posted =' \
	'stats->hw_completed =' \
	'stats->hw_valid = true;' \
	'spin_unlock(&ring->lock);' \
	'spin_unlock_irqrestore(&ring->nhi->lock, flags);' \
	'pm_runtime_put_noidle(&ring->nhi->pdev->dev);'
must_not_contain "$ring_stats" 'REG_RING_NOTIFY' \
	'non-destructive ring snapshot'
pass 'ring snapshots are lock-consistent, DMA-ordered, and non-destructive'

ring_kick=$(body_between_file "$NHI" '^int tb_ring_kick' \
	'^int nhi_mailbox_cmd')
must_order "$ring_kick" 'diagnostic ring kick removal safety' \
	'spin_lock_irqsave(&ring->nhi->lock, flags);' \
	'spin_lock(&ring->lock);' \
	'if (ring->nhi->going_away)' \
	'ret = -ENODEV;' \
	'if (!ring->running)' \
	'ret = -ESHUTDOWN;'
pass 'ring kick locks NHI before ring and rejects controller removal'
kick_schedule=$(body_from "$ring_kick" 'ring->kick_requests++;')
must_order "$kick_schedule" 'diagnostic ring kick scheduling' \
	'ring->kick_requests++;' \
	'ring->descriptors[ring->tail].flags & RING_DESC_COMPLETED' \
	'ring->kick_pending++;' \
	'schedule_work(&ring->work);' \
	'spin_unlock(&ring->lock);' \
	'spin_unlock_irqrestore(&ring->nhi->lock, flags);' \
	'return ret;'
must_not_contain "$ring_kick" 'iowrite' 'diagnostic ring kick'
must_not_contain "$ring_kick" '__ring_interrupt' 'diagnostic ring kick'
must_contain "$(<"$TB_HEADER")" \
	'int tb_ring_get_stats(struct tb_ring *ring, struct tb_ring_stats *stats);' \
	'ring diagnostic service API'
must_contain "$(<"$TB_HEADER")" \
	'int tb_ring_kick(struct tb_ring *ring);' \
	'ring diagnostic service API'
pass 'a kick schedules normal work without mutating descriptors or IRQ state'

fail_locked=$(body_between \
	'^static bool tbstream_zc_fail_locked' \
	'^static void tbstream_dev_zc_fail')
must_order "$fail_locked" 'first terminal error latch' \
	'if (sdev->zc_failed)' \
	'return false;' \
	'sdev->zc_failed = true;' \
	'sdev->zc_last_error = error;' \
	'sdev->zc_counters.failures++;' \
	'return true;'
fail_wrapper=$(body_between \
	'^static void tbstream_dev_zc_fail' '^static void$')
must_order "$fail_wrapper" 'terminal error wakeup' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'wake = tbstream_zc_fail_locked(sdev, error);' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);' \
	'if (wake)' \
	'wake_up_interruptible_poll(&sdev->wait, EPOLLHUP | EPOLLERR);'
pass 'the first terminal zero-copy error is latched and wakes blocked users'

failed_reader=$(body_between '^static bool tbstream_dev_zc_failed' \
	'^static bool tbstream_dev_zc_has_events')
must_order "$failed_reader" 'zero-copy failure synchronization' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'failed = sdev->zc_failed;' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);'
event_reader=$(body_between '^static bool tbstream_dev_zc_has_events' \
	'^static void$')
must_order "$event_reader" 'event FIFO synchronization' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'has_events = !kfifo_is_empty(&sdev->zc_events);' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);'
poll_body=$(body_between '^tbstream_dev_fops_poll' \
	'^static int tbstream_dev_zc_enable')
must_contain "$poll_body" 'tbstream_dev_zc_failed(sdev);' \
	'poll failure synchronization'
must_contain "$poll_body" 'tbstream_dev_zc_has_events(sdev)' \
	'poll FIFO synchronization'
pass 'failure and FIFO readiness are read under the zero-copy lock'

removed_reader=$(body_between '^static inline bool tbstream_dev_removed' \
	'^static inline bool tbstream_dev_closed')
must_contain "$removed_reader" 'return READ_ONCE(sdev->removed);' \
	'removed-state visibility'
must_order "$poll_body" 'removed-state poll termination' \
	'tbstream_dev_valid(sdev) != 0 || tbstream_dev_removed(sdev)' \
	'mask |= EPOLLHUP | EPOLLERR;'
drop_body=$(body_between '^tbstream_dev_drop_item' \
	'^static struct configfs_group_operations')
must_order "$drop_body" 'removed-state wake publication' \
	'list_del(&sdev->list);' \
	'WRITE_ONCE(sdev->removed, true);' \
	'wake_up_interruptible_poll(&sdev->wait, EPOLLHUP | EPOLLERR);'
pass 'removal is published before wakeup and terminates poll with HUP/ERR'

rx_callback=$(body_between '^tbstream_dev_rx_callback' \
	'^static struct tbstream_frame \*')
must_order "$rx_callback" 'RX descriptor error propagation' \
	'sdev->zc_counters.rx_callbacks++;' \
	'sdev->zc_counters.crc_errors++;' \
	'sdev->zc_counters.overrun_errors++;' \
	'u32 error = sf->frame.flags & RING_DESC_CRC_ERROR ?' \
	'TBSTREAM_ZC_ERROR_RX_CRC :' \
	'TBSTREAM_ZC_ERROR_RX_OVERRUN;' \
	'tbstream_dev_zc_fail(sdev, error);'
pass 'RX CRC and overrun descriptors become explicit terminal errors'

[ "$(grep -Fc 'if (!kfifo_put(&sdev->zc_events, ev))' "$STREAM")" -eq 2 ] ||
	fail 'both RX and TX event producers must check FIFO insertion'
if grep -Eq '^[[:space:]]*kfifo_put\(' "$STREAM"; then
	fail 'an event FIFO insertion result is still ignored'
fi
zc_rx=$(body_between '^tbstream_dev_zc_rx' '^static void$')
must_order "$zc_rx" 'RX FIFO-full failure' \
	'if (!kfifo_put(&sdev->zc_events, ev))' \
	'sdev->zc_counters.event_drops++;' \
	'wake_error = tbstream_zc_fail_locked(' \
	'TBSTREAM_ZC_ERROR_EVENT_DROP);' \
	'if (wake_error)' \
	'wake_up_interruptible_poll(&sdev->wait, EPOLLHUP | EPOLLERR);'
tx_callback=$(body_between '^tbstream_dev_tx_callback' \
	'^static int tbstream_dev_alloc_tx_buffers')
must_order "$tx_callback" 'TX FIFO-full failure' \
	'if (!kfifo_put(&sdev->zc_events, ev))' \
	'sdev->zc_counters.event_drops++;' \
	'wake_error = tbstream_zc_fail_locked(' \
	'TBSTREAM_ZC_ERROR_EVENT_DROP);' \
	'if (wake_error)' \
	'wake_up_interruptible_poll(&sdev->wait,' \
	'EPOLLHUP | EPOLLERR);'
pass 'event FIFO exhaustion is counted, terminal, and observable'

enable_body=$(body_between '^static int tbstream_dev_zc_enable' \
	'^static int tbstream_dev_zc_get_info')
must_order "$enable_body" 'diagnostic session reset' \
	'sdev->zc_failed = false;' \
	'sdev->zc_last_error = TBSTREAM_ZC_ERROR_NONE;' \
	'memset(&sdev->zc_counters, 0, sizeof(sdev->zc_counters));' \
	'WRITE_ONCE(sdev->zc, true);'
submit_body=$(body_between '^static int tbstream_dev_zc_submit_tx' \
	'^static int tbstream_dev_zc_post_rx')
must_order "$submit_body" 'partial TX diagnostic failure' \
	'sdev->zc_counters.tx_submit_calls++;' \
	'sdev->zc_counters.tx_enqueue_errors++;' \
	'if (i)' \
	'wake_error = tbstream_zc_fail_locked(' \
	'TBSTREAM_ZC_ERROR_TX_PARTIAL);'
post_body=$(body_between '^static int tbstream_dev_zc_post_rx' \
	'^static int tbstream_dev_zc_reap')
must_order "$post_body" 'partial RX diagnostic failure' \
	'sdev->zc_counters.rx_repost_calls++;' \
	'sdev->zc_counters.rx_repost_errors++;' \
	'if (i)' \
	'wake_error = tbstream_zc_fail_locked(' \
	'TBSTREAM_ZC_ERROR_RX_PARTIAL);'
reap_body=$(body_between '^static int tbstream_dev_zc_reap' \
	'^static void$')
must_order "$reap_body" 'successful reap accounting' \
	'sdev->zc_counters.reap_calls++;' \
	'tbstream_dev_zc_has_events(sdev)' \
	'tbstream_dev_zc_failed(sdev)' \
	'kfifo_peek(&sdev->zc_events, &ev);' \
	'copy_to_user(&uevents[n], &ev, sizeof(ev))' \
	'kfifo_skip(&sdev->zc_events);' \
	'sdev->zc_counters.reaped_events++;'
pass 'session, enqueue, repost, and reap counters preserve ownership ordering'

get_stats=$(body_between '^static int tbstream_dev_zc_get_stats' \
	'^static int tbstream_dev_zc_kick')
must_order "$get_stats" 'zero-copy diagnostic snapshot' \
	'mutex_lock_interruptible(&sdev->lock)' \
	'if (!sdev->zc || !sdev->tx_ring.ring || !sdev->rx_ring.ring)' \
	'spin_lock_irqsave(&sdev->zc_lock, flags);' \
	'stats.tx_submit_calls = c->tx_submit_calls;' \
	'if (sdev->closed)' \
	'TBSTREAM_ZC_STATS_F_CLOSED;' \
	'stats.last_error = sdev->zc_last_error;' \
	'spin_unlock_irqrestore(&sdev->zc_lock, flags);' \
	'if (tbstream_dev_removed(sdev))' \
	'TBSTREAM_ZC_STATS_F_REMOVED;' \
	'tb_ring_get_stats(sdev->tx_ring.ring, &tx);' \
	'tb_ring_get_stats(sdev->rx_ring.ring, &rx);' \
	'tbstream_dev_zc_copy_ring_stats(&stats.tx, &tx);' \
	'tbstream_dev_zc_copy_ring_stats(&stats.rx, &rx);' \
	'mutex_unlock(&sdev->lock);' \
	'copy_to_user(arg, &stats, sizeof(stats))'
[ "$(grep -Fc 'sdev->closed' <<<"$get_stats")" -eq 1 ] ||
	fail 'GET_STATS has an unlocked direct closed-state read'
kick_ioctl=$(body_between '^static int tbstream_dev_zc_kick' '^static long$')
must_order "$kick_ioctl" 'zero-copy diagnostic kick validation' \
	'if (!zc_diagnostic_kick)' \
	'return -EACCES;' \
	'if (!capable(CAP_SYS_RAWIO))' \
	'return -EPERM;' \
	'copy_from_user(&kick, arg, sizeof(kick))' \
	'if (!kick.rings ||' \
	'kick.rings & ~(TBSTREAM_ZC_KICK_TX | TBSTREAM_ZC_KICK_RX)' \
	'kick.reserved)' \
	'mutex_lock_interruptible(&sdev->lock)' \
	'ret = tbstream_dev_valid(sdev);' \
	'if (ret || tbstream_dev_removed(sdev))' \
	'ret = -ENXIO;' \
	'if (!sdev->zc || !sdev->tx_ring.ring || !sdev->rx_ring.ring)' \
	'if (kick.rings & TBSTREAM_ZC_KICK_RX)' \
	'int rx_ret = tb_ring_kick(sdev->rx_ring.ring);' \
	'if (kick.rings & TBSTREAM_ZC_KICK_TX)' \
	'int tx_ret = tb_ring_kick(sdev->tx_ring.ring);' \
	'ret = tx_ret;'
tx_kick_result=$(body_from "$kick_ioctl" \
	'int tx_ret = tb_ring_kick(sdev->tx_ring.ring);')
must_order "$tx_kick_result" 'TX kick result preservation' \
	'int tx_ret = tb_ring_kick(sdev->tx_ring.ring);' \
	'if (!ret)' \
	'ret = tx_ret;'
must_contain "$(<"$STREAM")" 'module_param(zc_diagnostic_kick, bool, 0600);' \
	'diagnostic kick opt-in'
ioctl_body=$(body_between '^tbstream_dev_fops_ioctl' \
	'^static int tbstream_dev_fops_mmap')
must_contain "$ioctl_body" 'case TBSTREAM_ZC_GET_STATS:' \
	'diagnostic ioctl dispatch'
must_contain "$ioctl_body" 'case TBSTREAM_ZC_KICK:' \
	'diagnostic ioctl dispatch'
copy_stats=$(body_between '^tbstream_dev_zc_copy_ring_stats' \
	'^static int tbstream_dev_zc_get_stats')
must_order "$copy_stats" 'hardware-valid snapshot publication' \
	'if (src->hw_valid)' \
	'TBSTREAM_ZC_RING_F_HW_VALID;'
pass 'stats and privileged opt-in kicks revalidate and safely route live rings'

stop_body=$(body_between '^static void tbstream_dev_stop' '^static ssize_t$')
must_order "$stop_body" 'diagnostic teardown reporting' \
	'if (!tb_ring_flush(sdev->tx_ring.ring, 500))' \
	'tb_ring_get_stats(sdev->tx_ring.ring, &stats);' \
	'TX ring %d flush timed out:' \
	'tb_ring_stop(sdev->tx_ring.ring);' \
	'tb_ring_flush(sdev->rx_ring.ring, 500);' \
	'tb_ring_stop(sdev->rx_ring.ring);' \
	'ret = tb_xdomain_disable_paths' \
	'if (ret)' \
	'failed to disable DMA paths: %d'
must_not_contain "$stop_body" 'sdev->misc.this_device' \
	'safe teardown diagnostics'
release_body=$(body_between '^static int tbstream_dev_fops_release' \
	'^static const struct file_operations')
must_order "$release_body" 'CLOSE retry reporting' \
	'ret = tbstream_dev_send_close(sdev);' \
	'if (ret)' \
	'int retry = tbstream_dev_send_close(sdev);' \
	'if (retry)' \
	'failed to send CLOSE twice:' \
	'tbstream_dev_stop(sdev);'
pass 'teardown reports TX progress timeouts and DMA-path disable failures'

"$CC_BIN" -std=c11 -Wall -Wextra -Werror \
	-I"$TEST_TREE/drivers/thunderbolt" \
	"$TEST_DIR/test-sg-flatten.c" -o "$TEST_TMP/test-sg-flatten" ||
	fail 'SG flatten helper test did not compile'
"$TEST_TMP/test-sg-flatten" >/dev/null ||
	fail 'SG flatten helper failed its static geometry cases'
pass 'SG flatten helper validates and flattens every static geometry case'

probe_body=$(body_between '^static int tbstream_dev_zc_dmabuf_probe' \
	'^tbstream_dev_fops_ioctl')
must_order "$probe_body" 'DMA-BUF probe gating and transactional teardown' \
	'if (!zc_diagnostic_dmabuf)' \
	'return -EACCES;' \
	'if (!capable(CAP_SYS_RAWIO))' \
	'return -EPERM;' \
	'probe.version != TBSTREAM_ZC_DMABUF_PROBE_VERSION ||' \
	'case TBSTREAM_ZC_DMABUF_RX:' \
	'mutex_lock_interruptible(&sdev->lock)' \
	'ret = tbstream_dev_valid(sdev);' \
	'if (sdev->zc || sdev->zc_tx_import || sdev->zc_rx_import)' \
	'ret = tbstream_dev_activate_locked(sdev);' \
	'dma_dev = tb_ring_dma_device' \
	'dmabuf = dma_buf_get(probe.fd);' \
	'!(dmabuf->file->f_mode & FMODE_WRITE)' \
	'probe.length > dmabuf->size - probe.offset' \
	'attach = dma_buf_dynamic_attach' \
	'ret = dma_buf_pin(attach);' \
	'sgt = dma_buf_map_attachment(attach, dir);' \
	'ret = tbstream_sg_flatten(' \
	'dma_buf_unmap_attachment(attach, sgt, dir);' \
	'dma_buf_unpin(attach);' \
	'dma_buf_detach(dmabuf, attach);' \
	'dma_buf_put(dmabuf);' \
	'mutex_unlock(&sdev->lock);' \
	'copy_to_user(arg, &probe, sizeof(probe))'
probe_rollback=$(body_from "$probe_body" 'err_unpin:')
must_order "$probe_rollback" 'DMA-BUF probe partial-error rollback' \
	'err_unpin:' \
	'dma_buf_unpin(attach);' \
	'err_detach:' \
	'dma_buf_detach(dmabuf, attach);' \
	'err_put:' \
	'dma_buf_put(dmabuf);' \
	'out_unlock:' \
	'mutex_unlock(&sdev->lock);'
must_not_contain "$probe_body" 'tb_ring_enqueue' \
	'no-traffic probe descriptor safety'
must_not_contain "$probe_body" 'ring_write' \
	'no-traffic probe descriptor safety'
sg_glue=$(body_between '^static int tbstream_sg_flatten' \
	'^#define TBSTREAM_DMABUF_PROBE_MAX')
must_contain "$sg_glue" 'for_each_sgtable_dma_sg(sgt, sg, i)' \
	'DMA-mapped SG walk'
must_not_contain "$sg_glue" 'for_each_sgtable_sg' \
	'original SG table is never walked for DMA addresses'
must_not_contain "$sg_glue" 'sgt->sgl' \
	'original SG table is never walked for DMA addresses'
must_contain "$(<"$STREAM")" 'module_param(zc_diagnostic_dmabuf, bool, 0600);' \
	'DMA-BUF probe opt-in'
must_contain "$(<"$STREAM")" 'MODULE_IMPORT_NS("DMA_BUF");' \
	'DMA-BUF namespace import'
ioctl_body=$(body_between '^tbstream_dev_fops_ioctl' \
	'^static int tbstream_dev_fops_mmap')
must_contain "$ioctl_body" 'case TBSTREAM_ZC_DMABUF_PROBE:' \
	'probe ioctl dispatch'
pass 'DMA-BUF probe is privileged, no-traffic, and transactional'

import_body=$(body_between '^static int tbstream_dev_zc_import' \
	'^static long$')
must_order "$import_body" 'import gating and pre-activation exclusivity' \
	'if (!zc_diagnostic_dmabuf)' \
	'return -EACCES;' \
	'if (!capable(CAP_SYS_RAWIO))' \
	'return -EPERM;' \
	'imp.version != TBSTREAM_ZC_IMPORT_VERSION' \
	'if (sdev->users != 1)' \
	'if (sdev->started || sdev->zc || sdev->zc_tx_import ||' \
	'tx_half = tbstream_dev_zc_import_half(sdev, &imp.tx,' \
	'rx_half = tbstream_dev_zc_import_half(sdev, &imp.rx,' \
	'tx_half->dmabuf == rx_half->dmabuf' \
	'sdev->zc_tx_import = tx_half;' \
	'sdev->zc_rx_import = rx_half;'
must_order "$import_body" 'import failure releases in reverse order' \
	'err_release_rx:' \
	'tbstream_dev_zc_import_half_release(rx_half, DMA_FROM_DEVICE);' \
	'err_release_tx:' \
	'tbstream_dev_zc_import_half_release(tx_half, DMA_TO_DEVICE);'
pass 'pool import is gated, exclusive, pre-activation, and transactional'

release_body=$(body_between '^static int tbstream_dev_fops_release' \
	'^static const struct file_operations tbstream_dev_fops')
must_order "$release_body" 'release ordering with peer-close suppression' \
	'if (--sdev->users == 0)' \
	'if (sdev->started)' \
	'if (!sdev->closed)' \
	'ret = tbstream_dev_send_close(sdev);' \
	'tbstream_dev_stop(sdev);' \
	'sdev->started = false;' \
	'tbstream_dev_zc_import_release(sdev);'
pass 'release skips CLOSE toward a closed peer and frees imports last'

stop_body=$(body_between '^static void tbstream_dev_stop' \
	'^static int tbstream_dev_activate_locked')
must_contain "$stop_body" 'pr_warn_ratelimited(' \
	'flush timeout warning is ratelimited'
pass 'TX flush timeout warning cannot flood the log'

send_close_body=$(body_between '^static int tbstream_dev_send_close' \
	'^static int tbstream_dev_start')
must_order "$send_close_body" 'imported-TX close frame' \
	'if (sdev->zc_tx_import) {' \
	'sf = &sdev->zc_close_frame;' \
	'if (!sdev->zc_close_frame_mapped)' \
	'sf->frame.eof = TBSTREAM_CLOSE;' \
	'tb_ring_tx(sdev->tx_ring.ring, &sf->frame);'
mmap_body=$(body_between '^static int tbstream_dev_fops_mmap' \
	'^static const struct file_operations')
must_contain "$mmap_body" 'if (!sdev->tx_ring.frames[i].page)' \
	'imported halves mmap as holes'
must_contain "$mmap_body" 'if (!sdev->rx_ring.frames[i].page)' \
	'imported halves mmap as holes'
pass 'imported-TX CLOSE uses the dedicated frame and imports mmap as holes'

printf '1..%d\n' "$PASS_COUNT"
