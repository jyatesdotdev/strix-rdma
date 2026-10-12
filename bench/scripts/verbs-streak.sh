#!/usr/bin/env bash
# Run strix-verbs-smoke repeatedly and stop at the first failure, keeping the
# per-run evidence the zero-copy validation rounds in docs/ZEROCOPY_VERBS.md
# were recorded from.
#
# Usage: verbs-streak.sh FIRST LAST [mode...]
#   mode: send (default: the SEND/RECV smoke) | send <iters> | write <iters> | read <iters>
#   e.g.  verbs-streak.sh 1 20 read 2000
#
# Environment: CONNECT, LISTEN, LISTEN_IP (required) and PORT, DEV, TOOL_DIR
# as for run-verbs-smoke.sh, plus
#   OUT_DIR=./verbs-streak   evidence directory (created)
#   QUIET_COUNTERS=regex     counters left out of the one-line summary
#
# Per run N, OUT_DIR gets run-N.log (connector output), run-N.listen (the
# listener's full log), run-N.delta (every native_stats counter that moved, per host) and
# run-N.snap (stall_snap plus live ring/QP state, per host). Stdout gets one
# line per run. A run passes only if BOTH sides print PASS with the exact
# expected byte count; exit status is 1 on the first failure.
set -u
here=$(cd "$(dirname "$0")" && pwd)
FIRST=${1:?usage: verbs-streak.sh FIRST LAST [mode...]}
LAST=${2:?usage: verbs-streak.sh FIRST LAST [mode...]}
shift 2
: "${CONNECT:?set CONNECT=user@host}" "${LISTEN:?set LISTEN=user@host}"
: "${LISTEN_IP:?set LISTEN_IP}"
DEV=${DEV:-strix_nhi0}
OUT_DIR=${OUT_DIR:-./verbs-streak}
LISTEN_LOG=${LISTEN_LOG:-/tmp/strix-verbs-smoke-listen.log}
QUIET_COUNTERS=${QUIET_COUNTERS:-'^(tx_frames|rx_frames|poll_.*|sched_calls|placed_.*|zc_bursts|zc_read_ops|zc_aborts)$'}
SYS=/sys/class/infiniband/$DEV
SSH=(ssh -o BatchMode=yes)

case "${1:-send}" in
send)
    if [ -n "${2:-}" ]; then
        MODE=(send "$2")
        bytes=$(($2 * 4 * 2097152))
        PASS="^PASS native SEND/RECV verified: $2 iters x 4 x 2097152 B, .* $bytes bytes placed and verified byte-for-byte"
    else
        MODE=()
        PASS='^PASS native CPU SEND/RECV: '
    fi
    ;;
write|read)
    iters=${2:?"$1 needs an iteration count"}
    MODE=("$1" "$iters")
    op=$(echo "$1" | tr '[:lower:]' '[:upper:]')
    # The smoke tool's RDMA modes move a window of 4 x 2 MiB per iteration.
    bytes=$((iters * 4 * 2097152))
    PASS="^PASS native RDMA $op: $iters iters x 4 x 2097152 B, .* $bytes bytes placed and verified byte-for-byte"
    ;;
*)
    echo "unknown mode: $1 (send [N] | write N | read N)" >&2
    exit 2
    ;;
esac

mkdir -p "$OUT_DIR"
counters() { # host -> "name value" lines for every numeric native_stats counter
    "${SSH[@]}" "$1" "sudo cat $SYS/native_stats" |
        awk 'NF == 2 && $2 ~ /^[0-9]+$/ && $1 !~ /^(zdata|data_|qp_)/' | sort
}
delta() { # before after -> "name=delta" for every counter that moved
    join "$1" "$2" | awk '$3 != $2 { printf "%s=%d ", $1, $3 - $2 }'
}
brief() { # deltas without the always-moving counters
    echo "$1" | tr ' ' '\n' | grep . | awk -F= -v q="$QUIET_COUNTERS" '$1 !~ q' | tr '\n' ' '
}

for n in $(seq "$FIRST" "$LAST"); do
    out=$OUT_DIR/run-$n
    counters "$CONNECT" >"$out.c0"; counters "$LISTEN" >"$out.l0"
    "$here/run-verbs-smoke.sh" "${MODE[@]+"${MODE[@]}"}" >"$out.log" 2>&1
    "${SSH[@]}" "$LISTEN" "cat $LISTEN_LOG" >"$out.listen" 2>&1
    counters "$CONNECT" >"$out.c1"; counters "$LISTEN" >"$out.l1"
    dc=$(delta "$out.c0" "$out.c1"); dl=$(delta "$out.l0" "$out.l1")
    rm -f "$out.c0" "$out.c1" "$out.l0" "$out.l1"
    printf 'connect %s\nlisten  %s\n' "$dc" "$dl" >"$out.delta"
    for h in "$CONNECT" "$LISTEN"; do
        echo "== $h"
        "${SSH[@]}" "$h" "sudo cat $SYS/stall_snap; sudo grep -E '^(zdata|data_|qp_)' $SYS/native_stats"
    done >"$out.snap" 2>&1
    errs=$(awk '/^== /{h=$2} /^err_count /{printf "%s err_count=%s ", h, $2}' "$out.snap")
    # The connector's verdict is in its own section of run-N.log; the listener's
    # is in run-N.listen. Both must pass.
    if sed -n '/^--- connector/,/^--- listener/p' "$out.log" | grep -qE "$PASS" &&
       grep -qE "$PASS" "$out.listen" &&
       ! grep -qE 'FAIL|MISMATCH' "$out.log" "$out.listen"; then
        mib=$(grep -oE '[0-9.]+ MiB/s' "$out.log" | head -1)
        echo "run $n PASS ${mib:-} | connect[$(brief "$dc")] listen[$(brief "$dl")] | $errs"
    else
        echo "run $n FAIL | connect[$(brief "$dc")] listen[$(brief "$dl")] | $errs"
        { grep -hE 'FAIL|MISMATCH' "$out.log" "$out.listen" ||
          tail -n 3 "$out.log" "$out.listen"; } | head -8 | sed 's/^/   /'
        exit 1
    fi
done
