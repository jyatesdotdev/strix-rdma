#!/usr/bin/env bash
set -u
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
HOST=jryates@max2
BIN=/home/jryates/ds4-stage3-build/ds4
MODEL=/home/jryates/models/ds4-pr-20260801/DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf
EXPECTED_BIN=4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7
EXPECTED_MODEL_BYTES=155976458848
EXPECTED_OUTPUT_SHA=21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae
CHECKER_LOCAL=/tmp/check-stage3-localize-dumps.py
CHECKER_REMOTE=/tmp/check-stage3-localize-dumps-f46cedfc.py
EXPECTED_CHECKER=f46cedfc35b5cb5dc79786a94cc428481d52f35f8eb2f424b5b07c13c098500b
PREFIX=/tmp/stage3-6460eb6-single-dump
LOG=/tmp/stage3-6460eb6-single.log
RC=/tmp/stage3-6460eb6-single.rc
LOCK=/run/ds4-stage3-localize-single.lock
LOCAL=/tmp/stage3-6460eb6-single-ssh.log
REPORT=/tmp/stage3-6460eb6-single-inventory.json
MARKER=/tmp/stage3-6460eb6-arm-a.success
COMMIT_MARKER=/tmp/stage3-6460eb6-arm-a.committed
LOCAL_MUTEX=/tmp/stage3-6460eb6-arm-a.orchestrator
REMOTE_OWNER=/run/ds4-stage3-localize-single.owner
p=
launched=0
token_owned=0
mutex_owned=0
marker_published=0
marker_committed=0
marker_tmp=
commit_tmp=
nonce=
owned_pid=
owned_start=
remote_stop() {
    local expect_pid=${owned_pid:-}
    local expect_start=${owned_start:-}
    "${SSH[@]}" "$HOST" 'sudo -n bash -s' <<EOS
set -u
BIN='$BIN'
nonce='$nonce'
expect_pid='$expect_pid'
expect_start='$expect_start'
[ "\$(cat '$REMOTE_OWNER' 2>/dev/null || true)" = "\$nonce" ] || exit 1
candidates=
for x in /proc/[0-9]*/exe; do
    [ "\$(readlink "\$x" 2>/dev/null || true)" = "\$BIN" ] || continue
    q=\${x#/proc/}; q=\${q%/exe}
    [ -r "/proc/\$q/cmdline" ] || continue
    cmd=\$(tr '\0' ' ' < "/proc/\$q/cmdline")
    tr '\0' '\n' < "/proc/\$q/environ" | grep -Fxq "DS4_STAGE3_RUN_NONCE=\$nonce" || continue
    case "\$cmd" in *'--ssd-streaming'*'--ssd-streaming-cold'*) candidates="\$candidates \$q";; esac
done
for q in \$candidates; do
    if [ -n "\$expect_pid" ] && [ "\$q" != "\$expect_pid" ]; then continue; fi
    if [ -n "\$expect_start" ]; then
        start=\$(awk '{print \$22}' "/proc/\$q/stat" 2>/dev/null || true)
        [ "\$start" = "\$expect_start" ] || continue
    fi
    kill -TERM "\$q" 2>/dev/null || true
    for _ in \$(seq 1 40); do [ -d "/proc/\$q" ] || break; sleep 0.25; done
    if [ -r "/proc/\$q/cmdline" ]; then
        cmd=\$(tr '\0' ' ' < "/proc/\$q/cmdline")
        start=\$(awk '{print \$22}' "/proc/\$q/stat" 2>/dev/null || true)
        nonce_ok=0
        tr '\0' '\n' < "/proc/\$q/environ" 2>/dev/null | grep -Fxq "DS4_STAGE3_RUN_NONCE=\$nonce" && nonce_ok=1
        if { [ -z "\$expect_start" ] || [ "\$start" = "\$expect_start" ]; } && [ "\$nonce_ok" -eq 1 ]; then
            case "\$cmd" in *"\$BIN"*'--ssd-streaming'*'--ssd-streaming-cold'*) kill -KILL "\$q" 2>/dev/null || true;; esac
        fi
    fi
done
sleep 1
for x in /proc/[0-9]*/exe; do
    [ "\$(readlink "\$x" 2>/dev/null || true)" != "\$BIN" ] || exit 1
done
EOS
}
release_local_mutex() {
    [ "$mutex_owned" -eq 1 ] || return 0
    [ "$(cat "$LOCAL_MUTEX/nonce" 2>/dev/null || true)" = "$nonce" ] || return 1
    rm -f "$LOCAL_MUTEX/nonce" || return 1
    rmdir "$LOCAL_MUTEX" || return 1
    mutex_owned=0
}
create_remote_owner() {
    "${SSH[@]}" "$HOST" "sudo -n bash -s" <<EOS
set -euo pipefail
umask 077
( set -o noclobber; printf '%s\n' '$nonce' > '$REMOTE_OWNER' ) 2>/dev/null
[ "\$(cat '$REMOTE_OWNER')" = '$nonce' ]
EOS
}
release_remote_owner() {
    [ "$token_owned" -eq 1 ] || return 0
    "${SSH[@]}" "$HOST" "sudo -n bash -s" <<EOS
set -euo pipefail
[ "\$(cat '$REMOTE_OWNER')" = '$nonce' ]
for x in /proc/[0-9]*/environ; do
    tr '\0' '\n' < "\$x" 2>/dev/null | grep -Fxq 'DS4_STAGE3_RUN_NONCE=$nonce' && exit 1 || true
done
rm -f '$REMOTE_OWNER'
EOS
    [ "$?" -eq 0 ] || return 1
    token_owned=0
}
abort_child() {
    local failed=0
    if [ "$launched" -eq 1 ]; then
        echo 'orchestrator: stopping owned standalone diagnostic' >&2
        remote_stop || failed=1
        [ -z "$p" ] || kill "$p" 2>/dev/null || true
    fi
    if [ "$token_owned" -eq 1 ] && [ "$failed" -eq 0 ]; then
        release_remote_owner || failed=1
    fi
    if [ "$marker_committed" -eq 0 ]; then
        if [ "$marker_published" -eq 1 ]; then
            if grep -Fxq "run_nonce=$nonce" "$MARKER" 2>/dev/null; then rm -f "$MARKER" || failed=1; else failed=1; fi
            rm -f "$COMMIT_MARKER" 2>/dev/null || failed=1
        fi
        [ -z "${marker_tmp:-}" ] || rm -f "$marker_tmp" 2>/dev/null || failed=1
        [ -z "${commit_tmp:-}" ] || rm -f "$commit_tmp" 2>/dev/null || failed=1
    fi
    release_local_mutex || failed=1
    if [ "$failed" -ne 0 ]; then
        echo 'orchestrator: standalone cleanup did not prove owned quiescence' >&2
        return 1
    fi
    return 0
}
run_remote() {
    "${SSH[@]}" "$HOST" 'bash -s' <<EOS
set -u
set -o pipefail
sudo -n timeout --signal=TERM --kill-after=15s 900s env \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
DS4_LOCK_FILE='$LOCK' \
DS4_STAGE3_RUN_NONCE='$nonce' \
DS4_MTP_SPEC_DISABLE=1 \
DS4_ROCM_DSV4_PREQUANT_DECODE=1 \
DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0 \
DS4_ROCM_GRAPH_DUMP_PREFIX='$PREFIX' \
DS4_ROCM_GRAPH_DUMP_NAME=hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output \
DS4_ROCM_GRAPH_DUMP_LAYER=all \
'$BIN' --rocm -m '$MODEL' \
--ssd-streaming --ssd-streaming-cold --ssd-streaming-cache-experts 32GB \
-c 8192 -n 1 -p Hello --raw-prompt --temp 0 2>&1 | tee '$LOG'
st=("\${PIPESTATUS[@]}")
work_rc=\${st[0]:-125}
log_rc=\${st[1]:-125}
printf '%s\n' "\$work_rc" | sudo -n tee '$RC' >/dev/null
rc_write=\$?
[ "\$log_rc" -eq 0 ] && [ "\$rc_write" -eq 0 ] || exit 125
exit "\$work_rc"
EOS
}
capture_owner() {
    "${SSH[@]}" "$HOST" "sudo -n bash -s" <<EOS
set -euo pipefail
p=\$(cat '$LOCK')
case "\$p" in ''|*[!0-9]*) exit 1;; esac
[ "\$(readlink /proc/\$p/exe)" = '$BIN' ]
cmd=\$(tr '\0' ' ' < /proc/\$p/cmdline)
case "\$cmd" in *'--ssd-streaming'*'--ssd-streaming-cold'*) ;; *) exit 1;; esac
tr '\0' '\n' < /proc/\$p/environ | grep -Fxq 'DS4_STAGE3_RUN_NONCE=$nonce'
printf '%s:%s\n' "\$p" "\$(awk '{print \$22}' /proc/\$p/stat)"
EOS
}
# Atomic local ownership is acquired before any preflight can race another
# invocation. The nonce also binds the remote token and child environment.
nonce=$(python3 -c 'import secrets; print(secrets.token_hex(16))') || exit 1
case "$nonce" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#nonce}" -eq 32 ] || exit 1
mkdir "$LOCAL_MUTEX" || { echo "Arm-A orchestrator already active or stale: $LOCAL_MUTEX" >&2; exit 1; }
printf '%s\n' "$nonce" > "$LOCAL_MUTEX/nonce" || { rmdir "$LOCAL_MUTEX" 2>/dev/null || true; exit 1; }
mutex_owned=1
trap abort_child EXIT
trap 'abort_child; launched=0; exit 130' INT
trap 'abort_child; launched=0; exit 143' TERM
# Claim the remote orchestration token before any remote preflight can race
# destructive prefix cleanup from another controller.
create_remote_owner || exit 1
token_owned=1
# The cleanup trap is safe during preflight: launched remains zero.
[ ! -e "$MARKER" ] || { echo "stale Arm-A marker exists: $MARKER" >&2; exit 1; }
[ ! -e "$COMMIT_MARKER" ] || { echo "stale Arm-A commit exists: $COMMIT_MARKER" >&2; exit 1; }
[ "$(sha256sum "$CHECKER_LOCAL" | awk '{print $1}')" = "$EXPECTED_CHECKER" ] || exit 1
actual=$("${SSH[@]}" "$HOST" "sha256sum '$BIN' | awk '{print \$1}'") || exit 1
[ "$actual" = "$EXPECTED_BIN" ] || exit 1
model_bytes=$("${SSH[@]}" "$HOST" "stat -Lc %s '$MODEL'") || exit 1
[ "$model_bytes" = "$EXPECTED_MODEL_BYTES" ] || exit 1
model_stat=$("${SSH[@]}" "$HOST" "stat -Lc '%d:%i:%s:%Y:%Z' '$MODEL'") || exit 1
model_sha=$("${SSH[@]}" "$HOST" "timeout 900s sha256sum '$MODEL' | awk '{print \$1}'") || exit 1
case "$model_sha" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#model_sha}" -eq 64 ] || exit 1
scp -q "$CHECKER_LOCAL" "$HOST:$CHECKER_REMOTE" || exit 1
remote_checker=$("${SSH[@]}" "$HOST" "sha256sum '$CHECKER_REMOTE' | awk '{print \$1}'") || exit 1
[ "$remote_checker" = "$EXPECTED_CHECKER" ] || exit 1
"${SSH[@]}" "$HOST" "sudo -n bash -s" <<EOS || exit 1
set -euo pipefail
BIN='$BIN'
[ "\$(cat '$REMOTE_OWNER')" = '$nonce' ]
for x in /proc/[0-9]*/exe; do [ "\$(readlink "\$x" 2>/dev/null || true)" != "\$BIN" ] || exit 1; done
[ \$(df -PB1 /tmp | awk 'NR==2 {print \$4}') -ge 1073741824 ]
rm -f '$PREFIX'_*
rm -f '$LOG' '$RC'
if [ -e '$LOCK' ]; then
    p=\$(cat '$LOCK' 2>/dev/null || true)
    case "\$p" in ''|*[!0-9]*) ;; *) [ ! -d "/proc/\$p" ] || exit 1;; esac
    exec 9<>'$LOCK'; flock -n 9; rm -f '$LOCK'; flock -u 9
fi
EOS
rm -f "$LOCAL" "$REPORT"
# The nonce is also carried in the child environment, so cleanup cannot adopt
# a merely similar process.
launched=1
run_remote >"$LOCAL" 2>&1 &
p=$!
owner=
owner_deadline=$((SECONDS + 120))
while [ "$SECONDS" -lt "$owner_deadline" ]; do
    owner=$(capture_owner 2>/dev/null) && break
    kill -0 "$p" 2>/dev/null || break
    sleep 1
done
[ -n "$owner" ] || exit 1
owned_pid=${owner%%:*}
owned_start=${owner#*:}
deadline=$((SECONDS + 930))
timed_out=0
while kill -0 "$p" 2>/dev/null; do
    if [ "$SECONDS" -ge "$deadline" ]; then timed_out=1; break; fi
    sleep 2
done
[ "$timed_out" -eq 0 ] || exit 1
wait "$p"; ssh_rc=$?
"${SSH[@]}" "$HOST" "sudo -n bash -c 'for x in /proc/[0-9]*/exe; do [ \"\$(readlink \"\$x\" 2>/dev/null || true)\" != \"$BIN\" ] || exit 1; done'" || exit 1
launched=0
remote_rc=$("${SSH[@]}" "$HOST" "cat '$RC' 2>/dev/null || echo missing")
printf 'standalone_ssh_rc=%s ds4_rc=%s\n' "$ssh_rc" "$remote_rc"
[ "$ssh_rc" -eq 0 ] && [ "$remote_rc" = 0 ] || exit 1
"${SSH[@]}" "$HOST" "python3 '$CHECKER_REMOTE' --prefix '$PREFIX' --mode standalone --expected-output-sha '$EXPECTED_OUTPUT_SHA' --log '$LOG'" >"$REPORT" || exit 1
cat "$REPORT" || exit 1
"${SSH[@]}" "$HOST" "sudo -n bash -s" <<EOS || exit 1
set -euo pipefail
[ -e '$LOCK' ] || exit 0
exec 9<>'$LOCK'; flock -n 9; rm -f '$LOCK'; flock -u 9
EOS
run_id=$(date -u +%Y%m%dT%H%M%SZ)-$$-$RANDOM || exit 1
a_script_sha=$(sha256sum "$0" | awk '{print $1}') || exit 1
marker_tmp=$MARKER.tmp.$$
umask 077
cat >"$marker_tmp" <<EOF
run_id=$run_id
run_nonce=$nonce
epoch=$(date +%s)
a_script_sha256=$a_script_sha
checker_sha256=$EXPECTED_CHECKER
binary_sha256=$EXPECTED_BIN
model_sha256=$model_sha
model_stat_max2=$model_stat
output_sha256=$EXPECTED_OUTPUT_SHA
EOF
[ "$?" -eq 0 ] || exit 1
mv "$marker_tmp" "$MARKER" || exit 1
[ -s "$MARKER" ] || exit 1
marker_published=1
release_remote_owner || exit 1
release_local_mutex || exit 1
marker_sha=$(sha256sum "$MARKER" | awk '{print $1}') || exit 1
commit_tmp=$COMMIT_MARKER.tmp.$$
cat >"$commit_tmp" <<EOF
run_nonce=$nonce
marker_sha256=$marker_sha
EOF
[ "$?" -eq 0 ] || exit 1
mv "$commit_tmp" "$COMMIT_MARKER" || exit 1
[ -s "$COMMIT_MARKER" ] || exit 1
marker_committed=1
trap - EXIT INT TERM
printf 'Arm A success marker: %s (commit %s)\n' "$MARKER" "$COMMIT_MARKER"
