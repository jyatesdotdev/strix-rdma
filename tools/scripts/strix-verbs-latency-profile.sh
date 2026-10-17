#!/usr/bin/env bash
# Apply (or undo) the runtime half of the low-latency profile for tensor
# parallelism on one host (docs/ZEROCOPY_VERBS.md, round nineteen). Run as root
# on both hosts after strix-verbs-bringup.sh.
#
#   apply:   pin the strix_nhi poller to POLLER_CPU at SCHED_FIFO 50, steer the
#            NHI's interrupts to IRQ_CPU (irqbalance is stopped so it keeps
#            them there), spin the poller 2 ms before sleeping, and send a 20 us
#            keepalive so the transmit path never goes cold between exchanges.
#   restore: undo all of that.
#
# Run the application on another core (APP_CPU is only advice, e.g.
# `taskset -c 6 ...`), on a different physical core from the poller and IRQ CPU.
#
# The load-time half needs a module load, which strix-verbs-bringup.sh does once
# per boot, so put it in modprobe.d and reboot:
#   options strix_nhi ctl_rx_poll=1   # poll the control RX ring
#   options thunderbolt clx=0         # no USB4 low-power link states
#
# Trade-off, measured 2026-10-16: 256 B round trip ~12-13 us even after idle
# gaps (35 us before this round), and zero-copy SEND/WRITE at ~1.1 GB/s; but
# zero-copy READ falls back often (~320 MiB/s). TP uses SEND/WRITE, not READ.
#
# Environment: POLLER_CPU=2 IRQ_CPU=4 NHI=0000:c4:00.6 SPIN_US=2000 KEEPALIVE_US=20
set -eu
MODE=${1:?usage: strix-verbs-latency-profile.sh apply|restore}
POLLER_CPU=${POLLER_CPU:-2}
IRQ_CPU=${IRQ_CPU:-4}
NHI=${NHI:-0000:c4:00.6}
SPIN_US=${SPIN_US:-2000}
KEEPALIVE_US=${KEEPALIVE_US:-20}
PARAMS=/sys/module/strix_nhi/parameters

[ "$(id -u)" = 0 ] || { echo "strix-verbs-latency-profile: must run as root" >&2; exit 1; }
POLLER=$(pgrep -x "strix_nhi/.*" || pgrep strix_nhi || true)
[ -n "$POLLER" ] || { echo "strix-verbs-latency-profile: no strix_nhi poller (run bring-up first)" >&2; exit 1; }
IRQS=$(awk -v nhi="$NHI" 'index($0, nhi) { sub(":", "", $1); print $1 }' /proc/interrupts)

case "$MODE" in
apply)
    systemctl stop irqbalance 2>/dev/null || true
    taskset -p -c "$POLLER_CPU" "$POLLER" >/dev/null
    chrt -f -p 50 "$POLLER"
    for irq in $IRQS; do echo "$IRQ_CPU" > "/proc/irq/$irq/smp_affinity_list"; done
    echo "$SPIN_US" > "$PARAMS/idle_spin_us"
    echo "$KEEPALIVE_US" > "$PARAMS/keepalive_us"
    ;;
restore)
    echo 0 > "$PARAMS/keepalive_us"
    echo 128 > "$PARAMS/idle_spin_us"
    chrt -o -p 0 "$POLLER"
    taskset -p -c "0-$(($(nproc) - 1))" "$POLLER" >/dev/null
    systemctl start irqbalance 2>/dev/null || true
    ;;
*)
    echo "usage: strix-verbs-latency-profile.sh apply|restore" >&2; exit 2 ;;
esac
echo "$MODE: poller $POLLER on cpu$(ps -o psr= -p "$POLLER" | tr -d ' ') $(chrt -p "$POLLER" | head -1 | sed 's/.*policy: //')," \
     "irqs $(echo $IRQS | tr ' ' ,) -> $(cat /proc/irq/"$(echo $IRQS | cut -d' ' -f1)"/effective_affinity_list)," \
     "spin $(cat $PARAMS/idle_spin_us) us, keepalive $(cat $PARAMS/keepalive_us) us," \
     "ctl_rx_poll=$(cat $PARAMS/ctl_rx_poll) clx=$(cat /sys/module/thunderbolt/parameters/clx)"
