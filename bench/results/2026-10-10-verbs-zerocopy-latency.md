# Native verbs zero-copy: latency and throughput — 2026-10-10

Measured on the lab nodes (max / max2, Strix Halo pair, Fedora 43 /
7.2.8-100.fc43, Thunderbolt link, no-TB-IP mode so the driver owns both path
tuples). Code at `fa7e500`. DS4 services stopped.

Follow-up to `2026-10-06-nhi-verbs-vs-stream.md`, which recorded the verbs path
at **62.9 MiB/s** against the stream path's 1016-1146 MB/s and concluded the
stream path was ~16x faster. That gap is now closed by the verbs path itself.

## Results

Latency, 256 B ping-pong RTT, 2000 iterations, true echo (listener re-arms its
receive before echoing):

| Receive path | min | avg | max tail |
|---|---|---|---|
| workqueue (`delayed_work`, before) | 35-37 us | ~52 us | 773 us - 6.4 ms |
| resident poller thread (now) | **32 us** | **36.4 us** | ~2.8 ms |

~32 us is the hardware floor for two NHI hops plus the wire, so the average now
sits within ~4 us of it.

Throughput, DS4's shape (window 4 x 2 MiB, bidirectional, per direction):

| Run condition | Throughput | Wall (200 iters) |
|---|---|---|
| healthy run | **1016-1313 MiB/s (8.6-9.8 Gbps)** | 1.36-1.56 s |
| run hit by a credit stall | 20-225 MiB/s | 7-77 s |

On-host 200 ms sampling of `placed_send_bytes` during a healthy run shows the
rate is steady, not a warm-up artifact:

```
1.0s:   39 MiB/s   (ramp)
1.2s: 1066          1.4s: 1046          1.6s: 1016
1.8s: 1282          2.0s: 1037          2.2s: 1313          2.4s: 1095
```

Correctness: `strix-verbs-smoke --live` PASS on both nodes (18,878,353 bytes
verified, DS4-shaped setup/window4/ERR-flush). 31/31 offline tests green.

## Comparison with the stream path

| Path | Bidirectional, DS4 shape | Notes |
|---|---|---|
| native verbs zero-copy (now) | **1016-1313 MiB/s** | 36.4 us avg RTT, header-less bursts straight from MR pages |
| USB4STREAM one-way zero-copy | 1146 MB/s (~9.6 Gbps) | from the 2026-10-06 A/B |
| native verbs staged (2026-10-06) | 62.9 MiB/s | CPU-copy, 1 ms worker cadence |

The verbs path is now in the same class as the stream path at DS4's message
shape, while keeping RDMA semantics (QP/MR/CQ, SEND/RECV, READ/WRITE) that the
stream path does not offer.

## What produced the gain

Not one change; the 1 ms cadence was only the first of several. Each fix below
came from a measurement that contradicted the model then held.

1. Resident poller thread replacing the two-workqueue-hop receive path
   (NHI interrupt -> `ring_work` -> frame callback -> `sn_work` -> drain).
2. A lost-wake latch: `poll_kick` was cleared immediately before
   `wait_event_timeout`, discarding a kick that raced the cold spin. Because
   `wait_event` re-tests its condition and sleeps again when false, frames
   waited a full tick. Ping-pong min was pinned at exactly 2004 us = 2 x 1 ms;
   after the fix, 32 us.
3. `sn_ring_pending` peeked TX slots, and a completed-but-unreaped TX slot is
   only bookkeeping, so it read as pending forever and pinned the poller at
   ~88% CPU. RX-only peek -> ~9% idle.
4. The peek took `ring_lock` ~1x/us (~2M acquisitions/sec) contending with the
   callback that sets the flag. Made lock-free (`READ_ONCE`); it is an
   advisory hint and the drain still re-reads under the lock.
5. The cold spin had no `cond_resched`, so it starved the `ring_work` that sets
   the flags being polled. Spinning longer made latency monotonically *worse*:
   128 us window -> 1308 us avg, 1 ms -> 1492 us, 5 ms -> 5161 us,
   20 ms -> 17646 us, min ~31 us throughout.
6. `sn_mr_dma` rescanned the scatterlist from the head per 4 KB frame
   (O(map_nents x 512) per burst). `dma_map_sg` returns merged segments, so
   cost depended on allocation contiguity. Cursor-cached, O(1) amortized.
7. A burst abort never reclaimed the data rings, so posted RX buffers and stale
   TX payload filled the 64-slot rings and deadlocked every later burst.
   `sn_data_abort` now resets both rings on abort.

The measurement tool needed fixing too: the ping-pong originally armed its next
receive only after waiting for its echo's send completion, so the peer's ping
usually arrived with no receive posted — **1992 RNR responses in 2000
iterations**, each retry then waiting on the 1 ms tick. It was reporting
~2000 us of RNR-retry latency as if it were path latency.

## Open: intermittent multi-second E2E credit stall

The cause of the bimodal throughput. Captured simultaneously on both nodes
mid-stall, frozen ~6 s of a 7.7 s run:

```
max : zc_state=SENDING  data_tx 3034686/3034623 (63 = FULL) frozen
      zc_timeouts frozen at 212   sq=3
max2: zc_expect=1       data_rx 3033462/3033399 (63 buffers armed) frozen
      zc_received 0/2097152       sq=0 (its own sends all completed)
```

Frames queued on one side, buffers armed on the other, nothing moving, then it
self-resolves. `zc_timeouts` frozen proves no retransmit was in flight, so this
is a hardware E2E-credit pause, not the software ring-reset desync first
suspected (tested and ruled out). `SN_ZC_SENDING` arms no deadline until the
burst completes, so a stuck burst is bounded only by the 30 s
`progress_deadline`.

A SENDING-state watchdog was tried and reverted: it bounded the stall correctly
but 5/6 runs then hard-failed, because a ~6 s stall always exhausts
`retry_cnt=3` at a 67 ms timeout and errors the QP. A recoverable stall beats a
dead QP.

Next step is the E2E credit accounting in the thunderbolt core.

## Reproducing

```sh
# latency (connector prints avg/min/max RTT)
./strix-verbs-smoke --live strix_nhi0 listen  <ip> 4789 ping 2000
./strix-verbs-smoke --live strix_nhi0 connect <ip> 4789 ping 2000

# throughput, DS4 shape (window 4 x 2 MiB, bidirectional)
./strix-verbs-smoke --live strix_nhi0 listen  <ip> 4789 200
./strix-verbs-smoke --live strix_nhi0 connect <ip> 4789 200

# live driver state, including the stall signature above
cat /sys/class/infiniband/strix_nhi0/native_stats
```

Bring-up requires the no-TB-IP mode and a single load per boot; see
`docs/ZEROCOPY_VERBS.md` ("Operational notes").
