#!/usr/bin/env bash
# Arm a record of this host's next shutdown or reboot. netconsole sends every
# kernel message as UDP to a listener on another machine
# (tools/scripts/netconsole-listen.py): systemd-shutdown's and dracut's steps,
# and with initcall_debug one "<device>: shutdown" line per device as
# device_shutdown() reaches it. journald stops before those steps, so without
# this a reboot that hangs there leaves nothing behind (docs/ZEROCOPY_VERBS.md,
# round seventeen). Everything set here is runtime-only and gone after the
# reboot. The record ends when the NIC itself shuts down, which on the lab hosts
# is after the Thunderbolt devices.
#
# Usage (root, on the host to watch):
#   arm-shutdown-capture.sh IFACE REMOTE_IP [REMOTE_MAC] [REMOTE_PORT]
# REMOTE_MAC is the next hop's: the listener's own on the same LAN, otherwise
# the gateway's. Default: resolved from the routing and neighbour tables.
# REMOTE_PORT defaults to 6666.
set -eu
IFACE=${1:?usage: arm-shutdown-capture.sh IFACE REMOTE_IP [REMOTE_MAC] [REMOTE_PORT]}
REMOTE_IP=${2:?usage: arm-shutdown-capture.sh IFACE REMOTE_IP [REMOTE_MAC] [REMOTE_PORT]}
REMOTE_MAC=${3:-}
REMOTE_PORT=${4:-6666}
TARGET=/sys/kernel/config/netconsole/shutdown-capture

[ "$(id -u)" = 0 ] || { echo "arm-shutdown-capture: must run as root" >&2; exit 1; }

if [ -z "$REMOTE_MAC" ]; then
    hop=$(ip -4 route get "$REMOTE_IP" | sed -n 's/.* via \([0-9.]*\) .*/\1/p')
    hop=${hop:-$REMOTE_IP}
    ping -c 1 -W 1 "$hop" >/dev/null 2>&1 || true
    REMOTE_MAC=$(ip neigh show "$hop" dev "$IFACE" |
        awk '{ for (i = 1; i < NF; i++) if ($i == "lladdr") print $(i + 1) }')
    [ -n "$REMOTE_MAC" ] || {
        echo "arm-shutdown-capture: no MAC for next hop $hop; pass REMOTE_MAC" >&2
        exit 1
    }
fi

modprobe netconsole
[ -d "$TARGET" ] || mkdir "$TARGET"
# Attributes are read-only while enabled, and disabling a disabled target fails.
if [ "$(cat "$TARGET/enabled")" = 1 ]; then echo 0 > "$TARGET/enabled"; fi
echo "$IFACE" > "$TARGET/dev_name"
echo "$REMOTE_IP" > "$TARGET/remote_ip"
echo "$REMOTE_PORT" > "$TARGET/remote_port"
echo "$REMOTE_MAC" > "$TARGET/remote_mac"
echo 1 > "$TARGET/enabled"
echo 1 > /sys/module/kernel/parameters/initcall_debug
dmesg -n 8
echo "<6>shutdown-capture: armed on $(hostname) to $REMOTE_IP:$REMOTE_PORT" > /dev/kmsg
echo "armed: $IFACE -> $REMOTE_IP:$REMOTE_PORT via $REMOTE_MAC"
