#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Linux build only; no downloads, installation, module actions or device access.
set -eu
[ "$#" = 2 ] || { echo "usage: $0 pinned-rdma-core.tar.gz new-output-dir" >&2; exit 2; }
[ "$(uname -s)" = Linux ] || { echo 'Linux development environment required' >&2; exit 1; }
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
archive=$(realpath "$1")
expected=8df90e5882d8e7c7fc95e7cb6bb00ea231aed9eeb9b9ae99c855aab3724344a4
actual=$(sha256sum "$archive" | cut -d ' ' -f 1)
[ "$actual" = "$expected" ] || { echo 'wrong rdma-core archive SHA256' >&2; exit 1; }
[ ! -e "$2" ] || { echo 'output must not exist' >&2; exit 1; }
mkdir -- "$2"
out=$(CDPATH= cd -- "$2" && pwd)
mkdir "$out/source"
tar -xzf "$archive" --strip-components=1 -C "$out/source"
patch -d "$out/source" -p1 < "$here/rdma-core-v62.patch"
mkdir "$out/source/providers/strix_nhi"
cp "$here/strix_nhi.c" "$here/identity.h" "$here/CMakeLists.txt" "$out/source/providers/strix_nhi/"
cmake -S "$out/source" -B "$out/build" -DIN_PLACE=1 -DNO_MAN_PAGES=1 \
    -DNO_PYVERBS=1 -DENABLE_STATIC=0 -DCMAKE_BUILD_TYPE=Debug
cmake --build "$out/build" --parallel "${JOBS:-2}" --target strix_nhi-rdmav59 ibv_devices ibv_devinfo
cc=${CC:-cc}
"$cc" -std=gnu11 -Wall -Wextra -Werror -g -I"$out/build/include" \
    "$here/../../tools/verbs/strix-verbs-smoke.c" -L"$out/build/lib" \
    -Wl,-rpath,"$out/build/lib" -libverbs -o "$out/strix-verbs-smoke"
sh "$here/test-provider.sh" "$out/build" "$out/tests"
"$out/strix-verbs-smoke" --selftest
printf 'Build and offline tests complete. No live devices were opened. Output: %s\n' "$out"
