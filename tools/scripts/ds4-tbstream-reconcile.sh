#!/usr/bin/env bash
# Converge one DS4 USB4STREAM endpoint and publish a stable runtime device link.
#
# This helper owns only ConfigFS children named $TBSTREAM_NAME.  It deliberately
# withdraws the stable link before resetting a stale generation and refuses
# cleanup whenever fuser reports an open tbstream holder. Raw /dev consumers must
# be quiesced during maintenance because they do not participate in this lock.
set -euo pipefail

readonly EX_TEMPFAIL=75
readonly EX_CONFIG=78

MODE=reconcile
SINGLE_PASS=0

usage() {
    cat <<'EOF'
Usage: ds4-tbstream-reconcile.sh [--cleanup|--once]

Configuration is read from the environment (normally /etc/sysconfig/ds4-tbstream):
  TBSTREAM_ROLE=allocator|follower   Exactly one endpoint must be allocator.
  TBSTREAM_NAME=ds4                  ConfigFS stream/property name (max 8 chars).
  TBSTREAM_RING_SIZE=4096            Ring size, 32 through 4096.
  TBSTREAM_THROTTLING=0              Interrupt throttling in nanoseconds.
  TBSTREAM_IN_HOPID=9                Optional exact local HopID pair (8-2047).
  TBSTREAM_OUT_HOPID=9               Set both values or leave both empty.
  TBSTREAM_NETDEV=thunderbolt0       Optional netdev whose carrier must be up
                                     before a new stream endpoint is created.
  TBSTREAM_WAIT_SECONDS=90           Reconcile retry window.
  TBSTREAM_HOLDER_WAIT_SECONDS=30    Time to wait for old device fds to close.

--cleanup withdraws the stable link and removes managed ConfigFS items only after
all /dev/tbstream* holders have drained. It never removes device nodes directly.
--once performs one watchdog pass without bootstrap, holder, or device waits.
EOF
}

while [ "$#" -gt 0 ]; do
    case "$1" in
        --cleanup)
            MODE=cleanup
            ;;
        --once)
            SINGLE_PASS=1
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            printf 'ds4-tbstream: unknown argument: %s\n' "$1" >&2
            usage >&2
            exit 64
            ;;
    esac
    shift
done

if [ "$MODE" = cleanup ] && [ "$SINGLE_PASS" -eq 1 ]; then
    printf 'ds4-tbstream: --cleanup and --once are mutually exclusive\n' >&2
    exit 64
fi

: "${TBSTREAM_ROLE:=}"
: "${TBSTREAM_NAME:=ds4}"
: "${TBSTREAM_RING_SIZE:=4096}"
: "${TBSTREAM_THROTTLING:=0}"
: "${TBSTREAM_WAIT_SECONDS:=90}"
: "${TBSTREAM_HOLDER_WAIT_SECONDS:=30}"
: "${TBSTREAM_POLL_SECONDS:=1}"
: "${TBSTREAM_DEVICE_WAIT_SECONDS:=10}"
: "${TBSTREAM_GROUP:=tbstream}"
: "${TBSTREAM_IN_HOPID:=}"
: "${TBSTREAM_OUT_HOPID:=}"
: "${TBSTREAM_NETDEV:=}"

: "${TBSTREAM_SYSFS_ROOT:=/sys/bus/thunderbolt/devices}"
: "${TBSTREAM_CONFIGFS_ROOT:=/sys/kernel/config/thunderbolt/stream}"
: "${TBSTREAM_MISC_ROOT:=/sys/class/misc}"
: "${TBSTREAM_NET_ROOT:=/sys/class/net}"
: "${TBSTREAM_DEV_ROOT:=/dev}"
: "${TBSTREAM_RUN_DIR:=/run/ds4-tbstream}"
: "${TBSTREAM_FUSER:=fuser}"
: "${TBSTREAM_FLOCK:=flock}"
: "${TBSTREAM_UDEVADM:=udevadm}"
: "${TBSTREAM_MODPROBE:=modprobe}"
: "${TBSTREAM_TEST_MODE:=0}"
: "${TBSTREAM_TEST_ROOT:=}"
: "${TBSTREAM_TEST_HOOK:=}"

if [ "$SINGLE_PASS" -eq 1 ]; then
    TBSTREAM_WAIT_SECONDS=0
    TBSTREAM_HOLDER_WAIT_SECONDS=0
    TBSTREAM_DEVICE_WAIT_SECONDS=0
fi

readonly DEVICE_LINK="$TBSTREAM_RUN_DIR/device"
readonly READY_FILE="$TBSTREAM_RUN_DIR/ready"
readonly GENERATION_FILE="$TBSTREAM_RUN_DIR/generation"
readonly DIRTY_FILE="$TBSTREAM_RUN_DIR/dirty"
readonly LOCK_FILE="$TBSTREAM_RUN_DIR/reconcile.lock"

log() {
    printf 'ds4-tbstream: %s\n' "$*" >&2
}

die_config() {
    log "$*"
    exit "$EX_CONFIG"
}

is_uint() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        0|[1-9]|[1-9][0-9]*) return 0 ;;
        *) return 1 ;;
    esac
}

validate_config() {
    if [ "$MODE" = reconcile ]; then
        case "$TBSTREAM_ROLE" in
            allocator|follower) ;;
            *) die_config 'TBSTREAM_ROLE must be allocator or follower' ;;
        esac
    elif [ -n "$TBSTREAM_ROLE" ]; then
        case "$TBSTREAM_ROLE" in
            allocator|follower) ;;
            *) die_config 'TBSTREAM_ROLE must be allocator or follower when set' ;;
        esac
    fi

    case "$TBSTREAM_NAME" in
        ''|*[!A-Za-z0-9_.-]*) die_config 'TBSTREAM_NAME contains unsupported characters' ;;
    esac
    [ "${#TBSTREAM_NAME}" -le 8 ] ||
        die_config 'TBSTREAM_NAME exceeds the kernel property-key limit (8)'

    if [ -n "$TBSTREAM_IN_HOPID" ] || [ -n "$TBSTREAM_OUT_HOPID" ]; then
        [ -n "$TBSTREAM_IN_HOPID" ] && [ -n "$TBSTREAM_OUT_HOPID" ] ||
            die_config 'TBSTREAM_IN_HOPID and TBSTREAM_OUT_HOPID must be set together'
        for hopid in "$TBSTREAM_IN_HOPID" "$TBSTREAM_OUT_HOPID"; do
            is_uint "$hopid" || die_config 'configured HopIDs must be integers'
            [ "${#hopid}" -le 4 ] || die_config 'configured HopID is too large'
            if [ "$hopid" -lt 8 ] || [ "$hopid" -gt 2047 ]; then
                die_config 'configured HopIDs must be between 8 and 2047'
            fi
        done
    fi

    case "$TBSTREAM_NETDEV" in
        '') ;;
        '.'|'..'|*[!A-Za-z0-9_.-]*)
            die_config 'TBSTREAM_NETDEV contains unsupported characters'
            ;;
    esac
    [ "${#TBSTREAM_NETDEV}" -le 15 ] ||
        die_config 'TBSTREAM_NETDEV exceeds the Linux interface-name limit (15)'
    case "$TBSTREAM_NET_ROOT" in
        /*) ;;
        *) die_config 'TBSTREAM_NET_ROOT must be absolute' ;;
    esac

    is_uint "$TBSTREAM_RING_SIZE" || die_config 'TBSTREAM_RING_SIZE must be an integer'
    [ "${#TBSTREAM_RING_SIZE}" -le 4 ] || die_config 'TBSTREAM_RING_SIZE is too large'
    if [ "$TBSTREAM_RING_SIZE" -lt 32 ] || [ "$TBSTREAM_RING_SIZE" -gt 4096 ]; then
        die_config 'TBSTREAM_RING_SIZE must be between 32 and 4096'
    fi
    is_uint "$TBSTREAM_THROTTLING" || die_config 'TBSTREAM_THROTTLING must be an integer'
    [ "${#TBSTREAM_THROTTLING}" -le 8 ] || die_config 'TBSTREAM_THROTTLING is too large'
    [ "$TBSTREAM_THROTTLING" -le 16776960 ] ||
        die_config 'TBSTREAM_THROTTLING exceeds the kernel maximum (16776960)'

    for value in "$TBSTREAM_WAIT_SECONDS" "$TBSTREAM_HOLDER_WAIT_SECONDS" \
                 "$TBSTREAM_POLL_SECONDS" "$TBSTREAM_DEVICE_WAIT_SECONDS"; do
        is_uint "$value" || die_config 'timeout values must be non-negative integers'
        [ "${#value}" -le 3 ] || die_config 'timeout values must be at most three digits'
    done
    [ "$TBSTREAM_WAIT_SECONDS" -le 120 ] || die_config 'TBSTREAM_WAIT_SECONDS exceeds 120'
    [ "$TBSTREAM_HOLDER_WAIT_SECONDS" -le 60 ] ||
        die_config 'TBSTREAM_HOLDER_WAIT_SECONDS exceeds 60'
    [ "$TBSTREAM_POLL_SECONDS" -ge 1 ] || die_config 'TBSTREAM_POLL_SECONDS must be at least 1'
    [ "$TBSTREAM_POLL_SECONDS" -le 10 ] || die_config 'TBSTREAM_POLL_SECONDS exceeds 10'
    [ "$TBSTREAM_DEVICE_WAIT_SECONDS" -le 30 ] ||
        die_config 'TBSTREAM_DEVICE_WAIT_SECONDS exceeds 30'

    if [ "$TBSTREAM_TEST_MODE" = 1 ]; then
        if [ -z "$TBSTREAM_TEST_ROOT" ] || [ "$TBSTREAM_TEST_ROOT" = / ]; then
            die_config 'test mode requires a non-root TBSTREAM_TEST_ROOT'
        fi
        case "$TBSTREAM_TEST_ROOT" in
            /*) ;;
            *) die_config 'TBSTREAM_TEST_ROOT must be absolute' ;;
        esac
        for path in "$TBSTREAM_SYSFS_ROOT" "$TBSTREAM_CONFIGFS_ROOT" \
                    "$TBSTREAM_MISC_ROOT" "$TBSTREAM_NET_ROOT" \
                    "$TBSTREAM_DEV_ROOT" "$TBSTREAM_RUN_DIR"; do
            case "$path" in
                "$TBSTREAM_TEST_ROOT"|"$TBSTREAM_TEST_ROOT"/*) ;;
                *) die_config "test path escapes TBSTREAM_TEST_ROOT: $path" ;;
            esac
        done
        [ -z "$TBSTREAM_TEST_HOOK" ] || [ -x "$TBSTREAM_TEST_HOOK" ] ||
            die_config 'TBSTREAM_TEST_HOOK is not executable'
    else
        [ -z "$TBSTREAM_TEST_HOOK" ] ||
            die_config 'TBSTREAM_TEST_HOOK is allowed only in test mode'
        [ "${EUID:-$(id -u)}" -eq 0 ] || die_config 'must run as root'
    fi
}

run_test_hook() {
    [ "$TBSTREAM_TEST_MODE" = 1 ] && [ -n "$TBSTREAM_TEST_HOOK" ] || return 0
    "$TBSTREAM_TEST_HOOK" "$@"
}

atomic_write() {
    local target=$1
    local contents=$2
    local tmp="$TBSTREAM_RUN_DIR/.${target##*/}.new.$$.${RANDOM:-0}"

    [ ! -d "$target" ] || {
        log "refusing to replace directory $target"
        return 1
    }
    umask 077
    if ! printf '%s\n' "$contents" >"$tmp"; then
        rm -f "$tmp" 2>/dev/null || true
        return 1
    fi
    if ! chmod 0644 "$tmp" || ! mv -f "$tmp" "$target"; then
        rm -f "$tmp" 2>/dev/null || true
        return 1
    fi
    return 0
}

atomic_symlink() {
    local target=$1
    local link=$2
    local tmp="$TBSTREAM_RUN_DIR/.${link##*/}.new.$$.${RANDOM:-0}"

    [ ! -d "$link" ] || {
        log "refusing to replace directory $link"
        return 1
    }
    rm -f "$tmp" || return 1
    if ! ln -s "$target" "$tmp"; then
        return 1
    fi
    if ! mv -f "$tmp" "$link"; then
        rm -f "$tmp" 2>/dev/null || true
        return 1
    fi
    return 0
}

withdraw_ready() {
    local rc=0

    rm -f "$DEVICE_LINK" || rc=1
    rm -f "$READY_FILE" || rc=1
    return "$rc"
}

mark_dirty() {
    local reason=$1
    local current

    if [ -f "$DIRTY_FILE" ] && [ ! -L "$DIRTY_FILE" ]; then
        current=$(read_one_line "$DIRTY_FILE" 2>/dev/null || true)
        [ "$current" = "$reason" ] && return 0
    fi
    atomic_write "$DIRTY_FILE" "$reason"
}

enter_fail_closed() {
    local reason=$1
    local rc=0

    if ! withdraw_ready; then
        log 'failed to withdraw the published endpoint'
        rc=1
    fi
    if ! mark_dirty "$reason"; then
        log 'failed to record the non-ready state'
        rc=1
    fi
    return "$rc"
}

read_one_line() {
    local path=$1
    local value

    [ -r "$path" ] || return 1
    IFS= read -r value <"$path" || [ -n "$value" ] || return 1
    printf '%s' "$value"
}

stat_identity() {
    local path=$1
    local identity

    if identity=$(stat -Lc '%d:%i' "$path" 2>/dev/null); then
        printf '%s' "$identity"
        return 0
    fi
    if identity=$(stat -f '%d:%i' "$path" 2>/dev/null); then
        printf '%s' "$identity"
        return 0
    fi
    return 1
}

service_is_bound_stream() {
    local service_dir=$1
    local key driver

    key=$(read_one_line "$service_dir/key" 2>/dev/null || true)
    [ "$key" = stream ] || return 1
    driver=$(readlink "$service_dir/driver" 2>/dev/null || true)
    [ "${driver##*/}" = thunderbolt_stream ]
}

LIVE_SERVICES=()
discover_services() {
    local key_path key service_dir

    LIVE_SERVICES=()
    for key_path in "$TBSTREAM_SYSFS_ROOT"/*/key; do
        [ -e "$key_path" ] || continue
        key=$(read_one_line "$key_path" 2>/dev/null || true)
        [ "$key" = stream ] || continue
        service_dir=${key_path%/key}
        LIVE_SERVICES[${#LIVE_SERVICES[@]}]=$service_dir
    done
}

service_generation() {
    local service_dir=$1
    local identity

    identity=$(stat_identity "$service_dir") || return 1
    printf '%s:%s' "${service_dir##*/}" "$identity"
}

netdev_carrier_ready() {
    local carrier_path carrier

    [ -n "$TBSTREAM_NETDEV" ] || return 0
    carrier_path="$TBSTREAM_NET_ROOT/$TBSTREAM_NETDEV/carrier"
    run_test_hook carrier_poll "$carrier_path" || return 1
    carrier=$(read_one_line "$carrier_path" 2>/dev/null || true)
    [ "$carrier" = 1 ]
}

MANAGED_ITEMS=()
collect_managed_items() {
    local item

    MANAGED_ITEMS=()
    for item in "$TBSTREAM_CONFIGFS_ROOT"/*/"$TBSTREAM_NAME"; do
        [ -d "$item" ] || continue
        MANAGED_ITEMS[${#MANAGED_ITEMS[@]}]=$item
    done
}

device_holder_state() {
    local device=$1
    local output rc

    if output=$("$TBSTREAM_FUSER" "$device" 2>&1); then
        return 0
    else
        rc=$?
    fi
    if [ "$rc" -eq 1 ] && [ -z "$output" ]; then
        return 1
    fi
    log "could not determine holders for $device"
    return 2
}

any_device_holder() {
    local device rc

    for device in "$TBSTREAM_DEV_ROOT"/tbstream[0-9]*; do
        [ -e "$device" ] || continue
        if device_holder_state "$device"; then
            log "open holder remains on $device"
            return 0
        else
            rc=$?
            [ "$rc" -eq 1 ] || return 2
        fi
    done
    return 1
}

wait_for_no_holders() {
    local deadline=$((SECONDS + TBSTREAM_HOLDER_WAIT_SECONDS))
    local rc

    while true; do
        if any_device_holder; then
            [ "$SECONDS" -lt "$deadline" ] || return 1
            sleep "$TBSTREAM_POLL_SECONDS" || return 1
        else
            rc=$?
            [ "$rc" -eq 1 ] && return 0
            return 1
        fi
    done
}

remove_managed_item() {
    local item=$1
    local parent=${item%/*}
    local rc

    if any_device_holder; then
        return 1
    else
        rc=$?
        [ "$rc" -eq 1 ] || return 1
    fi
    # This is also the kernel-side no-users gate: both stores return EBUSY if a
    # raw-device opener won the fuser/rmdir race. Setting either side below 8
    # withdraws the advertised property; setting both releases both HopIDs.
    if ! write_attr 0 "$item/in_hopid" || ! write_attr 0 "$item/out_hopid"; then
        log "failed to deactivate ConfigFS item $item"
        return 1
    fi
    run_test_hook deactivate "$item" || return 1
    run_test_hook remove "$item" || return 1
    if ! rmdir "$item"; then
        log "failed to remove ConfigFS item $item"
        return 1
    fi
    # The parent may legitimately contain another stream name.
    rmdir "$parent" 2>/dev/null || true
}

cleanup_managed() {
    local item

    withdraw_ready || return 1
    collect_managed_items
    [ "${#MANAGED_ITEMS[@]}" -gt 0 ] || return 0
    if ! wait_for_no_holders; then
        log 'refusing ConfigFS cleanup while a tbstream device is open'
        return 1
    fi
    for item in "${MANAGED_ITEMS[@]}"; do
        remove_managed_item "$item" || return 1
    done
    return 0
}

config_attrs_valid() {
    local item=$1
    local index in_hopid out_hopid ring_size throttling

    index=$(read_one_line "$item/index" 2>/dev/null || true)
    in_hopid=$(read_one_line "$item/in_hopid" 2>/dev/null || true)
    out_hopid=$(read_one_line "$item/out_hopid" 2>/dev/null || true)
    ring_size=$(read_one_line "$item/ring_size" 2>/dev/null || true)
    throttling=$(read_one_line "$item/throttling" 2>/dev/null || true)

    is_uint "$index" || return 1
    is_uint "$in_hopid" && [ "$in_hopid" -ge 8 ] || return 1
    is_uint "$out_hopid" && [ "$out_hopid" -ge 8 ] || return 1
    if [ -n "$TBSTREAM_IN_HOPID" ]; then
        [ "$in_hopid" = "$TBSTREAM_IN_HOPID" ] || return 1
        [ "$out_hopid" = "$TBSTREAM_OUT_HOPID" ] || return 1
    fi
    [ "$ring_size" = "$TBSTREAM_RING_SIZE" ] || return 1
    [ "$throttling" = "$TBSTREAM_THROTTLING" ] || return 1
}

write_attr() {
    local value=$1
    local path=$2

    if ! printf '%s\n' "$value" >"$path"; then
        log "failed to write $path"
        return 1
    fi
}

create_config_item() {
    local service_name=$1
    local parent="$TBSTREAM_CONFIGFS_ROOT/$service_name"
    local item="$parent/$TBSTREAM_NAME"
    local requested_in=-1
    local requested_out=-1

    [ ! -e "$item" ] || {
        log "refusing to replace unexpected existing item $item"
        return 1
    }
    if ! mkdir -p "$parent" || ! mkdir "$item"; then
        log "failed to create ConfigFS item $item"
        rmdir "$parent" 2>/dev/null || true
        return 1
    fi
    if ! run_test_hook create "$item" "$TBSTREAM_ROLE"; then
        rmdir "$item" 2>/dev/null || true
        rmdir "$parent" 2>/dev/null || true
        return 1
    fi

    if [ ! -e "$item/ring_size" ] || [ ! -e "$item/throttling" ] ||
       [ ! -e "$item/in_hopid" ] || [ ! -e "$item/out_hopid" ] ||
       [ ! -e "$item/index" ]; then
        log "ConfigFS attributes did not appear for $item"
        remove_managed_item "$item" || true
        return 1
    fi

    if ! write_attr "$TBSTREAM_RING_SIZE" "$item/ring_size" ||
       ! write_attr "$TBSTREAM_THROTTLING" "$item/throttling"; then
        remove_managed_item "$item" || true
        return 1
    fi

    if [ "$TBSTREAM_ROLE" = allocator ]; then
        # The allocator publishes a pair. The follower learns and reverses this
        # pair during its ConfigFS attach and must not allocate independently.
        # A configured exact pair is never allowed to fall back to another
        # allocation: either both writes and final validation succeed, or the
        # partially configured child is deactivated and removed below.
        if [ -n "$TBSTREAM_IN_HOPID" ]; then
            requested_in=$TBSTREAM_IN_HOPID
            requested_out=$TBSTREAM_OUT_HOPID
        fi
        if ! write_attr "$requested_in" "$item/in_hopid" ||
           ! write_attr "$requested_out" "$item/out_hopid"; then
            remove_managed_item "$item" || true
            return 1
        fi
    fi
    if ! run_test_hook configure "$item" "$TBSTREAM_ROLE"; then
        remove_managed_item "$item" || true
        return 1
    fi

    if ! config_attrs_valid "$item"; then
        if [ -n "$TBSTREAM_IN_HOPID" ]; then
            log "stream endpoint did not acquire configured local HopIDs $TBSTREAM_IN_HOPID/$TBSTREAM_OUT_HOPID"
        elif [ "$TBSTREAM_ROLE" = follower ]; then
            log 'allocator HopIDs are not advertised yet; retrying follower attach'
        else
            log "new ConfigFS item did not become valid: $item"
        fi
        remove_managed_item "$item" || true
        return 1
    fi
    printf '%s' "$item"
}

device_actual_number() {
    local device=$1
    local encoded major_hex minor_hex major minor

    encoded=$(stat -Lc '%t:%T' "$device" 2>/dev/null) || return 1
    major_hex=${encoded%%:*}
    minor_hex=${encoded#*:}
    major=$(printf '%d' "0x$major_hex") || return 1
    minor=$(printf '%d' "0x$minor_hex") || return 1
    printf '%s:%s' "$major" "$minor"
}

verify_device_once() {
    local item=$1
    local index device misc expected actual metadata

    index=$(read_one_line "$item/index" 2>/dev/null || true)
    is_uint "$index" || return 1
    device="$TBSTREAM_DEV_ROOT/tbstream$index"
    misc="$TBSTREAM_MISC_ROOT/tbstream$index"
    expected=$(read_one_line "$misc/dev" 2>/dev/null || true)
    case "$expected" in
        *:*) ;;
        *) return 1 ;;
    esac

    if [ "$TBSTREAM_TEST_MODE" = 1 ]; then
        [ -f "$device" ] || return 1
        actual=$(read_one_line "$device.mock-dev" 2>/dev/null || true)
        metadata=$(read_one_line "$device.mock-meta" 2>/dev/null || true)
    else
        [ -c "$device" ] || return 1
        actual=$(device_actual_number "$device" 2>/dev/null || true)
        metadata=$(stat -Lc '%a:%U:%G' "$device" 2>/dev/null || true)
    fi

    [ "$actual" = "$expected" ] || return 1
    [ "$metadata" = "660:root:$TBSTREAM_GROUP" ] || return 1
    printf '%s' "$device"
}

wait_for_verified_device() {
    local item=$1
    local deadline=$((SECONDS + TBSTREAM_DEVICE_WAIT_SECONDS))
    local device

    "$TBSTREAM_UDEVADM" settle --timeout="$TBSTREAM_DEVICE_WAIT_SECONDS" \
        >/dev/null 2>&1 || true
    while true; do
        if device=$(verify_device_once "$item"); then
            printf '%s' "$device"
            return 0
        fi
        [ "$SECONDS" -lt "$deadline" ] || return 1
        sleep "$TBSTREAM_POLL_SECONDS" || return 1
    done
}

ready_contents() {
    local item=$1
    local service_dir=$2
    local generation=$3
    local device=$4
    local index in_hopid out_hopid

    index=$(read_one_line "$item/index") || return 1
    in_hopid=$(read_one_line "$item/in_hopid") || return 1
    out_hopid=$(read_one_line "$item/out_hopid") || return 1
    printf 'service=%s\ngeneration=%s\ndevice=%s\nindex=%s\nin_hopid=%s\nout_hopid=%s\nring_size=%s\nthrottling=%s' \
        "${service_dir##*/}" "$generation" "$device" "$index" \
        "$in_hopid" "$out_hopid" "$TBSTREAM_RING_SIZE" "$TBSTREAM_THROTTLING"
}

published_ready_matches() {
    local item=$1
    local service_dir=$2
    local generation=$3
    local device expected actual published_generation link_target

    [ ! -e "$DIRTY_FILE" ] && [ ! -L "$DIRTY_FILE" ] || return 1
    [ -f "$GENERATION_FILE" ] && [ ! -L "$GENERATION_FILE" ] || return 1
    published_generation=$(read_one_line "$GENERATION_FILE" 2>/dev/null || true)
    [ "$published_generation" = "$generation" ] || return 1

    device=$(verify_device_once "$item") || return 1
    [ -L "$DEVICE_LINK" ] || return 1
    link_target=$(readlink "$DEVICE_LINK" 2>/dev/null || true)
    [ "$link_target" = "$device" ] || return 1

    [ -f "$READY_FILE" ] && [ ! -L "$READY_FILE" ] || return 1
    expected=$(ready_contents "$item" "$service_dir" "$generation" "$device") ||
        return 1
    actual=$(<"$READY_FILE") || return 1
    [ "$actual" = "$expected" ] || return 1

    service_is_bound_stream "$service_dir" || return 1
    [ "$(service_generation "$service_dir" 2>/dev/null || true)" = "$generation" ] ||
        return 1
}

publish_ready() {
    local item=$1
    local service_dir=$2
    local generation=$3
    local device=$4
    local ready

    ready=$(ready_contents "$item" "$service_dir" "$generation" "$device") ||
        return 1

    atomic_write "$GENERATION_FILE" "$generation" || return 1
    atomic_symlink "$device" "$DEVICE_LINK" || return 1
    if ! atomic_write "$READY_FILE" "$ready"; then
        rm -f "$DEVICE_LINK" 2>/dev/null || true
        return 1
    fi
    rm -f "$DIRTY_FILE" || return 1
    log "ready: ${service_dir##*/}/$TBSTREAM_NAME -> $device"
    return 0
}

reconcile_once() {
    local service_dir service_name generation previous item current_item device
    local reset=0

    discover_services
    if [ "${#LIVE_SERVICES[@]}" -eq 0 ]; then
        enter_fail_closed 'no stream service' || return "$EX_TEMPFAIL"
        cleanup_managed || return "$EX_TEMPFAIL"
        log 'no remote stream service is present'
        return "$EX_TEMPFAIL"
    fi
    if [ "${#LIVE_SERVICES[@]}" -ne 1 ]; then
        enter_fail_closed 'ambiguous stream services' || return "$EX_TEMPFAIL"
        log "expected exactly one stream service, found ${#LIVE_SERVICES[@]}"
        return "$EX_CONFIG"
    fi

    service_dir=${LIVE_SERVICES[0]}
    if ! service_is_bound_stream "$service_dir"; then
        enter_fail_closed 'stream service is not bound' || return "$EX_TEMPFAIL"
        log "stream service is not bound to thunderbolt_stream: $service_dir"
        return "$EX_TEMPFAIL"
    fi
    if ! generation=$(service_generation "$service_dir"); then
        enter_fail_closed 'could not identify stream generation' || true
        return "$EX_TEMPFAIL"
    fi
    service_name=${service_dir##*/}
    current_item="$TBSTREAM_CONFIGFS_ROOT/$service_name/$TBSTREAM_NAME"

    previous=$(read_one_line "$GENERATION_FILE" 2>/dev/null || true)
    [ -e "$DIRTY_FILE" ] && reset=1
    [ -n "$previous" ] && [ "$previous" != "$generation" ] && reset=1

    collect_managed_items
    if [ "${#MANAGED_ITEMS[@]}" -gt 1 ]; then
        log 'multiple managed ConfigFS items found; resetting stale service ordinals'
        reset=1
    elif [ "${#MANAGED_ITEMS[@]}" -eq 1 ]; then
        item=${MANAGED_ITEMS[0]}
        if [ "$item" != "$current_item" ] || ! config_attrs_valid "$item"; then
            log "managed ConfigFS item is stale or invalid: $item"
            reset=1
        fi
    fi

    if [ "$reset" -eq 1 ]; then
        withdraw_ready || return "$EX_TEMPFAIL"
        cleanup_managed || return "$EX_TEMPFAIL"
        rm -f "$GENERATION_FILE" "$DIRTY_FILE" || return "$EX_TEMPFAIL"
        # ConfigFS accepts syntactically valid but absent service names. Close the
        # discovery/create race before making a new item.
        service_is_bound_stream "$service_dir" || return "$EX_TEMPFAIL"
        [ "$(service_generation "$service_dir" 2>/dev/null || true)" = "$generation" ] ||
            return "$EX_TEMPFAIL"
        collect_managed_items
    fi

    if [ "${#MANAGED_ITEMS[@]}" -eq 0 ]; then
        # thunderbolt-net and USB4STREAM allocate HopIDs from the same finite
        # adapter pool. On boot, let a configured network link reach carrier
        # before creating a new stream endpoint so TCP keeps its stable HopIDs.
        # This gate is intentionally creation-only: once an endpoint exists,
        # carrier loss must not disturb its generation or the healthy fast path.
        if ! netdev_carrier_ready; then
            enter_fail_closed "waiting for $TBSTREAM_NETDEV carrier" ||
                return "$EX_TEMPFAIL"
            log "waiting for carrier on $TBSTREAM_NETDEV before creating a stream endpoint"
            return "$EX_TEMPFAIL"
        fi
        enter_fail_closed 'creating stream endpoint' || return "$EX_TEMPFAIL"
        if ! item=$(create_config_item "$service_name"); then
            enter_fail_closed 'stream endpoint creation failed' || true
            return "$EX_TEMPFAIL"
        fi
    else
        item=${MANAGED_ITEMS[0]}
    fi

    config_attrs_valid "$item" || {
        enter_fail_closed 'stream configuration is invalid' || true
        return "$EX_TEMPFAIL"
    }
    if published_ready_matches "$item" "$service_dir" "$generation"; then
        return 0
    fi
    # A failed identity or manifest check is a readiness transition, not a
    # reason to leave the old endpoint visible during the slower udev retry.
    withdraw_ready || return "$EX_TEMPFAIL"
    if ! device=$(wait_for_verified_device "$item"); then
        enter_fail_closed 'device verification failed' || true
        log "device verification failed for $item (node, devnum, or 0660 root:$TBSTREAM_GROUP)"
        return "$EX_TEMPFAIL"
    fi

    # Never publish a device discovered for an xdomain generation that vanished
    # while udev was settling.
    service_is_bound_stream "$service_dir" || {
        enter_fail_closed 'stream service changed during reconcile' || true
        return "$EX_TEMPFAIL"
    }
    [ "$(service_generation "$service_dir" 2>/dev/null || true)" = "$generation" ] || {
        enter_fail_closed 'stream generation changed during reconcile' || true
        return "$EX_TEMPFAIL"
    }

    if ! publish_ready "$item" "$service_dir" "$generation" "$device"; then
        enter_fail_closed 'endpoint publication failed' || true
        return "$EX_TEMPFAIL"
    fi
    return 0
}

main() {
    local deadline rc

    validate_config
    [ ! -L "$TBSTREAM_RUN_DIR" ] || die_config 'runtime directory must not be a symlink'
    mkdir -p "$TBSTREAM_RUN_DIR" || die_config 'failed to create runtime directory'
    chmod 0755 "$TBSTREAM_RUN_DIR" || die_config 'failed to set runtime-directory mode'
    command -v "$TBSTREAM_FLOCK" >/dev/null 2>&1 || die_config 'flock is required'
    command -v "$TBSTREAM_FUSER" >/dev/null 2>&1 || die_config 'fuser is required'

    if ! { exec 9>"$LOCK_FILE"; }; then
        die_config 'failed to open lifecycle lock'
    fi
    if [ "$SINGLE_PASS" -eq 1 ]; then
        # A bootstrap/udev reconcile already holding the lock will perform a
        # stronger pass; the periodic watchdog should not queue behind it.
        "$TBSTREAM_FLOCK" -n -x 9 || return 0
    else
        "$TBSTREAM_FLOCK" -x 9 || die_config 'failed to acquire lifecycle lock'
    fi

    if [ "$MODE" = cleanup ]; then
        # Keep the periodic reconciler fail-closed if this maintenance request
        # cannot finish because an fd is still open.
        mark_dirty 'explicit cleanup requested' || exit "$EX_TEMPFAIL"
        if [ -d "$TBSTREAM_CONFIGFS_ROOT" ]; then
            cleanup_managed || exit "$EX_TEMPFAIL"
        else
            withdraw_ready || exit "$EX_TEMPFAIL"
        fi
        rm -f "$GENERATION_FILE" "$DIRTY_FILE" || exit "$EX_TEMPFAIL"
        log 'managed ConfigFS state removed'
        return 0
    fi

    if [ "$TBSTREAM_TEST_MODE" != 1 ]; then
        if ! "$TBSTREAM_MODPROBE" thunderbolt_stream; then
            enter_fail_closed 'failed to load thunderbolt_stream' || true
            exit "$EX_TEMPFAIL"
        fi
    fi
    [ -d "$TBSTREAM_CONFIGFS_ROOT" ] || {
        enter_fail_closed 'ConfigFS stream root is unavailable' || true
        log "$TBSTREAM_CONFIGFS_ROOT is unavailable"
        exit "$EX_TEMPFAIL"
    }

    deadline=$((SECONDS + TBSTREAM_WAIT_SECONDS))
    while true; do
        if reconcile_once; then
            return 0
        else
            rc=$?
        fi
        [ "$rc" -ne "$EX_CONFIG" ] || return "$rc"
        [ "$SECONDS" -lt "$deadline" ] || return "$rc"
        sleep "$TBSTREAM_POLL_SECONDS" || return "$EX_TEMPFAIL"
    done
}

main
