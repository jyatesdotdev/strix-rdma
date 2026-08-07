#!/usr/bin/env bash
# Configure a USB4STREAM stream via ConfigFS and print its /dev/tbstreamX path.
#
# Usage: tbstream-setup.sh [stream-name]
#   SERVICE=0-1.0        xdomain service id; autodetected if unset
#   RING_SIZE=4096       TX/RX ring size (32-4096, default kernel value 256)
#   THROTTLING=          interrupt throttling in ns (kernel default 8192;
#                        lower = better latency); empty = leave default
#
# Run on BOTH hosts. The stream name must match on both sides.
set -euo pipefail

NAME=${1:-ds4}
BASE=/sys/kernel/config/thunderbolt/stream

modprobe thunderbolt_stream

if ! [ -d "$BASE" ]; then
    echo "error: $BASE does not exist (is configfs mounted? kernel has USB4_STREAM?)" >&2
    exit 1
fi

if [ -z "${SERVICE:-}" ]; then
    # A remote host running thunderbolt_stream advertises a "stream" service.
    for d in /sys/bus/thunderbolt/devices/*/key; do
        [ -e "$d" ] || continue
        if [ "$(cat "$d")" = "stream" ]; then
            SERVICE=$(basename "$(dirname "$d")")
            break
        fi
    done
fi
if [ -z "${SERVICE:-}" ]; then
    echo "error: no stream service found; is the cable connected and the peer's" >&2
    echo "thunderbolt_stream loaded? Or pass SERVICE=<xdomain>.<service> explicitly" >&2
    echo "(see 'tblist -A' from https://github.com/intel/tbtools)." >&2
    exit 1
fi

DIR="$BASE/$SERVICE/$NAME"
mkdir -p "$DIR"

# Ring/throttling must be set before the HopIDs activate the stream.
[ -n "${RING_SIZE:-}" ]  && echo "$RING_SIZE"  > "$DIR/ring_size"
[ -n "${THROTTLING:-}" ] && echo "$THROTTLING" > "$DIR/throttling"

# -1 = automatic HopID allocation
echo -1 > "$DIR/in_hopid"
echo -1 > "$DIR/out_hopid"

IDX=$(cat "$DIR/index")
DEVICE="/dev/tbstream$IDX"
echo "stream '$NAME' on service $SERVICE: $DEVICE" >&2
echo "  in_hopid=$(cat "$DIR/in_hopid") out_hopid=$(cat "$DIR/out_hopid")" >&2
echo "  ring_size=$(cat "$DIR/ring_size") throttling=$(cat "$DIR/throttling")" >&2
if [ -e "$DEVICE" ] && [ "$(stat -c '%a:%U:%G' "$DEVICE")" = "600:root:root" ]; then
    echo "  note: $DEVICE is root-only; install tools/udev/99-tbstream.rules" >&2
    echo "        and use the dedicated tbstream group instead of running DS4 as root" >&2
fi
echo "$DEVICE"
