# DS4 full-model ROCm event profile — 2026-08-06

Hosts: `max` and `max2`, one direct Thunderbolt/USB4 link, full
155,976,458,848-byte DeepSeek V4 Flash MXFP4 model. The profiling worker was
SHA-256 `d882a753db7e5c4219fae56ffe3075455846c05af5ed045552ea90aa448f5116`;
the profiling server was
`5669ada0eae7ecfcefd70694b7650fca6298e1be30c6cd9b8e7731acc1df7cfb`.

## Result

The distributed decode is GPU-execution/model-kernel-bound on this measured
path. Across two 800-token profiled TCP-v3 arms, the coordinator's stream-0
work averaged 33.98704 ms/token and
the worker's averaged 33.69936 ms/token. Their corresponding host evaluation
spans were 34.09416 and 33.82140 ms, leaving only 0.22916 ms combined outside
the timed GPU stream. The events therefore account for 99.66% of the combined
host evaluation span on this path.

Within sampled layer bodies, the weighted, layer-count-extrapolated split was:

| GPU operation group | Share of sampled layer work |
|---|---:|
| Routed MoE | 24.55% |
| Attention output projection/fused HC expansion | 24.24% |
| QKV preparation, RoPE, and raw-KV store | 20.71% |
| Shared expert and FFN post-processing | 10.05% |
| Compressor and indexer | 9.80% |
| FFN normalization and router | 7.33% |
| Attention core and inverse RoPE | 3.13% |
| Attention HC-only post step | 0.20% |

These percentages are the normalized sampled-layer segments, not independent
whole-request percentages. Timing events add a small cost to the selected
layer, so multiplying every sampled mean by the layer count slightly
overestimates the uninstrumented layer total. The ranking is stable across the
two hosts and both measured arms. No single transport operation or isolated
kernel group dominates the complete token; the three largest repeated groups
are routed MoE, attention output, and QKV preparation.

The worker's output head is a separate 2.51466 ms/token, about 3.72% of the
combined 67.68640 ms stream-0 span. It is the largest single non-layer target.

## Why direct NHI did not improve throughput

A same-binary, same-prompt NHI event comparison captured 128 complete samples
per host after warm-up. Staged mapped NHI and direct-TX plus the fast handoff
had essentially identical combined GPU time: 67.84875 versus 67.85050 ms/token.
Both modes had zero dropped samples and zero repeated-output mismatches.

Direct-TX did expose one real penalty. On the worker, writing the 505 KiB
logits output into the NHI-mapped system/GTT slot increased output-head time
from 2.51525 to 2.58050 ms: +0.06525 ms, or +2.594%. The direct mask was active
for 124/128 worker samples (`0x4`) and 127/128 coordinator samples (`0x2`).
The +0.065 ms head cost is small globally (about 0.096% of the two-host GPU
span), but it exceeds the approximately 0.015 ms/token of lease
synchronizations removed by the unsafe fast handoff. Other layer groups moved
only at noise scale. This provides a plausible mechanism consistent with the
earlier matched result of -0.13% for direct-TX NHI: the implementation is real
GPU access to NHI-mapped pages, but those pages are system/GTT memory rather
than peer VRAM, and the serial model split has almost no transport time left
to recover.

## Profiler overhead A/B

The profiler was tested on descriptor-framed TCP v3 so NHI behavior could not
confound its overhead. Both hosts used the same profiling binaries and service
arguments; the only arm difference was `DS4_ROCM_EVENT_PROFILE=1`. Disk KV
reuse was disabled, each new process received a discarded warm-up, and the
order was balanced (`OFF -> ON`, then `ON -> OFF`). Each arm served ten fixed
requests and 800 completion tokens with zero mismatches. A separate normalized
greedy-response hash matched exactly between OFF and ON:
`83d8ce7d879f52d1e285fc5695d29eb19c72ea083b1e48d89361f60491b088c6`.

| Pair | OFF mean service request | ON mean service request | ON latency change |
|---|---:|---:|---:|
| OFF -> ON | 7.1484 s | 7.1468 s | -0.0224% |
| ON -> OFF | 7.1480 s | 7.1562 s | +0.1147% |
| Pooled | 7.1482 s | 7.1515 s | +0.0462% |

The pooled service-time result corresponds to approximately -0.0461%
throughput, well below the 0.3% target. The client harness wall summaries are
retained in
[`2026-08-06-ds4-rocm-event-profile.csv`](2026-08-06-ds4-rocm-event-profile.csv),
but they are not used for this overhead conclusion: both
ON runs contained visible inter-request client idle gaps after a completed HTTP
response, while the server's ten per-request spans remained tightly grouped.

The two measured ON arms contributed 1,600 samples per host. They had zero
drops, zero malformed records, exact ratio-group coverage, direct-mask counts
equal to sample counts, and every rounding-aware component residual below
0.003 ms.

## Instrumentation

The ROCm backend owns a lazy, fixed pool of four concurrent samples, each with
16 timing-enabled HIP events. Collection calls `hipEventQuery` and elapsed-time
queries only; it never synchronizes a device, stream, or event. The frontend
records 14 events around one rotating layer per token and collects the 13
adjacent segments only after DS4's existing end-of-slice synchronization.
Failures disable profiling and leave inference behavior unchanged.

The profile is deliberately limited to ordinary, resident, non-streaming,
single-tier, non-speculative decode. It reports stream-0 work. `copyback`
contains only queued direct-output-to-canonical D2D work; an ordinary staged
GPU-to-host read happens after event collection and is included in
`host_minus_stream0`.

Use:

```text
DS4_ROCM_EVENT_PROFILE=1
DS4_ROCM_EVENT_PROFILE_INTERVAL=32
```

Aggregate raw stderr or journal output with:

```sh
bench/scripts/analyze-ds4-rocm-events.py coordinator.log worker.log
journalctl -u ds4.service -o json | \
  bench/scripts/analyze-ds4-rocm-events.py --json -
```

The analyzer weights each already-averaged window by its sample count, checks
segment sums and ratio-group coverage, tracks direct masks and drops, and can
emit human-readable or JSON results.

## Validation and final state

- Fake-HIP timing backend: 183 checks passed, including allocation/record/query
  failures, pool exhaustion, stale generations, cleanup/re-init, and a build
  with no synchronization symbols.
- Non-ROCm public stubs: 15 checks passed.
- Analyzer CLI: 6 tests passed, including malformed input, weighted
  aggregation, consistency checks, and adversarial numeric overflow.
- Local Metal build and strict CPU warning build passed.
- Both Strix hosts completed a full `strix-halo` ROCm build; live begin/mark,
  nonblocking early collect, pool exhaustion, and cleanup/re-init passed on
  `gfx1151`.
- The measured hardened profiler binaries were installed byte-identically on
  both hosts from `/opt/ds4-event-profile-20260806-r2`; a 160-token live
  TCP-v3 smoke produced five complete windows per host with zero drops,
  malformed records, coverage errors, or response mismatches.
- After final review, the exact source completed another full `strix-halo`
  ROCm build on both hosts and was installed byte-identically as
  `/opt/ds4-event-profile-20260806-r3` (`ds4`
  `318816d39cb14a90c282bc80ff370df19d2c0d3c4e3a4c21c18469d1da453e11`,
  `ds4-server`
  `d4ee97fab0da7a1593264212075ba5aea4db4e9c364660c03a9f4db70acf9c45`).
- Full-model TCP and NHI staged/direct runs completed with zero response
  mismatches, profiler drops, service failures, transport failures, or GPU
  failures.
- The standard `/opt/ds4-v3tcp-safe-20260805-r2` services were restored. They
  negotiated protocol v3 with `transport=tcp` and passed both two-request
  restore smokes (160 and 158 completion tokens) with zero mismatches.

The next optimization target is model compute rather than transport safety:
first routed-MoE and attention-output kernels, then QKV preparation. The output
head is also worth a focused kernel pass because it is a stable 2.515 ms/token
and mapped-GTT output measurably slows it.
