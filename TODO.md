# TODO

Open items that need an owner decision or physical access before work can
continue. Evidence and background live in the linked docs.

## Needs physical access

- [x] **Hot-unplug, first pass (2026-10-12).** An idle pull and a
  mid-transfer pull both removed cleanly and re-attached to ACTIVE with no
  reboot. A double ring-stop warning was found and fixed
  (`kernel/verbs/README.md`, "Hot-unplug on hardware").
- [ ] **Hot-unplug, remaining cases.** Repeated pull/replug cycles; a pull
  during bring-up or handshake; powering a peer off instead of pulling; a long
  disconnect (minutes); a pull during staged-only traffic. Each needs someone at
  the machines, and the inactive-peer wedge can still need a power cycle.
- [ ] **Reboot soak.** On 2026-10-12 one reboot in 22 hung in late shutdown
  on max: kernel alive, no user space, Thunderbolt domain not yet torn down. The
  hardware watchdog did not reset it, so it took a power cycle. The cause was
  not captured. `strix_nhi` now quiesces at shutdown, and one simultaneous
  reboot with the capture armed was clean (`docs/ZEROCOPY_VERBS.md`, round
  seventeen). A soak of 20 or more reboots with `arm-shutdown-capture.sh` armed
  would either show the hang is gone or locate it, but every hang needs someone
  to power-cycle the host.

## Needs a design decision

- [x] **Low latency by default (2026-10-17).** Polling the control RX ring
  plus a keepalive, bounded to 50 ms after real traffic, are now the defaults:
  - 256 B round trip ~12-13 µs and 10 KiB exchange ~20-22 µs, even after
    compute gaps (they were 35 and 167 µs)
  - bulk at its best measured: SEND 1.1 GB/s, READ 0.86-0.89 GB/s with 0-4
    fallbacks
  - an idle host sends nothing

  The keepalive also fixed polling's bulk and READ regression; round twelve's
  natural READ stalls were the cold transmit path.
  `tools/scripts/strix-verbs-latency-profile.sh` (pinned FIFO poller) is
  optional, for the scheduler's outliers. `docs/ZEROCOPY_VERBS.md`, round
  nineteen.
- [ ] **Run `gufo` TP2 on this stack? (wire version 3)** `gufo`'s TP2 uses a
  deliberately small subset of verbs:
  - one RC QP
  - `IBV_WR_RDMA_WRITE_WITH_IMM` as its only opcode
  - two `ibv_reg_mr` windows of pinned host memory (`mmap` + `hipHostRegister`)
  - a completion channel
  - GPU kernels that signal and wait on coherent host flags

  That is the design measured here at ~23 µs per 10 KiB step. Its decode is
  bound by exchange latency: Donnerkeule publishes 8.5 µs (64 B) and 28 µs
  (16 KiB) one-way. Missing here:
  - **RDMA WRITE with immediate:** consume a posted receive and complete it as
    `RECV_RDMA_WITH_IMM`, staged and zero-copy
  - **queue depth 16** (4 today, and part of the v2 wire validation)
  - **MRs of 144 MiB** (8 MiB cap)
  - **messages up to the window size**, 48 MiB in prefill (2 MiB cap)
  - **`retry_cnt`/`rnr_retry` of 7** at RTS (≤ 6 accepted)

  It would run with `--tp-gid-index 0`, since our GID is not RoCE v2. The
  wire changes need a version bump to 3.
- [ ] **TP integration choice: resident kernel or launch-and-sync.** A
  resident kernel with host-coherent buffers gives a 10 KiB TP step of
  ~23 µs; launch-and-sync gives ~35 µs with heavier tails. A resident kernel
  cannot share DMA-BUF MRs with the NIC. That is the application's design
  decision.

- [ ] **Bulk data on both NHI ring pairs.** The Strix Halo NHI has two usable
  DMA ring pairs, and one ring tops out near 10 Gbit/s per direction. Two rings
  in parallel carried ~19 Gbit/s (`docs/ZEROCOPY_VERBS.md`, round eighteen).
  Verbs uses one pair for control and one for data. Carrying zero-copy bursts
  on both could raise bulk throughput by up to about 1.7×. The costs:
  - control frames queue behind data bursts, which hurts small-operation
    latency (256 B RTT is about 36 µs today)
  - transfers have to be put back in order across rings, because the landing
    order and the recovery design assume one data ring
  - two E2E data rings (round twelve)

  Not yet measured: two rings each way at once.

- [ ] **E2E flow control on AMD.** Natural zero-copy timeouts are the NHI
  holding a burst's last frame for more than 67 ms while the peer has a buffer
  posted for it. That points to a withheld E2E credit, consistent with upstream
  reports of E2E credit-return trouble on this class of hardware
  (`docs/ZEROCOPY_VERBS.md`, round twelve). Recovery handles it today, at under
  1% of throughput. Options:
  - (a) Leave it as is (current recommendation).
  - [x] (b) Done 2026-10-12: control-ring E2E off (`ctl_e2e=0`), two boots per
    arm. No difference: 4 of 12 READ runs stalled with it on, 3 of 12 with it
    off, and throughput was the same (round seventeen). So a second E2E pair is
    not the cause. `ctl_e2e` stays on.
  - (c) Replace hardware E2E on the data ring with a software credit scheme,
    as thunderbolt-ibverbs does on AMD. This is a redesign: the zero-copy
    recovery design currently relies on E2E being lossless.
- [x] **`SN_VERSION` = 2 (wire ABI, 2026-10-12).** A mixed pair is now refused
  at the first frame, counted as `version_mismatch` (never as corruption) and
  reported in the log, instead of forming a session that fails mid-operation.
  The same bump carries `SN_READ_PROBE`, which recovers a READ_ACK lost while
  the connection is idle (the ok-24 limitation).
- [x] **GPU-direct registration, v1 (2026-10-12).** DMA-BUF MRs from
  `hipMalloc` (`docs/GPU_DIRECT_VERBS.md`): GPU-to-GPU write/read/send verified
  at volume, forced READ fallbacks through GPU memory, kill -9 releases cleanly.
- [ ] **GPU-direct, hot-unplug with GPU MRs registered (G5).** Needs someone
  at the machines. G3 (small/unaligned) and G4 (stale cache: the producer's
  release is required, the acquire was not observed to matter) passed on
  2026-10-12 (`docs/GPU_DIRECT_VERBS.md`).
