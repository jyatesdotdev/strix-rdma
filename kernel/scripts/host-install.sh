#!/usr/bin/env bash
# Install the built module set into /lib/modules/$(uname -r)/updates/ and
# reload the thunderbolt stack. Run after host-build.sh (and host-sign.sh
# on Secure Boot hosts).
set -euo pipefail

KREL=$(uname -r)
LOCALVER=-${KREL#*-}
SRC=${SRC:-$HOME/src/linux-stable}
cd "$SRC"

sudo make M=drivers/thunderbolt modules_install INSTALL_MOD_DIR=updates LOCALVERSION="$LOCALVER"
sudo make M=drivers/net/thunderbolt modules_install INSTALL_MOD_DIR=updates LOCALVERSION="$LOCALVER"
sudo depmod -a

for m in thunderbolt thunderbolt_stream thunderbolt-net; do
    path=$(modinfo -n "$m")
    case "$path" in
        */updates/*) echo "ok: $m -> $path" ;;
        *) echo "error: $m resolves to $path (not updates/)" >&2; exit 1 ;;
    esac
done

if [ "${RELOAD:-1}" = 1 ]; then
    echo "== reloading thunderbolt stack (drops thunderbolt0 link briefly)"
    # configfs stream groups pin thunderbolt_stream; remove them first
    sudo find /sys/kernel/config/thunderbolt/stream -depth -mindepth 1 \
        -type d -exec rmdir {} \; 2>/dev/null || true
    sudo modprobe -r thunderbolt_net 2>/dev/null || true
    sudo modprobe -r thunderbolt_stream 2>/dev/null || true
    sudo modprobe -r thunderbolt_dma_test 2>/dev/null || true
    # typec imports thunderbolt symbols; it has to come out first
    sudo modprobe -r typec_thunderbolt 2>/dev/null || true
    sudo modprobe -r ucsi_acpi 2>/dev/null || true
    sudo modprobe -r typec_ucsi 2>/dev/null || true
    sudo modprobe -r typec 2>/dev/null || true
    sudo modprobe -r thunderbolt
    sudo modprobe thunderbolt
    sudo modprobe typec_thunderbolt 2>/dev/null || true
    sudo modprobe ucsi_acpi 2>/dev/null || true
    sudo modprobe thunderbolt-net
    sudo modprobe thunderbolt_stream
    sudo dmesg | tail -3
fi
