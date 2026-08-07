# Hardware-assisted DS4 transport over Thunderbolt/USB4

> **Current outcome (2026-08-06):** The NHI design remains experimental, but
> zero-copy patches 11 and 12 repair the reproduced lost-notification and
> fresh-open failures. The exact asymmetric gate now passes 2,880 exchanges
> across 27 sessions, and required-NHI CPU-copy and mapped full-model runs pass.
> Production still selects protocol-v3 TCP because the controlled NHI cohort
> shows no throughput gain. In-process ROCm events now account for 99.66% of
> the two-host evaluation span and identify routed MoE, attention output, and
> QKV preparation as the dominant model work. Direct mapped logits add
> 0.065 ms to the worker output head, exceeding the lease-fence work they
> remove. Extended soak/active peer-reboot qualification remains. See
> `../bench/results/2026-08-05-nhi-msix-rx-prime-fix.md` and
> `../bench/results/2026-08-06-ds4-rocm-event-profile.md`.

## Goal

Reduce the latency and CPU overhead of moving DS4 boundary tensors between a
paired set of Strix Halo systems. The target is hardware-assisted data movement,
not a software RDMA implementation layered over the existing Thunderbolt TCP
link.

## Conclusion

Soft-RoCE (`rxe`) or software iWARP (`siw`) is not the useful path here. Both
would retain the network stack and execute the RDMA protocol in software, so
they are unlikely to improve on well-tuned TCP over `thunderbolt-net` for this
latency-sensitive workload.

The promising path is a custom zero-copy DS4 transport built on the USB4/Thunderbolt
Native Host Interface (NHI) DMA rings. The NHI already performs the physical
transfer from a local DMA address into pre-posted memory on the other host. A
kernel driver can expose fixed, mmap-able TX/RX buffer pools to userspace and
submit those buffers directly to the NHI rings. If ROCm can access the same
pages, boundary tensors can move:

```text
sending GPU -> shared DMA page -> NHI -> cable -> NHI -> shared DMA page -> receiving GPU
```

This would be hardware-DMA message passing with send/receive semantics. It is
not full InfiniBand-style RDMA: the Strix Halo NHI has no public support for
remote virtual addresses, rkeys, queue-pair state, RDMA reads/writes, atomics,
or an RDMA reliability engine. A kernel patch cannot add those missing hardware
features. True one-sided RDMA would require an external RDMA NIC or FPGA.

## Why the current TCP path costs more

The current DS4 layer-slice path reads a GPU tensor into host memory, sends it
through the distributed TCP transport, and writes the received host buffer into
the peer GPU tensor. Below that, `thunderbolt-net` copies TX payload into its own
4 KiB DMA buffers, while TCP/socket handling adds further copies, protocol work,
and wakeups.

The resulting data path is approximately:

```text
GPU -> DS4 host buffer -> socket/TCP -> thunderbolt-net DMA pages
    -> NHI/cable/NHI -> network/TCP -> DS4 host buffer -> GPU
```

The relevant DS4 calls are currently in `ds4_session_eval_layer_slice()` and
use `ds4_gpu_tensor_read()`, the distributed socket transport, and
`ds4_gpu_tensor_write()`.

Linux's [`thunderbolt-net` TX path](https://github.com/torvalds/linux/blob/master/drivers/net/thunderbolt/main.c#L1040-L1151)
supports scatter/gather and TCP segmentation offload, but it still copies SKB
payload into NHI ring pages.

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

Stock USB4STREAM is not yet zero-copy. It allocates and DMA-maps its own pages,
then uses [`copy_page_from_iter()` on TX](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/stream.c#L467-L505)
and [`copy_page_to_iter()` on RX](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/stream.c#L599-L745).
It should remove TCP overhead and is worth measuring as a baseline, but it will
not provide the desired GPU-to-NHI direct path without further changes.

## What the NHI can offload

An NHI ring descriptor contains a local physical/DMA address, a length, framing
flags, and completion/interrupt flags. See the upstream
[`nhi_ring_desc`](https://github.com/torvalds/linux/blob/master/drivers/thunderbolt/nhi_regs.h#L20-L38).
The public ring interface currently describes frames of at most 4096 bytes.

That is enough for an efficient pre-posted message transport:

1. The receiver posts DMA-mapped slots to its RX ring.
2. The sender fills a registered TX slot and submits descriptors that reference
   it.
3. NHI hardware moves the frames over the cable into the peer's posted slots.
4. The receiver gets one completion for the logical tensor and hands ownership
   to the GPU.

Large tensors will span many 4 KiB NHI frames. The driver should request a
completion interrupt only for the last descriptor in a logical message, rather
than waking the CPU for every frame. The current ring code may need a small core
change because descriptor interrupt behavior is not fully controlled by the
USB4STREAM client.

## Proposed DS4 NHI transport

Keep TCP for discovery, negotiation, health checks, and small control messages.
Use a dedicated NHI stream only for bulk boundary tensors and batched logits.

### Kernel side

Start from Linux 7.2 USB4STREAM or backport its driver, then add a zero-copy UAPI
with the following concepts:

- Fixed TX and RX slot pools allocated and DMA-mapped once.
- `mmap()` access to those pools from the DS4 process.
- Explicit `POST_RX`, `SUBMIT_TX`, and `REAP_COMPLETIONS` operations, either as
  ioctls or `io_uring` commands.
- Producer/consumer indices and generation numbers so stale completions cannot
  reuse a slot incorrectly.
- Credit-based flow control so the sender never overruns peer RX slots.
- Scatter/gather descriptors for messages larger than one page.
- Per-message rather than per-frame completion interrupts.
- Event notification through polling, `eventfd`, or `io_uring`, with a busy-poll
  option for the lowest-latency benchmark.
- Correct disconnect cancellation and cleanup of every pinned/mapped page.

Use multiple slots (at least double buffering, probably 4-8 per direction) so
the next tensor can be produced while the previous one is on the link.

### ROCm integration

The first proof of concept should mmap driver-owned pages into the DS4 process
and test whether `hipHostRegister()`/mapped host memory lets the Strix Halo GPU
read and write those pages at acceptable latency. The integrated GPU and shared
system memory make this plausible, but it must be demonstrated; it should not
be assumed.

If registering driver-mapped pages is unsupported or slow, investigate exporting
the slot pool as DMA-BUF and importing it through the supported ROCm/KFD memory
path. Explicit ownership fencing is required in either case:

```text
TX: GPU completion -> DMA sync/fence -> NHI owns slot -> TX completion -> reusable
RX: NHI completion -> DMA sync/fence -> GPU owns slot -> GPU completion -> repost
```

The driver must use the Linux DMA API correctly for the NHI device. Keep the
IOMMU enabled: it provides DMA isolation and does not prevent this design.
Disabling it would enlarge the blast radius of a driver or protocol bug and is
not a prerequisite for direct DMA.

### DS4 integration

Add a transport abstraction rather than replacing the TCP implementation. The
NHI backend should:

- Negotiate tensor shape, byte count, dtype, and slot ID over the control link.
- Have the producing GPU write the layer-boundary result into the registered TX
  slot where feasible.
- Send only a small header plus the fixed-slot payload.
- Allow the receiving GPU to consume the RX slot directly where feasible.
- Preserve TCP as a fallback for unsupported kernels, connection loss, and
  correctness comparison.
- Cover both hidden-state transfers and potentially large batched logit
  transfers.

## Suggested implementation order

1. Record current TCP latency, bandwidth, CPU time, copies, message sizes, and
   token throughput at batch sizes 1, 2, 4, and 8.
2. Boot Linux 7.2 (or backport USB4STREAM) on both hosts and benchmark unmodified
   `/dev/tbstreamX` with DS4-sized messages. This isolates the benefit of
   bypassing TCP even though copies remain.
3. Add mmap-able fixed buffer pools and a userspace ping-pong test. Validate
   correctness, NHI DMA, interrupt count, and QD1 latency before involving ROCm.
4. Prove GPU access to the mapped pool and measure GPU-to-GPU latency. Fall back
   to DMA-BUF work only if the simpler mapped-memory path fails.
5. Add the optional NHI backend to DS4 and pipeline tensor production, transfer,
   and consumption.
6. Tune ring depth, slot count, CPU affinity, interrupt affinity, interrupt
   throttling, frame batching, and spin-versus-sleep completion behavior.
7. Run long soak tests, disconnect/reconnect tests, IOMMU fault tests, and output
   equivalence checks before using it as the default transport.

## Benchmark matrix

Compare all three transports with identical tensors and model settings:

| Transport | Network stack | User/kernel payload copies | Intended role |
|---|---:|---:|---|
| TCP over `thunderbolt-net` | Yes | Yes | Production baseline |
| Stock USB4STREAM | No | Yes | Low-risk intermediate baseline |
| NHI stream | No | CPU-copy or mapped, by mode | Qualified experimental candidate |

For each, measure:

- One-way and round-trip latency at the actual hidden-state and logit sizes.
- p50, p95, p99, and maximum latency, not just aggregate bandwidth.
- Queue depth 1 and pipelined transfers.
- Batch sizes 1, 2, 4, and 8.
- Prefill and decode separately.
- CPU utilization, context switches, interrupts, and bytes copied.
- End-to-end tokens/second and time/token.

The end-to-end gain is bounded by the share of token time currently spent on
transport. A large reduction in microbenchmark latency may produce a smaller
token-rate improvement if GPU compute dominates. Batched inference and logit
movement are likely to benefit more because transferred messages are larger and
there is more opportunity to pipeline work.

## Decision gates

- The original hardware, mapped-pool, ROCm registration, DS4 integration, and
  correctness gates are complete. The measured transport/control residual is
  too small to justify more safety removal on the current serial split.
- Optimize and re-measure routed MoE, attention output, QKV preparation, and
  the output head before revisiting transport. A transport redesign becomes
  interesting again if model halves overlap, multiple tokens are in flight, or
  batch/logit traffic makes the boundary a material share of the critical path.
- The first compute pass is complete. Routed-MoE rows per wave `2/4/8`
  regressed by `1.97%/5.37%/9.80%`, so the one-row kernel remains selected.
  Fusing Q/KV RMSNorm with KV RoPE removes one launch per layer and improves
  balanced full-model internal decode by `0.194%`; the independent GPU stream
  ledger implies `0.218%`. This validates launch fusion as a real but small
  lever. The next bounded candidate is the FP8 KV quantize plus raw-store
  boundary, not further transport safety removal.
- If stock USB4STREAM does not materially beat TCP at DS4's message sizes,
  profile the GPU staging copies and scheduling before writing a large kernel
  patch.
- If mmap zero-copy materially beats stock USB4STREAM, proceed with GPU mapping.
- If the GPU cannot access the NHI pool efficiently, estimate DMA-BUF work versus
  the measured maximum benefit before continuing.
- If true one-sided RDMA semantics become a requirement, use external hardware;
  do not try to synthesize an HCA in the NHI driver.

## Rough effort

A focused proof of concept is approximately one week once both hosts have a
USB4STREAM-capable kernel: bring-up and baseline testing, mmap-able ring pools,
a userspace ping-pong tool, and a first ROCm registration experiment. Production
hardening and upstream-quality interfaces would take longer, especially around
disconnect handling, synchronization, security, and compatibility.
