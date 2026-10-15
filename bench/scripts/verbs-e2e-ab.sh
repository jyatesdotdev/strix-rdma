#!/usr/bin/env bash
# Control-ring E2E A/B (docs/ZEROCOPY_VERBS.md, rounds twelve and seventeen).
# Boots both hosts into one arm, reads back the parameter actually in effect
# (stopping on a mismatch), and runs one fixed verification set. ctl_e2e is
# read-only and strix_nhi must not be reloaded, so every arm costs a reboot of
# the pair, and boot-to-boot variance means several boots per arm.
#
# Usage: verbs-e2e-ab.sh A|B OUT_DIR
#   A: ctl_e2e=1, the default        B: ctl_e2e=0, set through modprobe.d
# Arm B's modprobe.d file stays until an arm A run removes it, so finish with
# arm A (or remove /etc/modprobe.d/strix-nhi-ctle2e-test.conf on both hosts).
#
# Environment: CONNECT, LISTEN, LISTEN_IP (and PORT, DEV, TOOL_DIR) as for
# verbs-streak.sh, plus
#   BRINGUP=PATH                bring-up script on the hosts
#                               (default ~/strix-rdma/tools/scripts/strix-verbs-bringup.sh)
#   NETCONSOLE="IFACE IP [MAC]" run tools/scripts/arm-shutdown-capture.sh on both
#                               hosts before the reboot (listener:
#                               tools/scripts/netconsole-listen.py)
#
# OUT_DIR gets arm.txt (the parameter read back on each host), summary.txt (one
# line per run, tagged with its mode, then each host's final counters) and
# runs/ (verbs-streak.sh evidence). Exit status is 1 if a host does not come
# back, the arm is not in effect, or any run fails.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ARM=${1:?usage: verbs-e2e-ab.sh A|B OUT_DIR}
OUT=${2:?usage: verbs-e2e-ab.sh A|B OUT_DIR}
: "${CONNECT:?set CONNECT=user@host}" "${LISTEN:?set LISTEN=user@host}"
: "${LISTEN_IP:?set LISTEN_IP}"
# Expanded by the remote shell, so ~ is the remote user's home.
BRINGUP=${BRINGUP:-'~/strix-rdma/tools/scripts/strix-verbs-bringup.sh'}
CONF=/etc/modprobe.d/strix-nhi-ctle2e-test.conf
SYS=/sys/class/infiniband/strix_nhi0
HOSTS=("$CONNECT" "$LISTEN")
SSH=(ssh -o BatchMode=yes)
case "$ARM" in
A) want=Y ;;
B) want=N ;;
*) echo "verbs-e2e-ab: arm must be A or B" >&2; exit 2 ;;
esac
mkdir -p "$OUT"
export OUT_DIR=$OUT/runs

for h in "${HOSTS[@]}"; do
    if [ "$ARM" = B ]; then
        "${SSH[@]}" "$h" "printf 'options strix_nhi ctl_e2e=0\n' | sudo tee $CONF >/dev/null"
    else
        "${SSH[@]}" "$h" "sudo rm -f $CONF"
    fi
    if [ -n "${NETCONSOLE:-}" ]; then
        "${SSH[@]}" "$h" "sudo bash -s -- $NETCONSOLE" < "$here/../../tools/scripts/arm-shutdown-capture.sh"
    fi
done

for h in "${HOSTS[@]}"; do "${SSH[@]}" "$h" 'sudo systemctl reboot' >/dev/null 2>&1 & done
wait
sleep 60
back=0
for _ in $(seq 1 36); do
    back=0
    for h in "${HOSTS[@]}"; do
        up=$("${SSH[@]}" -o ConnectTimeout=5 "$h" 'cut -d. -f1 /proc/uptime' 2>/dev/null)
        [ -n "$up" ] && [ "$up" -lt 900 ] && back=$((back + 1))
    done
    [ "$back" = 2 ] && break
    sleep 10
done
if [ "$back" != 2 ]; then
    echo "verbs-e2e-ab: a host did not come back within 7 minutes. Check its console" \
        "(and the netconsole record); do not retry a reload." >&2
    exit 1
fi

pids=()
for h in "${HOSTS[@]}"; do
    "${SSH[@]}" "$h" "sudo bash $BRINGUP >/tmp/bringup.log 2>&1" &
    pids+=($!)
done
for p in "${pids[@]}"; do
    wait "$p" || { echo "verbs-e2e-ab: bring-up failed (/tmp/bringup.log on the host)" >&2; exit 1; }
done

: > "$OUT/arm.txt"
for h in "${HOSTS[@]}"; do
    got=$("${SSH[@]}" "$h" 'cat /sys/module/strix_nhi/parameters/ctl_e2e')
    port=$("${SSH[@]}" "$h" "cat $SYS/ports/1/state")
    echo "arm=$ARM host=$h ctl_e2e=$got port=$port" | tee -a "$OUT/arm.txt"
    if [ "$got" != "$want" ]; then
        echo "verbs-e2e-ab: $h runs ctl_e2e=$got, arm $ARM needs $want; stopping" >&2
        exit 1
    fi
done

: > "$OUT/summary.txt"
status=0
n=0
run_streak() { # mode...; not in a pipeline, so n and status persist
    n=$((n + 1))
    "$here/verbs-streak.sh" "$n" "$n" "$@" > "$OUT/streak.log" 2>&1 || status=1
    grep -E '^run ' "$OUT/streak.log" | sed "s/^/[$*] /" | tee -a "$OUT/summary.txt"
}
for _ in 1 2 3 4 5 6; do run_streak read 2000; done
for _ in 1 2; do run_streak write 2000; done
for _ in 1 2; do run_streak send 2000; done
run_streak send
# The 256 B RTT probe exercises the control ring alone, repeated for its tail.
for _ in 1 2 3; do
    "$here/run-verbs-smoke.sh" ping 2000 > "$OUT/ping.log" 2>&1
    grep -q '^PINGPONG' "$OUT/ping.log" || status=1
    grep -E '^PINGPONG|^FAIL' "$OUT/ping.log" | sed 's/^/[ping 2000] /' | tee -a "$OUT/summary.txt"
done
rm -f "$OUT/streak.log" "$OUT/ping.log"

for h in "${HOSTS[@]}"; do
    stats=$("${SSH[@]}" "$h" "sudo grep -E '^(bad_frames|version_mismatch|zc_read_fallback|zc_read_retx|zc_ack_probe|zc_retries) ' $SYS/native_stats; sudo grep -m1 '^err_count' $SYS/stall_snap" | tr '\n' ' ')
    echo "[final] $h: $stats" | tee -a "$OUT/summary.txt"
done
exit "$status"
