# 2026-08-23 — Gate 4: live DMA-BUF import probe against the NHI DMA device

Gate 4 of the native HIP DMA-BUF experiment (`docs/GPU_TO_GPU_FEASIBILITY.md`,
"Recommended next gates"): run the no-traffic `TBSTREAM_ZC_DMABUF_PROBE`
diagnostic (zero-copy patch 13) on real hardware against the stream ring's
actual NHI DMA device, for a CPU-only udmabuf control and for
coarse/uncached/fine-grain native HIP exports, in both probe directions.

## Method

- Host: `max` (follower endpoint), kernel `7.1.5-101.fc43.x86_64`, IOMMU
  translated/default, Radeon 8060S (gfx1151), TheRock HIP 7.13.
- The gate-3 build artifact `~/src/linux-stable/drivers/thunderbolt/thunderbolt_stream.ko`
  (13-patch series head `a695894de`, exact vermagic) was live-swapped in via a
  transient `insmod ... zc_diagnostic_dmabuf=1` after stopping the reconcile
  timer and running the managed ConfigFS cleanup. **Nothing was installed**:
  `/lib/modules`, the initramfs, and the loaded `thunderbolt.ko` (12-patch
  production build) were untouched. Patch 13 needs no new `thunderbolt.ko`
  symbol (`tb_ring_dma_device()` is a static inline), so the patched stream
  module loads cleanly against the running production core module.
- The stream endpoint was recreated at the production exact-9/9 HopID policy
  and republished at `/run/ds4-tbstream/device`. The probe opens
  `/dev/tbstream0`, attaches the DMA-BUF to `tb_ring_dma_device()`, pins/maps
  one direction, validates SG geometry against the flatten rules, and tears
  down before returning aggregates. No ring descriptor is ever programmed.
- HIP arms use the new companion `tools/dmabuf-probe/tbstream-hip-export`:
  dedicated allocation (base/full-range verified with
  `hipMemGetAddressRange`), GPU-kernel fill, `hipDeviceSynchronize` quiesce,
  `hipMemGetHandleForAddressRange(..., hipMemRangeHandleTypeDmaBufFd, 0)`,
  then fork/exec of `tbstream-dmabuf-probe --fd` with the fd inherited; the
  allocation stays alive until the probe exits.
- `max2` was never touched. Production DS4 (TCP v3) ran on both hosts
  throughout.

## Results

All passing rows report `covered == length` with every mapped segment
frame-aligned (4 KiB).

| arm | direction | length | orig SG entries | mapped entries | min alignment | largest segment |
|---|---|---|---|---|---|---|
| udmabuf (CPU pages) | tx | 16 MiB | 4096 | 256 | 64 KiB | 64 KiB |
| udmabuf (CPU pages) | rx | 16 MiB | 4096 | 256 | 64 KiB | 64 KiB |
| HIP coarse (`hipMalloc`) | tx | 16 MiB | 4 | 1 | 16 MiB | 16 MiB |
| HIP coarse (`hipMalloc`) | rx | 16 MiB | 4 | 1 | 16 MiB | 16 MiB |
| HIP uncached (`hipDeviceMallocUncached`) | tx | 16 MiB | 4 | 1 | 16 MiB | 16 MiB |
| HIP uncached (`hipDeviceMallocUncached`) | rx | 16 MiB | 4 | 1 | 16 MiB | 16 MiB |
| HIP coarse | tx | 256 MiB | 48 | 1 | 256 MiB | 256 MiB |
| HIP coarse | rx | 256 MiB | 60 | 1 | 256 MiB | 256 MiB |
| HIP fine-grain (`hipDeviceMallocFinegrained`) | both | 16 MiB | — | — | — | precondition failed |

## Findings

1. **Native HIP pools import with ideal geometry.** Coarse and uncached
   GTT-backed allocations map through the enabled IOMMU into a **single
   IOVA-contiguous, fully covering DMA segment** at both 16 MiB and 256 MiB,
   in both directions. The frame flattener will always see one segment, so no
   per-slot segment-crossing fallback is needed for pools of at least this
   size. The udmabuf control (loose 4 KiB CPU pages) coalesces to uniform
   64 KiB segments and also fully covers — the import path itself is sound
   without a GPU.
2. **The fine-grain candidate is unavailable on this configuration.**
   `hipExtMallocWithFlags(hipDeviceMallocFinegrained)` succeeds but
   `hipMemRangeAttributeCoherencyMode` reports coarse-grain
   (`hipMemRangeCoherencyModeCoarseGrain = 1`; the installed header numbers
   fine-grain 0/coarse 1/indeterminate 2). `rocminfo` shows `XNACK enabled:
   NO`, and `HSA_XNACK=1` does not change the outcome, so this is a
   platform/driver mode, not a request error. The gate-5 stale-cache matrix
   must drop the fine-grain arm (or gate it behind an XNACK-enabled boot) and
   rely on coarse+acquire vs. uncached as the control arm.
3. **Tool fix found live:** the probe's `/dev/udmabuf` fallback declared
   `UDMABUF_CREATE` as `_IOWR` (kernel uses `_IOW`) and omitted the required
   `F_SEAL_SHRINK` seal on the backing memfd; both fixed, and the tool now
   prefers `<linux/udmabuf.h>` when present.
4. **Scope:** run on `max` only. Replicating on `max2` requires MOK-signing
   the gate-3 module under its Secure Boot/lockdown policy; the NHI, IOMMU
   mode, GPU, and HIP stack are identical, and gate 5 needs a signed two-host
   deployment anyway, so replication is deferred to that gate.

## Host state after the run

- Production `thunderbolt_stream.ko` reloaded from
  `/lib/modules/.../updates/`; `zc_diagnostic_dmabuf` parameter absent
  (production module confirmed), endpoint republished at 9/9, reconcile timer
  re-enabled, no device holders.
- `ds4-mxfp4-worker` (max) and `ds4-mxfp4-server` (max2) active; a live
  `/v1/chat/completions` request completed normally over the production TCP-v3
  path.

## Verdict

Gate 4 **passes** for coarse and uncached native HIP memory. Gate 5 (the
stale-cache ownership gate with real traffic, alternating generations, and a
GPU-computed payload digest) is unblocked, with the fine-grain arm removed for
this platform.
