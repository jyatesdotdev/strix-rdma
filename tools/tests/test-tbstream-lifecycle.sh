#!/usr/bin/env bash
# Offline lifecycle tests. The production helper is pointed exclusively at a
# temporary regular-filesystem model; live sysfs, ConfigFS, and /dev are untouched.
set -euo pipefail

TEST_DIR=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(unset CDPATH; cd -- "$TEST_DIR/../.." && pwd)
RECONCILE="$REPO_ROOT/tools/scripts/ds4-tbstream-reconcile.sh"
CLEANUP="$REPO_ROOT/tools/scripts/ds4-tbstream-cleanup.sh"
MOCK_KERNEL="$TEST_DIR/mock-tbstream-kernel.sh"
MOCK_FUSER="$TEST_DIR/mock-tbstream-fuser.sh"
RECONCILE_TIMER="$REPO_ROOT/tools/systemd/ds4-tbstream-reconcile.timer"
RECONCILE_WATCHDOG="$REPO_ROOT/tools/systemd/ds4-tbstream-reconcile-watchdog.service"

TEST_TMP=$(mktemp -d "${TMPDIR:-/tmp}/tbstream-lifecycle.XXXXXX")
case "$TEST_TMP" in
    /*/tbstream-lifecycle.*) ;;
    *) printf 'unsafe temporary path: %s\n' "$TEST_TMP" >&2; exit 1 ;;
esac
trap 'rm -rf "$TEST_TMP"' EXIT HUP INT TERM

PASS_COUNT=0
CASE_ROOT=
SYSFS_ROOT=
CONFIGFS_ROOT=
MISC_ROOT=
NET_ROOT=
DEV_ROOT=
RUN_DIR=
ROLE=
NETDEV=
WAIT_SECONDS=0
IN_HOPID=
OUT_HOPID=
LAST_OUTPUT=

pass() {
    PASS_COUNT=$((PASS_COUNT + 1))
    printf 'ok %d - %s\n' "$PASS_COUNT" "$1"
}

fail() {
    printf 'not ok - %s\n' "$*" >&2
    [ -z "$LAST_OUTPUT" ] || printf '%s\n' "$LAST_OUTPUT" >&2
    exit 1
}

assert_exists() {
    [ -e "$1" ] || fail "expected path to exist: $1"
}

assert_not_exists() {
    if [ -e "$1" ] || [ -L "$1" ]; then
        fail "expected path to be absent: $1"
    fi
}

assert_file_equals() {
    local expected=$1
    local path=$2
    local actual
    IFS= read -r actual <"$path" || true
    [ "$actual" = "$expected" ] ||
        fail "expected $path to contain '$expected', got '$actual'"
}

assert_link_target() {
    local expected=$1
    local path=$2
    local actual
    [ -L "$path" ] || fail "expected symlink: $path"
    actual=$(readlink "$path")
    [ "$actual" = "$expected" ] ||
        fail "expected $path -> $expected, got $actual"
}

path_identity() {
    local path=$1
    local value

    if value=$(stat -c '%d:%i' "$path" 2>/dev/null); then
        printf '%s' "$value"
        return 0
    fi
    if value=$(stat -f '%d:%i' "$path" 2>/dev/null); then
        printf '%s' "$value"
        return 0
    fi
    return 1
}

new_case() {
    local name=$1
    ROLE=$2
    CASE_ROOT="$TEST_TMP/$name"
    SYSFS_ROOT="$CASE_ROOT/sys/bus/thunderbolt/devices"
    CONFIGFS_ROOT="$CASE_ROOT/sys/kernel/config/thunderbolt/stream"
    MISC_ROOT="$CASE_ROOT/sys/class/misc"
    NET_ROOT="$CASE_ROOT/sys/class/net"
    DEV_ROOT="$CASE_ROOT/dev"
    RUN_DIR="$CASE_ROOT/run/ds4-tbstream"
    mkdir -p "$SYSFS_ROOT" "$CONFIGFS_ROOT" "$MISC_ROOT" "$NET_ROOT" \
        "$DEV_ROOT"
    NETDEV=
    WAIT_SECONDS=0
    IN_HOPID=
    OUT_HOPID=
    LAST_OUTPUT=
}

add_stream_service() {
    local name=$1
    local path="$SYSFS_ROOT/$name"
    mkdir -p "$path"
    printf '%s\n' stream >"$path/key"
    ln -s /mock/drivers/thunderbolt_stream "$path/driver"
}

remove_stream_service() {
    local name=$1
    local path="$SYSFS_ROOT/$name"
    case "$path" in
        "$CASE_ROOT"/*) rm -rf "$path" ;;
        *) fail "refusing unsafe mock service removal: $path" ;;
    esac
}

seed_item() {
    local service=$1
    local index=$2
    local in_hopid=${3:-9}
    local out_hopid=${4:-9}
    TBSTREAM_TEST_ROOT="$CASE_ROOT" \
    TBSTREAM_CONFIGFS_ROOT="$CONFIGFS_ROOT" \
    TBSTREAM_MISC_ROOT="$MISC_ROOT" \
    TBSTREAM_NET_ROOT="$NET_ROOT" \
    TBSTREAM_DEV_ROOT="$DEV_ROOT" \
    TBSTREAM_RUN_DIR="$RUN_DIR" \
        "$MOCK_KERNEL" seed "$CONFIGFS_ROOT/$service/ds4" "$index" \
            "$in_hopid" "$out_hopid" 4096 0
}

run_helper() {
    local expected_rc=$1
    shift
    local actual_rc

    set +e
    LAST_OUTPUT=$(env \
        TBSTREAM_TEST_MODE=1 \
        TBSTREAM_TEST_ROOT="$CASE_ROOT" \
        TBSTREAM_TEST_HOOK="$MOCK_KERNEL" \
        TBSTREAM_ROLE="$ROLE" \
        TBSTREAM_NAME=ds4 \
        TBSTREAM_RING_SIZE=4096 \
        TBSTREAM_THROTTLING=0 \
        TBSTREAM_IN_HOPID="$IN_HOPID" \
        TBSTREAM_OUT_HOPID="$OUT_HOPID" \
        TBSTREAM_NETDEV="$NETDEV" \
        TBSTREAM_WAIT_SECONDS="$WAIT_SECONDS" \
        TBSTREAM_HOLDER_WAIT_SECONDS=0 \
        TBSTREAM_DEVICE_WAIT_SECONDS=0 \
        TBSTREAM_POLL_SECONDS=1 \
        TBSTREAM_GROUP=tbstream \
        TBSTREAM_SYSFS_ROOT="$SYSFS_ROOT" \
        TBSTREAM_CONFIGFS_ROOT="$CONFIGFS_ROOT" \
        TBSTREAM_MISC_ROOT="$MISC_ROOT" \
        TBSTREAM_NET_ROOT="$NET_ROOT" \
        TBSTREAM_DEV_ROOT="$DEV_ROOT" \
        TBSTREAM_RUN_DIR="$RUN_DIR" \
        TBSTREAM_FUSER="$MOCK_FUSER" \
        TBSTREAM_FLOCK=/usr/bin/true \
        TBSTREAM_UDEVADM=/usr/bin/true \
        "$@" 2>&1)
    actual_rc=$?
    set -e
    [ "$actual_rc" -eq "$expected_rc" ] ||
        fail "expected exit $expected_rc, got $actual_rc from $*"
}

test_allocator_and_active_idempotence() {
    local link_identity ready_identity generation_identity

    new_case allocator allocator
    add_stream_service 1-2.1
    run_helper 0 "$RECONCILE"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    assert_file_equals 9 "$CONFIGFS_ROOT/1-2.1/ds4/in_hopid"
    assert_file_equals 4096 "$CONFIGFS_ROOT/1-2.1/ds4/ring_size"
    link_identity=$(path_identity "$RUN_DIR/device")
    ready_identity=$(path_identity "$RUN_DIR/ready")
    generation_identity=$(path_identity "$RUN_DIR/generation")
    # Carrier ordering applies only to a new ConfigFS child. Once this valid
    # endpoint exists, even a missing configured netdev must not perturb it.
    NETDEV=thunderbolt0
    printf '%s\n' "$DEV_ROOT/tbstream0" >"$CASE_ROOT/holders"
    run_helper 0 "$RECONCILE"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    [ "$(path_identity "$RUN_DIR/device")" = "$link_identity" ] ||
        fail 'steady-state reconcile replaced the stable device link'
    [ "$(path_identity "$RUN_DIR/ready")" = "$ready_identity" ] ||
        fail 'steady-state reconcile rewrote the ready file'
    [ "$(path_identity "$RUN_DIR/generation")" = "$generation_identity" ] ||
        fail 'steady-state reconcile rewrote the generation file'
    if printf '%s\n' "$LAST_OUTPUT" | grep -Fq 'ds4-tbstream: ready:'; then
        fail 'steady-state reconcile logged a duplicate ready transition'
    fi
    pass 'allocator fast path ignores carrier loss after endpoint creation'
}

test_exact_allocator_hopids() {
    new_case exact-allocator allocator
    add_stream_service 1-2.0
    IN_HOPID=9
    OUT_HOPID=10
    run_helper 0 "$RECONCILE"
    assert_file_equals '9:10' "$CASE_ROOT/requested-hopids"
    assert_not_exists "$CASE_ROOT/automatic-hopids-requested"
    assert_file_equals 9 "$CONFIGFS_ROOT/1-2.0/ds4/in_hopid"
    assert_file_equals 10 "$CONFIGFS_ROOT/1-2.0/ds4/out_hopid"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    pass 'allocator requests and publishes the configured exact local HopID pair'
}

test_exact_follower_hopids() {
    new_case exact-follower follower
    add_stream_service 1-2.0
    IN_HOPID=10
    OUT_HOPID=9
    printf '%s\n' '9:10' >"$CASE_ROOT/remote-hopids"
    run_helper 0 "$RECONCILE"
    assert_file_equals 10 "$CONFIGFS_ROOT/1-2.0/ds4/in_hopid"
    assert_file_equals 9 "$CONFIGFS_ROOT/1-2.0/ds4/out_hopid"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"

    new_case exact-follower-mismatch follower
    add_stream_service 1-2.0
    IN_HOPID=10
    OUT_HOPID=9
    printf '%s\n' '9:11' >"$CASE_ROOT/remote-hopids"
    run_helper 75 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    pass 'follower validates the reversed remote pair against both exact local HopIDs'
}

test_exact_hopid_configuration_validation() {
    new_case partial-hopids allocator
    add_stream_service 1-2.0
    IN_HOPID=9
    run_helper 78 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"

    new_case invalid-hopid-text allocator
    add_stream_service 1-2.0
    IN_HOPID=nine
    OUT_HOPID=9
    run_helper 78 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"

    new_case invalid-hopid-low allocator
    add_stream_service 1-2.0
    IN_HOPID=7
    OUT_HOPID=9
    run_helper 78 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"

    new_case invalid-hopid-high allocator
    add_stream_service 1-2.0
    IN_HOPID=9
    OUT_HOPID=2048
    run_helper 78 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    pass 'partial, non-numeric, and out-of-range exact HopID pairs are rejected'
}

test_exact_allocator_result_mismatch() {
    new_case exact-result-mismatch allocator
    add_stream_service 1-2.0
    IN_HOPID=9
    OUT_HOPID=10
    printf '%s\n' '9:11' >"$CASE_ROOT/result-hopids"
    run_helper 75 "$RECONCILE"
    assert_file_equals '9:10' "$CASE_ROOT/requested-hopids"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    if ! printf '%s\n' "$LAST_OUTPUT" | grep -Fq \
        'did not acquire configured local HopIDs 9/10'; then
        fail 'exact result mismatch did not report its configured pair'
    fi
    pass 'a non-exact allocator result is deactivated and never published'
}

test_exact_allocator_has_no_automatic_fallback() {
    new_case exact-unavailable allocator
    add_stream_service 1-2.0
    IN_HOPID=9
    OUT_HOPID=9
    : >"$CASE_ROOT/exact-hopids-unavailable"
    run_helper 75 "$RECONCILE"
    assert_file_equals '9:9' "$CASE_ROOT/requested-hopids"
    assert_not_exists "$CASE_ROOT/automatic-hopids-requested"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    pass 'unavailable exact HopIDs fail closed without retrying automatic allocation'
}

test_netdev_carrier_wait_then_success() {
    new_case carrier-success allocator
    add_stream_service 1-2.0
    NETDEV=thunderbolt0
    WAIT_SECONDS=2
    printf '%s\n' 2 >"$CASE_ROOT/carrier-ready-after"
    run_helper 0 "$RECONCILE"
    assert_file_equals 2 "$CASE_ROOT/carrier-polls"
    assert_file_equals 1 "$NET_ROOT/thunderbolt0/carrier"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    pass 'new endpoint waits for configured Thunderbolt netdev carrier'
}

test_netdev_carrier_timeout() {
    new_case carrier-timeout allocator
    add_stream_service 1-2.0
    NETDEV=thunderbolt0
    WAIT_SECONDS=1
    run_helper 75 "$RECONCILE"
    assert_file_equals 2 "$CASE_ROOT/carrier-polls"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    assert_file_equals 'waiting for thunderbolt0 carrier' "$RUN_DIR/dirty"
    pass 'carrier timeout fails closed without consuming stream HopIDs'
}

test_netdev_carrier_single_pass_is_bounded() {
    new_case carrier-once allocator
    add_stream_service 1-2.0
    NETDEV=thunderbolt0
    WAIT_SECONDS=5
    printf '%s\n' 2 >"$CASE_ROOT/carrier-ready-after"
    run_helper 75 "$RECONCILE" --once
    assert_file_equals 1 "$CASE_ROOT/carrier-polls"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    pass 'single-pass watchdog checks carrier once without waiting'
}

test_netdev_configuration_is_path_safe() {
    new_case carrier-config allocator
    add_stream_service 1-2.0
    NETDEV=../thunderbolt0
    run_helper 78 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    pass 'unsafe netdev names are rejected before filesystem access'
}

test_follower_waits_for_advertised_hopids() {
    new_case follower follower
    add_stream_service 1-2.0
    run_helper 75 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$RUN_DIR/device"
    printf '%s\n' '11:12' >"$CASE_ROOT/remote-hopids"
    run_helper 0 "$RECONCILE"
    assert_file_equals 12 "$CONFIGFS_ROOT/1-2.0/ds4/in_hopid"
    assert_file_equals 11 "$CONFIGFS_ROOT/1-2.0/ds4/out_hopid"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    pass 'follower stays unpublished until it can learn and reverse allocator HopIDs'
}

test_stale_minor_recovery() {
    new_case stale-minor allocator
    add_stream_service 1-2.0
    # Reproduce the observed kernel state: detached .1 retained ID 0 while a
    # newly enumerated .0 service allocated ID 1.
    seed_item 1-2.1 0
    seed_item 1-2.0 1
    run_helper 0 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.1/ds4"
    assert_not_exists "$DEV_ROOT/tbstream1"
    assert_file_equals 0 "$CONFIGFS_ROOT/1-2.0/ds4/index"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    pass 'ordinal-swap stale minor0/new minor1 is cleaned and converges back to minor0'
}

test_generation_swap_without_observed_gap() {
    new_case generation-swap allocator
    add_stream_service 1-2.1
    run_helper 0 "$RECONCILE"
    remove_stream_service 1-2.1
    add_stream_service 1-2.0
    run_helper 0 "$RECONCILE"
    assert_not_exists "$CONFIGFS_ROOT/1-2.1/ds4"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    assert_file_equals 0 "$CONFIGFS_ROOT/1-2.0/ds4/index"
    pass 'service identity change resets the detached ordinal even if polling misses the down gap'
}

test_holder_blocks_reset() {
    new_case held-stale allocator
    add_stream_service 1-2.0
    seed_item 1-2.1 0
    seed_item 1-2.0 1
    mkdir -p "$RUN_DIR"
    ln -s "$DEV_ROOT/tbstream1" "$RUN_DIR/device"
    printf '%s\n' ready >"$RUN_DIR/ready"
    printf '%s\n' "$DEV_ROOT/tbstream0" >"$CASE_ROOT/holders"
    run_helper 75 "$RECONCILE"
    assert_not_exists "$RUN_DIR/device"
    assert_exists "$CONFIGFS_ROOT/1-2.1/ds4"
    assert_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    pass 'stale reset withdraws readiness and fails closed while any device is held'
}

test_multiple_services_are_ambiguous() {
    new_case ambiguous allocator
    add_stream_service 1-2.0
    add_stream_service 2-3.0
    run_helper 78 "$RECONCILE"
    assert_not_exists "$RUN_DIR/device"
    [ -z "$(find "$CONFIGFS_ROOT" -type d -name ds4 -print -quit)" ] ||
        fail 'ambiguous discovery unexpectedly created ConfigFS state'
    pass 'multiple live stream services fail closed without first-match selection'
}

test_verification_precedes_publication() {
    new_case bad-device allocator
    add_stream_service 1-2.0
    seed_item 1-2.0 0
    run_helper 0 "$RECONCILE"
    assert_link_target "$DEV_ROOT/tbstream0" "$RUN_DIR/device"
    printf '%s\n' '660:root:wrong-group' >"$DEV_ROOT/tbstream0.mock-meta"
    run_helper 75 "$RECONCILE"
    assert_not_exists "$RUN_DIR/device"
    assert_not_exists "$RUN_DIR/ready"
    assert_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    pass 'device number and least-privilege metadata are verified before stable-link publication'
}

test_explicit_cleanup_refuses_then_succeeds() {
    new_case cleanup allocator
    add_stream_service 1-2.0
    run_helper 0 "$RECONCILE"
    printf '%s\n' "$DEV_ROOT/tbstream0" >"$CASE_ROOT/holders"
    ROLE=
    run_helper 75 "$CLEANUP"
    assert_not_exists "$RUN_DIR/device"
    assert_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    rm -f "$CASE_ROOT/holders"
    run_helper 0 "$CLEANUP"
    assert_not_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    assert_not_exists "$DEV_ROOT/tbstream0"
    assert_not_exists "$RUN_DIR/generation"
    assert_file_equals "$CONFIGFS_ROOT/1-2.0/ds4" "$CASE_ROOT/withdrawals"
    pass 'explicit cleanup never forces a held item and removes no /dev node itself'
}

test_fuser_error_fails_closed() {
    new_case fuser-error allocator
    add_stream_service 1-2.0
    seed_item 1-2.1 0
    seed_item 1-2.0 1
    : >"$CASE_ROOT/fuser-error"
    run_helper 75 "$RECONCILE"
    assert_not_exists "$RUN_DIR/device"
    assert_exists "$CONFIGFS_ROOT/1-2.1/ds4"
    assert_exists "$CONFIGFS_ROOT/1-2.0/ds4"
    pass 'an operational fuser error is not mistaken for an empty holder set'
}

test_publication_failure_withdraws_link() {
    new_case publication-failure allocator
    add_stream_service 1-2.0
    : >"$CASE_ROOT/fail-ready-publication"
    run_helper 75 "$RECONCILE"
    assert_not_exists "$RUN_DIR/device"
    assert_exists "$RUN_DIR/dirty"
    if printf '%s\n' "$LAST_OUTPUT" | grep -Fq 'ds4-tbstream: ready:'; then
        fail 'helper reported ready after injected atomic publication failure'
    fi
    pass 'injected readiness-write failure cannot leave or report a published device link'
}

test_timer_uses_low_churn_fallback() {
    grep -Fx 'OnUnitInactiveSec=10s' "$RECONCILE_TIMER" >/dev/null ||
        fail 'reconcile timer is not using the 10-second fallback interval'
    grep -Fx 'RandomizedDelaySec=1s' "$RECONCILE_TIMER" >/dev/null ||
        fail 'reconcile timer does not stagger peer fallback checks'
    grep -Fx 'Unit=ds4-tbstream-reconcile-watchdog.service' \
        "$RECONCILE_TIMER" >/dev/null ||
        fail 'reconcile timer does not target the bounded watchdog service'
    grep -Fx 'ExecStart=/usr/local/libexec/ds4-tbstream-reconcile.sh --once' \
        "$RECONCILE_WATCHDOG" >/dev/null ||
        fail 'periodic watchdog is not invoking single-pass mode'
    grep -F 'TBSTREAM_DEVICE_WAIT_SECONDS=0' "$RECONCILE" >/dev/null ||
        fail 'single-pass mode can still wait for a device node'
    grep -F 'TBSTREAM_FLOCK" -n -x 9' "$RECONCILE" >/dev/null ||
        fail 'single-pass mode can still queue behind bootstrap reconciliation'
    pass 'udev add uses bootstrap while the 10-second watchdog is single-pass'
}

printf '1..20\n'
test_allocator_and_active_idempotence
test_exact_allocator_hopids
test_exact_follower_hopids
test_exact_hopid_configuration_validation
test_exact_allocator_result_mismatch
test_exact_allocator_has_no_automatic_fallback
test_netdev_carrier_wait_then_success
test_netdev_carrier_timeout
test_netdev_carrier_single_pass_is_bounded
test_netdev_configuration_is_path_safe
test_follower_waits_for_advertised_hopids
test_stale_minor_recovery
test_generation_swap_without_observed_gap
test_holder_blocks_reset
test_multiple_services_are_ambiguous
test_verification_precedes_publication
test_explicit_cleanup_refuses_then_succeeds
test_fuser_error_fails_closed
test_publication_failure_withdraws_link
test_timer_uses_low_churn_fallback
