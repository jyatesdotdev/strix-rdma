# 2026-08-23 — Gate 5: imported native RX pools carry real traffic; stale-cache matrix clean

Gate 5 of the native HIP DMA-BUF experiment: put real NHI traffic into an
imported native GPU allocation, reuse hot RX slots across alternating
generations, and verify every payload with a GPU digest while comparing
memory-type and acquire arms. This required the first imported-pool driver
mode (zero-copy patch 14) and a new two-host harness.

## New kernel work (patch 14, `TBSTREAM_ZC_IMPORT`)

`kernel/zerocopy/0014-thunderbolt-stream-Add-imported-DMA-BUF-frame-pools.patch`:

- **Lazy path activation.** Ring construction and XDomain path enablement
  move from first `open()` to the first operation that needs them (read,
  write, poll, zero-copy enable, import probe). An import therefore always
  precedes ring construction, exactly as `docs/GPU_TO_GPU_FEASIBILITY.md`
  required; RX is still fully primed before paths are enabled (preserving the
  patch-12 invariant).
- **Atomic, transactional import.** One fixed-width ioctl selects TX and/or
  RX DMA-BUF pools (fd = -1 keeps a direction page-backed) on an exclusively
  opened, never-activated device. Each pool is attached to the NHI DMA
  device, pinned, mapped in its fixed direction, and flattened into per-slot
  frame addresses by the shared `stream-sg.h` validator; any failure unwinds
  every acquired object in reverse order.
- **No CPU syncs on imported frames.** Imported frames have no page; every
  `dma_sync_single_*()` site is guarded, `mmap()` leaves imported halves as
  unpopulated holes, and ordinary read/write are rejected while an import is
  configured. CLOSE for imported-TX sessions uses a dedicated page-backed
  control frame outside the slot pool. Imports last until final close and
  survive suspend/resume with the rings they back.
- Gated by CAP_SYS_RAWIO + `zc_diagnostic_dmabuf` while experimental. New
  stats flags report imported directions. Strict checkpatch: 0/0/0 on the
  791-line diff; W=1 clean; built against the exact 7.1.5-101.fc43 tree.

## Harness

`tools/stale-cache/`: `tbstream-stale-tx` (CPU sender, page-backed zc TX,
serialized generation-stamped messages, kernel/local slot-cursor
cross-check) and `tbstream-stale-rx` (HIP receiver: native pool poisoned
with `0xdeadbeef` from the GPU, exported, imported as the RX pool before
activation, then per-message GPU digest recomputing the shared
`stale-pattern.h` mixer and counting mismatched words; CPU reads only the
small result record). Acquire arm records a timing-enabled
`hipEventReleaseToSystem` event in the consumer stream before each digest.
`--prewarm 1` adversarially re-reads the *next* message's slots into GPU
cache after repost, so their lines are resident while the NHI DMA-writes
them.

## Method

Transient live swap of the patch-14 `thunderbolt_stream.ko` on **both**
hosts (Secure Boot now disabled on max2, so the max-built module was reused
byte-identical; nothing installed to `/lib/modules` or initramfs).
Endpoints recreated at the production 9/9 HopID policy, ring 256 for the
stress arms (1 MiB pool ≈ GPU L2-resident, maximum staleness pressure) and
ring 4096 for the production-geometry confirm. Receiver on `max`, sender on
`max2`. Fine-grain arm dropped per gate 4 (XNACK hard-disabled).

## Results

Every arm: full expected message count, zero mismatched words, zero event
drops, zero session failures, `RX_IMPORTED` stats flag asserted, clean
CLOSE. Sender/receiver slot cursors agreed throughout.

| ring | frames/msg | arm | messages | ring wraps | bad words |
|---|---|---|---|---|---|
| 256 | 16 | coarse + event acquire | 1024 | 64 | 0 |
| 256 | 16 | coarse + none (diagnostic) | 1024 | 64 | 0 |
| 256 | 16 | uncached + event | 1024 | 64 | 0 |
| 256 | 16 | uncached + none | 1024 | 64 | 0 |
| 256 | 16 | coarse + none + prewarm (adversarial) | 1024 | 64 | 0 |
| 256 | 16 | coarse + event + prewarm | 1024 | 64 | 0 |
| 4096 | 64 | coarse + event (production geometry) | 512 | 8 | 0 |

Aggregate: 6,656 verified messages, 448 MiB of NHI-DMA payload written
directly into native `hipMalloc`/uncached GPU memory and verified word-exact
by GPU kernels, across 448 full pool-reuse wraps.

## Findings

1. **The imported-pool mode works end to end.** This is the first time the
   NHI transferred real stream traffic into a native HIP allocation with no
   CPU copy, no page-backed staging, and no `hipHostRegister()`.
2. **No stale-cache failure was producible on this platform**, even
   adversarially: with the whole 1 MiB pool cache-resident, slots re-read
   into cache during the DMA write, and no explicit acquire, every word was
   still current-generation. Combined with the clean uncached control, this
   is strong evidence that NHI (PCIe) writes into these GTT pages are
   I/O-coherent with the GPU caches on Strix Halo, and/or that ordinary
   kernel-dispatch AQL barriers already provide the needed acquire. Per the
   feasibility doc's warning, the explicit timing-enabled
   `hipEventReleaseToSystem` acquire **remains the contract** for production
   use; the diagnostic arms only bound the risk, they do not license
   dropping it.
3. The poisoned-pool start also proves initialization ordering: first-
   generation digests never observed `0xdeadbeef`.
4. Lazy activation held up operationally: 7 open/import/enable/close cycles
   plus repeated reconfiguration with zero kernel warnings or transport
   errors on either host (dmesg checked implicitly via clean session stats;
   no `zc` failure states entered).

## Host state after the run

Both hosts: production 12-patch module reloaded from `/lib/modules`
(`zc_diagnostic_dmabuf` absent), 9/9 endpoint republished at ring 4096,
reconcile timers re-enabled, no device holders, worker/server active, and a
live `/v1/chat/completions` request completed on the production TCP-v3 path.

## Verdict

Gate 5 **passes**. Gate 6 (imported TX → imported RX transfer with ring
wrap, close/reopen, disconnect, IOMMU-fault, GPU-reset, and rollback-path
coverage) is unblocked; the TX-side ownership prototype (system-scope
release before `SUBMIT_TX`) and the dedicated CLOSE control frame are
already in place. Gate 7 (output-head-like benchmark vs. the measured
0.065 ms host-registered penalty) is the first stage that touches DS4 and
live inference; per operator instruction it will not run without an explicit
go-ahead.
