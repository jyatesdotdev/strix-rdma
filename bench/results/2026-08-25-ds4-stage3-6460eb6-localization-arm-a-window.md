# 2026-08-25 — DS4 Stage-3 `6460eb6` localization: Arm A broad dumps perturb the standalone oracle

## Verdict

**Arm A guard failed. p15 and Arm B did not run. Stage 3 remains NO-GO.**

Fresh bilateral ROCm and MXFP4 validation passed, and the bounded standalone
process itself exited 0 with a complete, finite all-layer dump inventory.
However, its final output SHA256 was:

```text
3eeab08a43fb795d52f6b5d41191eccdc531bd5daf1be41574ed79b07591666d
```

rather than the frozen output-only standalone oracle:

```text
21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae
```

The immutable Arm A runner therefore exited 1 and published neither its success
marker nor commit companion. Under the explicit no-retry contract, p15 was
never loaded and Arm B was never launched.

The broad-dump result is instrumentation/synchronization perturbation evidence,
not TP or production-model correctness evidence.

## Exact inputs

- DS4 source:
  `6460eb6a1704080da44fc6af9ace8d80f8c6a400`
- parent:
  `20182c95e33458e3c50059bb58004c94d700e249`
- unchanged runtime-source parent:
  `9b383ac47a26df8a18fdaf78aa33ca96cd42a8d2`
- clean archive SHA256:
  `4b3bd1b290582dee77b98ff112da57fa6370f1a3ef9449af4f91bc8a4189a9ff`
- canonical 1,488-file source manifest SHA256:
  `6f01a72e52d4fd69f9cb0f64105edf5e851ace1f06298850cc2c9830e9a4bb95`
- bilateral runtime SHA256:
  `4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7`
- production module SHA256:
  `ccd946330a5612ffe7674246ac387b39d4a2e71562ed585cfc65d8d58e2650e7`
- p15 SHA256, verified but never loaded:
  `8a80396bb27cd3403182ff1b5aef32325bd4f142530151345398d7d07db321e0`
- model size on both hosts: 155,976,458,848 bytes
- host/kernel: `max`, `max2`, `7.1.5-101.fc43.x86_64`

Immutable v5 inputs:

- Arm A script:
  `59ac8dbcfe01dc805f1b7e4a09bafd2d6edc3d9b32511274654168965808c862`
- Arm B script, never executed:
  `632880667cdd3b7fe74bb5a1983be4ae40e8977344900afa4d8fcb8fcc85f990`
- checker:
  `f46cedfc35b5cb5dc79786a94cc428481d52f35f8eb2f424b5b07c13c098500b`
- runbook v5:
  `d984977233fec296c6da284524ae86dbdd083424a7e4da55660ea4e2f7da8985`
- lifecycle validation runner:
  `aa28efd6403d30ecdb456d0a1322671ea447d0c6cdd548eb96c3fd359d896e75`

All 1,488 source files were freshly rechecked on both hosts before shutdown.
The local Arm-A mutex/marker/commit, bilateral remote owner tokens, Stage-3
locks, and staged processes were absent.

## Production preflight and quiescence

Fresh production preflight passed bilaterally:

- active worker/server/exporter and reconcile timers;
- exact production PrivateTmp lock inode equality with fd 3 plus live FLOCK;
- exact installed module, with diagnostic parameter absent;
- exact 9/9, ring-4096, throttling-0 endpoints and no holder;
- no diagnostic owner, lock, or staged process;
- bilateral TB-IP zero loss;
- `/v1/models`, fresh 5-prompt + 1-completion API inference, and exporter
  scrape.

Production was stopped worker-first, followed by coordinator/exporter and both
timers. Quiescence, endpoints, holder absence, runtime/module hashes, and TB-IP
were rechecked before validation.

## Exact bilateral validation: PASS

The one-shot validation commands explicitly selected TheRock with `PATH`,
`LD_LIBRARY_PATH`, `ROCM_HOME=/opt/rocm-therock`, and command-line
`HIPCC=/opt/rocm-therock/bin/hipcc`. Both command and log-pipeline statuses were
0 on both hosts.

Required results passed bilaterally:

```text
ROCm TP shared-half parity ... finite=4096/4096 prequant=off
 test_tp_combine_rocm: 9/9 checks passed (0 failed)
ROCm TP shared-half parity ... finite=4096/4096 prequant=on
 test_tp_combine_rocm: 1/1 checks passed (0 failed)
test_gpu_args_cli: PASS=49 FAIL=0
MXFP4 ROCm routed MoE: PASS
```

The rollback shared-half error remained `2.23517e-08`; production prequant was
`7.45058e-09`. Full MXFP4 span exactness, kernel variants, contiguous TP
ownership, and strict rank-local cached halves passed. The runtime and key
objects remained bilaterally exact. Test processes were gone, dead/unheld host
locks were FLOCK-removed, and marked dmesg had no added message.

## Conditional Arm A: workload succeeds, oracle guard fails

Arm A ran once on `max2` under the production module. It used:

```text
DS4_ROCM_DSV4_PREQUANT_DECODE=1
DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0
DS4_MTP_SPEC_DISABLE=1
--ssd-streaming --ssd-streaming-cold --ssd-streaming-cache-experts 32GB
-c 8192 -n 1 -p Hello --raw-prompt --temp 0
```

Dump filter:

```text
hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output
LAYER=all
```

The process and SSH both exited 0 and emitted `#`. The dump chronology was
exactly:

```text
for layers 0..42:
  hc_attn_post
  ffn_moe_topk
  ffn_moe_weights_scaled
  hc_ffn_post
then:
  result_output layer 43
```

There were 173 dump log entries and zero synchronize/resume errors.

Integrated inventory:

| Check | Observed | Required |
|---|---:|---:|
| files | 173 | 173 |
| bytes | 6,155,280 | 6,155,280 |
| finite f32 | 1,538,562 / 1,538,562 | all |
| valid top-k layers | 43 | 43 |
| output SHA256 | `3eeab08a…666d` | `21afe05c…05ae` |

Only the frozen-output hash gate failed. Consequently:

- Arm A runner rc: 1;
- success marker: absent;
- commit marker: absent;
- p15 swap: not attempted;
- Arm B: not launched;
- NHI traffic: none;
- retry/follow-on: none.

The runner's failure trap released its nonce owner and local mutex. Its broad
`/proc/*/environ` cleanup scan logged benign PID-disappearance races; final
audits proved no diagnostic owner, process, lock, or holder remained.

## Post-restoration numerical comparison

The broad-dump standalone remained finite and kept argmax token 5, but it was
not a small last-bit perturbation.

Broad-dump top five:

```text
5       13.9688711
372     13.7302523
13989   13.4593458
795     13.4179049
201     13.2760906
```

Frozen output-only top five:

```text
5       13.9951410
110704  13.2166653
795     12.9917479
372     12.9820995
7519    12.8689537
```

Full-vector broad-dump versus frozen error:

```text
nonzero f32       129,280 / 129,280
max absolute      4.12795591
mean absolute     0.476185395
RMSE              0.601984557
relative RMSE     0.173636721
cosine            0.985627434
absolute p50      0.398643017
absolute p99      1.58539104
```

Against the prior bad TP output `325a161c…`, the broad standalone had relative
RMSE `0.635848` and cosine `0.781872`. Since Arm A did not reproduce its frozen
oracle, no TP localization comparison was authorized or made.

## Capture note

The first in-window artifact tar command returned 2 because its relative
wildcard expanded before tar applied `-C /tmp`. It was not corrected in-window.
Production restoration began immediately, and the untouched artifacts were
captured only after restoration PASS. The failed partial tar is retained.

## Restoration

The production module was never swapped or unloaded. Timers were restarted,
then worker, coordinator, and exporter in order. Final bilateral audits passed:

- exact production module, diagnostic parameter absent;
- exact endpoints and active timers/services;
- production PrivateTmp path/fd inode equality and live FLOCK;
- no diagnostic token, lock, process, or stream-device holder;
- unchanged runtime and key objects;
- bilateral TB-IP zero loss;
- `/v1/models`, fresh API inference, and exporter scrape;
- marked dmesg contained only explicit v5 BEGIN/END markers.

The window opened at `2026-08-25T08:03:10.602798000Z` and closed at
`2026-08-25T08:11:24.344230720Z`.

## Artifacts

Complete evidence is under
`bench/results/2026-08-25-ds4-stage3-6460eb6-localization-arm-a/`, including:

- immutable scripts/checker/runbook and source provenance;
- fresh preflight, validation, quiescence, and restoration evidence;
- complete Arm A log, runner output, rc, checker report, chronology, and all
  173 dumps;
- frozen standalone and prior bad TP references;
- post-restoration numerical analysis;
- failed and successful artifact-capture archives;
- marked dmesg, API/exporter, TB-IP, `RUN-SUMMARY.txt`, `PROVENANCE.txt`, and
  `SHA256SUMS`.
