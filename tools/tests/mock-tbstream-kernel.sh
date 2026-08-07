#!/usr/bin/env bash
# Tiny regular-filesystem model of the ConfigFS/misc-device lifecycle used only
# by test-tbstream-lifecycle.sh. All mutable paths must stay below TEST_ROOT.
set -euo pipefail

: "${TBSTREAM_TEST_ROOT:?}"
: "${TBSTREAM_CONFIGFS_ROOT:?}"
: "${TBSTREAM_MISC_ROOT:?}"
: "${TBSTREAM_NET_ROOT:?}"
: "${TBSTREAM_DEV_ROOT:?}"
: "${TBSTREAM_RUN_DIR:?}"

case "$TBSTREAM_TEST_ROOT" in
    /*) ;;
    *) exit 98 ;;
esac
[ "$TBSTREAM_TEST_ROOT" != / ] || exit 98
for path in "$TBSTREAM_CONFIGFS_ROOT" "$TBSTREAM_MISC_ROOT" \
            "$TBSTREAM_NET_ROOT" "$TBSTREAM_DEV_ROOT" "$TBSTREAM_RUN_DIR"; do
    case "$path" in
        "$TBSTREAM_TEST_ROOT"|"$TBSTREAM_TEST_ROOT"/*) ;;
        *) exit 98 ;;
    esac
done

index_in_use() {
    local wanted=$1
    local path value

    for path in "$TBSTREAM_CONFIGFS_ROOT"/*/*/index; do
        [ -r "$path" ] || continue
        IFS= read -r value <"$path" || true
        [ "$value" = "$wanted" ] && return 0
    done
    return 1
}

make_item() {
    local item=$1
    local index=$2
    local in_hopid=$3
    local out_hopid=$4
    local ring_size=${5:-256}
    local throttling=${6:-8192}
    local devnum="10:$((260 + index))"
    local device="$TBSTREAM_DEV_ROOT/tbstream$index"
    local misc="$TBSTREAM_MISC_ROOT/tbstream$index"

    mkdir -p "$item" "$misc" "$TBSTREAM_DEV_ROOT"
    printf '%s\n' "$index" >"$item/index"
    printf '%s\n' "$in_hopid" >"$item/in_hopid"
    printf '%s\n' "$out_hopid" >"$item/out_hopid"
    printf '%s\n' "$ring_size" >"$item/ring_size"
    printf '%s\n' "$throttling" >"$item/throttling"
    printf '%s\n' "$devnum" >"$misc/dev"
    : >"$device"
    printf '%s\n' "$devnum" >"$device.mock-dev"
    printf '%s\n' '660:root:tbstream' >"$device.mock-meta"
}

remove_item_files() {
    local item=$1
    local index device misc

    index=
    if [ -r "$item/index" ]; then
        IFS= read -r index <"$item/index" || true
    fi
    case "$index" in
        ''|*[!0-9]*) ;;
        *)
            device="$TBSTREAM_DEV_ROOT/tbstream$index"
            misc="$TBSTREAM_MISC_ROOT/tbstream$index"
            rm -f "$device" "$device.mock-dev" "$device.mock-meta" "$misc/dev"
            rmdir "$misc" 2>/dev/null || true
            ;;
    esac
    rm -f "$item/index" "$item/in_hopid" "$item/out_hopid" \
          "$item/ring_size" "$item/throttling"
}

command=${1:-}
shift || true
case "$command" in
    carrier_poll)
        carrier_path=${1:?}
        case "$carrier_path" in
            "$TBSTREAM_NET_ROOT"/*/carrier) ;;
            *) exit 98 ;;
        esac
        poll_count=0
        if [ -r "$TBSTREAM_TEST_ROOT/carrier-polls" ]; then
            IFS= read -r poll_count <"$TBSTREAM_TEST_ROOT/carrier-polls" || true
        fi
        case "$poll_count" in
            ''|*[!0-9]*) exit 98 ;;
        esac
        poll_count=$((poll_count + 1))
        printf '%s\n' "$poll_count" >"$TBSTREAM_TEST_ROOT/carrier-polls"
        if [ -r "$TBSTREAM_TEST_ROOT/carrier-ready-after" ]; then
            IFS= read -r ready_after <"$TBSTREAM_TEST_ROOT/carrier-ready-after" || true
            case "$ready_after" in
                ''|*[!0-9]*) exit 98 ;;
            esac
            if [ "$poll_count" -ge "$ready_after" ]; then
                mkdir -p "${carrier_path%/carrier}"
                printf '%s\n' 1 >"$carrier_path"
            fi
        fi
        ;;
    create)
        item=${1:?}
        role=${2:?}
        index=0
        while index_in_use "$index"; do
            index=$((index + 1))
        done
        if [ "$role" = follower ] && [ -r "$TBSTREAM_TEST_ROOT/remote-hopids" ]; then
            IFS=: read -r remote_in remote_out <"$TBSTREAM_TEST_ROOT/remote-hopids"
            make_item "$item" "$index" "$remote_out" "$remote_in"
        else
            make_item "$item" "$index" 0 0
        fi
        ;;
    configure)
        item=${1:?}
        role=${2:?}
        if [ "$role" = allocator ]; then
            IFS= read -r requested_in <"$item/in_hopid" || true
            IFS= read -r requested_out <"$item/out_hopid" || true
            printf '%s:%s\n' "$requested_in" "$requested_out" \
                >>"$TBSTREAM_TEST_ROOT/requested-hopids"
            if [ "$requested_in" = -1 ] && [ "$requested_out" = -1 ]; then
                : >"$TBSTREAM_TEST_ROOT/automatic-hopids-requested"
                printf '%s\n' 9 >"$item/in_hopid"
                printf '%s\n' 9 >"$item/out_hopid"
            elif [ -e "$TBSTREAM_TEST_ROOT/exact-hopids-unavailable" ]; then
                exit 1
            fi
        elif [ -r "$TBSTREAM_TEST_ROOT/remote-hopids" ]; then
            IFS=: read -r remote_in remote_out <"$TBSTREAM_TEST_ROOT/remote-hopids"
            printf '%s\n' "$remote_out" >"$item/in_hopid"
            printf '%s\n' "$remote_in" >"$item/out_hopid"
        fi
        if [ -r "$TBSTREAM_TEST_ROOT/result-hopids" ]; then
            IFS=: read -r result_in result_out <"$TBSTREAM_TEST_ROOT/result-hopids"
            printf '%s\n' "$result_in" >"$item/in_hopid"
            printf '%s\n' "$result_out" >"$item/out_hopid"
        fi
        if [ -e "$TBSTREAM_TEST_ROOT/fail-ready-publication" ]; then
            mkdir -p "$TBSTREAM_RUN_DIR/ready"
        fi
        ;;
    remove)
        remove_item_files "${1:?}"
        ;;
    deactivate)
        item=${1:?}
        [ "$(<"$item/in_hopid")" = 0 ]
        [ "$(<"$item/out_hopid")" = 0 ]
        printf '%s\n' "$item" >>"$TBSTREAM_TEST_ROOT/withdrawals"
        ;;
    seed)
        item=${1:?}
        index=${2:?}
        in_hopid=${3:-9}
        out_hopid=${4:-9}
        ring_size=${5:-4096}
        throttling=${6:-0}
        make_item "$item" "$index" "$in_hopid" "$out_hopid" \
            "$ring_size" "$throttling"
        ;;
    *)
        printf 'mock-tbstream-kernel: unknown command %s\n' "$command" >&2
        exit 64
        ;;
esac
