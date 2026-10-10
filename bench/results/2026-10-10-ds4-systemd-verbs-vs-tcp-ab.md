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

## Tested and disproved: interrupt moderation

The leading suspect was `SN_DATA_THROTTLE_NS 128000`, the data-ring interrupt
moderation that fixed the per-frame interrupt storm and took bulk throughput
from bimodal 18–1313 MiB/s to a consistent 1182–1238 MiB/s. The arithmetic fit
suspiciously well: 5,768 frames/token at ~40 frames per 128 us window is ~145
windows/token, and 145 x 128 us = 18.6 ms/token — the size of the gap.

It is wrong. `data_throttle_ns` was made a module parameter and the gate re-run
with moderation **fully disabled**:

| `data_throttle_ns` | run 1 | run 2 | run 3 | median | sha |
|---|---|---|---|---|---|
| 128000 (default) | 10.99 | 11.56 | 11.52 | **11.52 t/s** | `d3d7c7514465a3e2` |
| 0 (moderation off) | 10.77 | 11.44 | 11.55 | **11.44 t/s** | `d3d7c7514465a3e2` |

No difference. Moderation is not the cause, and an adaptive-ITR scheme would
have bought nothing — worth recording so it is not built.

The parameter stays: it is now the only way to change the moderation window,
it is verified end to end (module loads, both ports ACTIVE, DS4 bit-exact at
both settings), and `/etc/modprobe.d/strix-nhi-throttle.conf` pins the
bulk-safe 128000 as the boot default. It is read-only at runtime because
`tb_ring_throttling()` requires a stopped ring, and the value lands in a
hardware per-vector register — `nhi.c:125` does
`itr = DIV_ROUND_UP(ring->interval_nsec, 256)` into
`REG_INT_THROTTLING_RATE + ring->vector * 4`, written only at ring interrupt
enable/disable. Live adaptation would need a core patch plus a `thunderbolt.ko`
reload, which is the most wedge-prone operation available; not worth it for a
dial that measurably does not move this workload.

## Current leading hypothesis

With bandwidth, retransmits, poller sleep, cold spin and interrupt moderation
all excluded, what remains is **per-frame cost in the provider's completion
path**. The numbers say the workload is dominated by small frames: 1,153,528
frames per request carrying 384 MiB is **~350 bytes per frame**, at only
66.7k frames/s per direction — versus the ~310k full 4 KiB frames/s this link
sustains in bench. So DS4 is at ~21% of the provider's frame-rate capability
while using 1.8% of its bandwidth.

19.6 ms/token over 5,768 frames/token is **~3.4 us per frame**, and the likely
sink is `d->lock`: the poller took it 1.52M times in 17.3 s (~88k/s, one pass
per ~1.3 frames) while DS4's own `poll_cq` calls need the same mutex. At that
acquisition rate an average wait of a few microseconds is exactly the right
order. This also explains why `idle_spin_us` changed nothing — pass count is
driven by frame count, not by the spin, so the lock churn is identical at 0.

The next measurement should be provider-side lock hold/wait time and
`poll_cq` cost per call, not another transport-level sweep.

## Build recipe (the earlier blocker was a false alarm)

The patched tree was present all along at `~/src/linux-stable` on both hosts —
Linux 7.2.8, clean, at `6c7bcbd24` ("RDMA: add opt-in experimental native NHI
software verbs"), with `native_dma_stopping` at `include/linux/thunderbolt.h:292`.
The earlier "not in stock headers" finding was my own `head -5` truncating the
grep.

The top-level `Module.symvers` is stale, so an `M=` build fails modpost with
`tb_xdomain_try_enable_native_paths` undefined. The patched core's symbols live
in the subdirectory symvers:

```sh
cd ~/src/linux-stable
make M=drivers/infiniband/hw/strix_nhi \
     KBUILD_EXTRA_SYMBOLS=$PWD/drivers/thunderbolt/Module.symvers modules
sudo cp drivers/infiniband/hw/strix_nhi/strix_nhi.ko /lib/modules/$(uname -r)/updates/
sudo depmod -a
```

BTF generation is skipped (no vmlinux); harmless here.

## Re-probe risk is real but survived twice

`tools/scripts/strix-verbs-bringup.sh` warns to load `strix_nhi` **exactly once
per boot**, because re-probing after heavy bursts has wedged the router's DMA
path config space (`DMA RX path activation failed: -110`), survivable only by
power cycle. This session re-probed both hosts twice — once after DS4 had
pushed ~3.5 GiB — and both came back ACTIVE in ~3 s with a bit-exact gate
afterwards. So the wedge is load-dependent, not guaranteed, but it is still a
gamble: **do not build a tuning loop on module reloads.** Set the parameter via
`modprobe.d` and take one boot per value.

## Verdict

Do not make verbs the default yet. It is correct (bit-exact across twelve
requests, two transports, three poller settings and two moderation settings;
zero bad frames; real 2 MiB zero-copy bursts) and it holds a stable link under
a real model server, but at the acceptance gate it is 22.6% behind plain TCP
over a *slower* 5 GbE LAN — while using 1.8% of the TB link's measured capacity
and 21% of the provider's own frame-rate capability. The transport is not
bandwidth- or interrupt-limited here; the cost is per-frame, most likely `d->lock`
contention between the resident poller and DS4's `poll_cq`.
