# TP transport integration contract

What the gate-4..7 campaign and the exchange probe validated, stated as
the rules an NHI TP backend must follow. Reference implementation:
`tools/stale-cache/tbstream-tp-exchange.cu` (measured: 29.0 µs/exchange
transport, 35.2 µs with single-workgroup reduce, sustained full-duplex).
Kernel side: thunderbolt_stream patches 1–15 (`kernel/zerocopy/`);
production today runs 1–12, so TP work needs the transient patch-14/15
module set (`tools/scripts/p14-swap.sh on /tmp/thunderbolt_stream-p15.ko`).

## Pool allocation and import

1. One pool per direction per device, `ring_size × 4096` bytes
   (production ring 4096 → 16 MiB). Allocate with
   `hipExtMallocWithFlags(..., hipDeviceMallocUncached)` — the spin
   design requires MTYPE_UC (below). 16 MiB allocations are dedicated
   BOs, but always verify: `hipMemGetAddressRange` must return
   `base == ptr && size == alloc_bytes`, else abort (slab suballocation
   would export the wrong BO — the driver's same-BO check catches only
   the both-directions case).
2. Export with `hipMemGetHandleForAddressRange(..,
   hipMemRangeHandleTypeDmaBufFd, 0)`; import with `TBSTREAM_ZC_IMPORT`
   (tx and rx ranges in one call, offset 0, length = pool bytes)
   **after `open()` and strictly before `TBSTREAM_ZC_ENABLE`** — lazy
   activation makes ENABLE the activation point, and import must precede
   it. Close the dmabuf fds after the ioctl; the kernel holds its own
   references until final device close.
3. Requires `CAP_SYS_RAWIO` + `thunderbolt_stream.zc_diagnostic_dmabuf=1`
   while the interface stays diagnostic-gated. Import failures are
   config errors; never fall back silently.
4. Free order at shutdown: `close(device_fd)` first (drops the kernel
   attachment), then `hipFree`.

## Memory model (the part that bites)

5. **Stamps travel in-band** (a word inside the DMA'd message — the
   probe uses the last word of the slot). They cannot live in a separate
   allocation: only pool bytes cross the wire.
6. **Wave-resident polling requires MTYPE_UC.** Coarse memory is
   coherent only across kernel-dispatch boundaries (AQL invalidation,
   gate 5); a persistent wave polling coarse memory never observes NHI
   writes (GPU L2 is not snooped mid-wave). One imported pool has one
   MTYPE, so spin-mode pools are whole-pool UC. Measured cost for
   once-per-exchange streaming payload traffic: negligible.
7. Poll with `__hip_atomic_load(ACQUIRE, SYSTEM)`, stamp with
   `__hip_atomic_store(RELEASE, SYSTEM)`. Payload writes must complete
   before the stamp: `__syncthreads()` (which waits the lane's
   outstanding stores) between payload write and lane-0 stamp is
   sufficient on UC memory.
8. TX visibility to the NHI needs a release: for persistent-kernel
   senders on UC pools the stamp store suffices; for dispatch-per-send
   designs a stream synchronize after the fill kernel is the release
   (gate 6c). Host-mediated signals (hipHostMalloc control blocks) work
   in both directions with plain CPU stores / system-scope GPU atomics.

## Ring protocol

9. Slots advance strictly in order on both sides:
   `slot(n) = (n % (ring/frames)) × frames`. Both peers must submit in
   the same order — the receive side has no addressing, only arrival
   order.
10. Message geometry: `frames` per message, wire cost is
    `frames × 4096` regardless of payload bytes; ring % frames == 0
    keeps messages contiguous (28 KiB payload → 8 frames; a 7-frame
    variant would need wrap-tolerant slot math).
11. Repost cadence: reap driver events off the critical path
    (`TBSTREAM_ZC_REAP` nonblocking each iteration) and `POST_RX` in
    consumption order. Slot reuse distance is `ring/frames` messages
    (512 at production geometry) — a detection-time reader has that
    much slack before the slot is overwritten.
12. TX submission is host-only (ioctl). Budget one thread that submits
    on GPU signal (`tx_ready` style); measured host cost ≈ 1–2 µs per
    submit. At most `ring - 1` frames in flight; lockstep exchange
    keeps occupancy at 1–2 messages.

## Lifecycle and the wedge rules (operational, hard-won)

13. **Never transmit toward a peer whose device is not open+enabled.**
    TX toward an inactive peer stalls on zero E2E credits and can wedge
    the entire XDomain connection (TB-IP included, reboot-only
    recovery). Sequence startup as: both sides open→import→ENABLE, then
    an out-of-band barrier (TCP), then first submit. The probe's
    `barrier_sync` is the reference.
14. Keep one side's device open across restarts of the other, or keep
    the reconcile timer running; never leave both closed with the timer
    stopped during a session gap.
15. Stop order for paired teardown: receiver-last is benign under
    patch 15 (a side that received the peer's CLOSE skips its own);
    worker-first-then-coordinator with a minimal gap is the production
    rule.
16. Log `TBSTREAM_ZC_GET_STATS` at close on both sides (flags should
    show TX_IMPORTED|RX_IMPORTED and zero
    failures/event_drops/crc/overrun); flush stderr before teardown so
    systemd stops don't eat the line.

## Performance envelope (measured, ring 4096, 28 KiB payload)

- One-way delivery under full-duplex load: 24.4 µs p50, 39 µs p99.
- Sustained exchange: 29.0 µs transport-only; 35.2 µs with a
  single-workgroup UC reduce (conservative — parallelize/overlap in
  production).
- Per-token sync at DS4's 86-gate schedule: **~2.5 ms transport-only,
  ~3.0 ms with reduce.**
- 4 KiB floor (control-sized messages): 9.8 µs RTT.
- Burst bandwidth ≈ 1.6 GB/s per direction; both directions
  concurrently sustain it.
