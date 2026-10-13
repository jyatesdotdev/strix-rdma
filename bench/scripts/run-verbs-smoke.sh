#!/usr/bin/env bash
# Run one strix-verbs-smoke exchange between two hosts over ssh: the listener
# detached on LISTEN, the connector in the foreground on CONNECT.
#
# Usage: run-verbs-smoke.sh [mode...]
#   mode: empty (SEND/RECV smoke) | <iters> | ping [iters] | write [iters] | read [iters]
#
# Environment (required):
#   CONNECT=user@hostA   ssh target that runs "connect"
#   LISTEN=user@hostB    ssh target that runs "listen"
#   LISTEN_IP=a.b.c.d    LISTEN's explicit IPv4 for the TCP rendezvous
# Environment (optional):
#   PORT=19476           TCP rendezvous port; must be open in LISTEN's firewall
#   DEV=strix_nhi0       native verbs device on both hosts
#   TOOL_DIR=~/strix-rdma/tools/verbs   built strix-verbs-smoke on both hosts
#   SMOKE_FLAGS=--gpu    flags placed before --live (e.g. GPU-memory buffers)
#   SMOKE_ENV='A=1 B=2'  environment for the remote tool (e.g. STRIX_GPU_NO_ACQUIRE=1)
#
# Prints the connector's tail, then the listener's log, and exits with the
# connector's status. The listener log stays on LISTEN at $LISTEN_LOG.
set -u
: "${CONNECT:?set CONNECT=user@host}" "${LISTEN:?set LISTEN=user@host}"
: "${LISTEN_IP:?set LISTEN_IP to the listener IPv4}"
PORT=${PORT:-19476}
DEV=${DEV:-strix_nhi0}
TOOL_DIR=${TOOL_DIR:-'~/strix-rdma/tools/verbs'}
LISTEN_LOG=${LISTEN_LOG:-/tmp/strix-verbs-smoke-listen.log}
MODE="$*"
SSH=(ssh -o BatchMode=yes)

# The smoke tool registers MRs, so it runs with an unlimited memlock limit and
# then drops back to the login user rather than running as root.
smoke() { # host role
    local user
    user=$("${SSH[@]}" "$1" id -un)
    printf "sudo sh -c 'ulimit -l unlimited; exec sudo -u %s sh -c \"cd %s && exec env %s ./strix-verbs-smoke %s --live %s %s %s %s %s\"'" \
        "$user" "$TOOL_DIR" "${SMOKE_ENV:-}" "${SMOKE_FLAGS:-}" "$DEV" "$2" "$LISTEN_IP" "$PORT" "$MODE"
}

listen_cmd=$(smoke "$LISTEN" listen)
"${SSH[@]}" "$LISTEN" "setsid nohup $listen_cmd >$LISTEN_LOG 2>&1 </dev/null &" 2>/dev/null
sleep 4
echo "--- connector ($CONNECT), mode='${MODE:-SEND/RECV smoke}':"
"${SSH[@]}" "$CONNECT" "$(smoke "$CONNECT" connect)" 2>&1 | tail -14
rc=${PIPESTATUS[0]}
sleep 2
echo "--- listener ($LISTEN):"
"${SSH[@]}" "$LISTEN" "tail -8 $LISTEN_LOG" 2>/dev/null
echo "connector rc=$rc"
exit "$rc"
