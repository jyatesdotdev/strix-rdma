# Benchmarks

The plan's benchmark matrix (docs/PLAN.md) compares three transports with
identical tensors and model settings:

| Transport | Network stack | User/kernel payload copies | Role |
|---|---:|---:|---|
| TCP over `thunderbolt-net` | Yes | Yes | Production baseline |
| Stock USB4STREAM (`/dev/tbstreamX`) | No | Yes | Low-risk intermediate baseline |
| NHI stream | No | CPU-copy or mapped, by mode | Qualified experimental candidate |

Measure for each: one-way and round-trip latency at real hidden-state and
logit sizes; p50/p95/p99/max (not just bandwidth); QD1 and pipelined; batch
1/2/4/8; prefill and decode separately; CPU util, context switches,
interrupts, bytes copied; end-to-end tokens/s.

The current release outcome is descriptor-framed protocol-v3 TCP. Select it
with `--dist-transport auto` and do not supply `--dist-nhi-device`; explicit
`--dist-transport tcp` selects legacy v2. Supplying an NHI device is reserved
for explicit single-link qualification while longer soak and active peer-reboot
testing continue. Zero-copy patches 11 and 12 repair the reproduced asymmetric
delivery and reopen failures; the initial post-fix gate passes, but the
controlled full-model cohort shows no NHI tokens/second gain.

## Step 1 — TCP baseline

On the current 7.1.5 kernels, over the `thunderbolt-net` interface:

- Raw link: `iperf3 -c <peer>` and `iperf3 -c <peer> -l <msgsize>`; latency
  with `./pingpong` over a TCP socat relay or plain `ping -f` for the floor.
- DS4 level: run the existing distributed setup (`ds4-bench` in
  `~/Repositories/ds4`) at batch 1/2/4/8, record tokens/s and time/token.
- CPU/copies: `pidstat -w -u`, `/proc/interrupts` deltas, `perf stat`.

Record results in `results/` as CSV plus a notes file per run
(kernel version, cable, ring settings, governor, batch size).

## Step 2 — Stock USB4STREAM

After the backport (see ../kernel/README.md) on both hosts:

```
hostA$ sudo ../tools/scripts/tbstream-setup.sh ds4     # prints /dev/tbstreamX
hostB$ sudo ../tools/scripts/tbstream-setup.sh ds4

hostB$ ../tools/pingpong/pingpong pong -d /dev/tbstream0 -s 64k
hostA$ ./scripts/run-pingpong.sh /dev/tbstream0 stock  # or PEER=user@hostB ...
```

Sweep `ring_size` (256 vs 4096) and `throttling` (8192 default, then lower)
via env vars to `tbstream-setup.sh` — both matter at QD1.

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

## Step 4 — ROCm mapping gate

Build and run `../tools/rocm-map/tbstream-hip-map` on each Strix Halo host. It
registers only the TX pool, then verifies GPU-to-pool and pool-to-GPU access.
This gate passed for all 16 MiB on both hosts on 2026-08-04; the result is in
`results/2026-08-04-usb4stream-zc-oneway-rocm.md`. It cleared implementation
work and showed that a DMA-BUF fallback is not currently required; it is not a
production-readiness result for the NHI transport.

The post-fix hardware and full-model evidence is in
`results/2026-08-05-nhi-msix-rx-prime-fix.md`; per-run data is in
`results/2026-08-05-ds4-full-model-tps.csv`. The earlier failure baseline and
TCP release decision remain in
`results/2026-08-05-ds4-nhi-reliability-and-tcp-release.md`.
The mapped direct-slot implementation, matched fast-path A/B, and distributed
decode hot-path ledger are in
`results/2026-08-05-ds4-nhi-direct-slot-profile.md`.
The low-overhead in-process ROCm event ledger, full-model GPU operation split,
profiler overhead A/B, and mapped-output penalty are in
`results/2026-08-06-ds4-rocm-event-profile.md`; its raw A/B rows are in the
same-named CSV. Parse saved profiler journals with
`scripts/analyze-ds4-rocm-events.py`; it emits weighted human-readable or JSON
summaries and validates segment, ratio-group, direct-mask, and drop accounting.
The first compute optimization pass, including the negative routed-MoE
rows-per-wave sweep and the bit-exact Q/KV RMSNorm plus KV-RoPE fusion, is in
`results/2026-08-06-ds4-rocm-qkv-fusion.md`; its balanced full-model arm and
host-role aggregates are in the same-named CSV. The fusion improves measured
internal decode throughput by 0.194% and combined GPU stream time by 0.218%.

## Decision gate

If stock USB4STREAM does not materially beat TCP at DS4 message sizes,
profile GPU staging copies and scheduling before writing the zero-copy
kernel patch (see docs/PLAN.md "Decision gates").
