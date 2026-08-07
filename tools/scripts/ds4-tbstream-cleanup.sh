#!/usr/bin/env bash
# Explicit maintenance cleanup for ConfigFS objects managed by DS4.
set -euo pipefail

SCRIPT_DIR=$(unset CDPATH; cd -- "$(dirname -- "$0")" && pwd)
SYSCONFIG=${TBSTREAM_SYSCONFIG:-/etc/sysconfig/ds4-tbstream}
if [ -z "${TBSTREAM_NAME+x}" ] && [ -r "$SYSCONFIG" ]; then
    set -a
    # This is the same root-owned configuration consumed by the systemd unit.
    # shellcheck disable=SC1090
    . "$SYSCONFIG"
    set +a
fi
exec "$SCRIPT_DIR/ds4-tbstream-reconcile.sh" --cleanup "$@"
