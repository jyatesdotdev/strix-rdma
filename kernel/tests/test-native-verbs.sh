#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strix-native-tests.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
cc=${CC:-cc}
# Optional sanitizer flags are supplied by the caller, not assumed portable.
for test in wire order engine; do
    # CFLAGS intentionally permits normal compiler flag word splitting.
    # shellcheck disable=SC2086
    "$cc" -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} \
        "$here/test-native-$test.c" -o "$tmp/test-$test"
    "$tmp/test-$test"
done
CC="$cc" sh "$here/test-native-objects.sh"
sh "$here/../verbs/generate-patch.sh" > "$tmp/native.patch"
cmp "$tmp/native.patch" "$here/../verbs/0002-RDMA-add-opt-in-native-NHI-software-verbs.patch"
printf 'ok - generated kernel patch exactly matches canonical sources\n'
# The core pre-fills the legacy write commands (ALLOC_PD, REG_MR, MODIFY_QP,
# QUERY_DEVICE, ...); plain assignment would disable them for libibverbs.
grep -F 'd->ib.uverbs_cmd_mask |= BIT_ULL(IB_USER_VERBS_CMD_POST_SEND)' "$here/../verbs/main.c" >/dev/null
if grep -E 'uverbs_cmd_mask[[:space:]]*=' "$here/../verbs/main.c" "$tmp/native.patch" >/dev/null; then
    echo 'uverbs_cmd_mask must be extended with |=, never assigned' >&2; exit 1
fi
printf 'ok - uverbs_cmd_mask extends the core default write commands\n'
# Source-contract checks only (no kernel runtime): netdev and worker lifetime.
main=$here/../verbs/main.c
awk '
 /^static int sn_net_event\(/ { f=1 }
 f && /event == NETDEV_UNREGISTER\) ib_device_set_netdev\(&d->ib, NULL, 1\);/ { core=1 }
 f && /d->netdev = NULL;/ { own=1 }
 f && /event == NETDEV_UNREGISTER\) dev_put\(netdev\);/ { if (!core || !own) exit 1; passed=1; exit }
 END { if (!passed) exit 1 }
' "$main"
awk '
 /^static int sn_probe\(/ { f=1 }
 f && /ib_device_set_netdev\(&d->ib, netdev, 1\)/ { set=1 }
 f && /register_netdevice_notifier\(/ { if (!set) exit 1; passed=1; exit }
 END { if (!passed) exit 1 }
' "$main"
awk '
 /^static void sn_remove\(/ { f=1 }
 f && /ib_unregister_device\(/ { unreg=1 }
 f && unreg && /cancel_delayed_work_sync\(&d->work\)/ { joined=1 }
 f && /ib_dealloc_device\(/ { if (!joined) exit 1; passed=1; exit }
 END { if (!passed) exit 1 }
' "$main"
printf 'ok - netdev released on NETDEV_UNREGISTER; worker joined after unregister (source contract)\n'
# The dynamic session handshake must be wire-carried: no DATA property blob,
# handshake frames gated on local paths only, one-shot static rendezvous.
if grep -F 'tb_property_add_data(dir, "session"' "$main" "$tmp/native.patch" >/dev/null; then
    echo 'session DATA property must not return; handshake is wire-carried' >&2; exit 1
fi
awk '
 /^int sn_ring_send\(/ { f=1 }
 f && /bool hs = h->opcode >= SN_HELLO && h->opcode <= SN_BIND_ACK;/ { hs=1 }
 f && /hs \? !sn_local_ready\(&d->ready\) : !sn_ready\(&d->ready\)/ { if (hs) { passed=1; exit } }
 END { if (!passed) exit 1 }
' "$here/../verbs/ring.c"
grep -F 'tb_property_add_immediate(dir, "rxhop", d->in_hop)' "$main" >/dev/null
printf 'ok - wire handshake: no DATA property, handshake gate, static rxhop rendezvous (source contract)\n'
