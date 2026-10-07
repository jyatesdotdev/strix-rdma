# Native verbs vs USB4STREAM throughput — 2026-10-06

A/B measurement on the lab nodes (max / max2, Strix Halo pair, Fedora 43 /
7.2.8, Thunderbolt 5 link). DS4's data shape: window 4 x 2 MiB chunks, both
directions. DS4 services were stopped for the measurement.

## Results

| Path | Mode | Throughput (per direction) | Notes |
|---|---|---|---|
| **native verbs** (`strix_nhi`) | bidirectional, window 4 x 2 MiB | **62.9 MiB/s (0.53 Gbps)** | 100 iters, 12.72 s wall |
| **USB4STREAM** (`thunderbolt_stream`) | one-way, copy | **1016 MB/s (~8.5 Gbps)** | pingpong tx/rx, 2 MiB |
| **USB4STREAM** | one-way, zero-copy | **1146 MB/s (~9.6 Gbps)** | pingpong ztx/zrx, 2 MiB |
| **USB4STREAM** | bidirectional RTT, 2 MiB | **1086 MB/s effective** | ping/pong, RTT p50 3837 us |

**The stream path is ~16x faster than the verbs path** at DS4's message shape.

## Why the verbs path is slow (measured)

The verbs path is CPU-copy staged with a serialized protocol engine and a
**1 ms worker cadence**. A 2 MiB message = 528 fragments; the engine drains
the RX ring at up to 64 frames per 1 ms pass, so each message has a multi-ms
floor from the cadence alone, and both directions serialize behind it. The
stream path is interrupt/callback driven with no such cadence.

## Decision implication

For DS4's TP data plane the stream path is decisively better (~16x). The verbs
path's value is API compatibility (any RDMA application works unmodified), but
at 0.53 Gbps it is not viable for DS4-class TP throughput without removing the
1 ms worker cadence (the dominant, fixable bottleneck) and possibly the CPU-copy
staging.

## Reproduce

```sh
# verbs (strix_nhi loaded, stream NOT loaded):
cd tools/verbs && make OUTPUT=strix-verbs-smoke
# max:  ./strix-verbs-smoke --live strix_nhi0 listen 10.99.0.1 4789 100
# max2: ./strix-verbs-smoke --live strix_nhi0 connect 10.99.0.1 4789 100

# stream (strix_nhi UNLOADED — ring HopIDs are scarce; thunderbolt_stream loaded):
sudo modprobe thunderbolt_stream   # plus the configfs group
cd tools/pingpong && make
# rx side:  ./pingpong rx  -d /dev/tbstream1 -s 2m -n 100
# tx side:  ./pingpong tx  -d /dev/tbstream1 -s 2m -n 100
# zc:       zrx/ztx;  bidirectional RTT: ping/pong
```

## What's next

1. If the verbs path should be viable for DS4, remove the 1 ms worker cadence
   (interrupt-driven or continuous worker) and re-measure — it is the dominant
   fixable cost.
2. Hardware RDMA READ/WRITE validation (DS4's RoCE path is SEND-only).
3. Hot-unplug and malformed-peer injection.
