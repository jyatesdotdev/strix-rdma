# TP transport integration contract

What the gate-4..7 campaign and the exchange probe validated, stated as
the rules an NHI TP backend must follow. Reference implementation:
`tools/stale-cache/tbstream-tp-exchange.cu` (measured: 29.0 µs/exchange
transport, 35.2 µs with single-workgroup reduce, sustained full-duplex).
Kernel side: thunderbolt_stream patches 1–15 (`kernel/zerocopy/`). The
current DS4 tensor-parallel production pair runs the patch-15 module with
`zc_diagnostic_dmabuf=1`; the former descriptor-v3 TCP production stack ran
patches 1–12. Keep the diagnostic module parameter disabled unless the
imported-pool TP path is intentionally deployed; see
`tools/modprobe.d/ds4-tbstream-zc.conf` and the systemd TP examples in
`tools/systemd/`.

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
   (gate 6c). Production TP uses a timing-enabled
   `hipEventReleaseToSystem` recorded after the producer/stamp and a
   service-thread `hipEventSynchronize()` before submit. An event recorded
   before a later same-stream one-CU spin was verified on both gfx1151
   hosts to synchronize while that spin remained active
   (`hip-event-before-spin`) — **never substitute a stream/device sync**,
   which would wait for the RX spin and bilaterally deadlock. Host-mediated
   signals (hipHostMalloc control blocks) also work in both directions
   with plain CPU stores / system-scope GPU atomics.

## Ring protocol

9. Slots advance strictly in order on both sides:
   `slot(n) = (n % (ring/frames)) × frames`. Both peers must submit in
   the same order — the receive side has no addressing, only arrival
   order. `tbstream_zc_tx.first` is kernel output, not a requested slot:
   assert after every submit that it equals the expected frame index.
   Validate completion geometry too: RX has the expected `first`,
   `nframes`, and wire `bytes`; TX_DONE has the expected `first` and
   `nframes` but **`bytes == 0`** (the field is RX-only).
10. Message geometry: `frames` per message, wire cost is
    `frames × 4096` regardless of payload bytes; ring % frames == 0
    keeps messages contiguous (28 KiB payload → 8 frames; a 7-frame
    variant would need wrap-tolerant slot math).
11. Reap driver events off the critical path. `POST_RX` is safe only
    after **both** (a) the complete eight-frame RX event has arrived and
    (b) the GPU's final reader of that slot has completed. Stamp detection
    can precede the host callback; posting then may return `EINVAL`. If a
    spin kernel only gates later readers, its completion is not consumption:
    record an event after the last downstream slot reader. Repost strictly
    in sequence order once both conditions hold; small batches are fine.
    Slot reuse distance is `ring/frames` messages (512 at production
    geometry, only 5.95 DS4 tokens), so this is slack, not permission to
    leave credits outstanding indefinitely.
12. TX submission is host-only (ioctl): exactly one submit per gate/message.
    Budget one service thread that event-synchronizes the GPU release then
    submits; measured ioctl cost ≈ 1–2 µs. At most `ring - 1` frames may be
    in flight. Before a producer overwrites slot `n % 512`, it must know the
    old `n - 512` TX completion returned that slot; reap/retry `ENOBUFS`
    without violating this producer-side ownership.

## Lifecycle and the wedge rules (operational, hard-won)

13. **Never transmit toward a peer whose device is not open+enabled.**
    TX toward an inactive peer stalls on zero E2E credits and can wedge
    the entire XDomain connection (TB-IP included). A full coordinated
    `thunderbolt_stream` reload healed the 2026-08-30 incident without
    reboot (see `bench/results/2026-08-30-ds4-tp-nhi-production.md`);
    keep coordinated reboot as the fallback if reload does not restore the
    link. Sequence startup as: both sides open→import→ENABLE, then an
    out-of-band barrier (TCP), then first submit. The probe's
    `barrier_sync` is the reference.
14. Keep one side's device open across restarts of the other, or keep
    the reconcile timer running; never leave both closed with the timer
    stopped during a session gap.
15. Stop order for paired teardown: first quiesce GPU slot users and
    submission, then drain ownership events. Close rank 1/worker while
    rank 0/coordinator keeps its device open; after rank 0 observes the
    CLOSE (or a post-close control acknowledgment), it closes and patch 15
    skips the impossible reciprocal CLOSE. A simultaneous teardown barrier
    passed the probes but is not the production ordering. Keep the survivor
    open across a peer restart.
16. Log `TBSTREAM_ZC_GET_STATS` at close on both sides (flags should
    show TX_IMPORTED|RX_IMPORTED and zero
    failures/event_drops/crc/overrun); flush stderr before teardown so
    systemd stops don't eat the line.
17. Root-run test binaries must not reuse a non-root manual run's
    `/tmp/ds4.lock`: Fedora `fs.protected_regular` rejects opening the
    other user's mode-0600 file in sticky `/tmp`. Set
    `DS4_LOCK_FILE=/run/ds4-stage3.lock` per host, or remove a
    confirmed-stale host-namespace lock only after proving no DS4 process
    remains. Production units currently use `PrivateTmp=yes`: their
    `/tmp/ds4.lock` files and flocks are healthy but live in private mount
    namespaces, so they **cannot** exclude a manual host-namespace process.
    Stopping production and auditing processes remains mandatory. The
    durable singleton fix is one shared lock under a writable `/run`
    RuntimeDirectory for both systemd and manual runs.

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
