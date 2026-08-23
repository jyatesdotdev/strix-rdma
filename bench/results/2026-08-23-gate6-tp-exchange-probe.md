# 2026-08-23 — Gate 6 fault/rollback coverage and the TP-shaped exchange probe

Two campaigns on the transiently deployed patch-14 module set (both hosts,
ring 256, nothing installed): gate 6 of the native DMA-BUF experiment, and a
tensor-parallel-shaped GPU-to-GPU exchange latency probe that bounds the
value of a future hybrid-TP architecture.

## Gate 6 — imported TX→RX transfer, wrap, lifecycle, and fault coverage

New tools: `tbstream-gpu-tx` (imported native TX pool; payload written only
by GPU kernels; `--release event|none` ownership arms) and
`tbstream-import-test` (rollback coverage using udmabuf objects).

| Test | Result |
|---|---|
| Rollback paths (17 cases: bad version/flags/reserved/fd, non-dmabuf fd, short/misaligned/past-end ranges, same-BO both dirs, two opens, import-after-activation, double import, release-without-activation, post-test health) | all correct errno, device healthy |
| GPU-written imported-TX → imported-RX, release=event | 1024 msgs / 64 MiB / 64 wraps, word-exact |
| Same, release=none (diagnostic) | word-exact (stream-sync after dirty work already emits the CLR system release) |
| Close/reopen endurance | 5/5 consecutive sessions clean |
| SIGKILL sender mid-stream | 85,526 msgs verified then clean CLOSE via the kernel release path and dedicated control frame; no failure state, no drops; next session clean |
| IOMMU audit (both hosts, whole campaign) | zero AMD-Vi faults across ~460 MiB imported-pool DMA |
| dmesg audit | max2 pristine; max shows only benign, explained `TX ring flush timed out` warnings: a final CLOSE toward an already-closed peer cannot complete (no E2E credits) and is canceled at stop. Pre-existing close-ordering artifact; polish candidate, not a defect |

Incidental: the kill-test run sustained roughly wire rate (~1 GB/s) through
the full GPU→GPU zero-copy path.

Deferred with justification: physical cable pull and host reboot (need
console/scheduling), deliberate IOMMU-fault injection and GPU reset (unsafe
on the production pair). The negative IOMMU evidence and SIGKILL coverage
bound the risk for the next stage.

## Two findings worth naming

1. **Small `hipMalloc`s share backing BOs.** Two 1 MiB pools were
   suballocated from one slab BO; the exported fds named the same BO and the
   driver's same-buffer-both-directions rejection correctly refused the
   import (a wrong-region mapping would otherwise have followed). Tools now
   pad pools to a proven-dedicated 16 MiB BO and import the leading range.
   Any future DS4 integration must verify `hipMemGetAddressRange` returns a
   dedicated base/size — this is now demonstrated, not theoretical.
2. **Gate 5's coherence is dispatch-scoped, not wave-scoped.** A
   long-running wave polling coarse memory with system-scope atomic loads
   never observes NHI writes: RDNA3 system-scope loads bypass L0/L1 but hit
   GPU L2, which PCIe writes do not invalidate mid-wave. Coarse memory
   appears coherent only across kernel-dispatch boundaries (AQL cache
   invalidation). Persistent-kernel signaling therefore requires uncached
   (MTYPE_UC) flag words; payload can stay coarse when read by
   post-detection dispatches.

## TP-shaped exchange probe (`tbstream-tp-pingpong`)

Both directions imported native pools on both hosts. Fixed-size ping-pong,
2000 iterations (200 warmup), CPU never touches payload. Detection arms:
`reap` (interrupt + REAP wakeup) vs `spin` (persistent GPU wave polling a
stamp word in uncached memory with system-scope atomics; CPU relays only
the submit ioctl; driver events drained off the critical path).

| Message | reap RTT p50 (p99) | spin RTT p50 (p99) | spin one-way |
|---|---|---|---|
| 4 KiB (1 frame) | 20.3 µs (23.6) | **9.8 µs (12.8)** | **~4.9 µs** |
| 32 KiB (8 frames, ≈ fp32 hidden vector) | 57.7 µs (60.6, max 88) | **45.3 µs (46.4, max 47)** | ~22.7 µs |

- GPU-polled delivery halves the small-message RTT and removes the
  interrupt/wakeup jitter entirely (32 KiB max fell from 88 µs to 47 µs).
- Implied burst DMA bandwidth from the size delta: ~1.6 GB/s per direction,
  above the steady-state streaming figure.
- Reference floors: stock zc read/write ping-pong 22.9 µs at 4 KiB; TCP ICMP
  64 µs.

**TP implication.** A two-host all-reduce is one simultaneous bidirectional
half-vector exchange plus a local reduce. Interpolating (one-way ≈ 4.9 µs +
bytes / 1.6 GB/s): a 14 KiB half of the 28 KiB fp32 hidden vector lands in
~14 µs, so ~120 exchanges/token cost ≈ 1.7–2 ms serialized — consistent with
the projected ~1.8–1.9× single-request decode speedup for a hybrid
TP/expert-parallel split (~34 ms compute + ~2 ms comm vs today's ~68.5 ms
pipeline token). The probe validates the transport leg of that estimate;
kernel-side row-split efficiency remains the open half.

## Host state

Both hosts restored to the production 12-patch module set, 9/9 ring-4096
endpoints republished, reconcile timers active, no holders, and a live
chat completion verified on the production TCP-v3 path.

## Verdict

Gate 6 **passes** (with the physical-fault subset deferred and bounded).
The TP probe upgrades hybrid tensor parallelism from speculation to a
measured transport budget. Gate 7 (output-head A/B against the 0.065 ms
host-registered penalty) is the first DS4/live-inference stage and awaits
explicit operator go-ahead.
