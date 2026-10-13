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

## Needs a design decision

- [ ] **E2E flow control on AMD.** Natural zero-copy timeouts are the NHI
  holding a burst's last frame for more than 67 ms while the peer has a buffer
  posted for it. That points to a withheld E2E credit, consistent with upstream
  reports of E2E credit-return trouble on this class of hardware
  (`docs/ZEROCOPY_VERBS.md`, round twelve). Recovery handles it today, at under
  1% of throughput. Options:
  - (a) Leave it as is (current recommendation).
  - (b) A/B test with E2E off on the control rings only, which leaves one E2E
    pair per host. The control ring then becomes lossy whenever a receiver's
    poller stalls (gaps of up to 30 ms have been measured). Boot-to-boot
    variance means several reboot cycles per arm.
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
