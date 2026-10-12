# TODO

Open items that need an owner decision or physical access before work can
continue. Evidence and background live in the linked docs.

## Needs physical access

- [ ] **Hot-unplug and abrupt peer loss.** Pull the Thunderbolt cable between
  max and max2 while the verbs stack is up, first with the link idle, then in
  the middle of a `read 2000` / `write 2000` run, and record what each side's
  QP, port state and `stall_snap` show, and whether re-plugging recovers
  without a reboot. Risk: the known inactive-peer NHI wedge
  (`docs/NATIVE_VERBS.md`, acceptance gate 4) can need a power cycle, so this
  needs someone at the machines.

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
- [ ] **GPU-direct registration.** Register GPU memory as verbs MRs (DMA-BUF
  via `ib_umem_dmabuf`, ROCm export on the hosts, IOMMU mapping for the NHI)
  instead of staging through pinned CPU pages. This is a project in its own
  right (see `docs/PLAN.md`, "Remaining effort"). Write a design doc before
  any code.
