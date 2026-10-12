# Verbs fast path: scheduling fix + zero-copy design

Goal: make the native verbs path a viable general transport. That required
closing the ~16x throughput gap vs USB4STREAM: 62.9 MiB/s for the staged verbs
path against 1016-1146 MB/s per direction for the stream path, measured
2026-10-06 on the lab host pair at window 4 x 2 MiB in both directions.

## Phase 1 — kill the 1 ms cadence (implemented, offline-verified)

Implemented in `main.c`/`ring.c`: the worker drains until idle with a bounded
budget, then busy-polls (`SN_IDLE_SPIN_US`, lock-free `sn_ring_pending` peek)
before sleeping; bursts reschedule at zero delay and the 1 ms requeue is a
backstop for missed callbacks only.

## Phase 2/3 — zero-copy data plane (engine offline-verified; kernel glue
## compiles on 7.2.8; hardware test pending lab window)

Implemented and offline-validated (16/16 engine tests incl. 8 ZC tests, 37/37
suite, ASan/UBSan clean):

- Wire: `SN_ZC_SEND` and `SN_ZC_WRITE` descriptors, `SN_ZC_READY`,
  `SN_CAP_ZDATA` cap bit (all in `native_wire.h`). `ZC_SEND` forbids
  rkey/address because its payload lands in a receive WQE the peer already
  posted; `ZC_WRITE` requires both, because its payload lands in the
  requester's remote MR.
- Engine (`protocol.c`): sender state NONE -> WAIT -> SENDING -> SENT with the
  existing ACK/retry/RNR machinery reused; receiver descriptor matching,
  duplicate-descriptor restart, byte-count completion, staged fallback when
  the peer lacks ZDATA; page-alignment required on both burst addresses.
- Core (`0001` patch): native path-lifetime API now admits **two** exact
  tuples (control + data); the 7-test core-contract model passes.

## Hardware bring-up status

Bring-up is solved and the data plane is validated end to end on
max <-> max2 (Strix Halo, 7.2.8-100.fc43, no-TB-IP mode). All figures below
were measured there on 2026-10-10.

### Measured results

Correctness: `strix-verbs-smoke --live` PASS on both nodes (18,878,353 bytes
verified, standard-verbs setup/window4/ERR-flush); 31/31 offline tests green.

Latency, 256 B ping-pong RTT over 2000 iterations (`... ping 2000`):

| | min | avg | max tail |
|---|---|---|---|
| workqueue receive path (before) | 35-37 us | ~52 us | 773 us - 6.4 ms |
| resident poller (now) | **31 us** | **35.1 us** | **345 us** |

~32 us is the hardware floor (two NHI hops plus the wire), so the average now
sits within ~4 us of it.

Throughput, window-4 x 2 MiB bidirectional (`... 200`):

- **1182-1238 MiB/s (9.9-10.4 Gbps) per direction**, consistent across 8/8
  consecutive runs, and sustained over a 6.4 GiB long run (800 iters,
  5168 ms, 1238.39 MiB/s). These runs were measured *pre-reboot*. After a clean
  reboot plus bring-up the same stack measures 1127-1191 MiB/s (mean 1158 over
  5 runs, `bad_frames` 0), which is at or above the 1070-1156 MiB/s recorded for
  previous script-brought-up post-reboot runs -- so a post-reboot number in the
  1130s is normal for this stack, not a regression. That is the practical link ceiling: iperf3 over
  `thunderbolt-net` on the same pair tops out at 9.49-9.51 Gbit/s one-way
  (measured 2026-09-18) against a 20 Gb/s per-direction nominal rate, and
  raising `thunderbolt.dma_credits` above its default 14 did not lift it. It is
  also above the USB4STREAM zero-copy path's 1146 MB/s.
- Earlier builds were **bimodal** (18-1313 MiB/s run to run); root cause and
  fix below.

On-host 200 ms sampling of `placed_send_bytes` during a healthy run confirms the
rate is steady, not a warm-up artifact (1066/1046/1016/1282/1037/1313/1095
MiB/s across consecutive samples).

### Defects found and fixed

Each of these came from a measurement that contradicted the model, so the
numbers are recorded with them.

1. **Sustained-load deadlock.** A burst aborting on timeout cleared the zc
   state but never reclaimed the data rings, so posted RX buffers and stale TX
   payload lingered, filled the 64-slot rings, and deadlocked every later burst
   (both rings frozen full with `zc_expect=0`). `sn_data_abort` now
   stops/starts both rings, drains canceled slots and resyncs head/tail on
   every abort. Before: 0/4 sustained runs completed. After: they complete.
2. **Two-workqueue-hop receive path.** NHI interrupt -> `ring_work` -> frame
   callback -> `sn_work` -> drain. Replaced with a resident poller thread
   running the identical pass logic (`sn_pass`/`sn_thread`): re-pass while
   busy, peek-spin while cold, then sleep on the same 1 ms maintenance tick.
   `sn_schedule` became a wake, so every existing caller is unchanged.
3. **Lost-wake latch.** `poll_kick` was cleared immediately before
   `wait_event_timeout`, discarding a kick that raced the cold spin (`wake_up`
   is a no-op when the thread is not yet queued). `wait_event` re-tests its
   condition and sleeps again when false, so the frame waited a full tick:
   ping-pong min was pinned at exactly 2004 us = 2 x 1 ms. Fixed by consuming
   the latch only when a pass acts on it -> min 32 us.
4. **Poller pinned at ~88% CPU.** `sn_ring_pending` peeked TX slots, but a
   completed-but-unreaped TX slot is only bookkeeping (the send path reaps it
   before needing the space), so it read as pending forever. RX-only peek ->
   ~9% idle.
5. **Peek lock contention.** `sn_slot_peek` took `ring_lock` per check and the
   poller peeks ~1x/us: roughly 2M acquisitions/sec contending with the
   callback that sets the flag. The peek is an advisory hint, so it is now a
   lock-free `READ_ONCE`; the drain still re-reads and clears under the lock.
6. **Cold spin starved the ring work.** The spin had no `cond_resched`, so it
   monopolised the CPU and starved the very `ring_work` that sets the flags it
   waits for. Spinning longer made latency *worse*, monotonically: 128 us
   window 1308 us avg, 1 ms 1492 us, 5 ms 5161 us, 20 ms 17646 us, while min
   stayed ~31 us. Restored the yield every 32 iterations.
7. **Quadratic MR DMA walk.** `sn_mr_dma` rescanned the scatterlist from the
   head for every 4 KB frame, so a 512-frame burst cost
   O(map_nents x 512). `dma_map_sg` returns *merged* segments, so the cost
   depended on allocation contiguity. Cursor-cached, now O(1) amortized.
8. **The latency probe was measuring RNR, not the path.** The ping-pong armed
   its next receive only after waiting for its echo's send completion, so the
   peer's next ping usually arrived with no receive posted: **1992 RNR
   responses in 2000 iterations**, each retry then waiting on the 1 ms tick.
   Reported ~2000 us avg. Arming before echoing -> `rnr_sent` ~0 and the true
   36.4 us.
9. **Per-frame interrupt storm on the data rings** -- the dominant defect, and
   the cause of the bimodal throughput. Neither data ring had interrupt
   moderation, so every 4 KiB frame raised an interrupt (~290k/s per ring per
   host at wire speed), starving the softirq/workqueue layer that drives the
   drain. `tb_ring_throttling(128 us)` on both data rings, as thunderbolt_net
   does, cut interrupts 42x and made throughput consistent. Full analysis in
   the next section.

`native_stats` now exposes the diagnostics that produced all of the above:
`data_tx`/`data_rx` head/tail, `zdata_posted`, `qp_zc_state`/`offset`/
`expect`/`received`, `zc_bursts`/`zc_retries`/`zc_aborts`, `poll_passes`/
`poll_sleeps`/`poll_wake_kick`/`poll_wake_tick`, `sched_calls`, `rnr_sent`.
`idle_spin_us` is a 0644 module param so the latency/CPU knee can be swept
without a reload.

### Resolved: the intermittent multi-second stall was a per-frame interrupt storm

The cause of the bimodal throughput. Captured simultaneously on both nodes
mid-stall (250 ms sampling), frozen for ~6 s of a 7.7 s run:

```
max : zc_state=SENDING  data_tx 3034686/3034623 (63 = FULL) frozen
      zc_timeouts frozen at 212   sq=3
max2: zc_expect=1       data_rx 3033462/3033399 (63 buffers armed) frozen
      zc_received 0/2097152       sq=0 (its own sends all completed)
```

Frames queued on one side, buffers armed on the other, nothing moving, then it
self-resolves. `zc_timeouts` (renamed `zc_retries`; it counts RNR and ACK
retries alike) frozen proves no retransmit was in flight, so this
was not the software ring-reset desync first suspected (tested and ruled out),
and not gradual E2E credit leakage either: raising `thunderbolt.dma_credits`
from 14 to 64 left the degradation pattern unchanged.

The actual cause was interrupt load. `/proc/interrupts` on a live link showed
the two data rings at 6,042,981 and 6,043,218 interrupts against ~41k on the
control rings -- **one interrupt per 4 KiB frame**, ~290k/s per ring per host
at wire speed. Counting precisely over 4.7 GiB received (1,228,800 frames):
RX ring 1,229,852 interrupts, TX ring 1,229,852 before moderation. That storm
starves the softirq/workqueue layer that drives the drain, and the pipeline
collapses into the frozen state above; whether a given run survived it was the
per-run lottery.

`thunderbolt_net` avoids exactly this on the same NHI with
`tb_ring_throttling(tx, 128000)` plus NAPI on the RX side. This driver had
neither. Applying the same 128 us moderation to both data rings (the control
ring stays unthrottled so ack latency keeps its floor) fixed it outright.

Interrupts for 1,638,400 frames (6.4 GiB) after the change: data TX 38,728,
data RX 38,656 -- a **42x reduction**, one interrupt per ~42 frames. Batching is
safe because the drain consumes every pending frame per pass (~37 frames
accumulate per 128 us at wire speed, well inside the 64-slot ring).

Two fixes were tried and reverted along the way, both recorded so they are not
retried:

- A `SN_ZC_SENDING` watchdog. It bounded the stall correctly but 5/6 runs then
  hard-failed: stalls last ~6 s, so a 67 ms watchdog always exhausts
  `retry_cnt=3` and errors the QP. A recoverable stall beats a dead QP -- and
  with moderation the stall no longer occurs.
- Larger data rings (1024 slots), which broke correctness; see below.

### Operational notes

`tools/scripts/strix-verbs-bringup.sh` performs the whole sequence below and is
idempotent: if strix_nhi is already loaded and the port is ACTIVE it changes
nothing, because reloading a live data plane is the one operation that can wedge
the router. It exits 3 (advisory, not fatal) when the module loaded but the port
has not gone ACTIVE yet, which is the normal case when both nodes boot together
-- the handshake retransmits every 100 ms and converges without intervention.

- The nodes' initramfs carries the **stock** thunderbolt module (no custom
  symbols). After any reboot, reload the stack from the updates build before
  strix_nhi will probe: `modprobe -r thunderbolt` then `modprobe thunderbolt`.
  The script verifies the loaded core actually exports
  `tb_xdomain_try_enable_native_paths` rather than trusting the path.
- thunderbolt_net **auto-loads** via its module alias when the XDomain appears.
  For no-TB-IP mode it must be blacklisted; `blacklist` alone is insufficient
  because it only suppresses alias-based autoload, so an explicit
  modules-load.d entry would still pull it in. The script writes `blacklist`
  plus `install thunderbolt_net /bin/false` to
  `/etc/modprobe.d/strix-nhi-verbs-zc.conf`.
- `strix_nhi` is not auto-loaded at boot; load it **once** per boot.
- Re-probe (`rmmod`/`modprobe`) has been reliable across many cycles when only
  the staged path or short benches ran. The router-level wedge seen earlier
  (`DMA RX path activation failed: -110`) correlated with heavy data-plane
  bursts, is not cleared by a clean teardown, a settle, or an NHI PCI FLR --
  only a power cycle. Treat reboot as the safe path after intensive benching.
- This mode conflicts with the USB4STREAM setup, which requires thunderbolt_net
  present and carrier up (see `tools/systemd/tbstream-lifecycle.md`). The two
  cannot be enabled on the same link at the same time.

### Reverted experiments (recorded so they are not retried blind)

- **1024-slot data rings.** Raised sustained throughput 254 -> 351 MiB/s but
  broke correctness: the data plane is one-burst-at-a-time per device (single
  `zc_expect`, one shared ring pair), and a ring large enough to hold a whole
  512-frame burst lets bursts interleave and defeats the retransmit
  flow-control that the 64-slot ring provides by pacing. Verified transfers
  hung with `zc_expect=0` and 512 frames stranded.
- **4 ms workqueue idle spin.** Unlocked a 1183 MiB/s peak but did not keep the
  worker resident and added contention; superseded by the poller thread.
- **SENDING watchdog** (see above).

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

Historical analysis (superseded by the resident poller; kept because the
diagnosis still holds): the worker drained one batch (<=64 RX frames) and slept
up to 1 ms. Frames arriving *during* a pass had their wake-up callback lost
(the work item was already queued, `mod_delayed_work` is a no-op), so they
waited for the next tick. At 512 frames per 2 MiB message that cadence
dominated.

The planned fix was drain-until-idle plus a bounded spin before requeueing --
the NAPI/busy-poll shape. That is what `sn_pass`/`sn_thread` now implement,
with the spin window runtime-tunable via `idle_spin_us`. Two refinements only
measurement revealed: the pending peek must be lock-free and must not look at
TX slots, and the cold spin must keep yielding, or it starves the ring work
that sets the flags being polled. See "Defects found and fixed" above.

No protocol change; the offline engine tests model the ring, not the scheduler,
so they were unaffected throughout.

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
  and per-message **ZC descriptors** (`SN_ZC_SEND`: sequence, total length —
  SEND needs no rkey since the receiver picks the WQE; `SN_ZC_WRITE`: the same
  plus rkey and remote address, since the receiver must place into the
  requester's MR instead of a posted receive).
- **Data ring pair**: posted with the current receive-WQE MR pages. Payload
  frames are header-less; message framing comes from the NHI sof/eof PDFs;
  ordering and reliability come from E2E (the link does not lose or reorder
  frames). The sender DMAs frames directly from its send-MR pages (TX
  zero-copy); the NHI writes them directly into the receiver's MR pages (RX
  zero-copy).

**Large-send flow** (2 MiB chunks):

1. Sender sends a staged `SN_ZC_SEND` descriptor on the control ring.
2. Receiver matches it to a posted receive WQE (or RNRs), posts the WQE's MR
   pages on the data ring, and ACKs `ZC_READY` on the control ring.
3. Sender blasts the payload as header-less frames on the data ring.
4. Receiver counts bytes; at `total` it completes the WQE and sends the
   completion ACK on the control ring, then posts the next receive WQE's
   pages (a consumer's window buffers are normally persistent, so reposts are
   cheap and steady-state-stable).

**Large-write flow** differs only in where the payload lands. The descriptor is
`SN_ZC_WRITE` and carries the requester's rkey and remote address; the receiver
resolves and holds that remote MR instead of matching a posted receive WQE, so
there is no RNR path and no receive completion. It then ACKs `ZC_READY`, the
sender blasts the same header-less frames, and at `total` the receiver retires
the remote MR, counts the bytes into `placed_write_bytes`, and sends the
completion ACK. Eligibility adds one rule over `SN_ZC_SEND`: the *remote*
address must also be frame-aligned, because both sides cap each frame at
`SN_FRAME` from a frame-aligned offset and the sender's `d->zdata_posted`
bookkeeping has to track the wire exactly. A WRITE that is below the threshold,
unaligned locally or remotely, or whose MR lacks `IB_ACCESS_REMOTE_WRITE`, stays
on the staged path.

Ordering: the engine already serializes one SQ operation at a time, so at most
one ZC burst is in flight per direction; the data ring carries exactly one
message's frames between descriptor/ACK pairs. E2E guarantees those frames
arrive in order and intact.

**Threshold**: messages <= one staged frame keep the current staged path
(control, ACKs, small sends) — copies are irrelevant there.

**TB-IP coexistence**: zero-copy mode needs ring HopID 1 (TB-IP's). The driver
detects at probe: if thunderbolt-net is absent it claims both hops and runs
zero-copy-capable; if present it runs staged-only (today's behavior). A
consumer's TCP control/metadata channel can use LAN addresses when TB-IP is
down. Management SSH to the nodes is over the LAN, not TB-IP, so dropping TB-IP
during zero-copy runs is survivable — to be confirmed on hardware.

## Failure/RNR semantics under zero-copy

- No matching receive WQE at descriptor time -> staged RNR, sender retries per
  RC rules (unchanged).
- Data-ring CRC/overrun -> count and fail the QP (E2E makes this
  theoretically unreachable; keep the check).
- Retry: a ZC burst is retried as a whole message (descriptor + payload), same
  as staged multi-fragment messages today; the receiver's byte accounting
  resets on a duplicate descriptor (same sequence).

## Next: zero-copy READ (`SN_ZC_READ`) — design

> **Status (2026-10-12): shipped, on by default, with `zc_read=0` as the
> kill-switch.** See "Outcome" at the end of this document. The sections from
> here to there are a dated record of how it got there. Statements in them that
> `zc_read` is off, gated or unsafe describe the state on that date.

Measured 2026-10-10 on the lab host pair after the WRITE work landed:

| op | path | throughput |
|---|---|---|
| SEND bulk | zero-copy data ring | 1127–1191 MiB/s |
| WRITE | zero-copy data ring | 456 MiB/s |
| **READ** | **staged control ring** | **62.6 MiB/s** |

READ is the last verb that does not use the data ring, and it is ~7x slower than
WRITE. The cost is framing, not copying: a 2 MiB READ reply is pushed through the
**control** ring as ~512 individually headered fragments by `sn_fragments_send()`,
then reassembled by `sn_fragment()`/`sn_fragment_commit()` into `read_incoming`
and copied into the requester's MR — about 32 ms per 2 MiB chunk. The responder's
`sn_mr_copy()` into `read_snapshot` is a ~0.2 ms memcpy and is not the bottleneck.

### Why READ is simpler than WRITE

The WRITE requester does not know where its payload will land until the responder
picks a posted receive WQE, so `SN_ZC_SEND`/`SN_ZC_WRITE` need an `SN_ZC_READY`
round trip before the burst may start. A READ requester *already* knows its
landing zone — it is the local MR and address it posted. So it can arm its own
data RX ring **before** sending the descriptor, and **no `ZC_READY` exchange is
needed**. One fewer round trip than either existing ZC path.

### Flow

1. Requester posts `IBV_WR_RDMA_READ`. If eligible, it sets
   `q->zc_mr = w->mr`, `q->zc_address = w->address`, `q->zc_total = w->length` and
   calls `sn_data_recv_start()` — the same arming `sn_zdata_repost()` already
   implements for the responder side — then sends an `SN_ZC_READ` descriptor
   carrying the *source* `rkey`, `address` and `total`.
2. Responder validates `IB_ACCESS_REMOTE_READ` and `max_dest_rd_atomic`, resolves
   and **retains** `q->remote_mr` (unlike the staged path, which copies and drops
   it immediately), records `read_request`, and sets `response_active`.
3. Responder bursts payload straight from `q->remote_mr` at `h->address` onto its
   data TX ring as header-less frames. No snapshot copy, no fragmentation.
4. Requester's data RX harvest counts bytes; at `total` it does the
   `dma_sync_sg_for_cpu()`, completes the READ WQE and sends `SN_READ_ACK`.
5. Responder releases `q->remote_mr` on the ACK and clears `response_active`.

### Required changes

- `native_wire.h`: `SN_ZC_READ` opcode (new value; do not renumber existing
  opcodes) plus decode validation — like `SN_ZC_WRITE` it *requires* rkey and
  address, and the reply carries no payload header at all.
- `ring.c`: `sn_data_send_burst()` currently reads `w = &q->sq[q->sq_head]` and
  bursts from that WQE's MR. A READ response has no SQ WQE, so generalise it to
  take an explicit `(mr, address, total, offset)` — or add a sibling — and have
  the SQ path call through. Land this refactor on its own with no behaviour
  change; the existing 38 assertions are its regression net.
- `protocol.c`: eligibility for READ (length above `SN_ZC_THRESHOLD`, local
  address frame-aligned, `d->zdata && d->peer_zc`, and a `zc_read` gate);
  requester arming and descriptor emission; a responder branch for `SN_ZC_READ`;
  a response-burst path parallel to the existing `response_active` block; and a
  READ case in `sn_engine_zc_bytes()` completion.
- `main.c`: `zc_read` module parameter, defaulting **off** until validated on
  hardware, exactly as `zc_write` did; flip it only after a clean run.

### Retry safety — apply the WRITE lesson up front

The WRITE replay race was found the hard way, so build the guards in from the
start rather than retrofitting them:

- The responder is now the burst *sender*, so its response-retry path must call
  `sn_data_tx_drained()` and defer the re-burst until every posted frame has been
  consumed, bounded by the retry budget. Restarting on top of frames still queued
  is what corrupted WRITE.
- The requester, on a duplicate descriptor, must reset `zc_received` and call
  `sn_data_recv_restart()` to flush its RX ring — which discards frames already
  received but cannot recall frames still in flight, hence the drain barrier
  above.
- The overshoot guard in the RX harvest already fails the QP rather than
  completing; it covers READ for free.

### One deliberate semantic change

The staged path snapshots into `read_snapshot` so a retried READ returns the
*original* bytes (offline assertion "READ retry returns original snapshot").
Zero-copy READ re-reads the source MR on retry, so a retry may observe newer
data. That is what a real RNIC does — RC guarantees per-operation consistency,
not a snapshot held across retransmits — so it is defensible, but it is a
difference between the two paths and must be documented, not discovered. Small
or unaligned READs keep the staged path and therefore keep the snapshot
guarantee.

### Test plan

Offline first, because the harness is the only way to iterate without a reboot
per hypothesis: extend the atomic `sn_data_send_burst` model to cover the
response direction, then mirror the existing ZC WRITE assertions — descriptor
validation, happy-path placement into the requester's MR, lost-ACK retry with the
drain barrier deferring, duplicate-descriptor restart, threshold/alignment
fallback to staged, and refusal without `IB_ACCESS_REMOTE_READ` or
`max_dest_rd_atomic`. Then hardware: `read 200` and `read 2000` verified
byte-for-byte against the 62.6 MiB/s staged baseline, `bad_frames` 0, and a
bulk SEND run to confirm nothing regressed.

### Hardware result: implemented, gated OFF, not yet safe to enable

`SN_ZC_READ` is implemented and offline-covered (47 assertions) but `zc_read`
**defaults off** and must stay off. Measured 2026-10-11 on the lab host pair
after a clean reboot and bring-up, `zc_read=1` forced via modprobe.d:

| run | result |
|---|---|
| `read 200` | **PASS** — 1,677,721,600 B/direction verified byte-for-byte, 410.68 MiB/s, 800 bursts, `bad_frames` 0 |
| `read 2000` | **FAIL at iteration 390** — `IBV_WC_RETRY_EXC_ERR` (status 12), `zc_retries` 37 and 48 |
| `write 200` (regression) | PASS, 451.09 MiB/s |
| SEND smoke (regression) | PASS, 18,878,353 B byte-for-byte |
| bulk SEND ×2 (regression) | 1230.14 / 1212.73 MiB/s |

So zero-copy READ is **6.6x the staged path** (410.68 vs 62.61 MiB/s) and correct
over 1.6 GB, but it does not survive sustained volume: it fails once retries
start occurring, at roughly the same place the WRITE replay race did. The
signature is the same class — the offline model's `sn_data_send_burst` is atomic,
so a partially posted response burst, a stale in-flight frame and an overshooting
byte count are all still unrepresentable, and the drain barrier that fixed WRITE
is present here (ok 20) without being sufficient.

Two bugs were found and fixed on the way, both only visible on hardware:

1. **Bidirectional deadlock.** `sn_receive_zc()` held *any* inbound burst
   descriptor while `q->zc_expect` was set. Correct for `SN_ZC_SEND`/`SN_ZC_WRITE`,
   which land inbound, but `SN_ZC_READ` answers by bursting OUT on the data TX
   ring and lands nothing, so holding it deadlocked simultaneous bidirectional
   READ: each side armed its landing zone for the response it was owed, then held
   the peer's request until that response landed. Symptom was `RETRY_EXC_ERR` on
   iteration 1 with `zc_bursts` 0 on both hosts, descriptors exchanged, no
   response, no `bad_frames`. Fixed by exempting `SN_ZC_READ` from the hold;
   ok 25 pins it and was verified differentially (reverting the one-line
   condition fails ok 25 alone).
2. **The smoke tool hid the diagnosis.** A bare "RDMA WC status/identity" failure
   does not distinguish retry-exceeded from a remote access refusal from a `wr_id`
   bookkeeping bug. It now prints status, `ibv_wc_status_str`, opcode, the
   `wr_id` decode and `qp_num` before failing.

Next step for whoever enables this: reproduce `read 2000` with the responder's
response-retry path instrumented, and extend the offline harness to deliver a
response burst in bounded chunks across engine passes so the overlap is
representable at all. Until then the staged READ path is the correct one and
keeps its snapshot guarantee.

Incidental: bulk SEND measured 1230/1213 MiB/s in this session, back inside the
1182-1238 MiB/s band and above the 1127-1191 MiB/s recorded for the previous
post-reboot runs. Post-reboot throughput on this stack varies run to run by more
than the ~4% that separates those two bands, so a single post-reboot measurement
is not evidence of regression in either direction.

### Second hardware round: one mechanism fixed, one still open

The in-flight harness model (commit `e0ba936`) immediately paid for itself: it
reproduced offline a mechanism that hardware had only shown as a failure at
iteration 390. `sn_read_response_rearm()` restarted the response whenever
`response_sent` was true, but that flag covers two different states — reply
posted with frames still queued in this host's data TX ring, or reply delivered
with the `READ_ACK` lost. Only the second owes a fresh burst. In the first, the
requester had already timed out, retransmitted and zeroed its own landing zone,
so the queued frames would complete the message by themselves; restarting as well
put two full messages into one zone. Fixed in `f2c7679` by re-bursting only when
`sn_data_tx_drained()`, and pinned by ok 27 (differentially verified).

The requester cannot guard against this itself, which is why the WRITE drain
barrier did not cover it: for WRITE the burst sender is the side that
retransmits, so its own barrier applies. For READ the roles are reversed — the
responder holds the in-flight frames while the requester retransmits, and the
requester has no visibility into the responder's ring.

Re-validated after a clean reboot with `zc_read=1`, three `read 2000` runs
(16,777,216,000 B per direction each):

| attempt | result | `zc_retries` |
|---|---|---|
| 1 | PASS, 416.36 MiB/s, byte-for-byte | 0 |
| 2 | **FAIL, payload verification** | **0** |
| 3 | PASS, 417.14 MiB/s, byte-for-byte | 0 |

**So a second mechanism remains open.** *(Correction, made after the third
round below: the inference originally drawn here — that `zc_retries` 0 meant no
descriptor was retransmitted, so the rearm path could not be involved — was
wrong. `zc_retries` is incremented only when `zc_state != SN_ZC_NONE`, and a
zero-copy READ requester always sits in `SN_ZC_NONE` because it queues no
payload. The counter cannot see READ retransmits at all; the `zc_retries` column
in this table is blind, not zero.)* `bad_frames` stayed 0 throughout and `zc_aborts` rose by
one per run. `zc_read` therefore remains **off by default** and the staged READ
path stays the correct one.

Where to look next, given that it happens with zero retries: the window-4
re-arming of the requester's landing zone across four sequential READs per
iteration; the `dma_sync_sg_for_cpu()` timing across back-to-back completions
into the same MR at different offsets; or bidirectional interleaving, where each
host is simultaneously requesting (RX armed) and responding (TX bursting) on the
same pair of data rings. The harness can now represent in-flight frames, but it
still does not separate "posted, not yet consumed by the NHI" from "consumed, not
yet landed at the peer" — that distinction is the prerequisite for modelling
chunked response posting, and it is noted in ok 27's comment.

Regression check in the same session, default paths: `write 200` PASS at
457.14 MiB/s byte-for-byte, SEND smoke PASS, bulk SEND 1070.95 / 1082.06 MiB/s.
That bulk figure is well below the 1230 / 1213 MiB/s measured earlier the same
day on the same build, immediately after three 16.8 GB READ runs. Combined with
the 1127–1191 and 1182–1238 bands recorded before it, this stack's bulk
throughput swings by more than 13% between runs for reasons not yet understood —
so no single bulk measurement on it is evidence of a regression *or* of an
improvement. Any future throughput claim needs a run count and a spread, not one
number.

### Third hardware round: instrumented, recovery path diagnosed

The blind counter is now fixed. `native_stats` exports five new counters, appended
after the existing fields so the smoke tool's fixed-order `fscanf` of the first six
is unaffected: `zc_read_retx` (READ requester retransmits — what `zc_retries`
could not see), `zc_read_dup_inflight` and `zc_read_rearm` (the two outcomes of a
duplicate READ descriptor at the responder), and `zc_seq_ahead` / `zc_seq_stale`
(the previously silent `h->sequence != q->receive.expected` drop in
`sn_receive_zc()`, split so a desync — *ahead* — is distinguishable from a benign
late retransmit — *stale*). The smoke tool also now classifies a payload mismatch
by what the bad bytes are (zeros, the previous iteration's pattern, or another
slot's pattern) instead of only reporting that one occurred.

Also ruled out this round: **bounce buffering.** The hosts log "Using software
bounce buffering for IO (SWIOTLB)", but the NHI (`c4:00.5`/`c4:00.6`) sits in a
`DMA-FQ` IOMMU domain as a trusted internal device, and
`/sys/kernel/debug/swiotlb/io_tlb_used` read 0 mid-burst across four `read 2000`
runs. DMA is translated, not bounced, so sync placement is not the culprit.

Captured failure, `read 2000` attempt 2 after a clean reboot,
`IBV_WC_RETRY_EXC_ERR` at iteration 161 slot 3 on max2:

| counter | max (responder) | max2 (requester, failed) |
|---|---|---|
| `zc_read_retx` | 0 | **3** |
| `zc_read_dup_inflight` | **3** | 0 |
| `zc_read_rearm` | **0** | 0 |
| `zc_seq_ahead` / `zc_seq_stale` | 0 / 0 | 0 / 0 |
| `bad_frames` | 0 | 0 |

No sequence drops, so the silent-drop path is not it. max2 timed out and
retransmitted three times; max received all three duplicates and ignored every
one through the "response still in flight" branch added in `f2c7679`. Its
response never drained, so it never re-armed, and max2 ran out of retries.

**Diagnosis: the zero-copy READ recovery path is unsound in both versions.** On a
retransmit the requester stops and restarts its own data RX ring
(`sn_data_recv_restart()`) while the responder may still have frames in flight.
Restarting a ring under a live peer burst cancels posted buffers and can drop
frames or strand E2E credits, so the "in flight" frames do not reliably complete
the reset zone. The original rule — re-burst on any duplicate — double-bursts into
that zone (fixed offline by `f2c7679`, ok 27). The `f2c7679` rule — ignore while
in flight — deadlocks whenever those frames cannot land, which is this failure.
Neither rule can be correct while the requester resets its RX ring underneath a
live peer burst. `f2c7679` remains a net improvement in one respect only: it
turns a silent double-burst into a loud `RETRY_EXC_ERR`.

**Proposed fix: zero-copy for the fast path, staged for recovery.** On a READ
response timeout the requester disarms the zero-copy landing zone and retransmits
as a staged `SN_READ`, whose header'd, offset-addressed control-ring fragments are
immune to stale data-ring frames and already carry snapshot/replay semantics. At
the observed rate (~1 failure per 56,000 READs) the staged fallback costs nothing
at steady state. Two questions must be settled before writing it: the responder's
replay-identity check (`sn_same_operation`) will see an `SN_READ` reusing the
sequence of an `SN_ZC_READ`, and an RX ring left unposted after disarming may
stall the peer's TX ring under E2E flow control — which would in turn block that
peer's later zero-copy responses through `sn_response_may_burst()`.

Next measurement worth taking first: split `zc_read_dup_inflight` into
"response not yet fully posted" vs "posted but TX ring not drained", to confirm
which side of the stall the responder is stuck on.

### Fourth hardware round: the stall is *undrained*, and it wedges the whole data plane

`zc_read_dup_inflight` is now split at its single increment site into
`zc_read_dup_posting` (duplicate arrived while the reply was still being posted
onto the data TX ring) and `zc_read_dup_undrained` (reply fully posted, ring not
yet drained); ok 27 asserts a fully-posted reply is attributed to the undrained
side. The smoke tool also gained a per-frame source diagnosis: within one 4 KiB
frame `pattern()` reduces to the source nonce plus one constant
`C = 2*frame ^ 29*iter ^ 53*slot`, so each bad frame reports whose buffer it came
from and its C delta.

One reboot cycle with `zc_read=1`, `read 2000` repeated:

| run | result | responder | requester |
|---|---|---|---|
| (pre-split) 1 | **corrupt payload**, whole 2 MiB slot non-zero wrong bytes | `zc_read_rearm` +1 | `zc_read_retx` +1 |
| 1–6 | PASS | — | — |
| 7 | **RETRY_EXC**, iter 189 | max2 `zc_read_dup_undrained` **+3** | max `zc_read_retx` +3 |
| 8 | RETRY_EXC at **iter 1**, both sides | `zc_read_dup_posting` +3 | `zc_read_retx` +3 |
| 9 | RETRY_EXC at **iter 1**, both sides | `zc_read_dup_posting` +3 | `zc_read_retx` +3 |

Then the default **SEND smoke failed** (`CQ poll/deadline`) on the same boot,
with the port ACTIVE, `bad_frames` 0 and dmesg silent. A normal reboot cleared
it (SEND PASS, `write 200` PASS at 457.27 MiB/s), so the wedge is boot-scoped.

What this settles:

1. **The stall is on the undrained side.** In run 7 the responder had fully
   posted its reply and its data TX ring never drained. Its frames could not be
   transmitted.
2. **It persists across QPs and is ring-wide.** Runs 8 and 9 were fresh QPs that
   could not even *post* a reply (`dup_posting`) — the stranded frames from run 7
   still filled the ring — and zero-copy SEND then failed too. One READ recovery
   failure silently disables the zero-copy data plane for the rest of the boot.
3. **Open design question 2 is answered: yes.** Restarting the requester's data
   RX ring (`sn_data_recv_restart()`: `tb_ring_stop` + `tb_ring_start`) while the
   peer still has a burst in flight leaves the peer's TX unable to drain under E2E
   flow control. The recovery path must therefore **never stop/start the data RX
   ring under a live peer burst**. Posted buffers must stay posted; late frames
   from an abandoned attempt must be absorbed and discarded, not cancelled.
4. **The drained re-burst path also corrupts** (pre-split run): with the ring
   drained the responder re-armed and re-burst, and the requester landed a whole
   slot of wrong, non-zero bytes. Re-bursting into a landing zone that was just
   stopped and restarted is not trustworthy either.

Together these rule out every recovery that keeps using the data ring after a
timeout. That is the case for the staged fallback: once a READ times out, the
requester stops expecting the data ring for that operation entirely and fetches
the bytes through header'd, offset-addressed control-ring fragments.

## Design: staged-SN_READ recovery for zero-copy READ

Zero-copy is the fast path; recovery is staged. Driven by the fourth round above:
the requester must never stop/start its data RX ring under a live peer burst, and
every recovery that keeps using the data ring after a timeout is ruled out.

**Requester, on a zero-copy READ response timeout** (the `zc_read_armed` branch
of the SQ retry path, which today zeroes `zc_received` and calls
`sn_data_recv_restart()`):

1. Do **not** restart the RX ring and do not zero `zc_received`. The landing zone
   stays armed and its buffers stay posted, so any frames the responder already
   queued land and the responder's TX ring can drain. This is the answer to the
   E2E stall question: buffers are never withdrawn under a live burst.
2. Set `zc_read_fallback` and retransmit the same operation as a **staged
   `SN_READ`** — same sequence, `total`, `address`, `rkey`; only the opcode
   differs. Count `zc_read_fallback`.
3. While falling back, the data-RX harvest keeps accounting (the overshoot guard
   stays live), but reaching `zc_total` no longer completes the WQE: completion is
   owned by the staged path. Late zero-copy frames carry the same source bytes the
   staged reply will.
4. When the staged `SN_READ_REPLY` is fully assembled, the responder has
   guaranteed (below) that its data TX ring drained before sending it, and E2E
   neither loses nor reorders frames, so nothing from the zero-copy attempt can
   still arrive. Only now is the landing zone disarmed and the RX ring reclaimed
   with `sn_data_recv_restart()` — safe because no peer burst is live. Then place,
   complete and send `SN_READ_ACK` exactly as the staged path does today.
5. Further timeouts retransmit the staged `SN_READ` through the existing staged
   retry machinery, bounded by `retry_cnt`.

**Cross-path replay identity.** `sn_same_operation()` compares `opcode`, so a
staged `SN_READ` reusing the sequence of a committed `SN_ZC_READ` currently yields
`SN_FRAGMENT_INVALID` and an `IB_WC_REM_INV_REQ_ERR` on the responder. The
comparison treats `SN_ZC_READ` and `SN_READ` as one operation class when sequence,
`total`, `address` and `rkey` all match. No wire field changes meaning; this is
local identity logic only. Every other opcode still compares exactly.

**Responder, on that staged `SN_READ` replay of a zero-copy response**:

1. A burst that has started queueing always finishes (the existing invariant), so
   if the zero-copy reply is mid-post it keeps posting until `response_sent`.
2. Once fully posted, it waits for `sn_data_tx_drained()` — the same barrier the
   response-retry path uses, bounded by the same retry budget.
3. Then it snapshots the retained source MR into `read_snapshot`, switches
   `read_request.opcode` to `SN_READ`, and serves the staged reply through
   `sn_fragments_send()`. The staged reply therefore regains the snapshot guarantee
   for retries. Count `zc_read_fallback_served`. `remote_mr` is still released on
   `SN_READ_ACK`.
4. If the zero-copy reply never started (offset 0, nothing queued), it switches to
   staged immediately.

The previous rearm logic for a duplicate `SN_ZC_READ` (re-burst only when drained)
is removed from the recovery path: a requester that times out no longer sends a
duplicate `SN_ZC_READ` at all, it sends `SN_READ`.

**Interactions.** While falling back the requester keeps `zc_expect`, so a
crossing inbound SEND/WRITE descriptor is still held (`zc_held`) until the landing
zone is disarmed, with the peer's retransmit as backstop, exactly as today.

**Proof obligations** (task 3 writes these failing first): a READ timeout whose
frames never land must complete via the staged path instead of deadlocking; a
timeout with frames still queued must not double-burst or overshoot; a staged
`SN_READ` must be accepted as the same operation as the `SN_ZC_READ` it replaces;
and the landed bytes must equal the source byte-for-byte. Hardware: `zc_read_fallback`
must be non-zero in at least one passing `read 2000` run of the 20-run streak.

### Fifth hardware round: staged fallback validated in part — streak broken at run 5

Fresh reboot with `zc_read=1` on the `18de548` build, `read 2000` streak
(evidence per run: byte counts on both sides plus counter deltas on both hosts):

| run | result | max (deltas) | max2 (deltas) |
|---|---|---|---|
| 1–3 | PASS, 411–413 MiB/s, 16,777,216,000 B/direction both sides | — | — |
| **4** | **PASS**, 411.80 MiB/s, byte-for-byte | `zc_read_retx` 1, `zc_read_fallback` 1 | `zc_read_fallback_served` 1 |
| **5** | **FAIL**, RETRY_EXC at iter 832 slot 3 on max | `zc_read_retx` 7, `zc_read_fallback` 4, `served` 1 | `zc_read_retx` 2, `zc_read_fallback` 1, `served` 3 |

What it proves:

- **The recovery path fires on hardware and works when it does** (run 4): a
  zero-copy READ timed out, fell back to a staged `SN_READ`, was served from a
  snapshot, and the run completed byte-for-byte.
- **The catastrophic mode is gone.** After run 5 failed, the default SEND smoke
  *passed on the same boot*. In round 4 the same kind of failure stranded the data
  TX ring and broke zero-copy SEND until reboot. Removing the requester's RX
  restart on timeout fixed that.
- **A residual remains:** max initiated 4 fallbacks, max2 served 3, and max then
  exhausted its retries on one. Some staged `SN_READ` was not answered by the
  responder — no corruption, no wedge, one QP failed loudly.

Candidate mechanisms, none yet confirmed: the responder holding `response_fallback`
while waiting for `sn_data_tx_drained()` and burning its own `response_retries`
under the zero-copy deadline; the fallback replay meeting a `read_request` already
released by a subsequent operation; or a benign counter gap where the original
`SN_ZC_READ` never committed, so the fallback was served as a fresh staged READ
that `zc_read_fallback_served` does not count. Distinguishing them needs counters on
each exit of the responder's staged-`SN_READ` replay branch. `zc_read` stays **off**.

### Rounds six and seven: the residual is a data-ring drain stall, contained to one QP

Every exit of the responder's fallback path is now counted (`zc_fb_wait_midpost`,
`zc_fb_wait_drain`, `zc_fb_reserve`, `zc_read_req_drop`, `zc_resp_retry_exc`,
`zc_resp_start_exc`). Two failures captured with them, on two different builds:

| round / build | requester | responder | where it parked |
|---|---|---|---|
| 6 / `18de548`+counters, run 6 | fallback 2, retx 5 | served 1, `wait_midpost` 109, `wait_drain` 38 | mid-post |
| 7 / `7977ae2`, run 2 | fallback 2, retx 5 | served 1, `wait_midpost` 1, `wait_drain` **162** | drain |

In both: zero drops, zero invalid replays, zero re-serves, zero response
exhaustions. The unanswered staged `SN_READ` was received and recognised every
time; the responder then waited on its own zero-copy burst. `7977ae2` stops
posting the moment a fallback arrives mid-post (ok 30) — harmless and correct,
since the staged reply supersedes the burst — but it only moved the wait from
mid-post to drain. **The frames already queued in the responder's data TX ring
are never transmitted.** The requester's data RX ring stops accepting them.

The shape is the same in both failures and in the round-5 one: the first
fallback of a run is served and completes, and a *later* zero-copy READ in the
same run stalls with its frames stuck. The staged completion path is the only
place that stops and restarts the requester's data RX ring, so the leading
suspect is that restart desynchronising E2E credit state with the peer's TX —
not every time (round 6 run 3 served three fallbacks in one run and passed), but
often enough. RX slot accounting was checked and is not it: the cancellation
callbacks set `done`, and the restart's drain loop reclaims them.

**Contained.** On the round-7 boot, after the READ failure, the default SEND
smoke passed and `write 200` passed at 456.62 MiB/s byte-for-byte. Round 4's
boot-wide data-plane wedge is gone; the residual fails one QP, loudly.

**Not fixable at the protocol layer with what is observable today.** The stall is
below the engine — ring and E2E state — and the offline harness has no model of
credits. The next step needs ring-level visibility at the moment of the stall:
export `data_tx_head/tail`, `data_rx_head/tail`, `zdata_posted` and the QP's
`zc_received/zc_total` in `native_stats`, capture them from both hosts while a
READ is stuck, and see whether the requester has RX buffers posted. If it does
not, the fix is in how the landing zone is reclaimed; if it does, the stall is in
the NHI's E2E credit state and needs a different reclaim than `tb_ring_stop` /
`tb_ring_start`.

`zc_read` stays **off by default**. Per this goal's contract it is not flipped:
the 20-run streak has not been achieved (best: 5 consecutive passes, two of them
with the recovery path firing and completing byte-for-byte).

### Round eight: ring state instrumented — and no natural timeout in 15 runs

The ring and QP state the previous round asked for is now visible, both live and
frozen at the moment a stall is detected:

- `native_stats` appends, after every existing counter (the smoke tool's
  fixed-order read of the first six is unaffected), `zdata`, `data_tx_head`,
  `data_tx_tail`, `data_rx_head`, `data_rx_tail` and `zdata_posted`, plus, while
  a QP exists, `qp_zc_received`, `qp_zc_total`, `qp_zc_expect`,
  `qp_zc_read_staged` and `qp_zc_read_armed`. The old combined `data_tx a/b`,
  `data_rx a/b` and `qp_zc_received a/b` lines are now separate named fields,
  and `zdata` prints at bring-up, not only while a QP is bound.
- A new `stall_snap` attribute holds two frozen snapshots. `drain_*` is taken
  when the responder waits for its data TX ring to drain under a READ fallback
  (the latest wait, at most one per millisecond). `err_*` is taken at a QP's
  first error other than a flush, before the flush clears the state. Each holds
  the driver's ring indices, the QP's zero-copy and response state, and the
  NHI's own view of both data rings from `tb_ring_get_stats()`:
  `hw_posted`/`hw_completed`, `queued`, `in_flight`. That last part answers the
  open question directly: whether the hardware still owns posted RX descriptors
  when frames stop arriving.

**Hardware: fresh reboot with `zc_read=1` on the instrumented build, 15
consecutive `read 2000` runs, all PASS byte-for-byte** (16,777,216,000 B per
direction, both sides, 410–415 MiB/s). Every READ counter delta is zero on both
hosts in every run: no `zc_read_retx`, no fallback, no drain wait, and
`drain_count = err_count = 0`. That is 30,000 zero-copy READs without one
timeout. Rounds 5–7 each saw their first timeout within the first 2–6 runs of
the boot.

What it shows: zero-copy READ is solid when nothing times out. The residual
lives entirely in the recovery path, and that path's trigger, a natural READ
timeout, is rare and varies from boot to boot. No stall was captured, so the
branch question is still open. Capturing one reliably needs the timeout to be
*induced*: make the requester time out a zero-copy READ while the responder's
burst is still live. That is the precondition of every failure captured so far.

### Round nine: an induced stall, captured on both hosts

**Inducing it.** A smoke-tool `--qp-timeout` option was the first plan, but the
driver accepts only `timeout 14` at RTS (`EINVAL` otherwise), so the timeout is
forced inside the driver instead. The new `zc_read_fault=N` module parameter
(0644, default 0, diagnostic) works like this: on every Nth zero-copy READ this
host requests, the retransmit window expires as soon as the reply is within 32
frames of landing. The requester then falls back to a staged `SN_READ` while the
tail of the responder's burst is still live, which is the precondition of every
recovery failure seen so far. `zc_read_ops` and `zc_read_fault_injected` count
it, and ok 31 covers it offline (differentially verified: with the selection
disabled, ok 31 fails and every other test passes).

**Result:** with `zc_read_fault=100` on both hosts, the first two `read 2000`
runs both failed with RETRY_EXC (run 1 at iter 51, run 2 likewise early). Both
show the same signature. In both, the injected READ fell back, was served staged
and completed byte-for-byte. The *next* zero-copy READ never received one frame.
Frozen state, verbatim (max = responder, `drain_*` taken during the stall;
max2 = requester, `err_*` taken at its retry exhaustion, before the flush):

```
--- run 1
max: drain_count 106
max: drain_status 0
max: drain_data_tx_head 102455
max: drain_data_tx_tail 102392
max: drain_data_rx_head 104448
max: drain_data_rx_tail 104448
max: drain_zdata_posted 2097152
max: drain_zc_received 2097152
max: drain_zc_total 2097152
max: drain_zc_expect 0
max: drain_zc_read_staged 0
max: drain_zc_read_armed 0
max: drain_response 1/1/1/258048
max: drain_hw_tx ret=0 head=55 tail=56 hw_posted=55 hw_completed=56 queued=0 in_flight=63 posted=102455 completed=102392 running=1 irq_en=1 hw_valid=1 tail_flags=0xc
max: drain_hw_rx ret=0 head=0 tail=0 hw_posted=0 hw_completed=0 queued=0 in_flight=0 posted=104448 completed=104448 running=1 irq_en=1 hw_valid=1 tail_flags=0x0
max2: err_count 1
max2: err_status 12
max2: err_data_tx_head 104448
max2: err_data_tx_tail 104440
max2: err_data_rx_head 102463
max2: err_data_rx_tail 102400
max2: err_zdata_posted 258048
max2: err_zc_received 0
max2: err_zc_total 2097152
max2: err_zc_expect 1
max2: err_zc_read_staged 1
max2: err_zc_read_armed 1
max2: err_response 0/1/0/2097152
max2: err_hw_tx ret=0 head=0 tail=0 hw_posted=0 hw_completed=0 queued=0 in_flight=0 posted=104448 completed=104448 running=1 irq_en=1 hw_valid=1 tail_flags=0x0
max2: err_hw_rx ret=0 head=63 tail=0 hw_posted=63 hw_completed=0 queued=0 in_flight=63 posted=102463 completed=102392 running=1 irq_en=1 hw_valid=1 tail_flags=0xc
--- run 2
max: drain_count 211
max: drain_status 0
max: drain_data_tx_head 567406
max: drain_data_tx_tail 567343
max: drain_data_rx_head 569856
max: drain_data_rx_tail 569856
max: drain_zdata_posted 2097152
max: drain_zc_received 2097152
max: drain_zc_total 2097152
max: drain_zc_expect 0
max: drain_zc_read_staged 0
max: drain_zc_read_armed 0
max: drain_response 1/1/1/258048
max: drain_hw_tx ret=0 head=55 tail=56 hw_posted=55 hw_completed=56 queued=0 in_flight=63 posted=567406 completed=567280 running=1 irq_en=1 hw_valid=1 tail_flags=0xc
max: drain_hw_rx ret=0 head=0 tail=0 hw_posted=0 hw_completed=0 queued=0 in_flight=0 posted=569856 completed=569856 running=1 irq_en=1 hw_valid=1 tail_flags=0x0
max2: err_count 2
max2: err_status 12
max2: err_data_tx_head 569856
max2: err_data_tx_tail 569848
max2: err_data_rx_head 567422
max2: err_data_rx_tail 567359
max2: err_zdata_posted 258048
max2: err_zc_received 0
max2: err_zc_total 2097152
max2: err_zc_expect 1
max2: err_zc_read_staged 1
max2: err_zc_read_armed 1
max2: err_response 0/1/0/2097152
max2: err_hw_tx ret=0 head=0 tail=0 hw_posted=0 hw_completed=0 queued=0 in_flight=0 posted=569856 completed=569856 running=1 irq_en=1 hw_valid=1 tail_flags=0x0
max2: err_hw_rx ret=0 head=63 tail=0 hw_posted=63 hw_completed=0 queued=0 in_flight=63 posted=567422 completed=567280 running=1 irq_en=1 hw_valid=1 tail_flags=0xc
```

Reading it (run 2; run 1 is identical in shape):

- **The requester still has data RX buffers posted. That answers the open
  question: yes.** max2's NHI holds 63 posted RX descriptors (`hw_posted=63
  in_flight=63`) and has completed none (`hw_completed=0`, tail flags `0xc` =
  POSTED|INTERRUPT, no COMPLETED). `zdata_posted` is 258048 = 63 frames.
- **The responder's frames are posted to the hardware and never sent.** max's
  NHI TX ring is full with 63 descriptors (`head=55 tail=56`, `in_flight=63`).
  The hardware consumer index has not moved past the software tail
  (`hw_completed=56`), and that tail descriptor is POSTED but not COMPLETED.
- **Nothing was lost.** max TX `completed` equals max2 RX `completed` exactly
  (102392 and 567280), so every frame the responder's NHI ever transmitted was
  received. Driver and core ring indices agree. The other direction (max2 TX to
  max RX) is fully drained.
- **The requester's RX ring was restarted immediately before.** Its core indices
  read `head=63 tail=0`: since the last `tb_ring_start`, exactly this READ's 63
  buffers were posted and nothing was received. The only restart in this path is
  `sn_data_recv_restart()` in the staged completion of the injected READ. In
  run 1 it canceled 8 posted buffers the truncated burst never filled
  (`data_rx_tail` 102400 minus `completed` 102392).
- **Restarting both ends recovers.** On the same boot, after each failure the
  QP teardown restarted both data rings on both hosts. The SEND smoke then passed
  and `write 200` passed at 401 MiB/s, byte-for-byte.

So a transmitter with frames posted, facing a receiver with buffers posted, sends
nothing. That is E2E credit starvation: the responder's TX ring holds no credits
for the requester's re-posted buffers. It begins exactly at a one-sided
`tb_ring_stop`/`tb_ring_start` of the requester's data RX ring while the peer's
data TX ring stays up, and it clears when both ends restart together. This also
explains rounds 5–7: in each, the first fallback of a run was served, and a
*later* zero-copy READ stalled.

### Decision: fixable in this driver (Branch A) — never restart one data RX ring alone

The round-nine capture rules out the NHI losing frames or holding stale ring
state of its own. TX completed equals RX completed, and driver and core indices
agree. Starvation begins at a single driver action, `sn_data_recv_restart()`
when a fallen-back READ completes. That restart existed only to reclaim
landing-zone buffers the truncated zero-copy reply would never fill, and the
driver created those buffers itself. In 7977ae2 the responder stopped posting
the moment a fallback arrived, so the zone could not fill and had to be
reclaimed. The cause is in this driver, so the thunderbolt core is not touched.

The fix: the zone always fills, so it never needs reclaiming.

- **Responder:** a fallback no longer truncates the zero-copy reply. The reply
  is posted in full (started if it never was, finished if it was mid-post), the
  data TX ring drains, and only then is the staged reply served. 7977ae2's
  truncation is reverted. Round six's mid-post stall was this same starvation,
  caused by an earlier fallback in the run, not by finishing the post.
- **Requester:** the staged reply completes the READ only once the abandoned
  burst has fully landed too (`zc_read_reply_ready` parks an assembled reply
  until the last byte lands). Frames that land after the fallback still count
  as progress against the retransmit deadline. The zone is then disarmed with
  every posted buffer consumed, and the data RX ring is never restarted on this
  path.
- **What this gives up:** a zero-copy reply that never fully lands now fails the
  READ with RETRY_EXC instead of completing through the staged copy. That can
  only happen if the lossless E2E data hop loses frames, and reclaiming the zone
  one-sided would starve the peer anyway. ok 19 and ok 29 now assert that loud
  failure, with no ring restart.

Offline: the harness gains an `e2e_model` in which a one-sided RX restart
starves the peer's TX: posts are accepted, never sent, and the ring never reads
as drained again. ok 32 replays round nine (a faulted READ, then the next one)
and failed first against the old code, at the third READ, with RETRY_EXC and
the responder parked in drain wait, which is the hardware signature. ok 19, 28,
29 and 30 were rewritten to the new contract. Differential check: each engine
test run in isolation passes 32/32 with the fix. With only `protocol.c` and
`strix_nhi.h` reverted to the previous commit, exactly those five fail and the
other 27 pass.

Out of scope and unchanged: a retried zero-copy SEND or WRITE descriptor still
restarts the receiver's data RX ring (`sn_receive_zc`, duplicate descriptor).
The same starvation may apply there, but that path was not exercised in this
work.

### Round ten: 20-run streak on the fix, with the recovery path firing in every run

Fresh reboot of both hosts on the `1ab0a0b` build with `zc_read=1` (test
override). The zero-copy READ timeout was induced throughout with
`zc_read_fault=1000` on both hosts, since 15 natural runs in round eight produced
none. That arms the fault on every 1000th READ, about 8 per side per run. An
armed fault becomes a real timeout only if the engine polls before the next
frame lands, because frames still landing refresh the deadline.
`zc_read_fallback` is therefore the count of timeouts that actually fired, and
`zc_read_fault_injected` the count armed. Runs 21–23 are a stress addendum at
`zc_read_fault=100`, the setting that failed two runs out of two in round nine.
Every run is `read 2000`, bidirectional, 8,000 zero-copy READs per side.
Per-run counter deltas:

| run | result (both sides, 16,777,216,000 B/direction) | max: fallback / served / retx / wait_drain / wait_midpost | max2: fallback / served / retx / wait_drain / wait_midpost |
|---|---|---|---|
| 1 | PASS, 416.99 MiB/s | 1 / 1 / 1 / 4 / 3 | 1 / 1 / 1 / 2 / 3 |
| 2 | PASS, 417.77 MiB/s | 1 / 4 / 1 / 12 / 24 | 4 / 1 / 4 / 5 / 5 |
| 3 | PASS, 416.68 MiB/s | 2 / 1 / 3 / 3 / 3 | 1 / 2 / 1 / 50 / 7 |
| 4 | PASS, 416.00 MiB/s | 3 / 3 / 3 / 10 / 15 | 3 / 3 / 3 / 34 / 10 |
| 5 | PASS, 414.30 MiB/s | 2 / 2 / 2 / 58 / 5 | 2 / 2 / 3 / 27 / 5 |
| 6 | PASS, 417.51 MiB/s | 3 / 0 / 3 / 0 / 0 | 0 / 3 / 0 / 11 / 16 |
| 7 | PASS, 416.48 MiB/s | 5 / 1 / 5 / 5 / 4 | 1 / 5 / 1 / 18 / 32 |
| 8 | PASS, 414.14 MiB/s | 6 / 2 / 6 / 58 / 0 | 2 / 6 / 3 / 23 / 37 |
| 9 | PASS, 417.21 MiB/s | 2 / 0 / 2 / 0 / 0 | 0 / 2 / 0 / 9 / 11 |
| 10 | PASS, 416.80 MiB/s | 1 / 2 / 1 / 6 / 13 | 2 / 1 / 2 / 22 / 0 |
| 11 | PASS, 416.15 MiB/s | 5 / 1 / 5 / 3 / 7 | 1 / 5 / 1 / 16 / 37 |
| 12 | PASS, 417.03 MiB/s | 1 / 2 / 1 / 28 / 3 | 2 / 1 / 2 / 4 / 5 |
| 13 | PASS, 408.23 MiB/s | 6 / 3 / 8 / 61 / 6 | 3 / 6 / 4 / 128 / 11 |
| 14 | PASS, 417.81 MiB/s | 2 / 1 / 2 / 3 / 11 | 1 / 2 / 1 / 6 / 10 |
| 15 | PASS, 416.04 MiB/s | 4 / 2 / 4 / 7 / 12 | 2 / 4 / 2 / 29 / 23 |
| 16 | PASS, 416.80 MiB/s | 3 / 0 / 3 / 0 / 0 | 0 / 3 / 0 / 11 / 21 |
| 17 | PASS, 416.51 MiB/s | 4 / 1 / 4 / 3 / 6 | 1 / 4 / 1 / 31 / 17 |
| 18 | PASS, 415.02 MiB/s | 2 / 4 / 3 / 16 / 22 | 4 / 2 / 4 / 57 / 6 |
| 19 | PASS, 417.09 MiB/s | 1 / 4 / 1 / 16 / 21 | 4 / 1 / 4 / 4 / 6 |
| 20 | PASS, 416.18 MiB/s | 2 / 2 / 2 / 26 / 6 | 2 / 2 / 2 / 6 / 13 |
| | *stress addendum, `zc_read_fault=100`* | | |
| 21 | PASS, 411.15 MiB/s | 15 / 29 / 15 / 101 / 163 | 29 / 15 / 29 / 54 / 100 |
| 22 | PASS, 410.72 MiB/s | 22 / 22 / 22 / 78 / 145 | 22 / 22 / 22 / 79 / 111 |
| 23 | PASS, 408.02 MiB/s | 17 / 24 / 17 / 172 / 131 | 24 / 17 / 26 / 71 / 104 |

- **20 of 20 consecutive runs pass byte-for-byte**, at 408–418 MiB/s. Each
  run moved 16,777,216,000 B per direction, verified on both sides. The stress
  addendum adds 3 of 3 at 408–411 MiB/s.
- **The recovery path fired in every one of the 23 runs.** Runs 1–20 totals:
  max initiated 56 fallbacks and max2 served exactly 56; max2 initiated 36 and
  max served exactly 36. Stress runs 21–23: 54 and 75 fallbacks, every one
  served. Before each staged reply, the responder finished posting and drained
  its zero-copy reply first. Runs 1–20 spent 1,248 progress passes in those two
  waits on the two hosts together (`zc_fb_wait_midpost` plus `zc_fb_wait_drain`;
  both count passes, not replies).
- **No QP error on either host in any run** (`stall_snap` `err_count` stayed 0
  all boot). No data RX ring is restarted on this path any more, so there is no
  starvation.
- **The default paths are unaffected on the same boot, with the fault back at
  0:** the SEND smoke passed, `write 200` passed at 433.37 MiB/s and `read 200`
  at 416.67 MiB/s, all byte-for-byte.

This meets the bar set for enabling zero-copy READ by default.
