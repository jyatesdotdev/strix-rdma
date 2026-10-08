# Verbs fast path: scheduling fix + zero-copy design

Goal: make the native verbs path the default DS4 transport. That requires
closing the ~16x throughput gap vs USB4STREAM (measured 62.9 MiB/s vs
1016-1146 MB/s per direction, `bench/results/2026-10-06-nhi-verbs-vs-stream.md`).

## Phase 1 — kill the 1 ms cadence (implemented, offline-verified)

Implemented in `main.c`/`ring.c`: the worker drains until idle with a bounded
budget, then busy-polls (`SN_IDLE_SPIN_US`, lock-free `sn_ring_pending` peek)
before sleeping; bursts reschedule at zero delay and the 1 ms requeue is a
backstop for missed callbacks only.

## Phase 2/3 — zero-copy data plane (engine offline-verified; kernel glue
## compiles on 7.2.8; hardware test pending lab window)

Implemented and offline-validated (11/11 engine tests incl. 5 ZC tests, 31/31
suite, ASan/UBSan clean):

- Wire: `SN_ZC_SEND` descriptor, `SN_ZC_READY`, `SN_CAP_ZDATA` cap bit
  (all in `native_wire.h`).
- Engine (`protocol.c`): sender state NONE -> WAIT -> SENDING -> SENT with the
  existing ACK/retry/RNR machinery reused; receiver descriptor matching,
  duplicate-descriptor restart, byte-count completion, staged fallback when
  the peer lacks ZDATA; page-alignment required on both burst addresses.
- Core (`0001` patch): native path-lifetime API now admits **two** exact
  tuples (control + data); the 7-test core-contract model passes.

Kernel glue, now **compile-verified on 7.2.8** (both modules build clean).
Hardware test pending a lab window: the zero-copy data plane needs
thunderbolt-net absent (its ring HopID), which in turn needs the production
stream group released to reload the core module. Also note the driver's probe
is now TB-IP-optional: with no thunderbolt-net the wire handshake alone proves
link liveness and the carrier gate is satisfied internally.

- `ring.c`: data ring pair alloc/start/stop/free (`sn_data_rings_start`),
  `sn_data_send_burst` (header-less frames straight from the send MR's DMA
  pages), `sn_zdata_repost`/`sn_data_recv_start`/`sn_data_recv_restart`
  (receive-MR pages posted to the data RX ring), `sn_data_receive` drain
  feeding `sn_engine_zc_bytes`, per-burst `dma_sync_sg_for_device/cpu`.
- `mr.c`: lazy `dma_map_sg` of the MR's umem sg list for the ring DMA device
  (`sn_mr_ensure_mapped`), `sn_mr_dma` sg-walk, unmap at deregistration.
- `main.c`: second in/out HopID (`in_hop2`/`out_hop2`, rendezvous immediate
  `rxhop2`), data-plane bring-up in `sn_rings_start` with EAGAIN retry in
  `sn_refresh`, teardown in `sn_rings_stop`/`sn_remove`.

Measured bottleneck: the worker drains one batch (<=64 RX frames) and sleeps
up to 1 ms. Frames arriving *during* a pass have their wake-up callback lost
(the work item is already queued, `mod_delayed_work` is a no-op), so they wait
for the next tick. At 528 frames per 2 MiB message the cadence dominates.

Fix, exactly the NAPI/busy-poll pattern the stream path effectively gets from
callbacks:

1. **Drain-until-idle**: after each pass, if any progress was made (frames
   received or sent), loop immediately instead of returning.
2. **Bounded spin before sleep**: when idle, spin-poll for new work for a
   bounded window (dropped `d->lock`, `cpu_relax` + `cond_resched`) before
   requeueing. While DS4's GPU is waiting on TP data the CPU is otherwise
   idle, so a spinning consumer is free latency. Ring callbacks still wake the
   worker at 0 delay once it does sleep, so the 1 ms timer is only a backstop.

No protocol change; offline engine tests are unaffected (they model the ring,
not the workqueue).

## Phase 2/3 — architecture (as designed; implementation state above)

The staged path copies payload: app MR -> coherent ring buffer -> cable ->
coherent ring buffer -> app MR. Zero-copy requires the NHI to DMA directly
from/to the application's registered-MR pages. The blocker is the per-frame
SNH1 header: an NHI ring descriptor is a single contiguous buffer, so a
112-byte header cannot be prepended to a payload that lives in an MR page
without a copy, and a header frame landing in an MR page corrupts app data.

A single ring cannot route control frames to scratch buffers and data frames
to MR pages — the NHI consumes posted buffers strictly FIFO, and the peer does
not know what is posted. The Strix NHI has exactly 3 ring HopIDs per direction
(0 = thunderbolt ctl, 1 = thunderbolt-net, 2 = strix_nhi), so a second verbs
ring requires giving up TB-IP coexistence on that host.

**Chosen architecture: two ring pairs, control + data.**

- **Control ring pair** (existing staged ring): coherent buffers, full SNH1
  protocol — HELLO/BIND handshake, ACK/RNR, small sends (below a threshold),
  and per-message **ZC descriptors** (`SN_ZC_SEND`: sequence, total length;
  SEND needs no rkey since the receiver picks the WQE).
- **Data ring pair**: posted with the current receive-WQE MR pages. Payload
  frames are header-less; message framing comes from the NHI sof/eof PDFs;
  ordering and reliability come from E2E (the link does not lose or reorder
  frames). The sender DMAs frames directly from its send-MR pages (TX
  zero-copy); the NHI writes them directly into the receiver's MR pages (RX
  zero-copy).

**Large-send flow** (DS4's 2 MiB chunks):

1. Sender sends a staged `SN_ZC_SEND` descriptor on the control ring.
2. Receiver matches it to a posted receive WQE (or RNRs), posts the WQE's MR
   pages on the data ring, and ACKs `ZC_READY` on the control ring.
3. Sender blasts the payload as header-less frames on the data ring.
4. Receiver counts bytes; at `total` it completes the WQE and sends the
   completion ACK on the control ring, then posts the next receive WQE's
   pages (DS4's window buffers are persistent, so reposts are cheap and
   steady-state-stable).

Ordering: the engine already serializes one SQ operation at a time, so at most
one ZC burst is in flight per direction; the data ring carries exactly one
message's frames between descriptor/ACK pairs. E2E guarantees those frames
arrive in order and intact.

**Threshold**: messages <= one staged frame keep the current staged path
(control, ACKs, small sends) — copies are irrelevant there.

**TB-IP coexistence**: zero-copy mode needs ring HopID 1 (TB-IP's). The driver
detects at probe: if thunderbolt-net is absent it claims both hops and runs
zero-copy-capable; if present it runs staged-only (today's behavior). DS4's
TP control channel (TCP metadata) can use LAN addresses when TB-IP is down.
Management SSH to the nodes is over the LAN, not TB-IP, so dropping TB-IP
during zero-copy runs is survivable — to be confirmed on hardware.

## Failure/RNR semantics under zero-copy

- No matching receive WQE at descriptor time -> staged RNR, sender retries per
  RC rules (unchanged).
- Data-ring CRC/overrun -> count and fail the QP (E2E makes this
  theoretically unreachable; keep the check).
- Retry: a ZC burst is retried as a whole message (descriptor + payload), same
  as staged multi-fragment messages today; the receiver's byte accounting
  resets on a duplicate descriptor (same sequence).
