#!/usr/bin/env bash
set -euo pipefail

: "${TBSTREAM_TEST_ROOT:?}"
device=${1:?}
holders="$TBSTREAM_TEST_ROOT/holders"
if [ -e "$TBSTREAM_TEST_ROOT/fuser-error" ]; then
    printf 'simulated fuser failure\n' >&2
    exit 2
fi
[ -r "$holders" ] && grep -Fqx -- "$device" "$holders"
