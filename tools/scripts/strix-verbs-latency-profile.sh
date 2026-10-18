#!/usr/bin/env bash
# Apply (or undo) the scheduling half of the low-latency profile on one host
# (docs/ZEROCOPY_VERBS.md, round nineteen). Run as root on both hosts after
# strix-verbs-bringup.sh.
#
#   apply:   pin the strix_nhi poller to POLLER_CPU at SCHED_FIFO 50 and steer
#            the NHI's interrupts to IRQ_CPU (irqbalance is stopped so it keeps
#            them there).
#   restore: undo both.
#
# The defaults already poll the control RX ring and keep the transmit path warm
# (ctl_rx_poll, keepalive_us), which is where the latency comes from: a 256 B
# round trip of ~12-13 us and a 10 KiB exchange of ~20-22 us, even after
# compute gaps. This profile adds about 1 us at the median and removes the
# scheduler's outliers (maxima of 0.1-2.6 ms under defaults, ~25-50 us here).
#
# Never run the poller SCHED_FIFO without pinning it away from the IRQ CPU:
# unpinned, it starved the NHI's interrupt work and halved SEND throughput.
# Run the application on a third core (e.g. `taskset -c 6 ...`), on a different
# physical core from POLLER_CPU and IRQ_CPU.
#
# Environment: POLLER_CPU=2 IRQ_CPU=4 NHI=0000:c4:00.6
set -eu
MODE=${1:?usage: strix-verbs-latency-profile.sh apply|restore}
POLLER_CPU=${POLLER_CPU:-2}
IRQ_CPU=${IRQ_CPU:-4}
NHI=${NHI:-0000:c4:00.6}
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
    ;;
restore)
    chrt -o -p 0 "$POLLER"
    taskset -p -c "0-$(($(nproc) - 1))" "$POLLER" >/dev/null
    systemctl start irqbalance 2>/dev/null || true
    ;;
*)
    echo "usage: strix-verbs-latency-profile.sh apply|restore" >&2; exit 2 ;;
esac
echo "$MODE: poller $POLLER on cpu$(ps -o psr= -p "$POLLER" | tr -d ' ') $(chrt -p "$POLLER" | head -1 | sed 's/.*policy: //')," \
     "irqs $(echo $IRQS | tr ' ' ,) -> $(cat /proc/irq/"$(echo $IRQS | cut -d' ' -f1)"/effective_affinity_list)," \
     "ctl_rx_poll=$(cat $PARAMS/ctl_rx_poll) keepalive $(cat $PARAMS/keepalive_us) us" \
     "(window $(cat $PARAMS/keepalive_window_us) us), spin $(cat $PARAMS/idle_spin_us) us"
