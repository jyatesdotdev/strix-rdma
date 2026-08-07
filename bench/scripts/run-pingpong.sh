#!/usr/bin/env bash
# Sweep pingpong latency over DS4-relevant message sizes and save CSV.
#
# Usage: run-pingpong.sh <device> [label]
#   PEER=user@otherhost  if set, starts the echo side over ssh per size
#   PEER_DEV=...         device path on the peer (default: same as local)
#   PEER_BIN=...         pingpong path on the peer (default: pingpong in PATH)
#   ITERS=1000 WARMUP=50 SIZES="4k 16k 64k 256k 1m 4m"
#
# Without PEER, start the echo side manually on the other host before each
# size:  ./pingpong pong -d /dev/tbstreamX -s <size>
set -euo pipefail

DEV=${1:?usage: run-pingpong.sh <device> [label]}
LABEL=${2:-tbstream}
ITERS=${ITERS:-1000}
WARMUP=${WARMUP:-50}
SIZES=${SIZES:-"4k 16k 64k 256k 1m 4m"}

HERE=$(cd "$(dirname "$0")" && pwd)
BIN="$HERE/../../tools/pingpong/pingpong"
OUT="$HERE/../results/pingpong-$LABEL-$(date +%Y%m%d-%H%M%S).csv"

echo "mode,size,iters,min_us,p50_us,p90_us,p95_us,p99_us,max_us,mean_us,mbps" > "$OUT"

for SIZE in $SIZES; do
    if [ -n "${PEER:-}" ]; then
        ssh "$PEER" "${PEER_BIN:-pingpong} pong -d ${PEER_DEV:-$DEV} -s $SIZE" &
        SSH_PID=$!
        sleep 1
    else
        read -r -p "start peer: pingpong pong -d $DEV -s $SIZE   then press enter"
    fi

    echo "== $SIZE ==" >&2
    "$BIN" ping -d "$DEV" -s "$SIZE" -n "$ITERS" -w "$WARMUP" -c | tee -a "$OUT"

    if [ -n "${PEER:-}" ]; then
        wait "$SSH_PID" 2>/dev/null || true
    fi
done

echo "results: $OUT" >&2
