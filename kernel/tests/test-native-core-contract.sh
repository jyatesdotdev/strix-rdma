#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Source-contract checks, not hardware/lifetime validation. Explicit source only.
set -eu
# Negative checks use explicit if/exit: POSIX set -e ignores a `! cmd` status.
[ "$#" -eq 1 ] || { echo "usage: sh $0 /explicit/prepared/linux" >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$(CDPATH= cd -- "$1" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strix-native-core.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
main=$here/../verbs/main.c
ring=$here/../verbs/ring.c
# Early platform rejection must precede even the netdev lookup and allocations.
awk '
 /static int sn_probe\(/ { probe=1 }
 probe && /if \(!sn_supported_nhi\(/ { guard=NR }
 probe && /netdev = sn_find_netdev/ { if (!guard || guard >= NR) exit 1; passed=1; exit }
 END { if (!passed) exit 1 }
' "$main"
grep -F 'tb_xdomain_try_disable_native_paths(xd)' "$ring" >/dev/null
grep -F 'tb_xdomain_try_enable_native_paths(xd, d->out_hop, d->tx_ring->hop,' "$ring" >/dev/null
if grep -E 'tb_xdomain_(enable_paths|disable_paths|disable_all_paths)|tb_unregister_protocol_handler' "$ring" "$main" >/dev/null; then
    echo 'native driver must not use generic XDomain path or protocol APIs' >&2; exit 1
fi
core=$src/drivers/thunderbolt/tb.c
sed -n '/^int tb_xdomain_try_enable_native_paths(/,/^EXPORT_SYMBOL_GPL(tb_xdomain_try_enable_native_paths)/p' "$core" > "$tmp/enable"
sed -n '/^int tb_xdomain_try_disable_native_paths(/,/^EXPORT_SYMBOL_GPL(tb_xdomain_try_disable_native_paths)/p' "$core" > "$tmp/disable"
sed -n '/^int tb_service_try_native_properties_changed(/,/^EXPORT_SYMBOL_GPL(tb_service_try_native_properties_changed)/p' "$core" > "$tmp/notify"
if grep -F 'tb_service_properties_changed(svc);' "$main" >/dev/null; then
    echo 'native driver must use the nonblocking properties notification' >&2; exit 1
fi
for method in enable disable notify; do
    grep -F 'mutex_trylock(&tb->lock)' "$tmp/$method" >/dev/null
    if grep -F 'mutex_lock(&tb->lock)' "$tmp/$method" >/dev/null; then
        echo "core try-$method must not wait for tb->lock" >&2; exit 1
    fi
done
grep -F '__tb_disconnect_xdomain_paths(xd->tb, xd, xd->native_tx_path,' "$core" >/dev/null
grep -F 'smp_store_release(&xd->native_dma_removed, true)' "$core" >/dev/null
grep -F 'smp_load_acquire(&xd->native_dma_removed)' "$tmp/disable" >/dev/null
awk '
 /^void tb_xdomain_remove\(/ { removal=1 }
 removal && /tb_xdomain_remove_native_paths\(xd\)/ { quiesced=1 }
 removal && /stop_handshake\(xd\)/ { if (!quiesced) exit 1; passed=1; exit }
 END { if (!passed) exit 1 }
' "$src/drivers/thunderbolt/xdomain.c"
awk '
 /^static void tb_stop\(/ { stopping=1 }
 stopping && /tb_stop_native_paths\(tb->root_switch\)/ { quiesced=1 }
 stopping && /list_for_each_entry_safe/ { if (!quiesced) exit 1; passed=1; exit }
 END { if (!passed) exit 1 }
' "$core"
grep -F 'if (ret && ret != -ENODEV) return ret;' "$ring" >/dev/null
grep -F 'd->reset_rings = true;' "$main" >/dev/null
grep -F 'if (d->reset_rings)' "$main" >/dev/null
grep -F 'd->dead || d->reset_rings || !sn_ready' "$ring" >/dev/null
# Freeze the audited CM decision shape and evaluate its actual case values.
# The core pre-fills uverbs_cmd_mask with the legacy write commands libibverbs
# uses for PD/MR/QP/query; the driver must extend it, never replace it.
sed -n '/^struct ib_device \*_ib_alloc_device(/,/^EXPORT_SYMBOL(_ib_alloc_device)/p' \
    "$src/drivers/infiniband/core/device.c" > "$tmp/alloc-device"
for command in ALLOC_PD REG_MR MODIFY_QP QUERY_QP QUERY_DEVICE GET_CONTEXT; do
    grep -F "BIT_ULL(IB_USER_VERBS_CMD_$command)" "$tmp/alloc-device" >/dev/null
done
sed -n '/struct tb \*icm_probe(/,$p' "$src/drivers/thunderbolt/icm.c" > "$tmp/icm-probe"
grep -F 'switch (nhi->pdev->device)' "$tmp/icm-probe" >/dev/null
grep -F 'if (!icm->is_supported || !icm->is_supported(tb))' "$tmp/icm-probe" >/dev/null
sed -n '/static struct tb \*nhi_select_cm(/,/static int nhi_probe(/p' "$src/drivers/thunderbolt/nhi.c" > "$tmp/cm"
grep -F 'tb = icm_probe(nhi);' "$tmp/cm" >/dev/null
grep -F 'tb = tb_probe(nhi);' "$tmp/cm" >/dev/null
{
    printf '#include <assert.h>\n'
    grep '^#define PCI_DEVICE_ID_' "$src/include/linux/pci_ids.h" "$src/drivers/thunderbolt/nhi.h" | sed 's/^[^:]*://'
    printf 'static int firmware_case(unsigned int device) { switch (device) {\n'
    awk '/^[[:space:]]*case PCI_DEVICE_ID_/ { print $0 }' "$tmp/icm-probe"
    printf 'return 1; default: return 0; } }\n'
    printf 'int main(void) { assert(!firmware_case(0x158d)); assert(!firmware_case(0x158e)); return 0; }\n'
} > "$tmp/cm-cases.c"
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror "$tmp/cm-cases.c" -o "$tmp/cm-cases"
"$tmp/cm-cases"
printf 'ok - admitted Strix IDs cannot select pinned firmware-CM cases; early guard, nonwaiting core APIs, pre-removal exact cleanup and EAGAIN ownership retention present\n'
sed -n '/^int tb_xdomain_try_enable_native_paths(/,/^#endif/p' "$core" |
    sed '/^#endif/d' > "$tmp/native-core-impl.inc"
# Execute the real core function bodies against deterministic lock/path adapters.
# These are ownership-branch tests, not real kernel lockdep/race validation.
# shellcheck disable=SC2086
"${CC:-cc}" -std=c11 -Wall -Wextra -Werror ${CFLAGS:-} -I"$tmp" \
    "$here/test-native-core.c" -o "$tmp/core-test"
"$tmp/core-test"
