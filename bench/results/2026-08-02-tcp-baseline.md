# TCP-over-thunderbolt-net baseline — 2026-08-02

Hosts: max (10.99.0.1) ↔ max2 (10.99.0.2), direct USB4 cable,
`thunderbolt0` MTU 65520, kernel 7.1.5-101.fc43.x86_64 both sides.
max was already running the backported thunderbolt stack (functionally
identical data path for TCP); max2 stock pending MOK enrollment.

## Latency (ICMP, 1000 packets @ 2 ms interval, max → max2)

| min | avg | max | mdev |
|--:|--:|--:|--:|
| 37 µs | 64 µs | 197 µs | 12 µs |

Small-packet RTT floor for the TCP path. QD1 pingpong over the NHI stream
needs to come in visibly under this to justify the transport.

## Throughput (iperf3, 5 s, 1 s omit, max → max2)

| block size | throughput | retransmits |
|--:|--:|--:|
| 128 KiB (default) | 9.27 Gbit/s (≈1.16 GB/s) | 14 |
| 64 KiB | 9.31 Gbit/s (≈1.16 GB/s) | 2 |

Link is nominally USB4 40 Gbit/s; TCP path delivers ~9.3 Gbit/s, i.e.
roughly a quarter of the wire rate — consistent with the copy/protocol
overhead the stream transport is meant to remove. Stock USB4STREAM
pingpong numbers go next to these once max2 is unblocked.
