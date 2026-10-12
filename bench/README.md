# Benchmarks

How to measure this transport, and what the paths measured on the lab host pair
(two Strix Halo systems, direct USB4/Thunderbolt cable). There is no committed
results tree: every figure below states when it was measured, and all of them
need re-measuring on your own hosts before you rely on them.

The matrix in [docs/PLAN.md](../docs/PLAN.md) compares the paths with identical
message shapes:

| Transport | Network stack | User/kernel payload copies | Role |
|---|---:|---:|---|
| TCP over `thunderbolt-net` | Yes | Yes | Ubiquitous baseline |
| Stock USB4STREAM (`/dev/tbstreamX`) | No | Yes | Low-risk intermediate baseline |
| USB4STREAM zero-copy pools | No | No | Mature high-throughput path |
| Native verbs, CPU-copy staging | No | Yes | Compatibility path for unmodified RDMA apps |
| Native verbs, zero-copy data plane | No | No | Current fastest measured path |

Measure for each: one-way and round-trip latency at the message sizes that
matter; p50/p95/p99/max, not just bandwidth; QD1 and pipelined; CPU
utilization, context switches, interrupts, and bytes copied; and a run long
enough to expose drift and interrupt storms. Every defect recorded in
[docs/ZEROCOPY_VERBS.md](../docs/ZEROCOPY_VERBS.md) was invisible in short
runs, and the verbs data plane was bimodal (18–1313 MiB/s) until interrupt
moderation landed. Do not accept a single run on this link.

## Step 1 — TCP baseline

Over the `thunderbolt-net` interface:

- Raw link: `iperf3 -c <peer>` and `iperf3 -c <peer> -l <msgsize>`; latency
  with `ping` for the floor.
- Application level: run the real workload's transport at its own message
  sizes and record its throughput and per-message cost.
- CPU/copies: `pidstat -w -u`, `/proc/interrupts` deltas, `perf stat`.

Measured 2026-09-18 on the lab pair: iperf3 (MTU 65520) reaches
**9.49–9.51 Gbit/s one-way** and 9.41 + 9.40 Gbit/s full duplex at the default
`thunderbolt.dma_credits=14`. The credit sweep is the interesting part: 1 gives
3333 Mbit/s, 4 already gives 9489, and 8/14/64 are indistinguishable — so the
XDomain DMA-tunnel credit window is *not* the limiter above 4, and the nominal
20 Gb/s per-direction link rate is not reachable by any path measured so far.

## Step 2 — Stock USB4STREAM

After the backport (see [../kernel/README.md](../kernel/README.md)) on both
hosts:

```sh
hostA$ sudo ../tools/scripts/tbstream-setup.sh link   # prints /dev/tbstreamX
hostB$ sudo ../tools/scripts/tbstream-setup.sh link

hostB$ ../tools/pingpong/pingpong pong -d /dev/tbstream0 -s 64k
hostA$ ./scripts/run-pingpong.sh /dev/tbstream0 stock  # or PEER=user@hostB ...
```

`run-pingpong.sh` sweeps message sizes and writes a CSV; `OUT_DIR` selects the
destination directory (default: the current directory). Sweep `RING_SIZE`
(256 vs 4096) and `THROTTLING` via environment — both matter at QD1.

Measured 2026-08-02 on the lab pair: 4 KiB QD1 round trip **22.9 µs p50**
against a 64 µs average ICMP floor over the same link (~2.8×), and bulk
~1.15 GB/s, i.e. TCP parity. Ring 4096 / throttling 0 tuning was near-noise,
which localized the cost to per-4 KiB-frame copy and processing
(~4.4 µs/frame at 64 KiB) — exactly what the zero-copy UAPI removes.

## Step 3 — Zero-copy USB4STREAM

After applying `kernel/zerocopy/*.patch` on top of the backport, use
`zping`/`zpong` for whole-message RTT and `ztx`/`zrx` for one-way throughput:

```sh
hostB$ ../tools/pingpong/pingpong zrx -d /dev/tbstream0 -s 1m -V
hostA$ ../tools/pingpong/pingpong ztx -d /dev/tbstream0 -s 1m -n 1000 -V
```

`ztx` waits for all TX completions before reporting, so its throughput includes
the complete DMA transfer rather than only ioctl submission time. `zrx` checks
each message's byte/frame geometry and optionally verifies the payload pattern.

Measured 2026-08-04 on the lab pair, one-way verified sender rates:
**100.5 MB/s at 4 KiB, 512.3 MB/s at 64 KiB, 1052.1 MB/s at 1 MiB**. A
512 × 1 MiB run reached 955.9 MB/s with 24 aggregate Thunderbolt interrupts
per host. Measured 2026-09-18: 1078–1117 MB/s one-way at 1 MiB, and
1146 MB/s at 2 MiB (2026-10-06).

## Step 4 — ROCm mapping gate

Build and run `../tools/rocm-map/tbstream-hip-map` on each Strix Halo host. It
registers only the TX pool, then verifies GPU-to-pool and pool-to-GPU access.
This gate passed for all 16 MiB on both hosts on 2026-08-04. It clears
implementation work and shows that a DMA-BUF fallback is not required for
CPU-visible GPU access; it is not a production-readiness result for any
transport path.

`../tools/rocm-map/tbstream-hip-zc` goes further and hands GPU-written pages
to the NHI, verifying the bytes on the peer. The two-host imported-pool
harnesses used for the ring contract live in `../tools/stale-cache/`.

## Step 5 — Native verbs

The verbs path has its own bring-up (`../tools/scripts/strix-verbs-bringup.sh`)
and its own smoke tool, which is also the latency/throughput probe:

```sh
make -C ../tools/verbs
# usage: strix-verbs-smoke --live strix_nhiN listen|connect <explicit-IPv4> <TCP-port>
#        [bench-iterations | ping [iters] | send [iters] | write [iters] | read [iters]]
./strix-verbs-smoke --live strix_nhi0 listen  10.99.0.1 18515   # endpoint A
./strix-verbs-smoke --live strix_nhi0 connect 10.99.0.1 18515   # endpoint B
# ... appending "ping 2000" for the 256 B RTT probe, "send 200" for verified
# window-4 x 2 MiB bidirectional SEND/RECV (every byte checked), or a bare
# iteration count (e.g. "200") for the same traffic as an unverified throughput run.
```

From a third machine with ssh access to both hosts, two scripts drive the same
tool. Each needs `CONNECT=user@hostA LISTEN=user@hostB LISTEN_IP=<hostB IPv4>`,
and optionally `PORT` (default 19476, which must be open in hostB's firewall),
`DEV` and `TOOL_DIR`:

```sh
scripts/run-verbs-smoke.sh read 200        # one exchange, both sides' output
scripts/verbs-streak.sh 1 20 read 2000     # stop at the first failure
scripts/verbs-streak.sh 1 5 write 2000
scripts/verbs-streak.sh 1 5 send 2000      # verified bulk SEND/RECV
scripts/verbs-streak.sh 1 5 send           # the SEND/RECV smoke
```

`verbs-streak.sh` passes a run only if both sides print PASS with the exact
expected byte count. Under `OUT_DIR` (default `./verbs-streak`) it keeps each
run's logs, every `native_stats` counter that moved on each host, and the
`stall_snap` snapshots. That is the evidence format of the zero-copy validation
rounds in `docs/ZEROCOPY_VERBS.md`.

It cannot be compared against the stream path on the same link at the same
time: zero-copy verbs mode blacklists `thunderbolt-net` and claims both ring
HopIDs. Run one, tear it down, run the other.

Measured on the lab pair:

| Date | Path | Result |
|---|---|---|
| 2026-10-06 | verbs, CPU-copy staging | 62.9 MiB/s per direction (window 4 × 2 MiB bidirectional) — ~16× below the stream path |
| 2026-10-06 | stream, one-way copy / zero-copy | 1016 MB/s / 1146 MB/s; bidirectional RTT 1086 MB/s effective |
| 2026-10-10 | verbs, zero-copy data plane | **1182–1238 MiB/s per direction**, 8/8 consecutive runs consistent; 1211 MiB/s sustained over 156 GiB per direction |
| 2026-10-10 | verbs, 256 B ping-pong RTT (2000 iters) | **31 µs min / 35.1 µs avg / 345 µs max tail** (was ~52 µs avg with a 6.4 ms tail before the resident poller) |

## Decision gate

If stock USB4STREAM does not materially beat TCP at your message sizes, profile
the staging copies and scheduling before writing a large kernel patch (see
[docs/PLAN.md](../docs/PLAN.md) "Decision gates"). And remember the bound:
application-level gain is limited by the share of the critical path that is
transport, so a large latency win in this matrix can still be a rounding error
end to end.
