#!/usr/bin/env bash
# Build the backported thunderbolt module set on a Fedora 7.1.5 host.
# Clones stable v7.1.5, applies kernel/backport/v7.1 and kernel/zerocopy, builds the
# thunderbolt + thunderbolt-net + thunderbolt_stream modules with a
# version string matching the running kernel exactly.
set -euo pipefail

KREL=$(uname -r)          # 7.1.5-101.fc43.x86_64
BASE=${KREL%%-*}          # 7.1.5
LOCALVER=-${KREL#*-}      # -101.fc43.x86_64
SRC=${SRC:-$HOME/src/linux-stable}
PATCHES=$(cd "$(dirname "$0")/../backport/v7.1" && pwd)
ZC_PATCHES=$(cd "$(dirname "$0")/../zerocopy" && pwd)

if ! git -C "$SRC" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    if [ -e "$SRC" ] &&
       [ -n "$(find "$SRC" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]; then
        echo "error: SRC exists but is not a Git worktree: $SRC" >&2
        exit 1
    fi
    mkdir -p "$(dirname "$SRC")"
    git clone --depth 1 --branch "v$BASE" \
        https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git "$SRC"
fi
cd "$SRC"

if ! git diff-index --quiet HEAD --; then
    echo "error: kernel source tree has uncommitted changes" >&2
    exit 1
fi

apply_series() {
    for patch in "$@"; do
        # RFC 2822 Subject headers may be folded across continuation lines.
        subject=$(awk '
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
        ' "$patch" | sed -E 's/^\[PATCH[^]]*\] //')
        if [ -z "$subject" ]; then
            echo "error: no Subject found in $patch" >&2
            exit 1
        fi
        # Do not use grep -q under pipefail: an early match can SIGPIPE
        # git-log and make an applied patch look absent.
        if git log -64 --format=%s | grep -Fx "$subject" >/dev/null; then
            echo "== already applied: $subject"
            continue
        fi
        echo "== applying: $subject"
        git -c user.name=builder -c user.email=builder@localhost am "$patch"
    done
}

apply_series "$PATCHES"/*.patch
apply_series "$ZC_PATCHES"/*.patch

cp "/boot/config-$KREL" .config
scripts/config --module CONFIG_USB4_STREAM
scripts/config --disable CONFIG_LOCALVERSION_AUTO
# unsigned is fine where Secure Boot is off; max2 signs explicitly (host-sign.sh)
scripts/config --disable CONFIG_MODULE_SIG_ALL
make olddefconfig LOCALVERSION="$LOCALVER"

REL=$(make -s kernelrelease LOCALVERSION="$LOCALVER")
if [ "$REL" != "$KREL" ]; then
    echo "error: kernelrelease '$REL' != running '$KREL'" >&2
    exit 1
fi

make -j"$(nproc)" modules_prepare LOCALVERSION="$LOCALVER"
[ -e "/usr/src/kernels/$KREL/Module.symvers" ] && cp "/usr/src/kernels/$KREL/Module.symvers" .

make -j"$(nproc)" M=drivers/thunderbolt modules LOCALVERSION="$LOCALVER"
# thunderbolt-net calls tb_ring_throttling(), exported by the module set we
# just built — MODPOST needs that Module.symvers, it is not in the stock one
make -j"$(nproc)" M=drivers/net/thunderbolt modules LOCALVERSION="$LOCALVER" \
    KBUILD_EXTRA_SYMBOLS="$SRC/drivers/thunderbolt/Module.symvers"

echo "== built modules:"
ls -l drivers/thunderbolt/thunderbolt.ko drivers/thunderbolt/thunderbolt_stream.ko \
      drivers/net/thunderbolt/thunderbolt_net.ko
echo "== vermagic: $(modinfo -F vermagic drivers/thunderbolt/thunderbolt.ko)"
