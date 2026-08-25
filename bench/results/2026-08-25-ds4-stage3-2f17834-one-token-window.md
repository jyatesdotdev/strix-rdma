# 2026-08-25 — DS4 Stage-3 `2f17834` one-token window: shared-down ROCm dispatch stub

## Verdict

**Stage 3 remains NO-GO. Full decode remains blocked.**

The exact `2f17834` binary passed corrected source provenance and bilateral
ROCm tests, but the separately authorized one-prefill TP/NHI arm failed on both
ranks at layer 0 after the first ATTN exchange:

```text
ds4: TP decode failed layer=0 pos=0 stage=shared_down rank=0
ds4: TP decode failed layer=0 pos=0 stage=shared_down rank=1
```

Both processes exited 1. No `result_norm` or `result_output` dump was produced.
There was no retry or follow-on.

The completed exchange and shutdown were transport-clean: TX 8/8 on both
ranks, zero failures/event drops/CRC/overrun, and CLOSED observed by the
coordinator. Marked dmesg was spotless. This is a deterministic ROCm graph
integration failure, not an NHI failure or performance result.

## Exact inputs and preflight

- DS4 source:
  `2f178345bf67ef99d4406b0fb4e94bfe50dfaaf3`
- canonical 1,488-file source manifest SHA256:
  `3603d1856fe6e0df6c46d749808deb318bbeb722fb4e8d45f1eb2f4c48bd824b`
- bilateral staged `ds4` SHA256:
  `dafe4d4b4ae19682029839aefca442dfc6a407a641c4fb3525fe4a00d13bfc3a`
- bilateral `ds4_rocm.o` SHA256:
  `38769a8c1bf7a79a6c072f22fb62225e96098356b231d3a6f8eae9f692afd4e5`
- bilateral `ds4_transport_nhi.o` SHA256:
  `d72d393ed93842bcdb9f2540dc8b57e6691ba0dc839b3efac58b8c932481c553`
- p15 SHA256:
  `8a80396bb27cd3403182ff1b5aef32325bd4f142530151345398d7d07db321e0`
- host/kernel: max and max2, `7.1.5-101.fc43.x86_64`, gfx1151,
  TheRock ROCm/HIP 7.13
- model size on both hosts: 155,976,458,848 bytes
- frozen standalone oracle SHA256:
  `21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae`

Immediately before this separate window, all 1,488 tracked source files were
rehashed on both hosts and again matched the canonical Git-tree manifest.
Staged binaries, objects, p15, production module, and swap scripts were
reverified. Fresh exact-source bilateral logs already covered:

- `make test-rocm`, including TP 9/9 and CLI 49/0;
- the complete real-shape MXFP4 suite, including strict rank-local cached
  expert halves and recombination.

Production preflight passed on both hosts:

- active worker/server/exporter and idle exporter request count;
- `/v1/models` plus fresh 11-prompt + 1-completion API inference;
- production `PrivateTmp=yes` lock path identical to fd 3 with a live FLOCK;
- installed production module SHA256 `ccd94633…e2650e7` and no diagnostic
  parameter;
- exact HopID 9/9, ring 4096, throttling 0 endpoints and no holders;
- active reconcile timers;
- bilateral TB-IP with zero loss;
- no host or `/run` test lock.

Production was stopped worker-first. P15 was swapped on max2 first and then
max. Diagnostic mode read `Y`; both endpoints remained exact and holder-free;
timers remained stopped; TB-IP remained healthy; fresh dmesg markers were
written.

## Bounded command contract

The coordinator and worker used the same exact binary and model with:

```text
DS4_LOCK_FILE=/run/ds4-stage3.lock
DS4_TP_SPIN_MAX=800000000
DS4_MTP_SPEC_DISABLE=1
DS4_ROCM_GRAPH_DUMP_NAME=result_norm,result_output
DS4_ROCM_GRAPH_DUMP_LAYER=43
-c 8192 -n 1 -p Hello --raw-prompt --temp 0
--tensor-parallel --transport nhi
--nhi-device /run/ds4-tbstream/device
```

Rank-specific dump prefixes were `/tmp/stage3-2f17834-tp-r0` and `...-r1`.
Rank 0 listened on `10.99.0.2:5601`; rank 1 connected as worker.

This existing-CLI path performs bilateral session warmup, one mirrored raw
prefill, samples one token from the resulting logits, and reaches the cap before
any ordinary or speculative token evaluation. The worker was not launched
until rank 0 logged its worker-listen barrier. No batch, VERIFY, sampled-token
EVAL, retry, or follow-on was authorized.

## Runtime result

Both resident shards loaded and warmed successfully. TCP control and imported
NHI pools initialized. The layer-0 ATTN gate completed, after which both ranks
reported the same first-failure label:

```text
stage=shared_down
```

No FFN gate completed, and neither final-layer dump existed.

### Close snapshots

| Host / rank | Flags | failures / drops / CRC / overrun | TX submitted/completed | RX posted/completed |
|---|---|---|---:|---:|
| max2 / rank 0 | `0x1a` CLOSED, TX_IMPORTED, RX_IMPORTED | 0 / 0 / 0 / 0 | 8 / 8 | 4104 / 9 |
| max / rank 1 | `0x18` TX_IMPORTED, RX_IMPORTED | 0 / 0 / 0 / 0 | 8 / 8 | 4103 / 8 |

These are the same clean one-gate transport counts as the earlier arm. There
was no FAILED flag, uncompleted TX, driver error, timeout, or marked kernel
message.

## Root cause

The diagnostic label localizes the return to the real network-TP shared-expert
down projection, before the routed-MoE call fixed by `2f17834`:

1. `metal_graph_encode_decode_layer_phase()` selects the TP shared K-slice and
   calls `metal_graph_matmul_dense_quant_kslice()`.
2. That graph wrapper dispatches through the generic
   `ds4_gpu_matmul_quant_kslice_tensor()` API.
3. ROCm `2f17834` implements the Q8-specific
   `ds4_gpu_matmul_q8_0_kslice_tensor()`, but not the generic typed dispatcher.
4. `ds4_rocm_unavailable.cu` still supplies the generic symbol as an
   unconditional `return 0` stub.

Therefore both ranks fail synchronously before a shared-down kernel launch.
The existing exact-geometry shared-slice test passed because it called the
Q8-specific API directly; it did not exercise the generic dispatcher used by
the live graph.

The required repair is narrow and testable: provide a ROCm generic K-slice
dispatch for Q8_0 to the existing implementation, fail closed for unsupported
types, remove the conflicting unavailable stub, and make the shared-slice
regression call the generic API. The old code must fail that focused test and
the repaired code must pass before another source review. The rank-local routed
MoE fix in `2f17834` remains unexercised on the live graph because execution did
not reach `routed_moe_folded`.

## Restoration

Both wrappers were gone and both devices holder-free before cleanup. Logs,
locks, endpoint state, and dmesg were captured; root test locks were removed
only after their PIDs were proven dead.

Production modules were restored worker host first and coordinator host second.
Worker was started before server, followed by exporter. Final bilateral audit
passed:

- installed production module hash restored exactly;
- diagnostic parameter absent;
- exact 9/9, ring-4096, throttling-0 endpoints and active timers;
- worker, server, and exporter active;
- no test lock or stream-device holder;
- production private lock inode matched fd 3 with a live FLOCK;
- TB-IP zero loss;
- `/v1/models` and fresh 11-prompt + 1-completion API inference passed;
- exporter returned to zero active requests;
- marked dmesg contained only the explicit window markers.

## Artifacts

Durable evidence is under
`bench/results/2026-08-25-ds4-stage3-2f17834-one-token/`:

- complete rank-0 and rank-1 logs and rc files;
- bilateral in-window and restored-state audits;
- pre/post production model and inference smokes;
- exact provenance summary;
- `SHA256SUMS`.

No throughput or model-correctness claim can be made from this window.
