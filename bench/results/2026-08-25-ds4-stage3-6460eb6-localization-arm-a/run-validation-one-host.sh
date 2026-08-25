#!/usr/bin/env bash
set -u
tag=${1:?usage: run-validation-one-host.sh max|max2}
case "$tag" in max|max2) ;; *) exit 2;; esac
cd /home/jryates/ds4-stage3-build || exit 1
TLOG=/tmp/stage3-6460eb6-v5-${tag}-test-rocm.log
MLOG=/tmp/stage3-6460eb6-v5-${tag}-mxfp4.log
TRC=/tmp/stage3-6460eb6-v5-${tag}-test-rocm.rc
MRC=/tmp/stage3-6460eb6-v5-${tag}-mxfp4.rc
rm -f "$TLOG" "$MLOG" "$TRC" "$MRC" || exit 1
set -o pipefail
env \
  PATH="/opt/rocm-therock/bin:$PATH" \
  LD_LIBRARY_PATH=/opt/rocm-therock/lib \
  ROCM_HOME=/opt/rocm-therock \
  timeout --signal=TERM --kill-after=15s 1500s \
  make test-rocm \
    ROCM_HOME=/opt/rocm-therock \
    HIPCC=/opt/rocm-therock/bin/hipcc \
  2>&1 | tee "$TLOG"
st=("${PIPESTATUS[@]}")
test_rc=${st[0]:-125}
test_log_rc=${st[1]:-125}
printf 'work_rc=%s log_rc=%s\n' "$test_rc" "$test_log_rc" > "$TRC" || exit 1
[ "$test_rc" -eq 0 ] && [ "$test_log_rc" -eq 0 ] || exit 1
env \
  PATH="/opt/rocm-therock/bin:$PATH" \
  LD_LIBRARY_PATH=/opt/rocm-therock/lib \
  ROCM_HOME=/opt/rocm-therock \
  timeout --signal=TERM --kill-after=15s 1500s \
  make test-mxfp4-rocm \
    ROCM_HOME=/opt/rocm-therock \
    HIPCC=/opt/rocm-therock/bin/hipcc \
  2>&1 | tee "$MLOG"
st=("${PIPESTATUS[@]}")
mxfp4_rc=${st[0]:-125}
mxfp4_log_rc=${st[1]:-125}
printf 'work_rc=%s log_rc=%s\n' "$mxfp4_rc" "$mxfp4_log_rc" > "$MRC" || exit 1
[ "$mxfp4_rc" -eq 0 ] && [ "$mxfp4_log_rc" -eq 0 ]
