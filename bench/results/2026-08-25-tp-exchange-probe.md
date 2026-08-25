# 2026-08-25 — Full-duplex TP exchange probe: 3.2 ms/token, TP clears the bar

`tbstream-tp-exchange`: both hosts import native pools in both directions
and send **simultaneously** every iteration — a two-way all-reduce
partial-sum exchange, not a ping-pong. Payload 28 KiB (7168 fp32, one
hidden vector) in 8-frame/32 KiB slots, ring 4096, 2000 iterations (200
warmup), sustained back-to-back. A TCP barrier orders startup after both
devices are imported+enabled (never transmitting toward an inactive peer —
the wedge rule). The reduce arms accumulate the received partial with
float adds and verify sampled accumulator sums against analytic values
**exactly**; payloads are also sample-verified in-flight. Patch-15
modules, transient swap, prod ds4 serving TCP untouched throughout;
dmesg spotless; zero event drops anywhere.

## Results (both roles symmetric; per-token = 90 exchanges)

| Arm | Sustained µs/exchange | ms per 90-exchange token | p50 (p99) latency |
|---|---|---|---|
| **spin, transport-only** | 29.0 | **2.61** | 24.4 µs (39.0) |
| **spin + reduce** | 35.2 | **3.17** | 42.6 µs (52.0) |
| reap, transport-only | 52.3 | 4.71 | 27.9 µs (91.6) |
| reap + reduce | 75.6 | 6.80 | 28.0 µs (99.4) |

## Reading

- **The decision number: spin+reduce = 3.17 ms/token — comfortably under
  the ≤5 ms "TP is worth building" threshold** (and the transport-only
  floor is 2.61 ms). Against a halved compute budget (~34 ms), projected
  TP decode ≈ 37 ms/token ≈ **27 t/s vs today's 14.3 — ~1.9×**.
- Full duplex is essentially free: one-way delivery under simultaneous
  bidirectional traffic (24.4 µs p50) costs ~2 µs over the ping-pong
  half-RTT (22.7 µs). The NHI rings do not contend meaningfully.
- The reap arms lose to dispatch overhead, not the interrupt itself: each
  exchange pays a fill-kernel dispatch + stream sync (and the reduce arm a
  second one). Spin's persistent kernel amortizes all of it.
- Spin+reduce's higher per-exchange latency (42.6 vs 24.4 µs) is the
  single-workgroup in-wave reduce reading 28 KiB of uncached payload
  (~18 µs). Production would parallelize the reduce across CUs and could
  overlap it; sustained cost (35.2 µs) already beats the latency because
  adjacent exchanges overlap. Conservative as measured.
- Wire geometry: 8 frames (32 KiB) transmitted for a 28 KiB payload; a
  7-frame message would shave ~2–3 µs/exchange. Compression to fp16
  halves the vector (~14 KiB → ~9 µs less). Both are future levers, not
  needed to clear the bar.
- Memory-type note (corrects an earlier assumption): stamps must travel
  in-band inside the DMA'd message and one imported pool has one MTYPE,
  so the spin arm uses whole-pool MTYPE_UC. This costs nothing for
  once-per-exchange streaming payloads; the "coarse payload + UC flags"
  split would require two pools per direction, which the import ABI
  (one pool per direction) does not support.

## Stage-3 literal event-ordering gate

The engine design records a TX-ready event and then eagerly enqueues the
RX spin-combine in the same default stream. Its service thread must be
able to synchronize the earlier event without waiting for the later spin;
a stream-tail wait would bilaterally deadlock before either host submitted.
`hip-event-before-spin` tests that literal chain:

```text
producer -> hipEventRecord(hipEventReleaseToSystem) -> one-CU spin
                   |
                   +-> service-thread hipEventSynchronize

max:  event_sync_while_later_spin_active=PASS rc=0 producer=0x51a9c0de
max2: event_sync_while_later_spin_active=PASS rc=0 producer=0x51a9c0de
```

Both runs used exact HIP 7.13/gfx1151 alongside serving production. The
spin kernel signaled that it was actively blocked before the check; the
event waiter returned while it remained blocked. Thus Stage 3 may safely
use event synchronization, but **must never substitute
`hipStreamSynchronize` or `hipDeviceSynchronize`** on that path.

## Verdict

The transport leg of hybrid tensor parallelism is measured and
affordable: **~3 ms/token sync at DS4's exchange shape and cadence,
with exact all-reduce arithmetic on the wire data**. The open half is
now entirely ds4-side (ROCm gate ops + NHI TP backend + kernel-split
efficiency), per the agreed division.
