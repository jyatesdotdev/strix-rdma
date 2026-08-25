# 2026-08-25 — DS4 Stage-3 one-token window: standalone oracle passes; TP fails after first gate

## Verdict

**Stage 3 remains NO-GO. Full decode remains blocked.**

The frozen standalone ROCm oracle completed and produced one exact-size,
all-finite full-logit dump. The first DS4 tensor-parallel NHI engine arm then
failed on both ranks during raw one-token prefill, after exactly one 8-frame
ATTN exchange and before the layer-0 FFN exchange. It produced no TP logit
dump and is not a correctness or performance sample.

The one completed exchange and ordered close were transport-clean: both ranks
reported TX 8/8, zero driver failures/event drops/CRC/overrun, and the
coordinator observed CLOSED. Marked dmesg was spotless and TB-IP remained
healthy. There was no retry.

## Reviewed and staged inputs

- DS4 source: `0a2cf2119cc9c55c51606222e8fecfe085b76371`
- staged `ds4` SHA256, identical on both hosts:
  `7241c9712807c965bbeac298ef6b25ccd84da935f92cc3f6ebfe9daf7e04dada`
- patch-15 module MD5, identical on both hosts:
  `18f21563e2c6051e06070c8765a20f2e`
- host/kernel: `max` and `max2`, `7.1.5-101.fc43.x86_64`, gfx1151,
  TheRock ROCm/HIP 7.13
- model:
  `DeepSeek-V4-Flash-MXFP4Experts-F16HC-F16Compressor-F16Indexer-Q8Attn-Q8Shared-Q8Out-chat-v2-mxfp4-0731.gguf`
- model size: 155,976,458,848 bytes on both hosts
- lock paths: standalone `/run/ds4-stage3-ref.lock`; TP
  `/run/ds4-stage3.lock`; the default `/tmp` lock was never used

Before the window, both staged binary hashes, p15 module hashes/vermagic,
swap-script hashes, production module hashes, endpoints, 4096-frame 9/9
configuration, timers, sudo access, and absence of device holders were
reverified. Bilateral TB-IP ping and fresh production API model/chat smokes
passed. Production was then stopped worker-first; the exporter and both
reconcile timers/watchdogs were also stopped. Process and holder audits were
empty before either diagnostic arm.

Both diagnostic commands used `--raw-prompt --temp 0`, making `Hello` the
intended raw one-token prompt and sampling deterministic.

## Standalone full-logit oracle: PASS

The standalone reference ran on `max2` while the production module was still
loaded, before either p15 swap. It used the same staged binary, an SSD-streamed
single-host model, and the existing graph diagnostic at final layer 43:

```text
DS4_LOCK_FILE=/run/ds4-stage3-ref.lock
DS4_ROCM_GRAPH_DUMP_PREFIX=/tmp/stage3-single
DS4_ROCM_GRAPH_DUMP_NAME=result_output
DS4_ROCM_GRAPH_DUMP_LAYER=43

ds4 --rocm -m MODEL \
  --ssd-streaming --ssd-streaming-cold \
  --ssd-streaming-cache-experts 32GB \
  -c 8192 -n 1 -p Hello --raw-prompt --temp 0
```

Result:

| Check | Result |
|---|---|
| exit status | 0 |
| emitted token | `#` |
| dump | `stage3-single_result_output-43_pos0.bin` |
| dump bytes | 517,120 |
| elements | 129,280 f32 |
| finite / nonfinite | 129,280 / 0 |
| SHA256 | `21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae` |
| argmax | token ID 5, logit 13.99514102935791 |
| top-5 IDs | 5, 110704, 795, 372, 7519 |

The log contained no ROCm/model failure and marked dmesg contained only the
window marker. The process was confirmed gone before its root-owned `/run`
lock was removed. The compressed dump is retained with this result so a future
TP dump can be compared without rerunning the standalone arm.

## Tensor-parallel NHI arm: FAIL, no retry

Patch 15 was transiently swapped on `max2` and then `max`; neither module was
installed. Both diagnostic parameters read `Y`, both production endpoints
were recreated, timers remained stopped, no process held either device, and
TB-IP remained healthy.

Rank 0 was launched first on `max2`. Rank 1 was not launched until rank 0 had
finished warming its 76.73 GiB shard and logged:

```text
ds4-tp: waiting for worker on 10.99.0.2:5601 ...
```

Both ranks used:

```text
DS4_LOCK_FILE=/run/ds4-stage3.lock
DS4_TP_SPIN_MAX=800000000
DS4_ROCM_GRAPH_DUMP_NAME=result_output
DS4_ROCM_GRAPH_DUMP_LAYER=43
--raw-prompt --temp 0
--tensor-parallel --transport nhi
--nhi-device /run/ds4-tbstream/device
```

Rank-specific dump prefixes were `/tmp/stage3-tp-r0` and
`/tmp/stage3-tp-r1`. Rank 0 used `-n 1 -p Hello`, role coordinator, and
`--listen 10.99.0.2 5601`; rank 1 used role worker and
`--coordinator 10.99.0.2 5601`.

Both shards loaded and warmed cleanly, the TCP control connection formed, and
NHI imported pools initialized. The engine then completed one 8-frame gate
and failed:

```text
rank 1: tp worker sync: rocm prefill failed
rank 0: ds4: prompt processing failed: tp: worker prefill sync failed
rank 0: ds4: tp: worker failed during session destroy
```

Both exit statuses were 1. Neither rank produced a
`result_output-43_pos0.bin` dump, so no TP SHA, finiteness, argmax, or
single-vs-TP error comparison exists.

### Close snapshots

| Host / rank | Flags | failures / drops / CRC / overrun | TX submitted/completed | RX posted/completed |
|---|---|---|---:|---:|
| max2 / rank 0 | `0x1a` CLOSED, TX_IMPORTED, RX_IMPORTED | 0 / 0 / 0 / 0 | 8 / 8 | 4104 / 9 |
| max / rank 1 | `0x18` TX_IMPORTED, RX_IMPORTED | 0 / 0 / 0 / 0 | 8 / 8 | 4103 / 8 |

The production ring starts with 4095 RX descriptors. Rank 1's 4103 posted is
4095 + the eight data descriptors; rank 0 additionally completed the worker's
CLOSE and reported CLOSED. There was no FAILED flag and no uncompleted TX.

The raw logs contain no TP gate-service failure, event timeout, enqueue error,
NHI submit error, or driver error before the generic prefill failure. Source
ordering localizes the return to the layer-0 interval after the ATTN gate
(`ds4.c` gate at line 23596 in `0a2cf21`) and before the FFN gate (line 25068).
It does not identify which operation in that interval returned false.

Post-window, a focused 4096-by-4 synthetic ROCm oracle exercised the TP-only
`ds4_gpu_hc_expand_add_tensor(a,b,residual,post,comb)` against an explicit
`add(a,b)` followed by the established HC expand and passed bit-for-bit; the TP
ROCm suite became 8/8. That rules out a generic failure of the HC add API/kernel
with exact-sized tensors, but not a live-graph interaction or a later stage.
First-failure labels across the post-ATTN/FFN interval are required before
considering another node window. No root cause is claimed here.

## Dmesg and restoration

Both TP processes and their wrappers were gone and both devices had no holders
before cleanup. Complete logs/state were copied before the root-owned test
locks were removed. No retry was attempted.

Production modules were restored worker host first and coordinator host
second with `p14-swap.sh off`. On both hosts:

- installed production module SHA256 reverified as
  `ccd946330a5612ffe7674246ac387b39d4a2e71562ed585cfc65d8d58e2650e7`;
- `zc_diagnostic_dmabuf` was absent;
- `/dev/tbstream0` and `/run/ds4-tbstream/device` were healthy;
- reconcile timers were active;
- marked dmesg contained no tbstream/thunderbolt/IOMMU/AMDGPU warning,
  error, timeout, or fault;
- bilateral TB-IP had zero loss;
- worker, server, and exporter were active;
- `/v1/models`, exporter metrics, and a fresh 8-prompt + 1-completion API
  inference passed;
- each production `PrivateTmp=yes` lock path matched fd 3 by device/inode and
  held a kernel write FLOCK;
- host-namespace and `/run` test locks were absent after cleanup.

A stale host-namespace `/tmp/ds4.lock` on `max` was removed only after proving
it differed from the active production-private inode and acquiring it
nonblocking.

## Artifacts

Durable artifacts are under
`bench/results/2026-08-25-ds4-stage3-one-token/`:

- complete standalone, rank-0, and rank-1 logs;
- per-host failure-state, lock, stats, marked-dmesg, and TB-IP snapshots;
- compressed standalone full-logit dump;
- `REFERENCE.txt` and `SHA256SUMS`.

Remote originals remain at:

```text
max2:/tmp/stage3-single.log
max2:/tmp/stage3-single_result_output-43_pos0.bin
max2:/tmp/stage3-tp-r0.{log,rc}
max:/tmp/stage3-tp-r1.{log,rc}
```

No throughput claim can be made from this window. A new bounded window remains
blocked on a source-level root cause, a focused regression, and fresh review.
