# 2026-08-25 — DS4 Stage-3 `9b383ac` one-prefill window: full transport, wrong logits

## Verdict

**Stage 3 model correctness remains NO-GO. Batch, ordinary decode, and
performance work remain blocked.**

The exact `9b383ac` repair removed the previous ROCm shared-down dispatch
failure: both TP ranks completed successfully, all 86 layer gates ran over NHI,
and both processes exited 0. Transport and kernel evidence were clean.

The model result was nevertheless incorrect:

- coordinator logits were finite and correctly sized, but had argmax token
  **201**, not the frozen standalone argmax **5**;
- the coordinator and standalone top-five sets had no overlap;
- coordinator-vs-standalone relative RMSE was `0.7203`;
- the worker did not execute/dump the output head, so bilateral final-tensor
  identity is also unproven.

There was no retry or follow-on. Production was fully restored and verified.

## Exact inputs

- DS4 source:
  `9b383ac47a26df8a18fdaf78aa33ca96cd42a8d2`
- parent:
  `2f178345bf67ef99d4406b0fb4e94bfe50dfaaf3`
- canonical 1,488-file manifest SHA256:
  `f330d6addfd168816b126659bf427833c9e2b8160156a421633a4a66e2e233a3`
- bilateral `ds4` SHA256:
  `4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7`
- bilateral `ds4_rocm.o` SHA256:
  `4251387ba9804180ee7c867d069ea14ff23ba02d05d5be02a8a8eec3f366e431`
- bilateral `ds4_rocm_unavailable.o` SHA256:
  `bfae461b279ba4a294cbc61107b525785abb43c157c8ab17e116e24a10569a06`
- bilateral `ds4_transport_nhi.o` SHA256:
  `d72d393ed93842bcdb9f2540dc8b57e6691ba0dc839b3efac58b8c932481c553`
- bilateral TP test SHA256:
  `cda5203f15e34612a0dd86a3043277581564fd2e0070297b284ca5155354b48c`
- bilateral live-NHI test SHA256:
  `c61188161b945c4e82f558e004681b1e1e2ce7f0a9432bc6f019e8b0707780ae`
- p15 SHA256:
  `8a80396bb27cd3403182ff1b5aef32325bd4f142530151345398d7d07db321e0`
- frozen standalone-logit SHA256:
  `21afe05ce62438777bfdd73708338c85e3a62a832336b17ac2c1f89792bc05ae`
- model size on each host: 155,976,458,848 bytes
- hosts/kernel: max and max2, `7.1.5-101.fc43.x86_64`, gfx1151,
  TheRock ROCm/HIP

The build was staged independently from `git archive` of the exact commit.
Every one of 1,488 tracked files was rehashed on each host and matched the
canonical archive manifest. Source-provider review also confirmed one unresolved
generic K-slice call in `ds4.o`, one strong provider in `ds4_rocm.o`, no provider
in the unavailable object, and one linked provider in the final binary.

## Test and production preflight

Fresh bilateral evidence passed:

- `make strix-halo`;
- `make test-rocm`, including the exact generic shared-half oracle at 9/9 and
  CLI checks at 49/0;
- full real-shape MXFP4 suite, including span exactness, all variants,
  contiguous-half recombination, and strict rank-local cached expert halves.

Before production shutdown, both hosts passed:

- active worker/server/exporter and idle exporter request count;
- fresh `/v1/models` and 11-prompt + 1-completion API inference;
- exact private production lock inode equality with fd 3 and a live FLOCK;
- production module SHA256 `ccd94633…e2650e7`, with diagnostic parameter
  absent;
- exact HopID 9/9, ring 4096, throttling 0 endpoints with no holder;
- active reconcile timers;
- bilateral TB-IP with zero loss;
- no staged process or Stage-3 lock.

Fresh test runs had left an unlocked host-namespace `/tmp/ds4.lock` on each
host. Each file's recorded PID was proven dead, no process or `/proc/locks`
entry held it, and it was removed before the window. Production private locks
were separate inodes and remained valid until orderly shutdown.

Production was stopped worker-first, followed by server/exporter. Timers were
stopped, quiescence was re-audited, and p15 was loaded max2-first. Both
endpoints read diagnostic mode `Y`, remained exact and holder-free, retained
healthy TB-IP, and received fresh dmesg markers.

## Bounded command

Each rank used the same binary and model with:

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

Rank-specific prefixes were `/tmp/stage3-9b383ac-tp-r0` and `...-r1`.
Rank 0 listened at `10.99.0.2:5601`; rank 1 was launched exactly once only
after the coordinator logged its listen barrier.

This existing CLI route performs bilateral warmup, one mirrored raw prefill,
and a CPU-side sample from the resulting logits. The `-n 1` cap is reached
before ordinary or speculative sampled-token evaluation. No batch, VERIFY,
sampled-token EVAL, retry, or follow-on was run.

## Execution result

Both shards loaded and warmed successfully. The network-TP path passed the
previous shared-down and routed-MoE failure points and completed all 43 ATTN and
43 FFN gates.

```text
rank 0 rc = 0
rank 1 rc = 0
```

### Transport close snapshots

| Host / rank | Flags | failures / drops / CRC / overrun | TX submitted/completed | RX posted/completed |
|---|---|---|---:|---:|
| max2 / rank 0 | `0x1a` CLOSED, TX_IMPORTED, RX_IMPORTED | 0 / 0 / 0 / 0 | 688 / 688 | 4784 / 689 |
| max / rank 1 | `0x18` TX_IMPORTED, RX_IMPORTED | 0 / 0 / 0 / 0 | 688 / 688 | 4783 / 688 |

Eight frames per gate times 86 gates equals the observed 688 completed TX
frames on each rank. There was no FAILED flag, uncompleted TX, timeout, CRC,
overrun, event drop, or marked kernel message. Marked dmesg contained only the
explicit arm markers.

The printed `11.18 t/s` prefill rate is diagnostic-window timing and is **not**
a benchmark result.

## Dump behavior

The coordinator wrote:

- `result_norm`: 4,096 float32 values / 16,384 bytes;
- `result_output`: 129,280 float32 values / 517,120 bytes.

The worker wrote neither dump. Its clean terminal sequence was `leader
finished` followed by transport statistics, without running the output head.
Therefore final rank-to-rank byte identity cannot be inferred from this arm.
This is a CLI/graph-observation limitation distinct from the numerical failure
below.

## Numerical result

Coordinator norm:

```text
finite       4096 / 4096
min          -3.18662
max           2.12827301
mean         -0.0020489319
RMS           0.310004774
SHA256        8ae3f3ce9cf430a7041d7d512eedce217cce01c6a6a3963b9674748fcdc51065
```

Coordinator logits:

```text
finite       129280 / 129280
argmax       201
SHA256       325a161c354876dac279a90bb8fef680acd6522ed7b211dbc94a24d4569ceae4

top 5:
201     20.5046806
271     16.7026997
539     15.8026400
18439   14.9197540
818     14.6042156
```

Frozen standalone logits:

```text
argmax       5

top 5:
5       13.9951410
110704  13.2166653
795     12.9917479
372     12.9820995
7519    12.8689537
```

The top-five sets have no overlap. Full-vector error was:

```text
max absolute error       18.5687013 (index 104937)
mean absolute error       1.93182607
RMSE                      2.49725025
relative RMSE             0.720308085
cosine similarity         0.77824149922
absolute-error p50        1.58171457
absolute-error p99        6.93628165
values with |error| > 1  86555 / 129280
values with |error| > 5   5524 / 129280
```

This is a substantive model-graph correctness failure, not floating-point
roundoff.

## Interpretation and next gate

`9b383ac` is valid execution progress: it correctly wires the generic ROCm
Q8 K-slice path, eliminates the bilateral shared-down return, and allows one
complete 86-gate TP prefill over clean NHI transport. It does **not** establish
model correctness.

Another arm of this same binary would add no justified information. A source
follow-up should first expose a tensor that both ranks necessarily compute,
such as final-layer (layer 42) `hc_ffn_post`, before the final FFN gate/output
transition. A separately reviewed diagnostic can then determine whether ranks
diverge and compare the replicated in-layer state against standalone. Any such
instrumentation requires its own source tests, review, staged provenance, and
separate no-retry window.

## Restoration

Logs, dumps, transport state, locks, and marked dmesg were captured before
cleanup. Root test locks were removed only after their processes were proven
dead and the stream device holder-free.

Production module restoration was performed worker host first and coordinator
host second. Worker was started before server, then exporter. Final bilateral
audit passed:

- exact installed production module restored; diagnostic parameter absent;
- exact 9/9, ring-4096, throttling-0 endpoints and active timers;
- worker/server/exporter active;
- no test lock or stream-device holder;
- production private-lock path and fd 3 had identical inodes with live FLOCKs;
- TB-IP zero loss;
- fresh `/v1/models` and 11-prompt + 1-completion API inference passed;
- exporter returned to zero active requests;
- marked dmesg remained spotless.

## Artifacts

Complete immutable evidence is under
`bench/results/2026-08-25-ds4-stage3-9b383ac-one-prefill/`, including:

- exact source manifest and provenance;
- bilateral build, test, and MXFP4 logs;
- prewindow, in-window, and restored-state audits;
- complete rank logs and rc files;
- coordinator norm and output dumps;
- frozen standalone reference and numerical analysis;
- exact runbook;
- pre/post production API and exporter evidence;
- `SHA256SUMS`.
