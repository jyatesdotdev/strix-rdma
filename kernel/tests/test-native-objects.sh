#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Execute exact production object function bodies with modeled kernel adapters.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strix-native-objects.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
# Function extraction fails closed if a named definition disappears. Kernel
# compilation checks the real structures/APIs; this test checks object behavior.
extract() {
    name=$1 source=$2
    awk -v name="$name" '
        !body && $0 ~ ("^[a-zA-Z_].*[ *]" name "\\(") { body=1 }
        body { print; if ($0 == "}") { found=1; exit } }
        END { if (!found) exit 1 }
    ' "$source"
}
: > "$tmp/native-objects.inc"
for fn in sn_mr_get sn_mr_put sn_dereg_mr; do
    extract "$fn" "$here/../verbs/mr.c" >> "$tmp/native-objects.inc"
done
for fn in sn_event sn_wc_qp sn_complete sn_flush_qp sn_qp_error sn_cq_remove_qp sn_poll_cq sn_notify_cq sn_post_send sn_post_recv; do
    extract "$fn" "$here/../verbs/verbs.c" >> "$tmp/native-objects.inc"
done
# shellcheck disable=SC2086
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-sign-compare ${CFLAGS:-} \
    -I"$tmp" "$here/test-native-objects.c" -o "$tmp/objects"
"$tmp/objects"
