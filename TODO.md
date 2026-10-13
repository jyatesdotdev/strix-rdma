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
- [ ] **`SN_VERSION` bump (wire ABI).** A mixed-version pair fails mid-operation
  today instead of refusing to pair. The 2026-10-12 SEND/WRITE change widens
  the gap: an old sender still re-bursts on retry, and a new receiver treats
  that as an overshoot error. A version bump negotiated in HELLO would refuse
  mixed pairs cleanly at handshake. The same bump could add a READ_ACK probe,
  which would close the remaining limit (ok 24): a zero-copy READ whose READ_ACK
  is lost, followed by an idle connection, fails the responder.
- [x] **GPU-direct registration, v1 (2026-10-12).** DMA-BUF MRs from
  `hipMalloc` (`docs/GPU_DIRECT_VERBS.md`): GPU-to-GPU write/read/send verified
  at volume, forced READ fallbacks through GPU memory, kill -9 releases cleanly.
- [ ] **GPU-direct, remaining gates.** G4: a stale-cache gate in which GPU
  kernels produce and consume hot buffers, run with and without the
  system-scope acquire (needs a small HIP program; the v1 test consumes through
  `hipMemcpy`). G5: hot-unplug with GPU MRs registered (needs someone at the
  machines). G3 remainder: small and unaligned operations on GPU MRs, which use
  the staged path through the CPU mapping.
