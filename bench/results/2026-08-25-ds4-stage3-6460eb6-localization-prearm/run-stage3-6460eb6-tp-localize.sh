#!/usr/bin/env bash
set -u
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
MAX=jryates@max
MAX2=jryates@max2
BIN=/home/jryates/ds4-stage3-build/ds4
MODEL=/home/jryates/models/ds4-pr-20260801/DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf
EXPECTED_BIN=4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7
EXPECTED_MODEL_BYTES=155976458848
R0_PREFIX=/tmp/stage3-6460eb6-tp-r0-dump
R1_PREFIX=/tmp/stage3-6460eb6-tp-r1-dump
R0_LOG=/tmp/stage3-6460eb6-tp-r0.log
R1_LOG=/tmp/stage3-6460eb6-tp-r1.log
R0_RC=/tmp/stage3-6460eb6-tp-r0.rc
R1_RC=/tmp/stage3-6460eb6-tp-r1.rc
LOCK=/run/ds4-stage3-localize-tp.lock
LOCAL_R0=/tmp/stage3-6460eb6-tp-r0-ssh.log
LOCAL_R1=/tmp/stage3-6460eb6-tp-r1-ssh.log
p0=
p1=
disarmed=0
remote_stop_one() {
    local host=$1
    "${SSH[@]}" "$host" 'sudo -n bash -s' <<'EOS' || true
set -u
f=/run/ds4-stage3-localize-tp.lock
[ -r "$f" ] || exit 0
p=$(cat "$f" 2>/dev/null || true)
case "$p" in ''|*[!0-9]*) exit 0;; esac
[ -r "/proc/$p/cmdline" ] || exit 0
cmd=$(tr '\0' ' ' < "/proc/$p/cmdline")
case "$cmd" in
  *'/home/jryates/ds4-stage3-build/ds4'*'--tensor-parallel'*) kill -TERM "$p" 2>/dev/null || true ;;
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
    [ -z "$p1" ] || kill "$p1" 2>/dev/null || true
    [ -z "$p0" ] || kill "$p0" 2>/dev/null || true
}
trap abort_children EXIT
trap 'abort_children; disarmed=1; exit 130' INT
trap 'abort_children; disarmed=1; exit 143' TERM
for host in "$MAX2" "$MAX"; do
    actual=$("${SSH[@]}" "$host" "sha256sum '$BIN' | awk '{print \$1}'") || exit 1
    [ "$actual" = "$EXPECTED_BIN" ] || { echo "binary mismatch on $host: $actual" >&2; exit 1; }
    model_bytes=$("${SSH[@]}" "$host" "stat -c %s '$MODEL'") || exit 1
    [ "$model_bytes" = "$EXPECTED_MODEL_BYTES" ] || { echo "model size mismatch on $host" >&2; exit 1; }
done
for spec in "$MAX2:$R0_PREFIX:$R0_LOG:$R0_RC" "$MAX:$R1_PREFIX:$R1_LOG:$R1_RC"; do
    IFS=: read -r host prefix log rc <<<"$spec"
    "${SSH[@]}" "$host" "sudo -n bash -s" <<EOS || exit 1
set -euo pipefail
BIN='$BIN'
for x in /proc/[0-9]*/exe; do
    [ "\$(readlink "\$x" 2>/dev/null || true)" != "\$BIN" ] || exit 1
done
[ \$(df -PB1 /tmp | awk 'NR==2 {print \$4}') -ge 1073741824 ]
rm -f '$prefix'_*
rm -f '$log' '$rc'
if [ -e '$LOCK' ]; then
    p=\$(cat '$LOCK' 2>/dev/null || true)
    case "\$p" in ''|*[!0-9]*) ;; *) [ ! -d "/proc/\$p" ] || exit 1 ;; esac
    exec 9<>'$LOCK'
    flock -n 9
    rm -f '$LOCK'
    flock -u 9
fi
EOS
done
rm -f "$LOCAL_R0" "$LOCAL_R1"
"${SSH[@]}" "$MAX2" "set -o pipefail; sudo -n env \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
DS4_LOCK_FILE='$LOCK' DS4_TP_SPIN_MAX=800000000 DS4_MTP_SPEC_DISABLE=1 \
DS4_ROCM_DSV4_PREQUANT_DECODE=1 DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0 \
DS4_ROCM_GRAPH_DUMP_PREFIX='$R0_PREFIX' \
DS4_ROCM_GRAPH_DUMP_NAME=hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output \
DS4_ROCM_GRAPH_DUMP_LAYER=all \
'$BIN' --rocm -m '$MODEL' -c 8192 -n 1 -p Hello --raw-prompt --temp 0 \
--tensor-parallel --role coordinator --listen 10.99.0.2 5601 \
--transport nhi --nhi-device /run/ds4-tbstream/device \
2>&1 | tee '$R0_LOG'; rc=\${PIPESTATUS[0]}; printf '%s\\n' \"\$rc\" | sudo -n tee '$R0_RC' >/dev/null; exit \"\$rc\"" >"$LOCAL_R0" 2>&1 &
p0=$!
echo "orchestrator: rank0 ssh pid=$p0"
listen=0
deadline=$((SECONDS + 240))
while [ "$SECONDS" -lt "$deadline" ]; do
    if "${SSH[@]}" "$MAX2" "grep -Fq 'ds4-tp: waiting for worker on 10.99.0.2:5601' '$R0_LOG' 2>/dev/null"; then
        listen=1
        break
    fi
    if ! kill -0 "$p0" 2>/dev/null; then
        echo 'orchestrator: rank0 exited before worker-listen barrier' >&2
        break
    fi
    sleep 2
done
[ "$listen" -eq 1 ] || { wait "$p0" 2>/dev/null || true; exit 1; }
echo 'orchestrator: rank0 worker-listen barrier reached'
"${SSH[@]}" "$MAX" "set -o pipefail; sudo -n env \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
DS4_LOCK_FILE='$LOCK' DS4_TP_SPIN_MAX=800000000 DS4_MTP_SPEC_DISABLE=1 \
DS4_ROCM_DSV4_PREQUANT_DECODE=1 DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0 \
DS4_ROCM_GRAPH_DUMP_PREFIX='$R1_PREFIX' \
DS4_ROCM_GRAPH_DUMP_NAME=hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output \
DS4_ROCM_GRAPH_DUMP_LAYER=all \
'$BIN' --rocm -m '$MODEL' -c 8192 -n 1 -p Hello --raw-prompt --temp 0 \
--tensor-parallel --role worker --coordinator 10.99.0.2 5601 \
--transport nhi --nhi-device /run/ds4-tbstream/device \
2>&1 | tee '$R1_LOG'; rc=\${PIPESTATUS[0]}; printf '%s\\n' \"\$rc\" | sudo -n tee '$R1_RC' >/dev/null; exit \"\$rc\"" >"$LOCAL_R1" 2>&1 &
p1=$!
echo "orchestrator: rank1 ssh pid=$p1"
deadline=$((SECONDS + 900))
timed_out=0
while kill -0 "$p0" 2>/dev/null || kill -0 "$p1" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        timed_out=1
        echo 'orchestrator: bilateral completion timeout' >&2
        break
    fi
    sleep 2
done
[ "$timed_out" -eq 0 ] || exit 1
wait "$p0"; ssh0=$?
wait "$p1"; ssh1=$?
disarmed=1
trap - EXIT INT TERM
r0=$("${SSH[@]}" "$MAX2" "cat '$R0_RC' 2>/dev/null || echo missing")
r1=$("${SSH[@]}" "$MAX" "cat '$R1_RC' 2>/dev/null || echo missing")
printf 'orchestrator: ssh_rc rank0=%s rank1=%s ds4_rc rank0=%s rank1=%s\n' "$ssh0" "$ssh1" "$r0" "$r1"
[ "$ssh0" -eq 0 ] && [ "$ssh1" -eq 0 ] && [ "$r0" = 0 ] && [ "$r1" = 0 ]
