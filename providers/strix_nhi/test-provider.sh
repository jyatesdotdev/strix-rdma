#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
set -eu
[ "$#" = 2 ] || { echo "usage: $0 rdma-core-build new-test-output" >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
build=$(realpath "$1")
[ ! -e "$2" ] || { echo 'test output exists' >&2; exit 1; }
mkdir -- "$2"
out=$(CDPATH= cd -- "$2" && pwd)
mkdir -p "$out/sys/bus/thunderbolt/drivers/strix_nhi" "$out/sys/ib/device"
ln -s "$out/sys/bus/thunderbolt/drivers/strix_nhi" "$out/sys/ib/device/driver"
# Actual provider linked to actual rdma-core, with only specified command mocks.
# CFLAGS is intentionally word-split for optional ASan/UBSan flags.
# shellcheck disable=SC2086
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -g ${CFLAGS:-} \
    -isystem "$build/include" "$here/test-provider.c" -L"$build/lib" \
    -Wl,-rpath,"$build/lib" -libverbs -lpthread \
    -Wl,--wrap=ibv_cmd_post_send -Wl,--wrap=ibv_cmd_post_recv \
    -Wl,--wrap=ibv_cmd_poll_cq -Wl,--wrap=ibv_cmd_dereg_mr \
    -Wl,--wrap=ibv_cmd_destroy_qp -o "$out/test-provider"
"$out/test-provider" "$out/sys/ib"
# Same identity, wrong real parent driver must fail (test portable helper alone).
${CC:-cc} -std=gnu11 -Wall -Wextra -Werror -I"$here" \
    "$here/test-identity.c" -o "$out/test-identity"
"$out/test-identity" "$out/sys/ib"
${CC:-cc} -std=c11 -Wall -Wextra -Werror "$here/test-loader.c" -ldl -o "$out/test-loader"
"$out/test-loader" "$build/lib/libstrix_nhi-rdmav59.so"
grep -Fx "driver $build/lib/libstrix_nhi" "$build/etc/libibverbs.d/strix_nhi.driver"
