# strix-rdma

[![CI](https://github.com/jyatesdotdev/strix-rdma/actions/workflows/ci.yml/badge.svg)](https://github.com/jyatesdotdev/strix-rdma/actions/workflows/ci.yml)

General RDMA transport between two Strix Halo hosts over a direct
Thunderbolt/USB4 link, built on the Linux `thunderbolt-stream` (USB4STREAM)
driver and the USB4 NHI DMA rings. It is not specific to any one workload.

Two data planes live here:

1. **Native software verbs** (default-off, experimental) — a real Linux
   `ib_device`/uverbs device (`kernel/verbs/`) plus a standard rdma-core v62
   provider (`providers/strix_nhi/`). Bounded SEND/RECV and software
   READ/WRITE over independent NHI rings, with CPU-copy staging and, in the
   zero-copy mode, payload DMA'd directly between registered MR pages.
2. **USB4STREAM** — the upstream `/dev/tbstreamX` character-device stream,
   backported to the hosts' kernel (`kernel/backport/`) and extended with a
   mmap-able zero-copy UAPI (`kernel/zerocopy/`). This is the substrate the
   verbs work builds on and the comparison point for it.

The Strix Halo NHI is not an RNIC: it does not offload rkeys, queue-pair state,
remote virtual-address placement, RDMA reads/writes, atomics, or a verbs
reliability engine. A kernel patch cannot add those hardware features, so the
verbs semantics here are implemented in software over the DMA rings. Soft-RoCE
(`rxe`) and soft-iWARP (`siw`) were rejected for the same reason in reverse:
they keep the network stack and execute RDMA in software on top of it.

**Validated on the lab host pair:** full two-host SEND/RECV exchange over the
provider, repeated module reload cycles, and an unmodified ROCm
tensor-parallel workload running over it (377 MB placed, ~140k frames per
direction, zero bad frames). That is a correctness result, not complete RC
conformance and not a performance claim. Limits, safety restrictions, and the
reproduction procedure are in [docs/NATIVE_VERBS.md](docs/NATIVE_VERBS.md) and
[kernel/verbs/README.md](kernel/verbs/README.md).

## Quick start

```sh
make            # build the portable userspace tools (pingpong, dmabuf-probe)
make check      # host-independent test suite
make rocm       # ROCm/HIP tools (needs hipcc, gfx1151 target)
```

The full path from clone to a measured stream between two hosts — kernel
backport, device access policy, stream bring-up, smoke tests — is
[docs/INSTALL.md](docs/INSTALL.md). Host-side device access policy installs
with `sudo make install-lifecycle`; bring-up and the teardown ordering rules
that keep the link alive are in
[tools/systemd/tbstream-lifecycle.md](tools/systemd/tbstream-lifecycle.md).

## Repository layout

```
docs/            Design docs. PLAN.md is the master plan.
kernel/          Kernel-side work: USB4STREAM backport to the hosts' 7.1.5
                 kernel, the zero-copy UAPI patches on top, and the separate
                 default-off native verbs service/core patches.
  backport/      Extracted upstream patches for the backport.
  zerocopy/      mmap-able pool UAPI for thunderbolt_stream.
  verbs/         Native RDMA service/core follow-on patches.
providers/strix_nhi/
                 Standard rdma-core v62 provider; no system-library replacement.
tools/verbs/     Explicit-live standard-verbs smoke test (not built by default).
tools/pingpong/  Userspace latency/bandwidth test against /dev/tbstreamX.
tools/dmabuf-probe/
                 Privileged no-traffic DMA-BUF import probe runner
                 (native-pool experiment gate; see kernel/README.md).
tools/rocm-map/  GPU access gates for the mapped and imported pool paths.
tools/stale-cache/
                 Two-host GPU exchange harnesses behind
                 docs/TP_TRANSPORT_CONTRACT.md.
tools/scripts/   Stream endpoint setup and verbs bring-up helpers.
tools/tests/     Offline host-policy test (`make -C tools/tests test`).
tools/systemd/   tbstream-lifecycle.md: endpoint bring-up and teardown rules.
tools/udev/      Scoped /dev/tbstreamN access policy.
bench/           Benchmark matrix and the scripts that produce measurements.
linux/           Sparse, blobless checkout of torvalds/linux for reference
                 (drivers/thunderbolt, drivers/net/thunderbolt, docs).
                 Not tracked by this repo.
```

## Status

Done and measured on the lab host pair:

- USB4STREAM backported to 7.1.5 as a three-module set, and the zero-copy
  series on top: mmap-able fixed TX/RX pools, explicit `POST_RX`/`SUBMIT_TX`/
  `REAP`, per-message rather than per-frame interrupts.
- ROCm access to the driver-owned pool proven: the full 16 MiB TX pool
  registered with `hipHostRegister()` and verified bidirectionally on both
  hosts. Native `hipMalloc` pools imported as DMA-BUF are the diagnostic-gated
  variant (`TBSTREAM_ZC_IMPORT`).
- Native verbs backend and provider, including the zero-copy data plane:
  256 B ping-pong round trip 35.1 µs average (31 µs min) and 1182–1238 MiB/s
  per direction on window-4 × 2 MiB bidirectional bursts, measured 2026-10-10
  on the lab host pair in no-TB-IP mode. That band was measured *pre-reboot*;
  after a clean reboot plus bring-up the same stack measures 1127–1191 MiB/s
  (mean 1158 over 5 runs), which is at or above the 1070–1156 MiB/s previously
  recorded for script-brought-up post-reboot runs — so compare post-reboot
  numbers against post-reboot numbers. For scale, the same pair's practical
  link ceiling is ~9.5 Gbit/s per direction (iperf3 over `thunderbolt-net`,
  measured 2026-09-18) against a 20 Gb/s per-direction nominal rate.
- RDMA WRITE and READ validated end to end on hardware (2026-10-10), including
  **zero-copy WRITE at 356–456 MiB/s**: 15.6 GiB (16,000 MiB) per direction verified
  byte-for-byte in a single `write 2000` run, `bad_frames` 0, 10,400 zero-copy
  bursts; READ verified byte-for-byte over 1,677,721,600 bytes. Zero-copy WRITE
  is ~7× the staged path (449.78 vs 58.81 MiB/s) and is on by default, with
  `zc_write=0` as a kill-switch. Hardware uncovered a burst-replay race that
  silently corrupted payload; the diagnosis, the drain barrier that fixes it and
  the offline test that now models it are in `kernel/verbs/README.md`.
- Zero-copy RDMA READ on by default (2026-10-12), with `zc_read=0` as a
  kill-switch: ~6.6× the staged path (408–418 vs 62.6 MiB/s), 20 consecutive
  `read 2000` runs (15.6 GiB per direction each) byte-for-byte with the timeout
  recovery forced and firing in every run. The last stall was E2E credit
  starvation caused by a one-sided data RX ring restart. How it was found and
  removed is in `docs/ZEROCOPY_VERBS.md`.

Open:

- RC conformance breadth. READ/WRITE are hardware-validated (above), but
  Lost/late ZC_READY, lost ACK/READ_ACK and forced zero-copy timeouts are now
  injected on hardware at volume (`zc_send_fault`, `ack_drop`, `zc_read_fault`;
  see `docs/ZEROCOPY_VERBS.md`), but malformed-peer frames and control-ring
  loss are still covered only offline.
- Hot-unplug beyond the first test. Two pulls (idle, and mid-transfer)
  recovered automatically with no reboot (2026-10-12, `kernel/verbs/README.md`).
  Repeated cycles, a pull during handshake, peer power-off and the known
  inactive-peer NHI wedge are untested, so there is no recovery guarantee yet.
- GPU-direct beyond v1. GPU memory registers as a DMA-BUF MR and moves
  GPU-to-GPU byte-for-byte at volume (2026-10-12, `docs/GPU_DIRECT_VERBS.md`).
  The stale-cache gate shows a GPU producer's system-scope release is required
  before posting. Still open: hot-unplug with GPU MRs registered.

## Test hosts

Two Strix Halo systems, direct Thunderbolt/USB4 link. USB4STREAM needs Linux
7.2 or a backport — that backport is step one of the kernel work, and the verbs
stack was validated on 7.2.8. Development happens here; kernel builds and all
measurements happen on the hosts.

## License

GPL-2.0 (see [LICENSE](LICENSE)). The `kernel/` patch series is derivative of
the Linux kernel and is necessarily GPL-2.0; the userspace tools and docs are
released under the same terms for consistency.
