#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Build only; never install/load modules, fetch dependencies, or patch a tree.
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: sh $0 /prepared/linux-7.1.5 /input/config /new/output-dir" >&2
    exit 2
fi
[ "$(uname -s)" = Linux ] || { echo 'a Linux build environment is required' >&2; exit 2; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
src=$(CDPATH= cd -- "$1" && pwd)
config=$2
out=$3
case "$out" in /*) ;; *) echo 'output path must be absolute' >&2; exit 2 ;; esac
[ ! -e "$out" ] || { echo 'output directory must not already exist' >&2; exit 2; }
version=$(awk '/^VERSION =/ {v=$3} /^PATCHLEVEL =/ {p=$3} /^SUBLEVEL =/ {s=$3} END {print v "." p "." s}' "$src/Makefile")
[ "$version" = 7.1.5 ] || { echo "unaudited target version: $version" >&2; exit 2; }
for file in Kconfig Makefile native_wire.h native_order.h native_platform.h strix_nhi.h main.c ring.c verbs.c mr.c protocol.c; do
    cmp "$here/$file" "$src/drivers/infiniband/hw/strix_nhi/$file"
done
sh "$here/../tests/test-native-core-contract.sh" "$src"
mkdir "$out"
cp "$config" "$out/.config"
"$src/scripts/config" --file "$out/.config" --module INFINIBAND_STRIX_NHI
make -C "$src" O="$out" olddefconfig
grep -qx 'CONFIG_INFINIBAND_STRIX_NHI=m' "$out/.config" || {
    echo 'input config lacks native-verbs prerequisites (see Kconfig)' >&2; exit 2;
}
{
    uname -sm
    "${CC:-cc}" --version
    sha256sum "$here"/*.[ch] "$out/.config"
} > "$out/build-inputs.txt"
make -C "$src" O="$out" W=1 -j"${JOBS:-2}" vmlinux modules > "$out/build.log" 2>&1
[ -s "$out/drivers/infiniband/hw/strix_nhi/strix_nhi.ko" ]
printf 'built (not loaded): %s\n' "$out/drivers/infiniband/hw/strix_nhi/strix_nhi.ko"
