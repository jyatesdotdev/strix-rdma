#!/usr/bin/env bash
# TP-shaped latency sweep (docs/ZEROCOPY_VERBS.md, round nineteen): the
# exchange (both sides send N bytes at once, each waits for the peer's) and the
# ping round trip, at each message size and idle gap. Real tensor parallelism
# computes for ~100-300 us between exchanges, so the gapped rows are the ones
# that predict it; back-to-back rows only measure a hot path.
#
# Usage: verbs-latency.sh [iters]        (default 20000 per row)
# Environment: CONNECT, LISTEN, LISTEN_IP (and PORT, DEV, TOOL_DIR) as for
# run-verbs-smoke.sh, plus
#   SIZES="256 10240 40960"   message sizes in bytes
#   GAPS="0 200 1000"         idle gaps between iterations, in us
#   MODES="exchange ping"
#   SMOKE_PREFIX=             e.g. "taskset -c 6" with the low-latency profile
# One line per row: mode, size, gap, then min/avg/p50/p90/p99/p99.9/max in us.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ITERS=${1:-20000}
SIZES=${SIZES:-256 10240 40960}
GAPS=${GAPS:-0 200 1000}
MODES=${MODES:-exchange ping}
status=0
for mode in $MODES; do
    for size in $SIZES; do
        for gap in $GAPS; do
            line=$(SMOKE_FLAGS="--len $size --gap-us $gap" \
                "$here/run-verbs-smoke.sh" "$mode" "$ITERS" 2>&1 |
                grep -E '^LATENCY .*\(connect|^FAIL' | head -1)
            case "$line" in
            LATENCY*) echo "$mode $size B gap ${gap} us: ${line#*excluded): }" ;;
            *) echo "$mode $size B gap ${gap} us: FAILED ${line:-no result}"; status=1 ;;
            esac
        done
    done
done
exit "$status"
