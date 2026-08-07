# DS4 ROCm Q/KV fusion optimization — 2026-08-06

Hosts: `max` and `max2`, one direct Thunderbolt/USB4 link, full
155,976,458,848-byte DeepSeek V4 Flash MXFP4 model. The controlled comparison
used byte-identical binaries from `/opt/ds4-qkv-fuse-20260806-r1`: `ds4`
SHA-256 `17cd24d553f44592f972e51d906ec99507f972814eee25f319b4ba8382777862`
and `ds4-server`
`4ff3db82637e5febfff6d5b8d2c4ac0ed9cfaa8ca8642041924fcba84f86f77f`.

## Result

Fusing DeepSeek V4 Q/KV weighted RMS normalization with KV RoPE removes one
ROCm launch per decoded layer and produces a small, repeatable full-model gain.
Across a balanced `OFF -> ON -> ON -> OFF` cohort, each state generated 1,600
measured completion tokens:

| Metric | Unfused | Fused | Change |
|---|---:|---:|---:|
| Server internal decode throughput | 14.57487 tok/s | 14.60320 tok/s | **+0.1944%** |
| Combined two-host GPU stream time | 67.30038 ms/token | 67.15398 ms/token | **-0.2175%** |
| Throughput equivalent of GPU stream change | — | — | **+0.2180%** |
| Combined sampled-layer QKV segment | 0.63719 ms | 0.62842 ms | **-1.3762%** |

Both order pairs were positive: +0.2923% for `OFF -> ON` and +0.0967% for
`ON -> OFF`, based on the server's internal decode spans. All 40 measured
responses produced 80 tokens with the same normalized response SHA-256,
`d36e327bfb816b6e582f76a4992a429e750f1300aa533c18dda00d1a0eeb2df5`.

Client HTTP wall time contained several pauses that were absent from the
server's request and decode spans, especially in both unfused arms. It implied
an implausible +5.3% change and is retained in the CSV for transparency, but it
is not used as the headline result. The stable server decode ledger and the
independent GPU-event delta agree at approximately +0.2%.

## Implementation and correctness

The resident one-token ROCm path now normalizes Q and KV in the existing
two-block layout, then has the KV block rotate its normalized tail after a
block-wide barrier. Q and KV remain distinct graph allocations. The production
shape has 32 RoPE pairs; shapes through 256 pairs use the fused kernel, while
larger shapes use the retained two-launch implementation. Set
`DS4_ROCM_DISABLE_QKV_KV_ROPE_FUSION=1` for a process-start diagnostic rollback.

The first strict differential test found a real YaRN-only discrepancy. A loop
in the fused kernel let ROCm fast-math hoist a reciprocal for the YaRN ramp,
changing blend rounding. Replacing the loop with one pair per thread restored
the retained kernel's arithmetic without volatile storage or a floating-point
pragma. The final differential suite is bit-exact for dense, scaled,
high-position, YaRN, inverse, multi-row, multi-head, zero-rotation, exact-256,
and 257-pair fallback cases, plus rejected shapes.

The retained RoPE launcher also now checks tensor, byte, and pair products
before narrowing to 32 bits, handles zero pairs without a zero-grid launch, and
uses a non-wrapping grid calculation.

## Rows-per-wave sweep

Before the fusion, the profiled routed-MoE kernel received a controlled
rows-per-wave sweep. The existing one-row specialization remained best:

| Rows per wave | 800-token throughput | Change from 1 |
|---:|---:|---:|
| 1 | 12.68110 tok/s | baseline |
| 2 | 12.43164 tok/s | -1.967% |
| 4 | 12.00071 tok/s | -5.365% |
| 8 | 11.43897 tok/s | -9.795% |

All variants passed the synthetic ROCm MXFP4 matrix and produced identical
full-model output. Separate gate/down controls showed that gate=2 regressed and
down=2 was effectively neutral. The experimental source was therefore reverted
instead of retaining unused runtime knobs.

## Method

- Same candidate binary for both states; only
  `DS4_ROCM_DISABLE_QKV_KV_ROPE_FUSION=1` changed.
- TCP distributed split with layers `0:21` on the coordinator and `22:output`
  on the worker; no disk KV reuse in the benchmark services.
- Each fresh process received a discarded 32-token warm-up followed by ten
  deterministic 80-token requests.
- The in-process event profiler sampled 832 tokens per host per arm with zero
  drops, malformed records, coverage errors, or accounting warnings.
- Direct decode throughput comes from the server's ten `gen=80` decode spans
  per arm; GPU time and QKV time are weighted profiler means.

Raw arm and host-role aggregates are in
[`2026-08-06-ds4-rocm-qkv-fusion.csv`](2026-08-06-ds4-rocm-qkv-fusion.csv).

## Validation and final state

- Both hosts completed warning-clean full `strix-halo` builds from the final
  source.
- The final ROCm Q/KV differential suite passed all 12 positive cases and its
  invalid-shape ledger bit-exactly.
- The ROCm MXFP4 routed-MoE suite passed tokens `1,2,3,4,5,32,128,512` plus
  streaming cold/warm paths.
- Local deterministic tests passed: layer pack 97/97, transport 111/111,
  protocol v3 87/87, ROCm timing 183, public timing stubs 15, distributed
  framing, placement 98/98, GPU arguments, Metal kernels, and Metal MXFP4.
  The aggregate target reached its final optional model smoke, which could not
  run because the local `ds4flash.gguf` fixture is absent; the full model was
  exercised live on both ROCm hosts instead.
- A strict CPU-only `-Werror` build completed.
- The final safety-only rebuild is installed byte-identically as
  `/opt/ds4-qkv-fuse-20260806-r2`: `ds4`
  `0166cbaa243ec265553114bfd49c74a4b1f6c7d80682373584c25109ae0b1035`,
  `ds4-server`
  `8ba7613bedace28c4cc97b5adfed648d1adad7a3081d3963257cd880b698633a`.
  Its 160-token full-model smoke matched the benchmark response hash.
- The standard `/opt/ds4-v3tcp-safe-20260805-r2` services were restored with
  fresh PIDs and zero restarts. The worker negotiated protocol v3 over TCP and
  a final two-request, 160-token API smoke matched the same response hash.

The next similarly bounded target is fusing the resident FP8 KV quantization
and raw-cache store. It should remove one more per-layer launch, but the measured
ceiling is again only a few tenths of one percent unless several launch-bound
stages are combined.
