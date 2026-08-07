# DS4 stage-7 readiness and non-disruptive live checks — 2026-08-04

> This is a historical record of the non-disruptive checks performed on
> 2026-08-04. Later full-model and exact asymmetric raw testing found an NHI
> reliability failure despite these symmetric component passes. See
> [2026-08-05-ds4-nhi-reliability-and-tcp-release.md](2026-08-05-ds4-nhi-reliability-and-tcp-release.md).

Hosts: `max` and `max2`, Fedora kernel `7.1.5-101.fc43.x86_64`, direct
Thunderbolt/USB4 link. An existing 156 GB DS4 model pair remained online over
TCP throughout these checks; its processes and sockets were not restarted or
reconfigured.

## Host and driver state

- Both hosts load matching `thunderbolt`, `thunderbolt_stream`, and
  `thunderbolt_net` modules with the running-kernel vermagic.
- `/dev/tbstream0` exists on both sides with ring size 4096 and throttling 0.
- The Thunderbolt IP link passed both-direction health checks with 0% loss;
  measured average RTT was 0.126 ms and 0.206 ms by direction.
- TheRock HIP 7.13 detects the `gfx1151` Radeon 8060S GPU.
- The full 16 MiB mapped TX-pool GPU read/write gate passed again on both
  hosts.
- No process held the stream device before or after testing, and no relevant
  kernel error appeared during the checks.

## Live zero-copy diagnostics

Payload verification and orderly CLOSE accounting passed for every run:

| Test | Result |
|---|---|
| 4 KiB `zping` | 100 measured + 10 warmup; p50 26.9 us |
| 64 KiB ring-wrap `zping` | 300 measured; p50 161.3 us |
| 1 MiB `zping` | 32 measured + 4 warmup; p50 2.233 ms |
| 1 MiB `ztx`/`zrx` | 64 messages / 64 MiB exact; 310.0 / 941.4 MB/s |

The 64 KiB run included one 184 ms scheduling/load outlier, and the one-way
sender/receiver intervals differ substantially. These are correctness and
readiness diagnostics alongside a live service, not publishable transport
benchmarks.

After the checks, the original DS4 worker/coordinator PIDs were unchanged, the
TCP route remained established with empty queues, the API model-health request
succeeded, and `/dev/tbstream0` had no open holder.

## Live DS4 transport-backend gate

The model-independent DS4 harness then exercised the production transport API,
v3 bulk descriptors, NHI envelopes, directional sequences, and
generation-seeded payload verification over the physical link. Both hosts
reported equal 4096-byte / 4096-frame geometry. Each endpoint verified 32 messages in each
direction, 64.09 MiB per direction, 16,424 frames per direction, and 4.01
literal physical-ring wraps:

| Harness | Required paths | Result on both endpoints |
|---|---|---|
| CPU-only NHI | CPU copy | PASS: TX 0 mapped / 32 copy; RX 0 mapped / 32 copy |
| ROCm NHI | mapped and wrap fallback | PASS: TX 28 mapped / 4 copy; RX 28 mapped / 4 copy |

Both roles exited zero. The final audited sources and cross-host-identical
binaries were checked before execution. The ROCm executable ran without an
`LD_LIBRARY_PATH` override through its embedded `/opt/rocm-therock/lib`
RUNPATH. This harness reads and writes mapped leases through their host alias;
the separate full-pool ROCm test above remains the proof that the GPU device
alias can access the mapping.

The final postflight again found no device holder or harness process. The
production PIDs were unchanged, its TCP queues were empty, API health passed,
Thunderbolt ping had zero loss, and no matching kernel error appeared. These
are correctness diagnostics beside a loaded service, not benchmark results.

## Software regression matrix

The macOS `make -B test` run completed every model-independent row: Q4_K and
MXFP4 scalar tests, extractor/agent/server checks, layer packing (97/97), core
transport (98/98), protocol v3 (87/87), distributed WORK/RESULT plus mapped
rejection, multi-GPU placement (98/98), GPU argument/CLI checks (53/53), the
Metal kernel exactness suite, and fused MXFP4 Metal MoE. Its final model-backed
invocation stopped only because the expected local `ds4flash.gguf` fixture is
absent; the smallest compatible model is larger than available local disk.

Strict C99 warnings-as-errors, ASan/UBSan, TSan, static analysis, and repeated
focused transport runs also pass. On both Linux hosts, clean isolated ROCm
builds produce all five applications plus the CPU and ROCm live harnesses;
transport, v3, distributed, CLI, and synthetic MXFP4 ROCm tests pass. The ROCm
test covers token widths 1, 2, 3, 4, 5, 32, 128, and 512 plus cold/warm SSD
paths with zero failures. Built artifacts are byte-identical across the two
hosts and execute with `LD_LIBRARY_PATH` unset.

## Readiness blockers found

1. The deployed DS4 binaries predate protocol v3 and `--dist-transport`. The
   current ROCm tree and harnesses build in an isolated per-user directory on
   both hosts, but the applications have not replaced the `/opt` deployment.
2. `/dev/tbstream0` is `0600 root:root`. Production DS4 must use the scoped
   `tools/udev/99-tbstream.rules` policy and a dedicated `tbstream` group, not
   run as root.
3. Both hosts boot with `amd_iommu=off`. Functional mapping passed, but DMA
   isolation and IOMMU fault recovery remain untested. Re-enabling it requires
   a coordinated reboot and repetition of the mapping/transfer gates.
4. Only 28–32 GiB was available while the existing split model was resident.
   A second 156 GB pair is not a safe or isolated benchmark; end-to-end NHI
   testing requires a maintenance-window stop/restart with the old versioned
   deployment retained for rollback.
5. Physical cable interruption, IOMMU fault injection, and an 8–24 hour soak
   are deliberately disruptive and were not inferred from a non-disruptive
   test pass.

This evidence advances stage 7 through host readiness, mapped-pool validation,
raw zero-copy correctness, ring wrap, orderly close, and the live DS4 NHI
backend's CPU-copy plus mapped/fallback paths. It does not mark model-backed
DS4 output equivalence, lifecycle/fault recovery, or soak testing complete.
