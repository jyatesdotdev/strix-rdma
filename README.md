# strix-rdma

[![CI](https://github.com/jyatesdotdev/strix-rdma/actions/workflows/ci.yml/badge.svg)](https://github.com/jyatesdotdev/strix-rdma/actions/workflows/ci.yml)

Experimental hardware-assisted DS4 tensor transport between two Strix Halo
systems over Thunderbolt/USB4, built on the Linux `thunderbolt-stream`
(USB4STREAM) driver and the USB4 NHI DMA rings. The NHI backend is a research
path; the production configuration currently uses descriptor-framed protocol-v3
TCP.

The full design and rationale live in [docs/PLAN.md](docs/PLAN.md)
(sourced from `~/Repositories/ds4/RDMA.md`). Short version:

- The current DS4 layer-slice path moves boundary tensors over TCP via
  `thunderbolt-net`, paying for multiple payload copies plus the network stack.
- Soft-RoCE / soft-iWARP are rejected: software RDMA keeps all of that overhead.
- Target: pre-posted send/receive message passing directly on the NHI DMA
  rings — mmap-able fixed TX/RX slot pools, credit flow control, per-message
  completions, and (if it works) ROCm mapping of the same pages so tensors go
  GPU → DMA page → cable → DMA page → GPU.
- This is *not* one-sided RDMA; the NHI has no rkeys/QPs/atomics and they
  cannot be synthesized in a driver.

## Quick start

```sh
make            # build the userspace tools (pingpong, ds4-shape)
make check      # run the no-hardware test suite
make rocm       # ROCm/HIP tools (needs hipcc)
```

The full path from clone to a measured stream between two hosts — kernel
backport, device access policy, stream bring-up, smoke tests — is
[docs/INSTALL.md](docs/INSTALL.md). Host deployment of the managed stream
lifecycle is `sudo make install-lifecycle ROLE=allocator|follower`.

## Repository layout

```
docs/            Design docs. PLAN.md is the master plan.
kernel/          Kernel-side work: USB4STREAM backport to the hosts' 7.1.5
                 kernel, then the zero-copy UAPI patches on top.
  backport/      Extracted upstream patches for the backport.
tools/pingpong/  Userspace latency/bandwidth test against /dev/tbstreamX.
bench/           Benchmark matrix scripts + results (TCP baseline vs stock
                 USB4STREAM vs zero-copy NHI stream).
linux/           Sparse, blobless checkout of torvalds/linux for reference
                 (drivers/thunderbolt, drivers/net/thunderbolt, docs).
                 Not tracked by this repo.
```

## Related trees

- `~/Repositories/ds4` — the DS4 codebase this transport plugs into.
  Integration points: `ds4_session_eval_layer_slice()`,
  `ds4_gpu_tensor_read()/ds4_gpu_tensor_write()`, `ds4_distributed.c`.

## Test hosts

Two Strix Halo systems, direct Thunderbolt/USB4 link, running Linux 7.1.5
(USB4STREAM needs 7.2 or a backport — that backport is step one of the kernel
work). Development happens here; kernel builds and all measurements happen on
the hosts.

## Implementation order (from the plan)

1. Record the TCP baseline (latency, bandwidth, CPU, copies, tokens/s at batch
   1/2/4/8).
2. Get USB4STREAM onto both hosts (7.2 kernel or backport) and benchmark stock
   `/dev/tbstreamX` with DS4-sized messages.
3. Zero-copy UAPI: mmap-able fixed buffer pools + userspace ping-pong test.
   **Complete:** built and measured on both hosts; see `LOG.md` and `bench/results/`.
4. Prove ROCm/GPU access to the mapped pool (`hipHostRegister()` first,
   DMA-BUF only if that fails).
   **Complete:** the full 16 MiB TX pool mapped and passed bidirectional
   GPU/CPU verification on both hosts; DMA-BUF is not needed for this gate.
   See `bench/results/2026-08-04-usb4stream-zc-oneway-rocm.md`.
5. Optional NHI transport backend in DS4.
   Integration contract: `docs/DS4_INTEGRATION.md`.
   **Single-link software path complete:** protocol v3 negotiation and bulk
   descriptors, persistent CPU-copy NHI, mapped 32-bit ROCm slot handoff,
   generation/sequence rejection, and TCP/v2 fallback are implemented. The
   mapped path copies graph tensors directly to/from registered driver slots;
   kernel-direct use of their GPU aliases and pipelined leases remain tuning
   work. Zero-copy patches 11 and 12 repair the reproduced lost-MSI-X and
   fresh-open path-order failures. Both CPU-copy and mapped required-NHI
   full-model runs now pass, but NHI remains non-default pending longer soak
   and active peer-reboot qualification.
6. Tune (ring depth, slots, affinities, interrupt throttling, spin vs sleep).
7. Soak, disconnect/reconnect, IOMMU fault, output-equivalence, and end-to-end
   DS4 hardware tests. **Initial NHI reliability gate passed:** the exact
   asymmetric workload completed 2,880 exchanges across 27 sessions and
   921,600 pair descriptor completions without a kick or transport error;
   required-NHI CPU-copy and mapped full-model correctness also pass. Controlled
   full-model rates are effectively identical to TCP v3, so production remains
   `--dist-transport auto` without an NHI device while longer soak and active
   peer-reboot testing continue. See
   `bench/results/2026-08-05-nhi-msix-rx-prime-fix.md`.
