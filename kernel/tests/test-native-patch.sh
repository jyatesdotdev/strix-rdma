#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Read only the supplied upstream integration files; never mutate KERNEL_SRC.
set -eu
if [ "$#" -ne 1 ]; then
    echo "usage: sh $0 /explicit/linux-source" >&2
    exit 2
fi
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$(CDPATH= cd -- "$1" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strix-native-patch.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/drivers/infiniband" "$tmp/drivers/thunderbolt" "$tmp/include/linux"
cp "$src/drivers/infiniband/Kconfig" "$src/drivers/infiniband/Makefile" "$tmp/drivers/infiniband/"
cp "$src/include/linux/thunderbolt.h" "$tmp/include/linux/"
for file in tb.c tb.h xdomain.c; do cp "$src/drivers/thunderbolt/$file" "$tmp/drivers/thunderbolt/"; done
for patch in "$here"/../verbs/*.patch; do
    (cd "$tmp" && git apply --check "$patch" && git apply "$patch")
done
for file in Kconfig Makefile native_wire.h native_order.h native_platform.h strix_nhi.h main.c ring.c verbs.c mr.c protocol.c; do
    cmp "$here/../verbs/$file" "$tmp/drivers/infiniband/hw/strix_nhi/$file"
done
printf 'ok - opt-in patch applies; all eleven installed files match canonical sources\n'
