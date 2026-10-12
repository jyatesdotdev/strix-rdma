#!/usr/bin/env bash
# Bring up the native NHI software verbs stack in zero-copy (no-TB-IP) mode.
#
# Why this script exists: the hosts' initramfs still carries the STOCK
# thunderbolt.ko, so after every boot the in-kernel module lacks the custom
# XDomain symbols strix_nhi needs (tb_xdomain_try_enable_native_paths and
# friends), and thunderbolt_net auto-loads via its module alias as soon as the
# XDomain appears. Both must be corrected before strix_nhi will reach
# PORT_ACTIVE with the zero-copy data plane live.
#
# Load strix_nhi exactly ONCE per boot. Re-probing after the data plane has
# carried heavy bursts has wedged the router's DMA path config space
# ("DMA RX path activation failed: -110"), which survives a clean teardown, a
# settle, and even an NHI PCI function-level reset -- only a power cycle clears
# it. See docs/ZEROCOPY_VERBS.md, "Operational notes".
#
# Usage: strix-verbs-bringup.sh [--skip-tbnet-blacklist]
set -euo pipefail

SKIP_BLACKLIST=0
[ "${1:-}" = "--skip-tbnet-blacklist" ] && SKIP_BLACKLIST=1

BLACKLIST_FILE=/etc/modprobe.d/strix-nhi-verbs-zc.conf

log() { printf '%s\n' "$*" >&2; }

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        log "error: must run as root (module load and modprobe.d writes)"
        exit 2
    fi
}

# Already converged? Do nothing. Tearing down a healthy stack to rebuild it is
# exactly the re-probe that can wedge the router, so this script must be safe to
# run repeatedly and after the stack is already up.
#
# Note: no `lsmod | grep -q` here. grep -q exits on its first match, lsmod then
# dies of SIGPIPE (141), and under `set -o pipefail` that reads as failure -- so
# the guard would never fire and every run would needlessly reload the stack.
already_up() {
    local state
    [ -d /sys/module/strix_nhi ] || return 1
    grep -q 'tb_xdomain_try_enable_native_paths' /proc/kallsyms || return 1
    state=$(cat /sys/class/infiniband/strix_nhi0/ports/1/state 2>/dev/null || echo "")
    case "$state" in
        *ACTIVE*) return 0 ;;
        *) return 1 ;;
    esac
}

# thunderbolt_net must be absent for zero-copy mode: the driver gates its ring
# bring-up on TB-IP carrier when a thunderbolt netdev exists, and without a
# peer running TB-IP that gate never opens. `blacklist` alone is not enough --
# it only suppresses alias-based autoload, so an explicit modules-load.d entry
# (the stream path installs one) would still pull it in.
disable_tbnet() {
    [ "$SKIP_BLACKLIST" = 1 ] && return 0
    cat > "$BLACKLIST_FILE" <<'CONF'
# Native NHI software verbs, zero-copy mode: the data plane needs both XDomain
# DMA path tuples, and thunderbolt_net's presence gates ring bring-up on a
# TB-IP carrier that no-TB-IP operation never establishes.
blacklist thunderbolt_net
install thunderbolt_net /bin/false
CONF
    log "wrote $BLACKLIST_FILE"
    # Undo any already-loaded instance from this boot.
    modprobe -r thunderbolt_net 2>/dev/null || true
}

# Replace the initramfs-provided stock core with the updates/ build. typec
# imports thunderbolt symbols, so it has to come out first.
reload_thunderbolt_core() {
    log "== reloading thunderbolt core from updates/"
    modprobe -r strix_nhi 2>/dev/null || true
    modprobe -r thunderbolt_net 2>/dev/null || true
    modprobe -r typec_thunderbolt 2>/dev/null || true
    modprobe -r ucsi_acpi 2>/dev/null || true
    modprobe -r typec_ucsi 2>/dev/null || true
    modprobe -r typec 2>/dev/null || true
    modprobe -r thunderbolt 2>/dev/null || true
    sleep 1
    modprobe thunderbolt
    modprobe typec_thunderbolt 2>/dev/null || true
    modprobe ucsi_acpi 2>/dev/null || true
}

# The XDomain must have enumerated before strix_nhi can bind to it.
wait_xdomain() {
    local i
    for i in $(seq 1 30); do
        if [ -n "$(ls -d /sys/bus/thunderbolt/devices/*-*.* 2>/dev/null)" ]; then
            return 0
        fi
        sleep 1
    done
    log "error: no Thunderbolt XDomain enumerated within 30 s" >&2
    log "       is the peer powered and the cable seated?" >&2
    exit 1
}

verify_core() {
    local path symbols
    path=$(modinfo -n thunderbolt)
    case "$path" in
        */updates/*) log "ok: thunderbolt -> $path" ;;
        *) log "error: thunderbolt resolves to $path, not updates/; run kernel/scripts/host-install.sh" >&2; exit 1 ;;
    esac
    # The custom core is what exports the native-path admission API.
    symbols=$(grep -c 'tb_xdomain_try_enable_native_paths' /proc/kallsyms || true)
    if [ "$symbols" -eq 0 ]; then
        log "error: loaded thunderbolt core lacks tb_xdomain_try_enable_native_paths" >&2
        log "       the stock initramfs module is still resident; reload failed" >&2
        exit 1
    fi
    log "ok: native path admission symbols present ($symbols)"
}

load_verbs() {
    local path
    path=$(modinfo -n strix_nhi)
    case "$path" in
        */updates/*) log "ok: strix_nhi -> $path" ;;
        *) log "error: strix_nhi resolves to $path, not updates/" >&2; exit 1 ;;
    esac
    if [ -d /sys/module/strix_nhi ]; then
        log "strix_nhi already loaded; not re-probing (see the wedge note above)"
        return 0
    fi
    log "== loading strix_nhi enable=1"
    modprobe strix_nhi enable=1
}

wait_active() {
    local state i limit
    limit=${WAIT_ACTIVE_SECONDS:-90}
    for i in $(seq 1 "$limit"); do
        state=$(cat /sys/class/infiniband/strix_nhi0/ports/1/state 2>/dev/null || echo "")
        case "$state" in
            *ACTIVE*)
                log "ok: port 1 ${state}"
                report_zdata
                return 0
                ;;
        esac
        sleep 1
    done
    # Not fatal: the driver retransmits HELLO every 100 ms forever, so the port
    # converges on its own as soon as the peer's stack is up. A slow peer boot
    # is the common case when both nodes are brought up together.
    log "loaded but port not ACTIVE within ${limit} s (state: ${state:-unknown})."
    log "This is expected if the peer is still booting -- the handshake"
    log "retransmits every 100 ms and will converge without further action."
    log "Check with: cat /sys/class/infiniband/strix_nhi0/ports/1/state"
    return 3
}

report_zdata() {
    # zdata=1 means the second path tuple enabled and the zero-copy data rings
    # are live; zdata=0 means staged-only (usually a missing second HopID).
    local stats
    stats=$(cat /sys/class/infiniband/strix_nhi0/native_stats 2>/dev/null || echo "")
    case "$stats" in
        *"zdata 1"*)
            log "ok: zero-copy data plane live (zdata=1)"
            ;;
        *"zdata 0"*)
            log "warning: staged-only (zdata=0); the zero-copy data rings are not live"
            ;;
        *)
            # Older modules print zdata only while a QP is bound, so an absent
            # line there is normal at bring-up and does not mean staged-only.
            if [ -n "$stats" ]; then
                log "note: no QP bound yet, so zdata is not reported (normal at"
                log "      bring-up). After a verbs client connects, confirm the"
                log "      zero-copy data plane with:"
                log "      cat /sys/class/infiniband/strix_nhi0/native_stats | grep zdata"
            else
                log "warning: native_stats unreadable; is strix_nhi0 registered?"
            fi
            ;;
    esac
}

require_root
if [ "${FORCE:-0}" != 1 ] && already_up; then
    log "strix_nhi already loaded and port 1 ACTIVE; leaving it alone"
    log "(re-probing a live data plane risks the router wedge -- set FORCE=1 to override)"
    report_zdata
    exit 0
fi
disable_tbnet
reload_thunderbolt_core
verify_core
wait_xdomain
load_verbs
rc=0
wait_active || rc=$?
if [ "$rc" -eq 0 ]; then
    log "done: strix_nhi0 up"
else
    log "done: strix_nhi0 loaded, awaiting peer (exit $rc)"
fi
exit "$rc"
