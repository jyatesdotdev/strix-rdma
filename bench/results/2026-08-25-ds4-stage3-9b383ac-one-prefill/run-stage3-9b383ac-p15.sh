#!/usr/bin/env bash
set -u

SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
MAX=jryates@max
MAX2=jryates@max2
CAP=/tmp/stage3-9b383ac-p15-arm-capture
R0_LOG=/tmp/stage3-9b383ac-tp-r0.log
R1_LOG=/tmp/stage3-9b383ac-tp-r1.log
R0_RC=/tmp/stage3-9b383ac-tp-r0.rc
R1_RC=/tmp/stage3-9b383ac-tp-r1.rc
R0_NORM=/tmp/stage3-9b383ac-tp-r0_result_norm-43_pos0.bin
R1_NORM=/tmp/stage3-9b383ac-tp-r1_result_norm-43_pos0.bin
R0_DUMP=/tmp/stage3-9b383ac-tp-r0_result_output-43_pos0.bin
R1_DUMP=/tmp/stage3-9b383ac-tp-r1_result_output-43_pos0.bin
LOCAL_R0="$CAP/rank0-ssh.log"
LOCAL_R1="$CAP/rank1-ssh.log"
MODEL=/home/jryates/models/ds4-pr-20260801/DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf
BIN=/home/jryates/ds4-stage3-build/ds4
EXPECTED_BIN=4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7
p0=
p1=
disarmed=0

remote_stop_one() {
    local host=$1
    "${SSH[@]}" "$host" 'sudo -n bash -s' <<'EOS' || true
set -u
f=/run/ds4-stage3.lock
[ -r "$f" ] || exit 0
p=$(cat "$f" 2>/dev/null || true)
case "$p" in ''|*[!0-9]*) exit 0;; esac
[ -r "/proc/$p/cmdline" ] || exit 0
cmd=$(tr '\0' ' ' < "/proc/$p/cmdline")
case "$cmd" in
  *'/home/jryates/ds4-stage3-build/ds4'*'--tensor-parallel'*)
    kill -TERM "$p" 2>/dev/null || true
    ;;
esac
EOS
}

abort_children() {
    [ "$disarmed" -eq 0 ] || return 0
    echo 'orchestrator: aborting worker first, then coordinator' >&2
    remote_stop_one "$MAX"
    sleep 2
    remote_stop_one "$MAX2"
    sleep 3
    if [ -n "$p1" ]; then kill "$p1" 2>/dev/null || true; fi
    if [ -n "$p0" ]; then kill "$p0" 2>/dev/null || true; fi
}
trap abort_children EXIT INT TERM

# Recheck the exact staged binary before touching this arm's output names.
for host in "$MAX2" "$MAX"; do
    actual=$("${SSH[@]}" "$host" "sha256sum $BIN | awk '{print \$1}'") || exit 1
    if [ "$actual" != "$EXPECTED_BIN" ]; then
        echo "orchestrator: staged binary hash mismatch on $host: $actual" >&2
        exit 1
    fi
done

# Remove only this arm's exact output names after proving no staged process exists.
for spec in "$MAX2:$R0_LOG:$R0_RC:$R0_NORM:$R0_DUMP" "$MAX:$R1_LOG:$R1_RC:$R1_NORM:$R1_DUMP"; do
    IFS=: read -r host log rc norm dump <<<"$spec"
    "${SSH[@]}" "$host" "sudo -n bash -c 'for x in /proc/[0-9]*/exe; do [ \"\$(readlink \"\$x\" 2>/dev/null || true)\" = $BIN ] && exit 1; done; rm -f $log $rc $norm $dump /run/ds4-stage3.lock'" || exit 1
done

# Rank 0 coordinator. The remote pipeline preserves the ds4 exit code and a full host-side log.
"${SSH[@]}" "$MAX2" "set -o pipefail; sudo -n env LD_LIBRARY_PATH=/opt/rocm-therock/lib DS4_LOCK_FILE=/run/ds4-stage3.lock DS4_TP_SPIN_MAX=800000000 DS4_MTP_SPEC_DISABLE=1 DS4_ROCM_GRAPH_DUMP_PREFIX=/tmp/stage3-9b383ac-tp-r0 DS4_ROCM_GRAPH_DUMP_NAME=result_norm,result_output DS4_ROCM_GRAPH_DUMP_LAYER=43 $BIN --rocm -m $MODEL -c 8192 -n 1 -p Hello --raw-prompt --temp 0 --tensor-parallel --role coordinator --listen 10.99.0.2 5601 --transport nhi --nhi-device /run/ds4-tbstream/device 2>&1 | tee $R0_LOG; rc=\${PIPESTATUS[0]}; printf '%s\n' \"\$rc\" >$R0_RC; exit \"\$rc\"" >"$LOCAL_R0" 2>&1 &
p0=$!
echo "orchestrator: rank0 ssh pid=$p0"

listen=0
deadline=$((SECONDS + 240))
while [ "$SECONDS" -lt "$deadline" ]; do
    if "${SSH[@]}" "$MAX2" "grep -Fq 'ds4-tp: waiting for worker on 10.99.0.2:5601' $R0_LOG 2>/dev/null"; then
        listen=1
        break
    fi
    if ! kill -0 "$p0" 2>/dev/null; then
        echo 'orchestrator: rank0 exited before worker-listen barrier' >&2
        break
    fi
    sleep 2
done
if [ "$listen" -ne 1 ]; then
    echo 'orchestrator: worker-listen barrier not reached' >&2
    wait "$p0" 2>/dev/null || true
    exit 1
fi
echo 'orchestrator: rank0 worker-listen barrier reached'

# Rank 1 worker, exactly once.
"${SSH[@]}" "$MAX" "set -o pipefail; sudo -n env LD_LIBRARY_PATH=/opt/rocm-therock/lib DS4_LOCK_FILE=/run/ds4-stage3.lock DS4_TP_SPIN_MAX=800000000 DS4_MTP_SPEC_DISABLE=1 DS4_ROCM_GRAPH_DUMP_PREFIX=/tmp/stage3-9b383ac-tp-r1 DS4_ROCM_GRAPH_DUMP_NAME=result_norm,result_output DS4_ROCM_GRAPH_DUMP_LAYER=43 $BIN --rocm -m $MODEL -c 8192 -n 1 -p Hello --raw-prompt --temp 0 --tensor-parallel --role worker --coordinator 10.99.0.2 5601 --transport nhi --nhi-device /run/ds4-tbstream/device 2>&1 | tee $R1_LOG; rc=\${PIPESTATUS[0]}; printf '%s\n' \"\$rc\" >$R1_RC; exit \"\$rc\"" >"$LOCAL_R1" 2>&1 &
p1=$!
echo "orchestrator: rank1 ssh pid=$p1"

deadline=$((SECONDS + 600))
timed_out=0
while kill -0 "$p0" 2>/dev/null || kill -0 "$p1" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        timed_out=1
        echo 'orchestrator: bilateral completion timeout' >&2
        break
    fi
    sleep 2
done
if [ "$timed_out" -eq 1 ]; then
    exit 1
fi

wait "$p0"; ssh0=$?
wait "$p1"; ssh1=$?
disarmed=1
trap - EXIT INT TERM
r0=$("${SSH[@]}" "$MAX2" "cat $R0_RC 2>/dev/null || echo missing")
r1=$("${SSH[@]}" "$MAX" "cat $R1_RC 2>/dev/null || echo missing")
printf 'orchestrator: ssh_rc rank0=%s rank1=%s ds4_rc rank0=%s rank1=%s\n' "$ssh0" "$ssh1" "$r0" "$r1"
[ "$ssh0" -eq 0 ] && [ "$ssh1" -eq 0 ] && [ "$r0" = 0 ] && [ "$r1" = 0 ]
