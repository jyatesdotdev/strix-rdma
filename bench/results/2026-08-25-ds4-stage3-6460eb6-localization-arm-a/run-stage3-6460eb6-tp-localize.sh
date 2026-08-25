#!/usr/bin/env bash
set -u
SSH=(ssh -o BatchMode=yes -o ConnectTimeout=10)
MAX=jryates@max
MAX2=jryates@max2
BIN=/home/jryates/ds4-stage3-build/ds4
MODEL=/home/jryates/models/ds4-pr-20260801/DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf
EXPECTED_BIN=4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7
EXPECTED_MODEL_BYTES=155976458848
EXPECTED_A_SCRIPT=59ac8dbcfe01dc805f1b7e4a09bafd2d6edc3d9b32511274654168965808c862
EXPECTED_OUTPUT_SHA=21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae
CHECKER_LOCAL=/tmp/check-stage3-localize-dumps.py
CHECKER_REMOTE=/tmp/check-stage3-localize-dumps-f46cedfc.py
EXPECTED_CHECKER=f46cedfc35b5cb5dc79786a94cc428481d52f35f8eb2f424b5b07c13c098500b
MARKER=/tmp/stage3-6460eb6-arm-a.success
COMMIT_MARKER=/tmp/stage3-6460eb6-arm-a.committed
GLOBAL_MUTEX=/tmp/stage3-6460eb6-arm-a.orchestrator
R0_OWNER=/run/ds4-stage3-localize-single.owner
R1_OWNER=/run/ds4-stage3-localize-worker.owner
R0_PREFIX=/tmp/stage3-6460eb6-tp-r0-dump
R1_PREFIX=/tmp/stage3-6460eb6-tp-r1-dump
R0_LOG=/tmp/stage3-6460eb6-tp-r0.log
R1_LOG=/tmp/stage3-6460eb6-tp-r1.log
R0_RC=/tmp/stage3-6460eb6-tp-r0.rc
R1_RC=/tmp/stage3-6460eb6-tp-r1.rc
R0_REPORT=/tmp/stage3-6460eb6-tp-r0-inventory.json
R1_REPORT=/tmp/stage3-6460eb6-tp-r1-inventory.json
LOCK=/run/ds4-stage3-localize-tp.lock
LOCAL_R0=/tmp/stage3-6460eb6-tp-r0-ssh.log
LOCAL_R1=/tmp/stage3-6460eb6-tp-r1-ssh.log
p0=
p1=
launched0=0
launched1=0
mutex_owned=0
token0_owned=0
token1_owned=0
b_nonce=
owned_pid0=
owned_start0=
owned_pid1=
owned_start1=
remote_stop_one() {
    local host=$1 role=$2 owner=$3 expect_pid=$4 expect_start=$5
    "${SSH[@]}" "$host" 'sudo -n bash -s' <<EOS
set -u
BIN='$BIN'
role='$role'
owner='$owner'
nonce='$b_nonce'
expect_pid='$expect_pid'
expect_start='$expect_start'
[ "\$(cat "\$owner" 2>/dev/null || true)" = "\$nonce" ] || exit 1
candidates=
for x in /proc/[0-9]*/exe; do
    [ "\$(readlink "\$x" 2>/dev/null || true)" = "\$BIN" ] || continue
    q=\${x#/proc/}; q=\${q%/exe}
    [ -r "/proc/\$q/cmdline" ] || continue
    cmd=\$(tr '\0' ' ' < "/proc/\$q/cmdline")
    tr '\0' '\n' < "/proc/\$q/environ" | grep -Fxq "DS4_STAGE3_RUN_NONCE=\$nonce" || continue
    case "\$cmd" in *'--tensor-parallel'*"--role \$role"*) candidates="\$candidates \$q";; esac
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
            case "\$cmd" in *"\$BIN"*'--tensor-parallel'*"--role \$role"*) kill -KILL "\$q" 2>/dev/null || true;; esac
        fi
    fi
    for _ in \$(seq 1 20); do [ -d "/proc/\$q" ] || break; sleep 0.1; done
done
for x in /proc/[0-9]*/exe; do
    [ "\$(readlink "\$x" 2>/dev/null || true)" = "\$BIN" ] || continue
    q=\${x#/proc/}; q=\${q%/exe}
    cmd=\$(tr '\0' ' ' < "/proc/\$q/cmdline" 2>/dev/null || true)
    case "\$cmd" in *'--tensor-parallel'*"--role \$role"*) exit 1;; esac
done
EOS
}
release_global_mutex() {
    [ "$mutex_owned" -eq 1 ] || return 0
    [ "$(cat "$GLOBAL_MUTEX/nonce" 2>/dev/null || true)" = "$b_nonce" ] || return 1
    rm -f "$GLOBAL_MUTEX/nonce" || return 1
    rmdir "$GLOBAL_MUTEX" || return 1
    mutex_owned=0
}
create_remote_owner() {
    local host=$1 owner=$2
    "${SSH[@]}" "$host" "sudo -n bash -s" <<EOS
set -euo pipefail
umask 077
( set -o noclobber; printf '%s\n' '$b_nonce' > '$owner' ) 2>/dev/null
[ "\$(cat '$owner')" = '$b_nonce' ]
EOS
}
release_remote_owner() {
    local host=$1 owner=$2
    "${SSH[@]}" "$host" "sudo -n bash -s" <<EOS
set -euo pipefail
[ "\$(cat '$owner')" = '$b_nonce' ]
for x in /proc/[0-9]*/environ; do
    tr '\0' '\n' < "\$x" 2>/dev/null | grep -Fxq 'DS4_STAGE3_RUN_NONCE=$b_nonce' && exit 1 || true
done
rm -f '$owner'
EOS
}
abort_children() {
    local failed=0
    echo 'orchestrator: worker-first cleanup of owned TP diagnostics' >&2
    if [ "$launched1" -eq 1 ]; then
        remote_stop_one "$MAX" worker "$R1_OWNER" "${owned_pid1:-}" "${owned_start1:-}" || failed=1
        [ -z "$p1" ] || kill "$p1" 2>/dev/null || true
    fi
    if [ "$launched0" -eq 1 ]; then
        remote_stop_one "$MAX2" coordinator "$R0_OWNER" "${owned_pid0:-}" "${owned_start0:-}" || failed=1
        [ -z "$p0" ] || kill "$p0" 2>/dev/null || true
    fi
    if [ "$token1_owned" -eq 1 ] && [ "$failed" -eq 0 ]; then
        release_remote_owner "$MAX" "$R1_OWNER" || failed=1
        [ "$failed" -ne 0 ] || token1_owned=0
    fi
    if [ "$token0_owned" -eq 1 ] && [ "$failed" -eq 0 ]; then
        release_remote_owner "$MAX2" "$R0_OWNER" || failed=1
        [ "$failed" -ne 0 ] || token0_owned=0
    fi
    release_global_mutex || failed=1
    if [ "$failed" -ne 0 ]; then
        echo 'orchestrator: TP cleanup did not prove bilateral owned quiescence' >&2
        return 1
    fi
    return 0
}
launch_rank0() {
    "${SSH[@]}" "$MAX2" 'bash -s' <<EOS
set -u
set -o pipefail
sudo -n timeout --signal=TERM --kill-after=15s 1200s env \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
DS4_LOCK_FILE='$LOCK' DS4_STAGE3_RUN_NONCE='$b_nonce' \
DS4_TP_SPIN_MAX=800000000 DS4_MTP_SPEC_DISABLE=1 \
DS4_ROCM_DSV4_PREQUANT_DECODE=1 DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0 \
DS4_ROCM_GRAPH_DUMP_PREFIX='$R0_PREFIX' \
DS4_ROCM_GRAPH_DUMP_NAME=hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output \
DS4_ROCM_GRAPH_DUMP_LAYER=all \
'$BIN' --rocm -m '$MODEL' -c 8192 -n 1 -p Hello --raw-prompt --temp 0 \
--tensor-parallel --role coordinator --listen 10.99.0.2 5601 \
--transport nhi --nhi-device /run/ds4-tbstream/device 2>&1 | tee '$R0_LOG'
st=("\${PIPESTATUS[@]}"); work_rc=\${st[0]:-125}; log_rc=\${st[1]:-125}
printf '%s\n' "\$work_rc" | sudo -n tee '$R0_RC' >/dev/null; rc_write=\$?
[ "\$log_rc" -eq 0 ] && [ "\$rc_write" -eq 0 ] || exit 125
exit "\$work_rc"
EOS
}
launch_rank1() {
    "${SSH[@]}" "$MAX" 'bash -s' <<EOS
set -u
set -o pipefail
sudo -n timeout --signal=TERM --kill-after=15s 1200s env \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
DS4_LOCK_FILE='$LOCK' DS4_STAGE3_RUN_NONCE='$b_nonce' \
DS4_TP_SPIN_MAX=800000000 DS4_MTP_SPEC_DISABLE=1 \
DS4_ROCM_DSV4_PREQUANT_DECODE=1 DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0 \
DS4_ROCM_GRAPH_DUMP_PREFIX='$R1_PREFIX' \
DS4_ROCM_GRAPH_DUMP_NAME=hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output \
DS4_ROCM_GRAPH_DUMP_LAYER=all \
'$BIN' --rocm -m '$MODEL' -c 8192 -n 1 -p Hello --raw-prompt --temp 0 \
--tensor-parallel --role worker --coordinator 10.99.0.2 5601 \
--transport nhi --nhi-device /run/ds4-tbstream/device 2>&1 | tee '$R1_LOG'
st=("\${PIPESTATUS[@]}"); work_rc=\${st[0]:-125}; log_rc=\${st[1]:-125}
printf '%s\n' "\$work_rc" | sudo -n tee '$R1_RC' >/dev/null; rc_write=\$?
[ "\$log_rc" -eq 0 ] && [ "\$rc_write" -eq 0 ] || exit 125
exit "\$work_rc"
EOS
}
capture_owner() {
    local host=$1 role=$2
    "${SSH[@]}" "$host" "sudo -n bash -s" <<EOS
set -euo pipefail
p=\$(cat '$LOCK')
case "\$p" in ''|*[!0-9]*) exit 1;; esac
[ "\$(readlink /proc/\$p/exe)" = '$BIN' ]
cmd=\$(tr '\0' ' ' < /proc/\$p/cmdline)
case "\$cmd" in *'--tensor-parallel'*'--role $role'*) ;; *) exit 1;; esac
tr '\0' '\n' < /proc/\$p/environ | grep -Fxq 'DS4_STAGE3_RUN_NONCE=$b_nonce'
printf '%s:%s\n' "\$p" "\$(awk '{print \$22}' /proc/\$p/stat)"
EOS
}
assert_no_live() {
    local host=$1
    "${SSH[@]}" "$host" "sudo -n bash -c 'for x in /proc/[0-9]*/exe; do [ \"\$(readlink \"\$x\" 2>/dev/null || true)\" != \"$BIN\" ] || exit 1; done'"
}
safe_remove_lock() {
    local host=$1
    "${SSH[@]}" "$host" "sudo -n bash -s" <<EOS
set -euo pipefail
[ -e '$LOCK' ] || exit 0
exec 9<>'$LOCK'; flock -n 9; rm -f '$LOCK'; flock -u 9
EOS
}
marker_value() { awk -F= -v k="$1" '$1 == k {print substr($0, length(k) + 2)}' "$MARKER"; }
# Atomically reserve the same local/max2 exclusion used by Arm A before
# marker validation. A max-side token also excludes cross-controller B races.
b_nonce=$(python3 -c 'import secrets; print(secrets.token_hex(16))') || exit 1
case "$b_nonce" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#b_nonce}" -eq 32 ] || exit 1
mkdir "$GLOBAL_MUTEX" || { echo "Stage-3 orchestrator already active or stale: $GLOBAL_MUTEX" >&2; exit 1; }
printf '%s\n' "$b_nonce" > "$GLOBAL_MUTEX/nonce" || { rmdir "$GLOBAL_MUTEX" 2>/dev/null || true; exit 1; }
mutex_owned=1
trap abort_children EXIT
trap 'abort_children; launched0=0; launched1=0; exit 130' INT
trap 'abort_children; launched0=0; launched1=0; exit 143' TERM
create_remote_owner "$MAX2" "$R0_OWNER" || exit 1
token0_owned=1
create_remote_owner "$MAX" "$R1_OWNER" || exit 1
token1_owned=1
# Machine-enforced Arm-A and content preflights follow while exclusions remain held.
[ "$(sha256sum "$CHECKER_LOCAL" | awk '{print $1}')" = "$EXPECTED_CHECKER" ] || exit 1
[ -f "$MARKER" ] && [ "$(wc -l < "$MARKER")" -eq 9 ] || exit 1
[ -f "$COMMIT_MARKER" ] && [ "$(wc -l < "$COMMIT_MARKER")" -eq 2 ] || exit 1
marker_a=$(marker_value a_script_sha256)
marker_checker=$(marker_value checker_sha256)
marker_bin=$(marker_value binary_sha256)
marker_model=$(marker_value model_sha256)
marker_stat=$(marker_value model_stat_max2)
marker_output=$(marker_value output_sha256)
marker_epoch=$(marker_value epoch)
marker_run=$(marker_value run_id)
marker_nonce=$(marker_value run_nonce)
[ "$marker_a" = "$EXPECTED_A_SCRIPT" ] || exit 1
[ "$marker_checker" = "$EXPECTED_CHECKER" ] || exit 1
[ "$marker_bin" = "$EXPECTED_BIN" ] || exit 1
[ "$marker_output" = "$EXPECTED_OUTPUT_SHA" ] || exit 1
case "$marker_model" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#marker_model}" -eq 64 ] || exit 1
case "$marker_epoch" in ''|*[!0-9]*) exit 1;; esac
case "$marker_run" in ''|*[!A-Za-z0-9T-]*) exit 1;; esac
case "$marker_nonce" in ''|*[!0-9a-f]*) exit 1;; esac
[ "${#marker_nonce}" -eq 32 ] || exit 1
commit_nonce=$(awk -F= '$1 == "run_nonce" {print $2}' "$COMMIT_MARKER")
commit_sha=$(awk -F= '$1 == "marker_sha256" {print $2}' "$COMMIT_MARKER")
[ "$commit_nonce" = "$marker_nonce" ] || exit 1
[ "$commit_sha" = "$(sha256sum "$MARKER" | awk '{print $1}')" ] || exit 1
now=$(date +%s); age=$((now - marker_epoch)); [ "$age" -ge 0 ] && [ "$age" -le 3600 ] || exit 1
for host in "$MAX2" "$MAX"; do
    actual=$("${SSH[@]}" "$host" "sha256sum '$BIN' | awk '{print \$1}'") || exit 1
    [ "$actual" = "$EXPECTED_BIN" ] || exit 1
    bytes=$("${SSH[@]}" "$host" "stat -Lc %s '$MODEL'") || exit 1
    [ "$bytes" = "$EXPECTED_MODEL_BYTES" ] || exit 1
    scp -q "$CHECKER_LOCAL" "$host:$CHECKER_REMOTE" || exit 1
    remote_checker=$("${SSH[@]}" "$host" "sha256sum '$CHECKER_REMOTE' | awk '{print \$1}'") || exit 1
    [ "$remote_checker" = "$EXPECTED_CHECKER" ] || exit 1
done
current_stat=$("${SSH[@]}" "$MAX2" "stat -Lc '%d:%i:%s:%Y:%Z' '$MODEL'") || exit 1
[ "$current_stat" = "$marker_stat" ] || exit 1
worker_model_sha=$("${SSH[@]}" "$MAX" "timeout 900s sha256sum '$MODEL' | awk '{print \$1}'") || exit 1
[ "$worker_model_sha" = "$marker_model" ] || exit 1
# Atomically reserve the one-shot A result before any destructive remote
# prefix/log/lock preflight. Failure after this point remains consumed.
consumed_commit=$COMMIT_MARKER.consumed.$marker_run
consumed=$MARKER.consumed.$marker_run
[ ! -e "$consumed_commit" ] && [ ! -e "$consumed" ] || exit 1
mv "$COMMIT_MARKER" "$consumed_commit" || exit 1
mv "$MARKER" "$consumed" || exit 1
for spec in "$MAX2:$R0_PREFIX:$R0_LOG:$R0_RC" "$MAX:$R1_PREFIX:$R1_LOG:$R1_RC"; do
    IFS=: read -r host prefix log rc <<<"$spec"
    "${SSH[@]}" "$host" "sudo -n bash -s" <<EOS || exit 1
set -euo pipefail
BIN='$BIN'
for x in /proc/[0-9]*/exe; do [ "\$(readlink "\$x" 2>/dev/null || true)" != "\$BIN" ] || exit 1; done
[ \$(df -PB1 /tmp | awk 'NR==2 {print \$4}') -ge 1073741824 ]
rm -f '$prefix'_* '$log' '$rc'
if [ -e '$LOCK' ]; then
    p=\$(cat '$LOCK' 2>/dev/null || true)
    case "\$p" in ''|*[!0-9]*) ;; *) [ ! -d "/proc/\$p" ] || exit 1;; esac
    exec 9<>'$LOCK'; flock -n 9; rm -f '$LOCK'; flock -u 9
fi
EOS
done
rm -f "$LOCAL_R0" "$LOCAL_R1" "$R0_REPORT" "$R1_REPORT"
# Workload ownership begins only after every preflight and one-shot marker consumption.
launched0=1
launch_rank0 >"$LOCAL_R0" 2>&1 &
p0=$!
owner=
owner_deadline=$((SECONDS + 120))
while [ "$SECONDS" -lt "$owner_deadline" ]; do
    owner=$(capture_owner "$MAX2" coordinator 2>/dev/null) && break
    kill -0 "$p0" 2>/dev/null || break
    sleep 1
done
[ -n "$owner" ] || exit 1
owned_pid0=${owner%%:*}; owned_start0=${owner#*:}
listen=0
deadline=$((SECONDS + 240))
while [ "$SECONDS" -lt "$deadline" ]; do
    if "${SSH[@]}" "$MAX2" "grep -Fq 'ds4-tp: waiting for worker on 10.99.0.2:5601' '$R0_LOG' 2>/dev/null"; then listen=1; break; fi
    kill -0 "$p0" 2>/dev/null || break
    sleep 2
done
[ "$listen" -eq 1 ] || exit 1
launched1=1
launch_rank1 >"$LOCAL_R1" 2>&1 &
p1=$!
owner=
owner_deadline=$((SECONDS + 120))
while [ "$SECONDS" -lt "$owner_deadline" ]; do
    owner=$(capture_owner "$MAX" worker 2>/dev/null) && break
    kill -0 "$p1" 2>/dev/null || break
    sleep 1
done
[ -n "$owner" ] || exit 1
owned_pid1=${owner%%:*}; owned_start1=${owner#*:}
deadline=$((SECONDS + 900))
while :; do
    alive0=0; alive1=0
    kill -0 "$p0" 2>/dev/null && alive0=1
    kill -0 "$p1" 2>/dev/null && alive1=1
    [ "$alive0" -eq 1 ] || [ "$alive1" -eq 1 ] || break
    if [ "$alive0" -ne "$alive1" ]; then
        sleep 5
        a0=0; a1=0
        kill -0 "$p0" 2>/dev/null && a0=1
        kill -0 "$p1" 2>/dev/null && a1=1
        [ "$a0" -eq "$a1" ] || exit 1
    fi
    [ "$SECONDS" -lt "$deadline" ] || exit 1
    sleep 2
done
wait "$p0"; ssh0=$?
wait "$p1"; ssh1=$?
assert_no_live "$MAX2" || exit 1
assert_no_live "$MAX" || exit 1
launched0=0; launched1=0
safe_remove_lock "$MAX2" || exit 1
safe_remove_lock "$MAX" || exit 1
r0=$("${SSH[@]}" "$MAX2" "cat '$R0_RC' 2>/dev/null || echo missing")
r1=$("${SSH[@]}" "$MAX" "cat '$R1_RC' 2>/dev/null || echo missing")
printf 'ssh_rc rank0=%s rank1=%s ds4_rc rank0=%s rank1=%s\n' "$ssh0" "$ssh1" "$r0" "$r1"
[ "$ssh0" -eq 0 ] && [ "$ssh1" -eq 0 ] && [ "$r0" = 0 ] && [ "$r1" = 0 ] || exit 1
"${SSH[@]}" "$MAX2" "python3 '$CHECKER_REMOTE' --prefix '$R0_PREFIX' --mode coordinator --log '$R0_LOG'" >"$R0_REPORT" || exit 1
"${SSH[@]}" "$MAX" "python3 '$CHECKER_REMOTE' --prefix '$R1_PREFIX' --mode worker --log '$R1_LOG'" >"$R1_REPORT" || exit 1
cat "$R0_REPORT" "$R1_REPORT" || exit 1
release_remote_owner "$MAX" "$R1_OWNER" || exit 1
token1_owned=0
release_remote_owner "$MAX2" "$R0_OWNER" || exit 1
token0_owned=0
release_global_mutex || exit 1
trap - EXIT INT TERM
printf 'TP localization capture complete; Arm-A marker consumed at %s\n' "$consumed"
