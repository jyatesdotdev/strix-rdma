# Stage-3 6460eb6 invasive-path localization — separate-window v5 proposal

Status: OFFLINE PROPOSAL ONLY. No window is open. Stage 3 remains NO-GO.

## Immutable inputs

- source commit: `6460eb6a1704080da44fc6af9ace8d80f8c6a400`
- source manifest: 1,488 files, SHA256 `6f01a72e52d4fd69f9cb0f64105edf5e851ace1f06298850cc2c9830e9a4bb95`
- bilateral runtime: SHA256 `4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7`
- frozen standalone output: SHA256 `21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae`
- prior bad TP output: SHA256 `325a161c354876dac279a90bb8fef680acd6522ed7b211dbc94a24d4569ceae4`
- model path: `/home/jryates/models/ds4-pr-20260801/DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf`
- model size: `155976458848`
- Arm A runner: `/tmp/run-stage3-6460eb6-standalone-localize.sh`, SHA256 `59ac8dbcfe01dc805f1b7e4a09bafd2d6edc3d9b32511274654168965808c862`
- Arm B runner: `/tmp/run-stage3-6460eb6-tp-localize.sh`, SHA256 `632880667cdd3b7fe74bb5a1983be4ae40e8977344900afa4d8fcb8fcc85f990`
- checker: `/tmp/check-stage3-localize-dumps.py`, SHA256 `f46cedfc35b5cb5dc79786a94cc428481d52f35f8eb2f424b5b07c13c098500b`

All claims are limited to the invasive graph-dump arithmetic used by the frozen oracle and prior bad TP arm. `DS4_ROCM_GRAPH_DUMP_NONINVASIVE=0` is intentional. A changed B output is perturbation evidence only, never a production correctness pass.

## Lifecycle and no-retry contract

Peer owns module/service/timer/wrapper lifecycle. Perform the usual bilateral production/API/exporter/PrivateTmp-lock/module/endpoints/TB-IP/holder/process/dmesg preflight. Stop production worker-first, then coordinator/exporter and reconcile timers. Prove quiescence.

No retry within the outage. Any validation, Arm A, inventory, module-swap, Arm B, transport, or restoration gate failure means capture and unconditional production restoration. No EVAL, sampled-token evaluation, ordinary decode, batch, benchmark, or follow-on.

Before validation require the local Arm-A orchestrator directory, Arm-A marker, remote Arm-A owner token, all Stage-3 locks, and staged processes absent. A stale token/mutex is never automatically adopted; audit it separately or fail closed.

## Exact bilateral validation gate under p14

Run exactly once per host from `/home/jryates/ds4-stage3-build`, with an explicit Bash pipeline-status wrapper and independent rc/log capture:

```bash
PATH=/opt/rocm-therock/bin:$PATH \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
ROCM_HOME=/opt/rocm-therock \
timeout --signal=TERM --kill-after=15s 1500s \
make test-rocm \
  ROCM_HOME=/opt/rocm-therock \
  HIPCC=/opt/rocm-therock/bin/hipcc
```

Only if that command is rc0 on both hosts, run exactly once per host:

```bash
PATH=/opt/rocm-therock/bin:$PATH \
LD_LIBRARY_PATH=/opt/rocm-therock/lib \
ROCM_HOME=/opt/rocm-therock \
timeout --signal=TERM --kill-after=15s 1500s \
make test-mxfp4-rocm \
  ROCM_HOME=/opt/rocm-therock \
  HIPCC=/opt/rocm-therock/bin/hipcc
```

Require TP oracle 9/9 in F32 rollback, production-prequant oracle 1/1 with finite=4096/4096, CLI 49/0, full MXFP4 pass, no runtime/kernel errors, and post-validation bilateral runtime SHA still exact `4c20e811...10d7`. Prove test processes gone and safely FLOCK-remove only dead/unheld host `/tmp/ds4.lock` files. Any failure: no Arm A and restore; correcting a command in the same outage is forbidden.

## Conditional Arm A — standalone max2

Execute the final reviewed Arm-A runner once. It independently verifies the runtime/model size, computes a full max2 model SHA, verifies checker identity, atomically owns a local mutex and remote nonce token, pins production prequant and invasive mode, and launches one bounded standalone command:

```text
--rocm -m MODEL
--ssd-streaming --ssd-streaming-cold --ssd-streaming-cache-experts 32GB
-c 8192 -n 1 -p Hello --raw-prompt --temp 0
```

Dump filter:

```text
hc_attn_post,hc_ffn_post,ffn_moe_topk,ffn_moe_weights_scaled,result_output
LAYER=all
```

Require runner rc0 and its integrated checker rc0:

- exactly 173 files / 6,155,280 bytes;
- 43 each of both HC tensors, top-k IDs, and router weights, plus one output;
- exact sizes 65,536 / 24 / 24 / 517,120;
- exactly 1,538,562 finite f32 values;
- 43 valid top-k vectors, each six unique IDs in `[0,255]`;
- no dump synchronize/resume error in the required log;
- output SHA exactly `21afe05...05ae`;
- process absent, lock safely removed, no holder.

Only after all gates does A atomically publish a fresh nine-line result marker binding its runner/checker/runtime/full-model/stat/output/run nonce while the local mutex and remote owner token remain held. It then releases both owners and atomically publishes a two-line commit companion containing the nonce and result-marker SHA. On any trappable failure before commit, its owned provisional marker is removed. B requires both files and absent A owners.

Any A failure: no p15, no B, restore.

## Conditional p15 and Arm B

Peer validates the A report/marker and swaps p15 max2-first under the usual module/endpoints/TB-IP/holder/dmesg gates. Then execute the final reviewed B runner exactly once.

B atomically acquires the same local/max2 exclusion used by A plus a max worker owner token, all under a fresh B nonce, before marker validation. While holding those exclusions it verifies the fresh A result+commit pair, max2 model stat unchanged, the complete max model hash against A's frozen-output-bound max2 digest, and bilateral runtime/checker identities. It atomically consumes the commit and result markers before any destructive remote prefix/log/lock preflight, carries its nonce in both rank environments, and then performs a single barrier-gated worker launch:

- max2: coordinator/rank0, `10.99.0.2:5601`;
- max: worker/rank1, NHI `/run/ds4-tbstream/device`;
- same raw Hello / `-n 1` / temp0 / MTP disabled / prequant1 / invasive all-layer dump filter;
- root 1,200-second bounds, local 900-second completion cap;
- prompt one-sided failure abort;
- worker-first TERM/wait/start-revalidate/KILL/no-live cleanup.

Require rc0/rc0, no dump/log errors, clean transport and all 86 gates. Integrated inventories must pass:

- rank0: 173 files / 6,155,280 bytes / 1,538,562 finite f32;
- rank1: 172 files / 5,638,160 bytes / 1,409,282 finite f32;
- 43 valid top-k vectors each; worker has no output.

No retry or follow-on under any outcome.

## Restoration before analysis

Capture logs, rc, dump manifests, endpoint/transport state, marked dmesg, locks, processes, and holders. Prove diagnostic processes gone and safely remove only owned locks/tokens. Restore production module worker-first then coordinator, timers, worker/server/exporter, and perform the complete API/exporter/PrivateTmp-lock/endpoints/TB-IP/dmesg audit. Analysis begins only after restoration PASS.

## Post-restoration analysis

1. Require byte identity rank0/rank1 for every replicated HC tensor, top-k ID vector, and router-weight vector.
2. Against standalone, report every HC stage's max absolute error, MAE, RMSE, relative RMSE, cosine, and SHA; distinguish first byte difference from first relative-RMSE thresholds `1e-6`, `1e-4`, `1e-3` and cosine `<=0.999999`.
3. Report first exact router ID/order mismatch. Compare router weights only while IDs/order match.
4. Compare coordinator output against frozen and prior bad SHAs, argmax/top-5, full error metrics.
5. Classify all results as invasive-path localization only. Full decode remains NO-GO.
