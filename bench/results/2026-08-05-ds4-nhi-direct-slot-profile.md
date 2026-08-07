# DS4 NHI direct-slot and hot-path profile — 2026-08-05

Hosts: `max` and `max2`, one direct Thunderbolt/USB4 link, full
155,976,458,848-byte DeepSeek V4 Flash MXFP4 model. The coordinator binary was
SHA-256 `623351a9549870a53248000973d5a50297e08b18458c737545f9b7a00dd7f53d`;
the worker binary was
`dababc2841012013116759a37ca2c0468b5676699e41e3b553c207470fcdc0d8`.

## Result

The decode hot path is inside model evaluation, not NHI lease synchronization
or boundary control. A staged NHI token averaged 68.074 ms in the internal
critical-path ledger. The synchronized coordinator layer-slice evaluation
averaged 33.508 ms and the synchronized worker evaluation averaged 33.651 ms.
Those two spans account for 98.66% of the end-to-end token step. They include
GPU command execution plus evaluation setup, stream mapping, and any staging
inside the layer-slice call, so they are not claimed as pure kernel time.

The opt-in fast path removes the userspace mapped-memory synchronization after
the caller has already completed GPU work and omits copyback of terminal values
that are dead before the next graph. It did not improve throughput. In the
matched, uninstrumented full-model A/B, staged NHI completed 800 tokens in
71.075 s (11.2557 tok/s) and direct-TX fast NHI completed the same prompts,
seeds, and 800 tokens in 71.170 s (11.2407 tok/s). The measured change is
-0.0150 tok/s, or -0.13%, and is not a measured speedup. This single aggregate
A/B does not establish a formal variance or significance interval.

Both cohorts passed ten requests with zero repeated-seed mismatches. Disk KV
reuse was disabled, each service was restarted between cohorts, and an
independent warmup was discarded. The rows are also recorded in
`2026-08-05-ds4-full-model-tps.csv`.

## Critical-path ledger

The following values are means from a fixed 32-token staged-NHI profile. The
profile emits one line per token and is therefore used to attribute time, not
as the headline throughput measurement.

| Span | Mean | Interpretation |
|---|---:|---|
| Coordinator local evaluation | 33.508 ms | Synchronized layers 0:21 layer-slice call |
| Worker model evaluation | 33.651 ms | Synchronized layers 22:output layer-slice call |
| Coordinator remote span | 34.557 ms | Worker evaluation plus NHI/control overhead |
| Complete distributed token span | 68.074 ms | Local slice followed by remote slice |
| Coordinator RESULT wait | 34.352 ms | Mostly the 33.651 ms worker evaluation |
| Coordinator result copy/release | 0.170 ms | Logits copy plus lease synchronization/repost |
| Coordinator control send | 0.029 ms | TCP descriptor/control message |

The four conservative mapped-lease synchronizations measured approximately
0.006 ms (coordinator RX), 0.004 ms (coordinator TX), 0.003 ms (worker RX), and
0.002 ms (worker TX): about 0.015 ms combined per token. The fast path removed
these events, but the complete profiled token moved from 68.074 ms to
68.221 ms. That +0.22% difference is consistent with normal run-to-run GPU
variation.

The measured NHI operations were also small: coordinator RX repost averaged
0.109 ms, coordinator RX acquire wait 0.436 ms, coordinator TX submit 0.008 ms,
worker RX repost 0.007 ms, worker RX acquire wait 0.020 ms, and worker TX submit
0.036 ms. Acquire timing includes locking, event validation, and envelope work,
not only hardware event latency. These are correlated subspans and must not be
added directly.

Subtracting the two evaluation spans leaves 0.915 ms in staged mode and
0.911 ms in fast mode. That residual bounds boundary/control work outside the
evaluation calls, but not staging or mapped-memory access performed inside
them. Its unchanged size is the strongest reason to instrument within model
evaluation next.

## What “direct” means here

The NHI pools are driver-owned system/GTT pages registered with ROCm and exposed
to a GPU as mapped host memory. A decode graph can read or write those pages
without an intervening CPU memcpy. Thunderbolt DMA transfers the same pages.
This is not peer-GPU VRAM DMA, GPUDirect RDMA, or a one-sided GPU-initiated RDMA
operation: control and descriptors remain on TCP, the kernel owns NHI DMA, and
the next model half does not start until the transfer has completed.

`DS4_DIST_NHI_DIRECT_SLOTS=tx` selects graph output aliases into mapped TX
slots. `DS4_DIST_NHI_UNSAFE_FAST=1` additionally selects the benchmark-only
GPU-quiesced lease handoff and dead-output copyback elision. Both are opt-in;
the conservative path remains the default.

## MTP finding

Distributed MTP did not expose a hidden transport win. A qualification sample
with draft depth two accepted 18 of 19 drafted slots (94.74%), but speculative
verification took about 135 ms and direct slots are deliberately disabled for
the multi-row verification exchange. The measured short-greedy cohorts were
approximately 10.19 tok/s on TCP, 10.29 tok/s on staged NHI, and 9.86 tok/s on
direct-slot NHI. Those cohorts used different nonces and are directional rather
than a matched A/B, but none beats the non-MTP operating point.

## Instrumentation and next target

`bench/scripts/analyze-ds4-profile.py` summarizes coordinator/worker decode
ledgers, NHI acquire/sync/submit/repost pairs, direct-slot masks, and MTP draft
acceptance. Enable the qualification-only logs with:

```text
DS4_DIST_DECODE_PROFILE=1
DS4_DIST_NHI_TRACE=1
DS4_DIST_NHI_DIRECT_TRACE=1
```

The next useful pass should put low-overhead ROCm event timers around model
operation groups inside each 33–34 ms evaluation span: attention,
routed-expert selection and gather, routed MoE GEMMs, shared-expert work, and
output projection. It should record events while enqueuing, aggregate counters
after the existing end-of-slice synchronization, and print at request end,
rather than add a synchronization or log per kernel. The distributed boundary
has already been resolved finely enough to show that further transport-safety
removal cannot provide a material gain.

An attempted `rocprofv3` attach to already-running transient DS4 processes
caused both processes to receive SIGSEGV. The transient units were stopped and
subsequent NHI gates and full-model runs passed; no kernel or persistent device
fault followed. Attach mode was not used again. Internal timing was retained as
the safe profiling method for this pass.

After qualification, the standard services were restored from
`/opt/ds4-v3tcp-safe-20260805-r2`. They negotiated protocol v3 with
`transport=tcp` and passed a two-request, 160-token repeated-output smoke test
with zero mismatches.
