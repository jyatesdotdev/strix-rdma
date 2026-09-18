# Link rate ceiling and DMA-tunnel credits — 2026-09-18

Hosts: `max` (10.99.0.1) ↔ `max2` (10.99.0.2), direct USB4 link, kernel
7.1.5-101.fc43 with the deployed module set. Production DS4 services stopped
for the window; link and stream endpoints restored afterwards.

## Question

Every path — `thunderbolt-net` TCP and the NHI/USB4STREAM zero-copy path —
saturates near 1.15-1.19 GB/s per direction while the link reports Gen3/DUAL.
Is the XDomain DMA-tunnel hop-credit window (`thunderbolt.dma_credits`,
default 14) the limiter, and can raising it reach the wire rate?

## Link state

`/sys/bus/thunderbolt/devices/1-2/{rx,tx}_speed = 20.0 Gb/s`,
`{rx,tx}_lanes = 2` before and after the reloads; `tb.c` budgets
`speed × width × 1000` = 40 Gb/s aggregate, i.e. ~20 Gb/s per direction
nominal.

## Baseline (default credits = 14)

| path | one-way | full-duplex |
|---|--:|--:|
| iperf3 (thunderbolt-net TCP, MTU 65520) | 9.49-9.51 Gbit/s | 9.41 + 9.40 Gbit/s |
| tbstream zero-copy (`pingpong ztx/zrx`, 1 MiB) | 1078-1117 MB/s | — |

## Credit sweep

`dma_credits` is read-only at runtime, so each point is a full module reload:
`ds4-tbstream-cleanup.sh`, `rmmod thunderbolt_stream thunderbolt_net
typec_thunderbolt ucsi_acpi typec_ucsi typec thunderbolt`, override in
`/etc/modprobe.d/`, `modprobe` back, then measure.

| dma_credits | TCP one-way |
|--:|--:|
| 1 | 3333 Mbit/s |
| 4 | 9489 Mbit/s |
| 8 | 9437 Mbit/s |
| 14 (default) | 9514 Mbit/s |
| 64 | 9514 Mbit/s |

## Conclusion

- The credit window is real and observable: 1 credit pins throughput to
  ~0.42 GB/s, consistent with a 4 KiB hop window over a ~9.8 µs credit-return
  latency.
- It **saturates at ~4 credits**. The default 14 is already well past the
  knee, so credits are not the limiter and no override can raise the rate.
- ~9.5 Gbit/s per direction (~19 Gbit/s / ~2.4 GB/s aggregate) is therefore the
  practical payload ceiling of this link as seen by the host — the same
  ballpark as real-world Thunderbolt 3/USB4 40G PCIe-tunnel throughput. The
  nominal 40 Gbit/s is a PHY figure; per-direction usable payload is ~10 Gbit/s.
- Both transports (tbnet and NHI) hit the same ceiling, so this is a link/
  controller property, not a transport implementation limit. Getting more
  bandwidth needs different hardware (100GbE NICs, USB4 v2/TB5 class), not a
  software change.

## Consequence for DS4 TP

TP prefill gates move `rows × 20480` bytes each way; at the measured ceiling a
2048-row gate (41.9 MB) costs ~35 ms per direction regardless of transport.
NHI cannot speed that up; only overlapping the exchange with compute can hide
it. Decode-side small-message gains (GPU-polled NHI RTT ~10-45 µs vs the TCP
floor) are unaffected by this finding.