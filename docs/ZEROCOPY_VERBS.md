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

## Hardware bring-up status

Bring-up is solved and the data plane is validated end to end on
max <-> max2 (Strix Halo, 7.2.8-100.fc43, no-TB-IP mode).

### Measured results

Correctness: `strix-verbs-smoke --live` PASS on both nodes (18,878,353 bytes
verified, DS4-shaped setup/window4/ERR-flush); 31/31 offline tests green.

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
  5168 ms, 1238.39 MiB/s). This is at the link ceiling documented in
  `bench/results/2026-09-18-link-rate-credits.md` (~9.5 Gbit/s per direction)
  and above the USB4STREAM zero-copy path's 1146 MB/s.
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
  modules-load.d entry (the stream path installs `ds4-tbnet.conf`) would still
  pull it in. The script writes `blacklist` plus `install thunderbolt_net
  /bin/false` to `/etc/modprobe.d/strix-nhi-verbs-zc.conf`.
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
