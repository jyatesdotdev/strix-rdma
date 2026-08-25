#!/usr/bin/env bash
set -u
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
HOST=jryates@max2
BIN=/home/jryates/ds4-stage3-build/ds4
MODEL=/home/jryates/models/ds4-pr-20260801/DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf
EXPECTED_BIN=4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7
EXPECTED_MODEL_BYTES=155976458848
PREFIX=/tmp/stage3-6460eb6-single-dump
LOG=/tmp/stage3-6460eb6-single.log
RC=/tmp/stage3-6460eb6-single.rc
LOCK=/run/ds4-stage3-localize-single.lock
LOCAL=/tmp/stage3-6460eb6-single-ssh.log
p=
disarmed=0
remote_stop() {
    "${SSH[@]}" "$HOST" 'sudo -n bash -s' <<'EOS' || true
set -u
f=/run/ds4-stage3-localize-single.lock
[ -r "$f" ] || exit 0
p=$(cat "$f" 2>/dev/null || true)
case "$p" in ''|*[!0-9]*) exit 0;; esac
[ -r "/proc/$p/cmdline" ] || exit 0
cmd=$(tr '\0' ' ' < "/proc/$p/cmdline")
case "$cmd" in
  *'/home/jryates/ds4-stage3-build/ds4'*'--ssd-streaming'*'--ssd-streaming-cold'*) ;;
  *) exit 0;;
esac
kill -TERM "$p" 2>/dev/null || true
for _ in $(seq 1 40); do
    [ -d "/proc/$p" ] || exit 0
    sleep 0.25
done
[ -r "/proc/$p/cmdline" ] || exit 0
cmd=$(tr '\0' ' ' < "/proc/$p/cmdline")
case "$cmd" in
  *'/home/jryates/ds4-stage3-build/ds4'*'--ssd-streaming'*'--ssd-streaming-cold'*) kill -KILL "$p" 2>/dev/null || true ;;
esac
EOS
}
abort_child() {
    [ "$disarmed" -eq 0 ] || return 0
    echo 'orchestrator: stopping bounded standalone diagnostic' >&2
    remote_stop
    [ -z "$p" ] || kill "$p" 2>/dev/null || true
}
trap abort_child EXIT
trap 'abort_child; disarmed=1; exit 130' INT
trap 'abort_child; disarmed=1; exit 143' TERM
actual=$("${SSH[@]}" "$HOST" "sha256sum '$BIN' | awk '{print \$1}'") || exit 1
[ "$actual" = "$EXPECTED_BIN" ] || exit 1
model_bytes=$("${SSH[@]}" "$HOST" "stat -c %s '$MODEL'") || exit 1
[ "$model_bytes" = "$EXPECTED_MODEL_BYTES" ] || exit 1
"${SSH[@]}" "$HOST" "sudo -n bash -s" <<EOS || exit 1
set -euo pipefail
BIN='$BIN'
for x in /proc/[0-9]*/exe; do
    [ "\$(readlink "\$x" 2>/dev/null || true)" != "\$BIN" ] || exit 1
done
[ \$(df -PB1 /tmp | awk 'NR==2 {print \$4}') -ge 1073741824 ]
rm -f '$PREFIX'_*
rm -f '$LOG' '$RC'
if [ -e '$LOCK' ]; then
    p=\$(cat '$LOCK' 2>/dev/null || true)
    case "\$p" in ''|*[!0-9]*) ;; *) [ ! -d "/proc/\$p" ] || exit 1 ;; esac
    exec 9<>'$LOCK'
    flock -n 9
    rm -f '$LOCK'
    flock -u 9
fi
EOS
rm -f "$LOCAL"
"${SSH[@]}" "$HOST" "set -o pipefail; sudo -n timeout --signal=TERM --kill-after=15s 900s env \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
DS4_LOCK_FILE='$LOCK' \
DS4_MTP_SPEC_DISABLE=1 \
DS4_ROCM_DSV4_PREQUANT_DECODE=1 \
DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0 \
DS4_ROCM_GRAPH_DUMP_PREFIX='$PREFIX' \
DS4_ROCM_GRAPH_DUMP_NAME=hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output \
DS4_ROCM_GRAPH_DUMP_LAYER=all \
'$BIN' --rocm -m '$MODEL' \
--ssd-streaming --ssd-streaming-cold --ssd-streaming-cache-experts 32GB \
-c 8192 -n 1 -p Hello --raw-prompt --temp 0 \
2>&1 | tee '$LOG'; rc=\${PIPESTATUS[0]}; printf '%s\\n' \"\$rc\" | sudo -n tee '$RC' >/dev/null; exit \"\$rc\"" >"$LOCAL" 2>&1 &
p=$!
deadline=$((SECONDS + 930))
timed_out=0
while kill -0 "$p" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        timed_out=1
        echo 'orchestrator: standalone completion timeout' >&2
        break
    fi
    sleep 2
done
[ "$timed_out" -eq 0 ] || exit 1
wait "$p"; ssh_rc=$?
"${SSH[@]}" "$HOST" "sudo -n bash -c 'for x in /proc/[0-9]*/exe; do [ \"\$(readlink \"\$x\" 2>/dev/null || true)\" != \"$BIN\" ] || exit 1; done'"
live=$?
[ "$live" -eq 0 ] || exit 1
disarmed=1
trap - EXIT INT TERM
remote_rc=$("${SSH[@]}" "$HOST" "cat '$RC' 2>/dev/null || echo missing")
printf 'standalone_ssh_rc=%s ds4_rc=%s\n' "$ssh_rc" "$remote_rc"
[ "$ssh_rc" -eq 0 ] && [ "$remote_rc" = 0 ]
