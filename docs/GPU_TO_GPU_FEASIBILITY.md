# GPU-to-GPU DMA over Thunderbolt/USB4: feasibility follow-up

> Investigation date: 2026-08-14. This is a handoff record for the next
> implementation pass. No installed module, service, or host configuration was
> changed. The initial read-only checks included one temporary HIP gate under
> `/tmp` on `max`, which removed itself. The synchronization and importer
> follow-up was source-only; it did not access `max` or `max2`.

## Bottom line

It is **not physically impossible** to move DS4 payloads between the two Strix
Halo GPUs through the USB4 NHI without a CPU payload copy. The repository has
already demonstrated the following two-sided path with NHI-owned system pages:

```text
local GPU -> GPU-mapped page -> local NHI -> USB4 cable
          -> remote NHI -> GPU-mapped page -> remote GPU
```

The remaining promising improvement is to invert buffer ownership:

```text
native HIP allocation -> DMA-BUF -> local NHI -> USB4 cable
                       -> remote NHI -> DMA-BUF -> native HIP allocation
```

ROCm and the Linux AMDGPU driver already contain the required sharing pieces.
The missing component is a `thunderbolt-stream` importer that pins and maps the
exported DMA-BUF for the NHI and builds its 4 KiB ring descriptors from the
returned DMA scatterlist. Actual NHI-device mapping is still a required gate,
not a proven result. Production correctness also needs an explicit GPU
system-release/system-acquire ownership bridge; DMA-BUF address sharing alone
does not provide one.

This can provide zero-copy, hardware-DMA, GPU-addressable **message passing**.
It still cannot provide InfiniBand/RoCE-style one-sided RDMA, GPU-issued NHI
work requests, remote virtual addresses/rkeys, or remote atomics. Those features
are absent from the NHI descriptor and transport hardware.

## What is already proven in this repository

The current zero-copy stream driver allocates ordinary pages, DMA-maps them for
the NHI, mmaps them into DS4, and registers the complete mapping with ROCm using
`hipHostRegister(..., hipHostRegisterMapped)`. Relevant implementation and
evidence:

- `kernel/zerocopy/`: NHI TX/RX pools, mmap UAPI, ownership synchronization,
  completion batching, and reliability fixes.
- `tools/rocm-map/tbstream-hip-map.cu`: both GPUs read and wrote the complete
  16 MiB TX pool.
- `tools/rocm-map/tbstream-hip-zc.cu`: GPU-produced bytes are handed to NHI and
  verified on the peer.
- `bench/results/2026-08-04-usb4stream-zc-oneway-rocm.md`: mapped-pool gate.
- `bench/results/2026-08-05-nhi-msix-rx-prime-fix.md`: 2,880 exact asymmetric
  exchanges over 27 sessions and passing mapped full-model runs after the MSI-X
  and ring-start fixes.
- `bench/results/2026-08-05-ds4-nhi-direct-slot-profile.md` and
  `bench/results/2026-08-06-ds4-rocm-event-profile.md`: graph kernels execute
  against mapped NHI slot aliases in the direct mode.

The direct path is therefore real GPU/NHI access to the same backing pages. It
is not merely a CPU memcpy hidden behind the API.

## Live hardware findings

Read-only checks were run on `max` and `max2`, both on
`7.1.5-101.fc43.x86_64`.

### Topology

Both systems have the same relevant topology:

```text
00:08.1 -> c2:00.0  Radeon 8060S / gfx1151
00:08.3 -> c4:00.5  Strix Halo USB4 NHI
        -> c4:00.6  Strix Halo USB4 NHI (the connected domain is here on max)
```

The GPU and NHIs are integrated endpoints under separate internal GPP bridges
of the same Strix Halo root complex. They are in separate IOMMU groups, which is
normal and does not prevent both devices from mapping the same pages through
the DMA API.

On `max`:

- GPU DMA mask: 44 bits.
- Both NHI DMA masks: 64 bits.
- IOMMU default domain: translated.
- `CONFIG_PCI_P2PDMA=y`.
- `CONFIG_HSA_AMD_P2P=y`.
- `amdgpu.pcie_p2p=Y`.

### The important UMA detail

These machines do not use a large discrete-VRAM allocation for DS4:

```text
AMDGPU VRAM total:       536,870,912 bytes (512 MiB)
AMDGPU visible VRAM:     536,870,912 bytes
AMDGPU GTT total:    133,143,986,176 bytes (~124 GiB)
```

At the time of inspection, `max` had about 102.8 GB of GTT in use and `max2`
had about 86.1 GB in use. The boot configuration explicitly supplies a
126,976 MiB GTT limit. In the AMDGPU source, an APU with VRAM smaller than GTT
sets `adev->apu_prefer_gtt`; KFD then implements allocations requested as VRAM
using `AMDGPU_GEM_DOMAIN_GTT`. Consequently, the large native HIP allocations
used by DS4 are system-memory/GTT pages on this platform, not inaccessible
memory chips behind a discrete GPU BAR.

That removes the main physical objection to direct NHI access: NHI already DMA
accesses this host DRAM class through the IOMMU.

## Newly proven ROCm DMA-BUF path

The installed TheRock HIP 7.13 runtime on both hosts exposes:

```c
hipMemGetHandleForAddressRange(...,
                               hipMemRangeHandleTypeDmaBufFd,
                               flags);
```

The symbol is present in `libamdhip64.so`, and ROCr exports
`hsa_amd_portable_export_dmabuf()` and `_v2()`. The installed headers explicitly
state that allocations from `hipMalloc` can be exported.

A temporary 4 MiB gate was compiled and run on `max` against gfx1151. It:

1. allocated with `hipMalloc`;
2. filled the allocation in a GPU kernel;
3. synchronized the GPU;
4. exported the complete allocation with
   `hipMemGetHandleForAddressRange(..., hipMemRangeHandleTypeDmaBufFd, 0)`;
5. mmapped the returned fd as a diagnostic; and
6. sampled the GPU-written pattern from the CPU mapping.

Observed output:

```text
GPU=Radeon 8060S Graphics integrated=1 canMapHost=1
flags=0 export=no error fd=6 ptr=... bytes=4194304
fd-target=/dmabuf: st_size=4194304
cpu-mmap=... sampled_bad=0 values=...
flags=1 export=invalid argument ...
```

The ordinary DMA-BUF export (`flags=0`) therefore works for a native
`hipMalloc` allocation and exposes the same bytes. The PCIe mapping-type flag
was rejected on this integrated/GTT allocation; it is not needed for the
proposed system-page DMA path and should not be used in the first gate.

## Why the Linux side should work

The local Linux source already supplies both halves:

1. `drivers/gpu/drm/amd/amdgpu/amdgpu_dma_buf.c`,
   `amdgpu_dma_buf_map()`:
   - pins an exported BO;
   - for `TTM_PL_TT`/GTT, creates an SG table from its pages; and
   - calls `dma_map_sgtable(attach->dev, ...)` for the importing device.
2. `drivers/thunderbolt/nhi_regs.h` and `drivers/thunderbolt/nhi.c`:
   - each NHI descriptor contains an arbitrary local `dma_addr_t`, length,
     framing fields, and flags; and
   - the hardware does not require that the address came from the stream
     driver's current `alloc_page()` path.

The existing NHI path already proves that this NHI can DMA normal pages under
the enabled IOMMU. An exported native HIP allocation is also GTT/system pages,
so no peer-BAR transaction is required. `PCI_P2PDMA` topology acceptance is
therefore not a prerequisite for the first Strix Halo implementation. A dynamic
DMA-BUF attachment with `allow_peer2peer = true` is still appropriate and also
keeps a later VRAM-capable experiment possible.

## Source-only follow-up: constraints on the implementation

This follow-up used the local Linux `zerocopy` tree, the applied stream series
under `/tmp/strix-rdma-final-linux`, and DS4 commit `b6c6edb`. The exact public
source provenance for the installed HIP version is now resolved. AMD's public
[gfx1151 `rocm_sdk_core-7.13.0` wheel](https://repo.amd.com/rocm/whl/gfx1151/rocm_sdk_core-7.13.0-py3-none-linux_x86_64.whl)
carries the same
`7.13.99004-3309c6114a` string and its
`share/therock/therock_manifest.json` records:

```text
TheRock commit:      6d2136cd12be28c6251eb38c700e980c8c2f8cf6
GitHub build run:    25753625030
rocm-systems pin:    79e85e1468f96a867108043c953e9547c13b4c5e
```

The only listed patch against `rocm-systems` is an unrelated RCCL deadlock fix.
`3309c6114a` is a generated HIP package hash rather than a public repository
commit, which explains the failed direct GitHub commit lookup. Every relevant
HIP/CLR conclusion below was rechecked against `79e85e1468f`; current upstream
comparison used `533edabf41f8`. None of this source/provenance work accessed
either Strix host.

### The exported fd is not a sliced range

ROCr returns both a DMA-BUF fd and an allocation offset, but CLR's
`Buffer::GetFDHandleForMem()` stores the fd and discards the returned offset.
The fd therefore names the backing BO, not a new zero-based object containing
only the requested subrange. The first gate should export only the base and full
size of each dedicated allocation. Any later subrange support must call
`hipMemGetAddressRange()`, compute `dptr - allocation_base`, pass that offset in
the stream UAPI, and validate `offset + length <= dma_buf->size` without integer
overflow.

### Import must precede path activation

The current stream driver's first `open()` allocates page-backed rings, starts
both rings, primes RX, and enables the XDomain paths before userspace can issue
an ioctl. A production import ioctl cannot simply replace those buffers in
place: the peer could already transmit into the old RX ring. The preferred
refactor is lazy activation:

1. `open()` creates an unconfigured session but does not enable paths;
2. an import or zero-copy operation requires `users == 1` and claims the same
   exclusive ownership already required by `TBSTREAM_ZC_ENABLE`;
3. userspace selects page-backed or imported mode, and the driver constructs
   and fully validates both rings transactionally; and
4. enable starts TX/RX, fully primes RX, then enables paths.

Legacy read/write can lazily select and activate page-backed mode; the existing
zero-copy enable ioctl can select page-backed mode before its current mmap
sequence. A transient import/map/unmap diagnostic can run against an
already allocated ring's `tb_ring_dma_device()` because it never changes a
descriptor, but that exception must not become the production activation model.

### Reservation fences do not represent ordinary HIP kernels

The exported AMDGPU BO shares its reservation object with the DMA-BUF, but KFD
user-mode AQL dispatch does not add a useful per-kernel read/write fence there.
The KFD fences found on these BOs are principally `DMA_RESV_USAGE_BOOKKEEP`
eviction/memory-management fences. An importer must still wait for all existing
fences required by `dma_resv_usage_rw(is_rx)` and add NHI read/write fences when
access cannot be stopped immediately, but that does **not** replace explicit HIP
completion.

AMDGPU's DMA-BUF `begin_cpu_access` callback only moves readable BOs toward GTT;
it does not invalidate GPU L2, and AMDGPU supplies no matching
`end_cpu_access` callback. `DMA_BUF_IOCTL_SYNC` is a CPU-mapping cache API, not a
GPU/NHI ownership primitive. Likewise, AMDGPU maps GTT attachments with
`DMA_ATTR_SKIP_CPU_SYNC`.

The existing stream driver's per-frame `dma_sync_single_*()` calls are valid for
its separate `dma_map_page()` mappings. They must not be copied blindly to
interior IOVAs returned by a mapped SG table. Native GPU/NHI mode has no CPU
payload owner; it should use the DMA-BUF device mapping, hardware completion,
reservation/explicit fences, and the GPU system-scope bridge. If a target
platform requires streaming DMA cache maintenance, use the matching SG API only
across a range whose complete ownership is serialized; there is no generic
partial-SG sync API suitable for concurrently owned slots. Whether Strix Halo's
I/O-coherent path needs any additional NHI-side maintenance is a hardware gate.

## Proposed bounded proof of concept

### Userspace

1. Allocate one fixed TX pool and one fixed RX pool with `hipMalloc`; use the
   allocation base and complete allocation size for the first gate.
2. Quiesce the owning HIP device before import, then export each allocation with
   `hipMemGetHandleForAddressRange(..., hipMemRangeHandleTypeDmaBufFd, 0)`.
3. Pass fd, fixed direction, offset, and length to a new stream ioctl before
   traffic or path activation.
4. Keep the HIP allocations logically alive until the stream driver has
   detached; do not call `hipFree()` while DS4 can use the pointer. The kernel's
   successful `dma_buf_get()` owns its own file reference, so the UAPI must not
   depend on the userspace fd remaining open, although the first gate should
   keep it open for simple lifetime auditing.
5. Bind DS4's existing external tensor wrappers directly to slot offsets in the
   native pools. No `hipHostRegister()` and no intermediate graph tensor should
   be required.

Use fixed contiguous logical slots or explicit wrap padding. The current direct
lease path falls back when a message wraps the circular pool; retaining that
rule is acceptable for the first gate.

### Version-1 UAPI contract

Configure TX and RX atomically in one fixed-width, pointer-free ioctl before
`TBSTREAM_ZC_ENABLE`. The header can use a shape equivalent to:

```c
struct tbstream_import_range {
        __s32 fd;
        __u32 flags;
        __u64 offset;
        __u64 length;
};

struct tbstream_import_pools_v1 {
        __u32 version;
        __u32 flags;
        struct tbstream_import_range tx;
        struct tbstream_import_range rx;
        __u64 reserved[4];
};
```

Version 1 requires `version == 1`, every flag/reserved field zero, distinct
DMA-BUF objects, `offset == 0`, and `length == dma_buf->size` for both complete
dedicated allocations. TX direction is fixed to `DMA_TO_DEVICE`; RX is fixed to
`DMA_FROM_DEVICE`. Supporting offsets later requires a new advertised version
or flag and the allocation-base calculation above; do not silently broaden v1.
There are no user pointers or native `long` fields, so compat ioctl layout is
identical.

Add a new versioned `TBSTREAM_ZC_GET_CAPS` structure with `struct_size`, feature
bits, and zeroed reserved fields; do not enlarge `tbstream_zc_info`, because the
structure size is encoded in its existing ioctl number. Advertise import-v1 and
the diagnostic separately. Both peers negotiate the import bit over the existing
TCP control plane before sending either ioctl. An old module therefore remains
a clean `ENOTTY`/feature-miss fallback rather than a partially configured
session.

The import ioctl is transactional: acquire both references, construct and
validate both mappings and frame tables off to the side, then publish imported
mode with one state transition. Any error releases both sides and leaves the
session in its original unconfigured state. A second configuration attempt,
mmap, legacy read/write, or mode change after commit returns `-EBUSY` or
`-EOPNOTSUPP` as appropriate. A successful kernel reference survives userspace
`close(fd)`; it is released only after NHI quiescence during stream close.

Use an explicit state machine rather than scattered booleans:

```text
OPEN_UNCONFIGURED -> CONFIGURED_PAGE     -> ENABLED_PAGE     -> CLOSING
                  -> CONFIGURED_IMPORTED -> ENABLED_IMPORTED -> CLOSING
```

Only close/reopen returns to `OPEN_UNCONFIGURED`. Both peers complete
configuration and exchange control-plane READY before either enables paths.
Legacy first I/O can select and activate page mode lazily; an unconfigured
`TBSTREAM_ZC_ENABLE` selects page mode before mmap, while a configured import
activates imported mode. This preserves the current ABI without exposing an
imported payload through the stream driver's mmap.

The diagnostic ioctl is separate and privileged. Its fixed-width input contains
fd, direction, offset, length, version, and zeroed flags/reserved fields. Its
only outputs are requested/covered bytes, original/mapped entry counts, minimum
alignment, and largest mapped segment. It returns those statistics only after a
successful unmap/unpin/detach; it never returns a DMA address. Run it only in an
unconfigured session with no submitted descriptors.

### Kernel

Use an integrated pre-enable ioctl rather than an isolated diagnostic module.
The stream instance already owns the exact ring/NHI binding, can obtain the
actual `tb_ring_dma_device()`, and can prove that no descriptor is active; a
standalone module would have to guess a PCI function or add a new Thunderbolt
core lookup interface.

First add a privileged, no-traffic import probe. It should accept one fd,
direction, aligned offset, and bounded length; attach to
`tb_ring_dma_device(ring)`; pin/map; return only aggregate SG geometry and
coverage; and immediately tear down. Raw IOVAs are sensitive and should be
available only through a privileged debug path, not the normal unprivileged
UAPI. The probe must not write a descriptor or enable/disable a path.

Then add an opt-in imported-pool mode to `thunderbolt-stream`:

1. `dma_buf_get(fd)` for distinct TX and RX allocations, reject an unwritable RX
   fd, and cap each pool to the configured ring geometry.
2. `dma_buf_dynamic_attach()` to `tb_ring_dma_device(ring)` with importer ops
   that permit peer resources. Add `MODULE_IMPORT_NS("DMA_BUF")`.
3. Under the reservation lock, call `dma_buf_pin()`, then
   `dma_buf_map_attachment()` with `DMA_TO_DEVICE` for TX and
   `DMA_FROM_DEVICE` for RX. Before NHI access, wait existing fences through
   `dma_resv_usage_rw(is_rx)`. Permanent pinning is allowed only for these
   fixed, exclusive, bounded pools.
4. Validate page-aligned offset/length, exact logical coverage, nonzero DMA
   lengths, overflow-free DMA addresses, and the mapped rather than original SG
   entries. Do not assume one physical or one IOVA-contiguous segment.
5. Split mapped SG segments into exact 4 KiB NHI frame addresses; no descriptor
   may cross an SG DMA segment. Userspace never supplies an IOVA.
6. Start both rings, fully prime imported RX descriptors, and only then enable
   the XDomain paths, preserving zero-copy patch 12's ordering.
7. Do not reuse the page-backed mode's `dma_sync_single_*()` calls on
   SG-derived IOVAs. Add one session-lifetime `DMA_RESV_USAGE_READ` fence to TX
   and one session-lifetime `DMA_RESV_USAGE_WRITE` fence to RX before the first
   descriptor; signal them only after that ring is quiescent. Prove the
   remaining Strix coherency boundary in the stale-cache gate.
8. Give CLOSE a dedicated page-backed control frame; the existing close path
   cannot call `page_address()` on a native imported slot.
9. On teardown: reject new submissions, disable paths, stop/flush/cancel rings,
   signal terminal fences, unmap, unpin, detach, and put every DMA-BUF in strict
   reverse order. Every partial-setup failure uses the same reverse rollback.

Keep SG flattening in a pure, unit-tested helper. For v1 it iterates only
`for_each_sgtable_dma_sg()` entries, rejects zero or non-4-KiB-aligned DMA
address/length, checks every `dma_addr + dma_len` and cumulative-length addition
for overflow, requires cumulative mapped length to equal both `dma_buf->size`
and the configured pool length, and emits exactly `length / 4096` addresses.
Each emitted address is `sg_dma_address(sg) + n * 4096` with
`n * 4096 + 4096 <= sg_dma_len(sg)`. Never modify the exporter's SG entries and
never walk `orig_nents` to obtain DMA addresses.

Static tests should cover one segment, many segments, DMA-coalesced entries,
exact boundaries, zero/unaligned segments, short and overlong coverage,
address/sum overflow, excessive frame count, and a final partial segment. Add
fault injection after every get/attach/pin/map/table-allocation/publish stage and
assert one reverse cleanup of every acquired object. The diagnostic and
production path must share this helper so a successful probe tests the same
geometry rules used for descriptors.

For asynchronous implicit-sync bookkeeping, use one driver-owned session fence
per dedicated pool, not one fence per 4 KiB operation. Before any descriptor is
visible to hardware, reservation-lock the pool, wait pre-existing fences through
`dma_resv_usage_rw(is_rx)`, reserve a slot, and add an unsignaled
`DMA_RESV_USAGE_READ` fence to TX or `DMA_RESV_USAGE_WRITE` fence to RX. Keep it
unsignaled for the complete enabled lifetime because some slot can always be
owned by NHI. Per-frame fences would either accumulate needlessly or make a new
submission wait on the stream's own unsignaled fence. Signal the session fence
only after the corresponding ring is synchronously quiescent; use success for a
normal close and a specific terminal error such as `-ECANCELED`, `-ENODEV`, or
`-EIO` for cancellation, disconnect, or DMA failure. Fence signaling must not
need the stream mutex or reservation lock. These fences exclude other
implicit-sync importers; they still do not represent or replace DS4's per-slot
HIP ownership transitions.

Teardown first atomically enters `CLOSING` and rejects all ioctls. A graceful
close submits the dedicated CLOSE frame and boundedly flushes TX, then disables
both paths and synchronously stops/cancels the rings. Only after every
descriptor is returned or canceled and both session fences are signaled may it
reservation-lock, unmap, unpin, unlock, detach, and put RX and TX in reverse
setup order. Never wait for a callback while holding a reservation lock.

A timeout is not proof that DMA stopped. Escalate a failed ring stop through the
existing NHI reset/device-disable path; if quiescence still cannot be proven,
retain the bounded mapping and fail the session rather than unmap memory that
hardware might still address. Suspend, GPU reset, or an invalidation policy that
cannot prove the same bounded quiescence requires close/reopen rather than
silently reusing mappings.

A pinned attachment without `invalidate_mappings` follows the existing
InfiniBand fixed-pinning model. If revocation is required, the callback must
synchronously stop NHI access and arrange bounded unmap before it returns; merely
setting an error flag is insufficient. Retrofitting pools after a live
page-backed RX ring is not the preferred implementation.

### Ownership and coherency

DMA-BUF solves addressability and lifetime, not synchronization. TX and RX are
asymmetric:

```text
TX: producer kernel -> GPU system-scope release -> CPU observes completion
    -> NHI submit on its persistent DMA mapping -> TX completion -> slot reusable

RX: NHI owns posted slot -> RX hardware completion
    -> GPU system-scope acquire -> consumer kernels -> GPU completion -> repost
```

A plain `hipDeviceSynchronize()` or `hipStreamSynchronize()` is sufficient for
TX only when it follows dirty GPU work: exact 7.13 CLR's `HostQueue::finish()`
then forces a marker, and `VirtualGPU::releaseGpuMemoryFence()` emits and waits
for a barrier with system acquire/release scope. It is **not** a valid RX acquire
on an otherwise idle stream. `finish()` returns immediately when there is no
last command, and external NHI writes do not mark CLR's fence state dirty.

The clearest TX prototype is a timing-enabled event created with
`hipEventReleaseToSystem`, recorded after the producer in the same stream, and
synchronized before `SUBMIT_TX`. For RX, exact 7.13 CLR can be made to submit a
GPU packet after `REAP` by recording such an event in the consumer stream and
queueing the consumer behind it:

1. `Event::recordCommand()` creates a profiling `EventMarker` and selects
   `kCacheStateInvalid` unless `hipEventDisableSystemFence` was requested;
2. `VirtualGPU::submitMarker()` emits `kBarrierVendorPacketHeader` or
   `kBarrierPacketHeader`; and
3. both headers encode system-scope acquire and release.

The same source comments state that system release flushes L2; the HSA system
acquire is the corresponding external-write visibility operation. Exact 7.13
creates an internal profiling marker even for a timing-disabled event, but
newer inspected CLR can coalesce repeated idle timing-disabled records. Use a
timing-enabled marker and never set `hipEventDisableSystemFence` so the prototype
is safe across both implementations.

The RX event technique remains an implementation-pinned gate, not yet a
sufficient portable contract: HIP documents `hipEventReleaseToSystem` primarily
as a release operation even though this exact backend emits both scopes. Verify
the packet with AQL logging and stale-cache stress, or have ROCm expose a
supported explicit acquire primitive. The obvious alternatives are not
currently available:

- `hipStreamMemOpFlushRemoteWrites` and stream memory barriers are declared but
  explicitly rejected by exact 7.13's `ihipBatchMemOperation()`;
- `hipStreamWaitValue*` is not a portable payload-coherency contract. Exact
  7.13 does use a system-scoped barrier-value packet when the non-default
  `GPU_STREAMOPS_CP_WAIT=1` mode is enabled (and then requires
  `hipMallocSignalMemory`), while its default path uses a system-scoped blit
  kernel. This is a useful AQL cross-check, but the API documents only the
  control-word comparison, not visibility of an unrelated DMA-BUF payload;
- HIP external semaphores are documented unsupported on Linux; exact 7.13's
  ROC backend stubs `importExtSemaphore()` to false, while newer libhsakmt
  Linux entry points likewise return `NOT_SUPPORTED`;
- DMA-BUF implicit fences do not track ordinary KFD AQL kernels; and
- CPU DMA-BUF synchronization does not invalidate GPU L2.

Add two cache-avoiding RX diagnostic arms:

- `hipExtMallocWithFlags(..., hipDeviceMallocUncached)`: exact CLR passes the HSA
  uncached allocation flag, which reaches the KFD uncached BO flag and gfx11
  `MTYPE_UC`. This is the clearest stale-L2 control arm, but payload bandwidth
  may resemble the slower mapped host pool.
- `hipExtMallocWithFlags(..., hipDeviceMallocFinegrained)`: require
  `hipMemRangeAttributeCoherencyMode` to report fine grain before export.
  AMDGPU's coherent gfx11 mapping is also UC, but ROCr explicitly warns that
  DMA-BUF sharing is not guaranteed to retain fine-grain coherence.

Treat both as measured fallbacks, not assumed solutions. The target remains a
coarse native payload with an explicit acquire because it preserves the best
chance of normal GPU cache/bandwidth behavior.

The IOMMU remains enabled in every arm.

## What this can and cannot achieve

### Achievable

- Native HIP/GTT allocation as the actual NHI TX/RX backing store.
- No CPU payload copy.
- No application heap staging buffer.
- GPU kernels reading received bytes and writing transmitted bytes directly.
- CPU/kernel participation limited to control, descriptor submission,
  completions, credits, and lifecycle.

### Not achievable with this NHI

The NHI descriptor contains only a **local** DMA address. The packet does not
carry a remote virtual address or protection key; data lands in the peer's next
pre-posted RX descriptor. The hardware exposes no public:

- queue pairs or work-request engine;
- remote virtual addresses/rkeys;
- one-sided reads or writes;
- remote atomics;
- RDMA reliability/retry engine; or
- supported GPU-visible NHI submission queue/doorbell.

Mapping a peer integrated GPU through a host-to-host USB4 PCIe tunnel is not an
alternative: XDomain host links do not merge the two root complexes or expose
one host's integrated GPU BAR to the other.

Therefore “GPU-to-GPU DMA message transport over USB4” is accurate. “True
GPUDirect RDMA” or “one-sided RDMA” is not. Those semantics require an HCA,
FPGA, or other external endpoint that implements them.

## Performance reality

The DMA-BUF path is technically worthwhile as the only remaining route to a
native-allocation data plane, but it is unlikely to improve the current serial
DS4 decode materially:

- NHI one-way throughput is already about 1.0-1.1 GB/s, near the measured TCP
  bulk rate.
- Controlled full-model rates were effectively equal: 10.9540 tok/s TCP v3,
  10.9421 CPU-copy NHI, and 10.9438 mapped NHI.
- About 67.7 ms/token is GPU stream work; only a small residual remains for
  transport/control.
- Writing the 505 KiB logits directly into the current host-registered NHI pool
  slowed the output head by 0.065 ms, more than the lease fences it removed.

Native DMA-BUF pools may remove that host-mapping penalty while preserving the
zero-copy topology. Even a perfect result has a small single-request upper
bound unless model halves are pipelined across independent sequences,
multiple tokens/batches are in flight, or the boundary becomes larger.

## Recommended next gates

1. **Done:** prove native `hipMalloc` -> DMA-BUF export on Strix Halo.
2. **Done source-only:** resolve the exact public HIP source pin and specify
   bounded import, offset, activation, teardown, reservation, and cache-ownership
   requirements.
3. Implement the no-traffic import/map/unmap probe and compile/test it against
   the exact kernel tree without deploying it. Report requested/covered bytes,
   mapped entry count, alignment, largest segment, and clean rollback; keep raw
   IOVAs privileged.
4. Only after explicit host authorization, run the probe for coarse TX/RX plus
   uncached and fine-grain RX candidates against the actual NHI DMA device.
5. Add a stale-cache ownership gate. Reuse hot RX slots with alternating
   generations and compare coarse memory with the timing-enabled system event,
   coarse memory without that marker as a diagnostic arm, uncached memory, and
   confirmed fine-grain memory. The remote GPU computes a full payload digest
   and mismatch count; the CPU reads only the small result record.
6. Transfer a GPU-written pattern from imported TX to imported RX, then test
   ring wrap, close/reopen, disconnect, IOMMU faults, GPU reset, and every
   partial-import rollback path.
7. Benchmark an output-head-like native allocation against the existing
   host-registered pool. Continue only if it removes the measured 0.065 ms
   penalty or produces another clear transport win.
8. Integrate fixed native slots into DS4 only after mapping and synchronization
   gates pass on both hosts.

All host gates remain paused until the user explicitly says `max` and `max2` are
safe to use. Do not weaken disconnect handling, IOMMU isolation, bounded
pinning, or ownership fencing to chase the current sub-percent end-to-end
ceiling.

## Documentation status

`docs/DS4_INTEGRATION.md`, `docs/PLAN.md`, `docs/INSTALL.md`, `kernel/README.md`,
and the root `README.md` are updated alongside this record: current DS4 can
execute eligible graph boundaries against host-registered lease aliases, while
native HIP DMA-BUF pools remain a separate, unimplemented experiment with an
unresolved portable RX-acquire contract.
