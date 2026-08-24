#!/usr/bin/env bash
# Transient patch-14 thunderbolt_stream swap for test windows.
# Usage: sudo p14-swap.sh on [/path/to/module.ko] | sudo p14-swap.sh off
#
#   on : stop reconcile timer, remove managed ConfigFS state, replace the
#        loaded thunderbolt_stream with the staged patch-14 module
#        (zc_diagnostic_dmabuf=1), recreate the production endpoint.
#   off: reverse it — restore the installed production module and timer.
#
# Never touches /lib/modules or the initramfs. Refuses to proceed while
# any process holds the stream device.
set -euo pipefail

MODE=${1:?usage: p14-swap.sh on|off [module.ko]}
KO=${2:-/tmp/thunderbolt_stream-p14.ko}
SCRIPT_DIR=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
SYSCONFIG=${TBSTREAM_SYSCONFIG:-/etc/sysconfig/ds4-tbstream}

[ "$(id -u)" -eq 0 ] || { echo "error: run as root" >&2; exit 1; }

if [ -e /dev/tbstream0 ] && fuser -s /dev/tbstream0 2>/dev/null; then
    echo "error: /dev/tbstream0 has holders; quiesce them first" >&2
    fuser -v /dev/tbstream0 >&2 || true
    exit 1
fi

reconcile_once() {
    set -a
    # shellcheck disable=SC1090
    . "$SYSCONFIG"
    set +a
    "$SCRIPT_DIR/ds4-tbstream-reconcile.sh" --once
}

case "$MODE" in
on)
    [ -f "$KO" ] || { echo "error: $KO missing" >&2; exit 1; }
    modinfo "$KO" | grep -q zc_diagnostic_dmabuf || {
        echo "error: $KO lacks zc_diagnostic_dmabuf (not patch >= 13?)" >&2
        exit 1
    }
    systemctl stop ds4-tbstream-reconcile.timer
    TBSTREAM_SYSCONFIG="$SYSCONFIG" "$SCRIPT_DIR/ds4-tbstream-cleanup.sh"
    rmmod thunderbolt_stream
    insmod "$KO" zc_diagnostic_dmabuf=1
    reconcile_once
    echo "p14-swap: patch module live, endpoint ready, timer stopped"
    ;;
off)
    systemctl stop ds4-tbstream-reconcile.timer 2>/dev/null || true
    TBSTREAM_SYSCONFIG="$SYSCONFIG" "$SCRIPT_DIR/ds4-tbstream-cleanup.sh"
    rmmod thunderbolt_stream
    modprobe thunderbolt_stream
    if [ -e /sys/module/thunderbolt_stream/parameters/zc_diagnostic_dmabuf ]; then
        echo "error: production module still exposes patch-14 param?" >&2
        exit 1
    fi
    reconcile_once
    systemctl start ds4-tbstream-reconcile.timer
    echo "p14-swap: production module restored, endpoint ready, timer running"
    ;;
*)
    echo "usage: p14-swap.sh on|off [module.ko]" >&2
    exit 1
    ;;
esac
