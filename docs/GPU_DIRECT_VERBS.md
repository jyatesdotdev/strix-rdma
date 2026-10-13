# GPU-direct registration for native verbs (DMA-BUF MRs)

Status (2026-10-12): **v1 implemented and validated on hardware.** GPU-to-GPU
SEND, WRITE and READ pass byte-for-byte at volume, including small and
unaligned operations. The stale-cache gate (G4) shows the producer's
system-scope release is required. Open: G5, hot-unplug with GPU MRs
registered. See "Results" at the end.

## Goal

Let a verbs application register GPU memory (a `hipMalloc` allocation) as an
MR, so the zero-copy data plane moves payload directly between one host's GPU
memory and the other host's GPU memory:

```text
hipMalloc -> DMA-BUF -> local NHI -> USB4 -> remote NHI -> DMA-BUF -> hipMalloc
```

Today every MR is a CPU-page `ib_umem`. A GPU application must either stage
through a host buffer (an extra copy on each side), or register host memory
with `hipHostRegister` and let kernels run against it at host-mapped speed.

This is message passing between GPUs with zero CPU payload copies. It is not
"GPUDirect RDMA" in the hardware sense. The one-sided READ/WRITE semantics
come from this driver's software verbs engine, as they already do for CPU
memory. The GPU cannot post work to the NHI, and the NHI has no remote keys of
its own (`docs/GPU_TO_GPU_FEASIBILITY.md`, "What this can and cannot achieve").

## Why this platform makes it straightforward

On Strix Halo, `hipMalloc` memory is GTT, which means ordinary system pages: KFD
backs "VRAM" requests with GTT when VRAM (512 MiB) is smaller than GTT. The
NHI already DMAs that class of memory through the IOMMU. Exporting a
`hipMalloc` allocation as a DMA-BUF with
`hipMemGetHandleForAddressRange(..., hipMemRangeHandleTypeDmaBufFd, 0)` is
proven on these hosts (`GPU_TO_GPU_FEASIBILITY.md`, "Newly proven ROCm DMA-BUF
path"). No peer-to-peer BAR access is needed.

## Interface: standard verbs, no new UAPI

- **Application:** `ibv_reg_dmabuf_mr(pd, offset, length, iova, fd, access)`.
  This is standard rdma-core, available in the installed v58.
- **Provider:** `reg_dmabuf_mr` forwards to `ibv_cmd_reg_dmabuf_mr`, with no
  private bytes.
- **Kernel:** the uverbs `REG_DMABUF_MR` method, enabled by implementing
  `ib_device_ops.reg_user_mr_dmabuf`.

`iova` is the address the MR is known by. It is normally the HIP device
pointer, so a peer's remote address is that pointer. `offset` is the position
of `iova` inside the DMA-BUF. HIP's export names the whole backing allocation,
so `offset` = `ptr - allocation base` (`hipMemGetAddressRange`), which is 0
for a dedicated allocation.

## Kernel design

**Import.** `sn_reg_user_mr_dmabuf()` calls
`ib_umem_dmabuf_get_pinned_with_dma_device(&d->ib, nhi_dev, offset, length,
fd, access)`. Here `nhi_dev` is the NHI's DMA device
(`tb_service_parent(svc)->tb->nhi->dev`, the same pointer
`tb_ring_dma_device()` returns for every ring of this service). The IB device
itself is registered without a DMA device, as for any software-verbs device,
so the plain `ib_umem_dmabuf_get_pinned()` would attach to nothing. That core
helper:
- attaches,
- pins the buffer (pinned attachment, no move notifier),
- maps the exporter's scatterlist **for the NHI**,
- trims it to `[offset, offset + length)`,
- waits for the exporter's kernel fences.

The memory then stays resident and mapped until deregistration.

**Validation**, before import:
- the same access flags and caps as CPU MRs (≤ `SN_MR_BYTES` each, at most
  `SN_MAX_MRS`, page-rounded total within `SN_PIN_BYTES`; GPU MRs count toward
  the same caps);
- `REMOTE_WRITE` requires `LOCAL_WRITE`;
- no on-demand paging;
- `offset` and `iova` page-aligned, so a frame-aligned `iova` gives
  frame-aligned DMA addresses;
- no integer overflow in `offset + length` or `iova + length`.

**Zero-copy data path.** It uses the exporter's mapping exactly as given:

- `map_dev`/`map_nents` are set at registration from the trimmed table, so
  `sn_mr_ensure_mapped()` is a no-op.
- `sn_mr_dma()` already walks DMA segments from `sgt.sgl` and never builds a
  frame across a segment. That is the property a foreign scatterlist needs.
- No `dma_map_sg`, `dma_unmap_sg` or `dma_sync_sg_*` is ever applied to a
  GPU MR. The exporter owns that mapping: amdgpu maps GTT for importers with
  `DMA_ATTR_SKIP_CPU_SYNC`, and these hosts are I/O-coherent with no bounce
  buffering. The CPU-MR sync calls are skipped for GPU MRs.

**Staged paths need CPU access.** These go through `sn_mr_copy()`:
- messages at or below the zero-copy threshold;
- unaligned operations;
- the responder's snapshot for a zero-copy READ fallback;
- the staged READ completion.

An importer must not touch the scatterlist's pages, so a GPU MR is mapped
once at registration with `dma_buf_vmap_unlocked()`. amdgpu pins the buffer
for the vmap, which is compatible with the pinned attachment. `sn_mr_copy()`
then uses `iosys_map_memcpy_to`/`_from` on that mapping. If the exporter
refuses vmap, registration fails; a GPU MR that silently could not take the
fallback path would fail the QP on the first READ timeout.

**Teardown.** `sn_dereg_mr()` keeps its EBUSY rule (an MR in use by a queued
operation cannot go). For a GPU MR it vunmaps, then `ib_umem_release()`
unmaps, unpins, detaches and drops the DMA-BUF. Hot unplug tears the device
down only after the QP is destroyed, so no MR is still referenced by the data
path.

**Revocation is not supported.** This is a pinned attachment, like other
fixed-pinning RDMA importers. An exporter that must move the buffer cannot
while it is registered.

## Coherency contract (the application's job)

DMA-BUF solves addressability and lifetime, not GPU cache ownership
(`GPU_TO_GPU_FEASIBILITY.md`, "Ownership and coherency"). The driver adds no
fences. The application must:

- **Before posting** a SEND/WRITE from a GPU MR, or before a peer READs it:
  make the GPU's writes system-visible. Use `hipDeviceSynchronize()` /
  `hipStreamSynchronize()` after the producing work, or a
  `hipEventReleaseToSystem` event synchronized before the post. **This is
  required; G4 measured what happens without it.** A completion wait that is
  explicitly fence-free (`hipEventDisableSystemFence`) left the producer's
  writes dirty in the GPU L2, and the NHI then sent stale bytes in 5–97% of
  iterations.
- **After a completion** that wrote a GPU MR (RECV, a READ's local landing
  buffer, or a peer's WRITE signalled by a later message): perform a
  system-scope acquire before GPU kernels read it. A timing-enabled
  `hipEventReleaseToSystem` event recorded in the consumer stream is the
  pinned-version technique. An idle-stream synchronize is **not** an acquire.
  This is the stale-cache gate below.
- **Nothing in flight:** do not touch a buffer region with the GPU while an
  operation on it is in flight, as with any NIC.

## Gates

- [x] **G0 Offline.** Object tests ok 5 and ok 6 run the extracted
  `sn_reg_user_mr_dmabuf()`, `sn_mr_release()` and `sn_dereg_mr()` against
  modeled imports. They cover validation before import, the NHI as the attach
  device, the reservation returned on import failure, vmap failure and the
  device dying mid-import, and GPU MRs never being dma-unmapped. Two mutations
  (leaking the reservation, attaching to the wrong device) are each caught.
  The provider test (ok 3) checks the arguments are forwarded unchanged, runs
  against the hosts' rdma-core 58 build, and passes. `make check-native-verbs`
  is green (61).
- [x] **G1 Import on hardware.** Each side registers three 8 MiB `hipMalloc`
  buffers (offset 0, dedicated allocations). The kernel's DMA-BUF table shows
  exactly three objects per host while a run is live, each attached to
  `0000:c4:00.6`, the NHI. After about 30 GPU runs, both hosts show 0 objects:
  nothing leaked.
- [x] **G2 Data path.** Every buffer is in GPU memory on both hosts.
  `write 2000`, `read 2000` and `send 2000` (16.8 GB per direction each) all
  verified byte-for-byte on both sides, with no kernel message.
- [x] **G3 Staged paths on GPU MRs.**
  - **Forced READ fallbacks:** `read 2000` 3/3 PASS with `zc_read_fault=100`.
    18 fallbacks fired, each snapshotting a GPU source through the vmap and
    completing into a GPU landing buffer through it, byte-for-byte.
  - **Varied geometry** (`strix-verbs-smoke --len/--off`): for each of WRITE,
    READ and SEND, on CPU and on GPU memory, all 18 combinations PASS
    byte-for-byte. Every landing slot is also checked to be untouched outside
    the transfer range (no overrun).
    - 3,000 B, aligned, ×500: staged, 20–35 MiB/s.
    - 1 MiB at offset 100, ×30: staged because unaligned, 60–68 MiB/s.
    - 2,093,055 B at offset 4096, ×200: zero-copy with a partial last frame,
      870–1,390 MiB/s.
  - This was also the first hardware run of small and unaligned operations at
    volume for CPU memory.
- [x] **G4 Stale-cache gate.** GPU kernels produce and check the data
  (`strix-verbs-smoke --gpu-kernels`). The result is below: the release is
  required, and the acquire made no observed difference.
- [~] **G5 Lifecycle.** `kill -9` of one side mid-transfer: both processes
  exit, both hosts drop to 0 DMA-BUF objects within seconds, no kernel message,
  and the next GPU runs pass. Not yet: hot-unplug with GPU MRs registered.
- [x] **G6 Performance.**

## Results

**Throughput.** Transfer-only throughput, measured from post until both
directions complete, is the link figure. It excludes the tool's pattern work
and the staging copies. Five runs of 500 iterations each, MiB/s per direction
with both directions running:

| | CPU memory: median (range) | GPU memory: median (range) |
|---|---|---|
| WRITE | 1,008 (954–1,073) | 1,289 (1,167–1,291) |
| SEND | 1,052 (1,032–1,089) | 1,183 (1,076–1,305) |
| READ (one run each) | 877 | 882 |

GPU memory is never slower. For WRITE it is consistently faster: the ranges
don't overlap. One unverified explanation is that GPU allocations come in large
physically contiguous pieces, which means fewer IOMMU page walks for the NHI
than scattered 4 KiB CPU pages. That hasn't been measured.

**G4: GPU caches.** GPU kernels from a code object
(`tools/verbs/strix-verbs-gpu-kernels.hip`) fill the source and count wrong
bytes at the destination. "Hot" mode (`STRIX_GPU_HOT=1`) clears the landing
buffer only once and checks only the 1 MiB of transfer ranges. That keeps the
probed lines resident in the GPU L2 across the NHI's writes, and every byte
changes between iterations, so a stale line always shows as a mismatch.

Results with `--len 262144`, 300 iterations, mismatched iterations per side:

| arm | WRITE | READ | SEND |
|---|---|---|---|
| positive control (check against the wrong iteration) | 20/20, every byte | | |
| release + acquire (the contract) | 0, 0 | 0, 0 | 0, 0 |
| no acquire | 0, 0 | 0, 0 | 0, 0 |
| **no release** | 30, 24 | 56, 259 | 196, 33 |
| neither | 15, 20 | 28, 291 | 205, 26 |

- **The producer's release is required.** Without it the NHI reads memory
  that the GPU L2 still holds dirty.
- **A missing acquire was not observed to matter** on this platform (Strix
  Halo, ROCm 7.15, coarse-grained `hipMalloc`). It stays in the contract: it
  is cheap, and an unobserved hazard is not a guarantee.
- **The first, non-hot run passed every arm.** Its per-iteration 8 MiB clear
  and full-window check evicted exactly the lines under test. A cache test has
  to keep its probe lines resident to mean anything.

**A correction to earlier throughput figures.** The verified modes' *wall*
rates in earlier records (about 410–490 MiB/s) are bound by the test tool's
own per-iteration pattern generation and checking, not by the link. The link
itself moves about 1.0–1.3 GB/s per direction, consistent with the unverified
bulk-SEND figures of 1,127–1,238 MiB/s. Those records remain accurate as wall
rates of verified runs.
