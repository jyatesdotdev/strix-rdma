# Stock USB4STREAM vs TCP — 2026-08-02

First `/dev/tbstream0` numbers between max ↔ max2, backported driver on
both (7.1.5-101.fc43 + kernel/backport/v7.1 series), stream `ds4` on
service 1-2.1, payloads verified (`-V`) in smoke run. Raw CSVs alongside
this file (`usb4stream-stock.csv`, `usb4stream-tuned.csv`).

## QD1 round-trip latency (pingpong, p50 µs)

| msg size | TCP (ICMP floor) | stock (ring 256, thr 8192) | tuned (ring 4096, thr 0) |
|--:|--:|--:|--:|
| 4 KiB  | 64 (avg) | **22.9** | 24.0 |
| 16 KiB | — | 56.2 | **47.4** |
| 64 KiB | — | 147.0 | **141.0** |
| 256 KiB | — | 520.6 | **511.7** |
| 1 MiB  | — | 1908.0 | **1884.0** |
| 4 MiB  | — | 7259.7 | **7108.7** |

## Effective throughput (half-duplex pingpong wire rate / one-way stream)

| metric | TCP (iperf3) | USB4STREAM |
|--|--:|--:|
| bulk one-way | 9.3 Gbit/s ≈ 1160 MB/s | 1152 MB/s (rx-clocked, 1 MiB msgs) |
| 4 MiB pingpong wire rate | — | 1180 MB/s |

## Read

- **Latency win is real: ~2.8× at 4 KiB** (22.9 µs RTT vs 64 µs ICMP floor —
  and real TCP request/response would be worse than the ICMP floor).
- **Bandwidth is wire-parity with TCP**, no better: both saturate ~1.15 GB/s,
  around a quarter of nominal USB4 40 Gbit/s.
- **Tuning ring/throttling is nearly a no-op** → the cost is per-4KiB-frame
  copying and processing, not interrupt moderation. 64 KiB = 16 frames each
  way at ~4.4 µs/frame. This is exactly the overhead the zero-copy UAPI +
  per-message completion design targets (plan step 3).
- Decision gate 1 (plan): stock USB4STREAM **does** materially beat TCP at
  DS4 message sizes on latency → proceed with the zero-copy work.

## Gotchas hit

- A leftover `pingpong rx` can survive if it opens the device while the
  previous `pong` still holds it (both consume frames; CLOSE gets stolen).
  Kill stragglers before reconfiguring: `pkill -f pingpong`.
- `ring_size`/`throttling` return EBUSY while any fd is open on the device.
- On max2's first boot after install, the initramfs-era module load pulled
  stock `thunderbolt.ko` and broke `thunderbolt_net`/`thunderbolt_stream`
  loading (missing new symbols); fixed by live reload + `dracut -f`.
  Watch the next reboot on both hosts.
