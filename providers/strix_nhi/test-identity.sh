#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Portable no-device test of the exact provider identity predicate.
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strix-provider-identity.XXXXXX")
trap 'rm -rf -- "$tmp"' EXIT HUP INT TERM
mkdir -p "$tmp/sys/bus/thunderbolt/drivers/strix_nhi" "$tmp/sys/ib/device"
ln -s "$tmp/sys/bus/thunderbolt/drivers/strix_nhi" "$tmp/sys/ib/device/driver"
# shellcheck disable=SC2086
"${CC:-cc}" -std=gnu11 -Wall -Wextra -Werror ${CFLAGS:-} \
    "$here/test-identity.c" -o "$tmp/test-identity"
"$tmp/test-identity" "$tmp/sys/ib"
