# 2026-10-10 — DS4 systemd test: native verbs vs TCP, same-session A/B

First end-to-end run of a real DS4 tensor-parallel server over the zero-copy
native verbs provider, driven entirely by systemd drop-ins. **The path is
functionally correct — bit-exact against the TCP reference — but 22.6% slower
at the documented acceptance gate.** That is a regression, not a win, and it
gates "make verbs the default".

## Setup

Both hosts in no-TB-IP zero-copy mode (`thunderbolt_net` absent, `strix_nhi`
ACTIVE via `kernel/verbs/bring-up.sh`), DS4 `pr1036-cab7369-metrics`, model
`ds41-flash-q2` (151.76 GiB, 50/50 expert split), coordinator on max2 /
worker on max.

The production `95-nhi-rdma.conf` drop-in binds the TP control channel to
`10.99.0.2` (TB-IP), which does not exist in no-TB-IP mode. A test-only
`96-nhi-rdma-lan.conf` was layered on top of it — identical `ExecStart` with
the listen address moved to the 5 GbE LAN (`192.168.1.163`) — so deleting one
file restores the documented config. Firewall ports 19475/8080/4789 were opened
on both hosts (19475 was blocked; the worker's first connect failed with
"No route to host").

Provider negotiated on both ranks:

```
ds4-tp: RoCE RC device=strix_nhi0 port=1 gid-index=0 window=4 chunk=2097152 host-staging=16MiB
ds4-tp: worker connected, transport=rdma gate-timeout=750ms
tensor parallelism bound: rank 0, 50/50 expert split, rdma transport
```

`window=4 chunk=2097152` is exactly the message shape the verbs benchmarks use,
so this exercises the same data path, not a fallback.

## Bring-up ordering is a real trap

The coordinator's listener accepts **one** worker. Restarting only the worker
after a successful bind leaves the coordinator holding a dead peer: the new
worker gets `Connection refused` forever and every request fails with
`tp: prefill synchronization failed` — while both units report `active`. The
first inference attempt hit exactly this. Correct order is stop worker, stop
server, start server, wait for `waiting for worker`, start worker once, wait
for `worker connected`.

Grepping the journal for `worker connected` without scoping to the current
`MainPID` produces a false positive from the previous instance's lines; scope
the match to the live pid. `StartLimitIntervalSec=600 StartLimitBurst=2` also
means a restart loop wedges the unit until `systemctl reset-failed`.

## Results — gate = 200 tokens, temperature 0

Same session, same build, same model, same hosts, same LAN control channel.
Only `--transport` differs (`rdma --rdma-device strix_nhi0 --rdma-gid-index 0`
vs `tcp`).

| arm | run 1 | run 2 | run 3 | median | sha256(output)[:16] |
|---|---|---|---|---|---|
| verbs (rdma/strix_nhi0) | 10.99 | 11.56 | 11.52 | **11.52 t/s** | `d3d7c7514465a3e2` |
| tcp (control) | 14.07 | 14.89 | 14.90 | **14.89 t/s** | `d3d7c7514465a3e2` |

Plus two further verbs runs at `idle_spin_us=0` (10.91/11.52/11.45) and
`idle_spin_us=32` (11.57/11.51/11.57) — all the same sha.

**Correctness: pass, and strongly.** Nine requests across two transports and
three poller settings produced byte-identical output (`len=1025`,
sha `d3d7c7514465a3e2`). `bad_frames 0` on both hosts throughout. The TCP arm's
14.89 t/s also matches the documented v3-TCP reference of 14.29 t/s
(`bench/results/2026-08-24-gate7-imported-pool-live-inference.md`), so this is
not build drift since August — the verbs path is genuinely slower.

**Performance: fail.** 11.52 vs 14.89 t/s is **-22.6%**. Per token the verbs
arm costs `200 x (1/11.52 - 1/14.89)` = **+19.6 ms**.

## Where the 19.6 ms/token does NOT come from

Instrumented one gate request with full `native_stats` deltas on both hosts:

| counter | delta | per token |
|---|---|---|
| `tx_frames` / `rx_frames` | 1,153,528 each | 5,768 |
| `placed_send_bytes` | 403,046,400 (384 MiB) | 1.92 MiB |
| `zc_bursts` | 80 | 0.4 |
| `poll_passes` | 1,522,199 | 7,611 |
| `poll_sleeps` / `poll_wake_tick` | 1,521,825 each | 7,609 |
| `poll_wake_kick` | 141 | 0.7 |
| `zc_retries` / `zc_aborts` / `bad_frames` | 0 / 0 / 0 | 0 |
| `sched_calls` / `rnr_sent` | 2,040 / 14 | 10.2 / 0.07 |

1. **Not link bandwidth.** 384 MiB in 17.3 s is 22 MiB/s — **1.8% of the
   1211 MiB/s** this same link sustains in `bench`. Wire time is ~1.6 ms/token,
   so ~18 ms/token is overhead, not transfer.
2. **Not retransmits or corruption.** `zc_retries`, `zc_aborts` and
   `bad_frames` were all zero for the request. No RNR storm (`rnr_sent` 14).
3. **Not poller sleep latency.** 1.52M wakes in 17.3 s is ~88k/s, far above the
   1000/s the 1 ms maintenance tick could produce, so the wakes are prompt and
   `poll_wake_tick` merely misattributes them (only `sn_schedule` sets
   `poll_kick`; the ring-callback `wake_up` at main.c:416 does not, so every
   prompt wake is counted as a "tick"). The counter is misleading, not the
   scheduler.
4. **Not the cold spin.** `idle_spin_us` is runtime-writable; 0, 32 and 128 all
   land inside noise (10.91–11.57 t/s). The spin is real — the
   `strix_nhi/1-2.0` kthread burned **3840 ms of CPU during a 6684 ms request
   (57% of a core)**, nearly all of it `udelay(1)` — but removing it changes
   nothing at gate granularity. It is still worth fixing for CPU efficiency,
   just not the cause of the regression.

## Leading hypothesis (untested)

`SN_DATA_THROTTLE_NS 128000`, the data-ring interrupt moderation that fixed the
per-frame interrupt storm and took bulk throughput from bimodal 18–1313 MiB/s
to a consistent 1182–1238 MiB/s. DS4's decode moves data in small batches, so
each batch can wait up to one 128 us window for its interrupt. The request's
5,768 frames/token at ~40 frames per window is ~145 windows/token, and
145 x 128 us = **18.6 ms/token** — the size of the gap almost exactly.

That would make this a straightforward bulk-vs-latency trade: the moderation is
right for a streaming bench and wrong for a latency-bound TP gate. The fix is
not to remove it (that reintroduces the storm) but to make it adaptive — e.g.
moderate only while the ring is actually backlogged, and leave it off when a
pass drains the ring empty, so a lone batch is delivered immediately.

A `data_throttle_ns` module parameter was added (main.c / ring.c / strix_nhi.h,
**uncommitted**) to A/B the window without a rebuild per value.

## Blocker on testing that hypothesis

The module cannot be built against stock kernel headers. `main.c:163` reads
`tb_service_parent(d->service)->native_dma_stopping`, a field added by
`0001-thunderbolt-native-path-lifetime.patch`; an out-of-tree build against
`/usr/src/kernels/7.2.8-100.fc43.x86_64` fails with
`compiletime_assert_atomic_type` on that `smp_load_acquire`. The hosts run a
patched `thunderbolt.ko` from `updates/`, and `native_dma_stopping` is absent
from their stock headers — but the patched build tree is no longer present
(only `~/linux-stable-handedits-backup-20260831.patch`). **So the param change
has no compile verification and is deliberately not committed.** Restoring a
patched tree is the prerequisite for any further kernel-side tuning.

## Verdict

Do not make verbs the default yet. It is correct (bit-exact, zero bad frames,
real 2 MiB zero-copy bursts) and it holds a stable link under a real model
server, but at the acceptance gate it is 22.6% behind plain TCP over a *slower*
5 GbE LAN — while using 1.8% of the TB link's measured capacity. The transport
is leaving its advantage on the table in the latency regime DS4 actually runs
in. Next step is the moderation knee, which needs the patched build tree back.
