#!/usr/bin/env bash
# Offline host-lifecycle tests for the USB4STREAM endpoint. Static assertions
# on the installed-artifact templates only; live sysfs, ConfigFS, and /dev are
# untouched.
set -euo pipefail

TEST_DIR=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(unset CDPATH; cd -- "$TEST_DIR/../.." && pwd)
UDEV_DEVICE_RULE="$REPO_ROOT/tools/udev/99-tbstream.rules"

fail() {
    printf 'not ok - %s\n' "$*" >&2
    exit 1
}

test_udev_least_privilege() {
    [ -e "$UDEV_DEVICE_RULE" ] || fail "expected path to exist: $UDEV_DEVICE_RULE"
    grep -F 'MODE="0660"' "$UDEV_DEVICE_RULE" >/dev/null ||
        fail 'tbstream udev rule lost MODE=0660'
    if grep -F 'MODE="0666"' "$UDEV_DEVICE_RULE"; then
        fail 'tbstream udev rule fail-opens the DMA mmap device'
    fi
    grep -F 'GROUP="tbstream"' "$UDEV_DEVICE_RULE" >/dev/null ||
        fail 'tbstream udev rule lost the dedicated tbstream group'
    printf 'ok 1 - tbstream character devices stay 0660 root:tbstream, not world-writable\n'
}

printf '1..1\n'
test_udev_least_privilege
