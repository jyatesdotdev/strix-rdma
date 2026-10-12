# RDMA transport between two Strix Halo hosts over Thunderbolt/USB4

> **Current state:** two working data planes on the same link. The USB4STREAM
> stream path (`/dev/tbstreamX`) is the mature substrate: backported to the
> hosts' kernel, extended with a mmap-able zero-copy UAPI, and proven with
> ROCm access to the driver-owned pool. The native software-verbs path
> (`kernel/verbs/` + `providers/strix_nhi/`) is default-off and experimental,
> but after its resident poller and data-ring interrupt moderation it is the
> fastest measured path on the link: 35.1 µs average 256 B round trip and
> 1182–1238 MiB/s per direction on window-4 × 2 MiB bidirectional bursts
> (measured 2026-10-10 on the lab host pair, no-TB-IP mode; that band is
> *pre-reboot* — after a clean reboot plus bring-up the same stack measures
> 1127–1191 MiB/s, at or above the 1070–1156 MiB/s recorded for previous
> post-reboot runs, so compare like with like). Native `hipMalloc`
> DMA-BUF export works on Strix Halo; actual NHI import and a portable RX
> system-acquire contract remain gated experiments
> (`GPU_TO_GPU_FEASIBILITY.md`).

## Goal

Move buffers between a paired set of Strix Halo hosts with hardware-assisted
data movement and low CPU cost, and let a GPU on either side reach the same
pages. The transport is general: message-passing semantics over the NHI DMA
rings, with the payload either in driver-owned pages, in pages a GPU runtime
has registered, or in pages imported from a GPU allocation.

The target is hardware-assisted data movement, not a software RDMA
implementation layered over the existing Thunderbolt TCP link.

## Conclusion

Soft-RoCE (`rxe`) or software iWARP (`siw`) is not the useful path here. Both
retain the network stack and execute the RDMA protocol in software, so they are
unlikely to improve on well-tuned TCP over `thunderbolt-net` for a
latency-sensitive workload.

The productive path is a custom transport built on the USB4/Thunderbolt Native
Host Interface (NHI) DMA rings. The NHI performs the physical transfer from a
local DMA address into pre-posted memory on the other host. The implemented
driver exposes fixed mmap-able TX/RX pools, and ROCm access to those same pages
is proven, so a buffer can move:

```text
sending GPU -> shared DMA page -> NHI -> cable -> NHI -> shared DMA page -> receiving GPU
```

That is hardware-DMA message passing with send/receive semantics. It is not
full InfiniBand-style RDMA: the Strix Halo NHI has no public support for remote
virtual addresses, rkeys, queue-pair state, RDMA reads/writes, atomics, or an
RDMA reliability engine. A kernel patch cannot add those missing hardware
features. The verbs implementation in `kernel/verbs/` therefore provides
**bounded** software semantics — real uverbs objects, SEND/RECV, and
size-limited software READ/WRITE — and rejects what it cannot honor. True
one-sided RDMA would require an external RDMA NIC or FPGA.

## Why the TCP path costs more

A TCP transport over `thunderbolt-net` reads a device buffer into host memory,
sends it through the socket, and writes the received host buffer back into the
peer's device memory. Below that, `thunderbolt-net` copies TX payload into its
own 4 KiB DMA buffers, while TCP/socket handling adds further copies, protocol
work, and wakeups.

The resulting data path is approximately:

```text
GPU -> host buffer -> socket/TCP -> thunderbolt-net DMA pages
    -> NHI/cable/NHI -> network/TCP -> host buffer -> GPU
```

Linux's [`thunderbolt-net` TX path](https://github.com/torvalds/linux/blob/master/drivers/net/thunderbolt/main.c#L1040-L1151)
supports scatter/gather and TCP segmentation offload, but it still copies SKB
payload into NHI ring pages.

Measured on the lab pair: iperf3 over `thunderbolt-net` reaches 9.49–9.51
Gbit/s one-way at the default `thunderbolt.dma_credits=14`, and raising the
credit window did not raise it (2026-09-18). The nominal link rate is
20 Gb/s per direction, so the practical ceiling for any path is well under
that, and the copies plus the network stack are what a direct ring path avoids.

## Upstream USB4STREAM support

Upstream Linux has a much better starting point than `thunderbolt-net`:

- [`USB4STREAM` support](https://github.com/torvalds/linux/commit/6db21d817b43f8ce5654ccc7aff80d40e4dba4ac)
  adds the `thunderbolt-stream` driver and `/dev/tbstreamX` character devices.
- It transfers data directly over a Thunderbolt/USB4 tunnel without traversing
  the network stack.
- Multiple bidirectional streams can coexist, including alongside
  `thunderbolt-net`.
- It supports configurable ring sizes and interrupt throttling.
- The ABI entry in the upstream change identifies Linux 7.2. The test systems
  were running 7.1.5 when this was investigated, so using it requires a 7.2
  kernel or a backport.

The [upstream kernel documentation](https://docs.kernel.org/admin-guide/thunderbolt.html#streaming-data-directly-over-thunderbolt-cable)
describes ConfigFS setup and the `/dev/tbstreamX` interface.

Stock USB4STREAM is not zero-copy. It allocates and DMA-maps its own pages,
then uses [`copy_page_from_iter()` on TX](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/stream.c#L467-L505)
and [`copy_page_to_iter()` on RX](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/stream.c#L599-L745).
It removes TCP overhead and is worth measuring as a baseline, but it does not
provide a GPU-to-NHI direct path without further changes.

## What the NHI can offload

An NHI ring descriptor contains a local physical/DMA address, a length, framing
flags, and completion/interrupt flags. See the upstream
[`nhi_ring_desc`](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/nhi_regs.h#L20-L38).
The public ring interface describes frames of at most 4096 bytes.

That is enough for an efficient pre-posted message transport:

1. The receiver posts DMA-mapped slots to its RX ring.
2. The sender fills a registered TX slot and submits descriptors that reference
   it.
3. NHI hardware moves the frames over the cable into the peer's posted slots.
4. The receiver gets one completion for the logical message and hands ownership
   to the consumer.

Large messages span many 4 KiB NHI frames. The driver requests a completion
interrupt only for the last descriptor in a logical message rather than waking
the CPU for every frame. Two consequences were measured the hard way and are
now design rules:

- Descriptor interrupt behavior is not fully controlled by the USB4STREAM
  client, so per-message interrupt suppression needed a small NHI core change
  (`RING_FRAME_NO_INTERRUPT`).
- Every 4 KiB frame raising an interrupt (~290k/s per ring per host at wire
  speed) starves the softirq/workqueue layer that drives the drain and collapses
  the pipeline. Both verbs data rings now use `tb_ring_throttling(128 us)`, as
  `thunderbolt_net` does; that cut interrupts 42x and made throughput
  consistent (`ZEROCOPY_VERBS.md`).

## Stream transport design

Keep an ordinary TCP or socket link for discovery, negotiation, health checks,
and small control messages. Use a dedicated NHI stream for bulk payload.

### Kernel side

The Linux 7.2 USB4STREAM backport and the driver-owned zero-copy UAPI implement
the following concepts; the native DMA-BUF experiment extends rather than
replaces them:

- Fixed TX and RX slot pools allocated and DMA-mapped once.
- `mmap()` access to those pools from the consuming process.
- Explicit `POST_RX`, `SUBMIT_TX`, and `REAP_COMPLETIONS` operations.
- Producer/consumer indices and generation numbers so stale completions cannot
  reuse a slot incorrectly.
- Credit-based flow control so the sender never overruns peer RX slots.
- Scatter/gather descriptors for messages larger than one page.
- Per-message rather than per-frame completion interrupts.
- Event notification through polling, `eventfd`, or `io_uring`, with a busy-poll
  option for the lowest-latency benchmark.
- Correct disconnect cancellation and cleanup of every pinned/mapped page.

Use multiple slots (at least double buffering, probably 4-8 per direction) so
the next message can be produced while the previous one is on the link.

### Native verbs side

`kernel/verbs/` implements the same ring substrate as an `ib_device` with
uverbs objects, so an unmodified rdma-core application can use it. Its limits
are explicit and enforced rather than approximated: one context/PD/RC QP per
device, SQ4/RQ4, one SGE per WR, four MRs of at most 8 MiB, MTU 4096,
SEND/RECV plus software READ/WRITE capped at 2 MiB. `ZEROCOPY_VERBS.md`
documents the scheduling and zero-copy work that made it competitive;
`NATIVE_VERBS.md` documents the full contract and the remaining acceptance
gates.

### GPU integration

The driver-owned proof is complete: a consumer registers the fixed mmap with
`hipHostRegister(..., hipHostRegisterMapped)` and can then use those aliases as
external graph tensors. That path is true shared-page GPU/NHI access, but
writing results directly into mapped system pages cost about 0.065 ms per
output-head invocation in the workload that measured it, so mapped-direct is
not automatically a win.

Native allocation ownership is a distinct experiment rather than a fallback for
failed registration. A complete base `hipMalloc` allocation can be exported as
a DMA-BUF on Strix Halo. The driver mode imports, pins, and maps one bounded TX
and RX allocation to the NHI device and creates 4 KiB descriptors from the
mapped SG tables. It remains gated on actual NHI-device mapping and explicit
asymmetric ownership:

```text
TX: GPU system release -> NHI owns mapped slot -> TX completion -> reusable
RX: NHI completion -> GPU system acquire -> GPU completion -> repost
```

An idle `hipDeviceSynchronize()` is not an RX acquire because external NHI DMA
does not dirty the GPU's queue-fence state. DMA-BUF CPU sync and KFD
reservation fences do not invalidate GPU L2 either. The exact prototype and
source-pinned event-marker candidate are specified in
`GPU_TO_GPU_FEASIBILITY.md`.

The driver must use the Linux DMA API correctly for the NHI device. Keep the
IOMMU enabled: it provides DMA isolation and does not prevent this design.
Disabling it would enlarge the blast radius of a driver or protocol bug and is
not a prerequisite for direct DMA.

### Consumer integration

Integrate as an additional transport rather than by replacing an existing one.
An NHI backend should:

- Negotiate message shape, byte count, dtype, and slot ID over the control link.
- Have the producing GPU write into the registered TX slot where feasible.
- Send only a small header plus the fixed-slot payload.
- Allow the receiving GPU to consume the RX slot directly where feasible.
- Preserve a TCP fallback for unsupported kernels, connection loss, and
  correctness comparison.

`TP_TRANSPORT_CONTRACT.md` states the rules a GPU-resident consumer must follow
on this transport: pool allocation and import order, the cache/ownership model,
ring protocol invariants, and the teardown ordering that avoids wedging the
link.

## Suggested implementation order

1. Record the TCP baseline: latency, bandwidth, CPU time, copies, and message
   sizes for the workload in question.
2. Get USB4STREAM onto both hosts (7.2 kernel or backport) and benchmark
   unmodified `/dev/tbstreamX` at the real message sizes. This isolates the
   benefit of bypassing TCP even though copies remain.
3. Add mmap-able fixed buffer pools and a userspace ping-pong test. Validate
   correctness, NHI DMA, interrupt count, and QD1 latency before involving a
   GPU. **Complete.**
4. Prove GPU access to the mapped pool. **Complete** for `hipHostRegister()`;
   treat native HIP DMA-BUF import as a separate bounded experiment because
   mapped registration succeeds but direct mapped output has a measured
   penalty.
5. Expose the same substrate as a standard verbs device so unmodified RDMA
   applications can use it. **Complete and hardware-validated**, within the
   bounded contract above.
6. Tune ring depth, slot count, CPU affinity, interrupt affinity, interrupt
   throttling, frame batching, and spin-versus-sleep completion behavior.
7. Run long soak tests, disconnect/reconnect tests, IOMMU fault tests, and
   output-equivalence checks before treating any path as production.

## Benchmark matrix

Compare the paths with identical message shapes:

| Transport | Network stack | User/kernel payload copies | Intended role |
|---|---:|---:|---|
| TCP over `thunderbolt-net` | Yes | Yes | Ubiquitous baseline |
| Stock USB4STREAM | No | Yes | Low-risk intermediate baseline |
| USB4STREAM, driver-owned zero-copy pools | No | No | Mature high-throughput path |
| USB4STREAM, native HIP DMA-BUF pools | No | Intended zero payload copies | Import/synchronization experiment only |
| Native verbs, CPU-copy staging | No | Yes | Compatibility path for unmodified RDMA apps |
| Native verbs, zero-copy data plane | No | No | Current fastest measured path |

For each, measure:

- One-way and round-trip latency at the message sizes that matter.
- p50, p95, p99, and maximum latency, not just aggregate bandwidth.
- Queue depth 1 and pipelined transfers.
- CPU utilization, context switches, interrupts, and bytes copied.
- Sustained runs long enough to expose drift, leaks, and interrupt storms —
  every defect in `ZEROCOPY_VERBS.md` was invisible in short runs.

The end-to-end gain for any consumer is bounded by the share of its critical
path that is transport. A large reduction in microbenchmark latency may produce
a much smaller application-level improvement if compute dominates.

## Decision gates

- Optimize the consumer's own hot path before removing more transport safety.
  A transport redesign becomes interesting again when multiple messages are in
  flight or when boundary traffic is a material share of the critical path.
- If stock USB4STREAM does not materially beat TCP at your message sizes,
  profile the staging copies and scheduling before writing a large kernel
  patch.
- Mmap zero-copy and direct graph aliases are proven, but topology correctness
  is not a speedup. Measure before believing.
- Proceed with native DMA-BUF import only through the bounded no-traffic
  mapping and stale-cache ownership gates. Stop if it does not remove the
  mapped-output penalty or show another clear benefit.
- Do not accept a throughput number from a single run on this link: the verbs
  data plane was bimodal (18–1313 MiB/s) until interrupt moderation landed.
  Require consistency across consecutive runs and a long sustained run.
- If true one-sided RDMA semantics become a requirement, use external hardware;
  do not try to synthesize an HCA in the NHI driver.

## Remaining effort

The page-backed proofs are complete on both planes. The native DMA-BUF variant
is not a small buffer swap: it needs lazy path activation, transactional
bounded import, exact SG tests, no-traffic hardware mapping, and a stale-cache
ownership gate. On the verbs side the throughput A/B against the stream path
is done; hardware READ/WRITE are now validated end to end (zero-copy WRITE at
356–456 MiB/s with 15.6 GiB (16,000 MiB) per direction verified byte-for-byte, and
zero-copy READ, on by default since 2026-10-12, at 408–418 MiB/s over 20 consecutive
15.6 GiB-per-direction runs with its timeout recovery forced, `bad_frames` 0); the open work is conformance breadth
(hot-unplug, malformed peers,
packet loss) and GPU-direct registration instead of CPU
staging. Production
work on any path remains dominated by disconnect/reset cancellation,
synchronization, security, and compatibility rather than descriptor creation.
