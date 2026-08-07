# Five-patch zero-copy deployment, one-way throughput, and ROCm gate — 2026-08-04

> This is a historical five-patch measurement. Patches 6 and 7 were deployed on
> 2026-08-05, and a later exact asymmetric workload exposed a delivery/reopen
> failure not covered by this one-way gate. See
> [2026-08-05-ds4-nhi-reliability-and-tcp-release.md](2026-08-05-ds4-nhi-reliability-and-tcp-release.md).

Hosts: `max` and `max2`, Fedora kernel `7.1.5-101.fc43.x86_64`, direct
Thunderbolt/USB4 link. Both hosts used ring size 4096 and throttling 0. The
deployed source was the eight-patch v7.1 USB4STREAM backport followed by all
five patches in `kernel/zerocopy/`.

## Correctness and boundary regression

- Patch 4 transferred and verified 400 x 64 KiB messages, but the receiver
  waited indefinitely for CLOSE after the sender completed. CLOSE had landed
  on the suppressed first descriptor of the next repost group.
- Patch 5 keeps interrupts on both the first and last descriptor of each
  fixed-size message. Repeating the same ring-wrap test delivered exactly 400
  messages / 26,214,400 bytes and both processes exited normally. The sender
  reported 119.6 MB/s with full payload verification.
- While one process held zero-copy mode open, a second open failed with
  `EBUSY`; both devices reopened normally after the holder exited.

## One-way throughput

`ztx` drains all TX completions before reporting, so its value is the
complete-transfer rate. `zrx` starts timing at the first received message and
can report a higher value because its interval excludes startup.

| Message size | Count | `ztx` | `zrx` |
|---:|---:|---:|---:|
| 4 KiB | 5000 | 100.5 MB/s | 838.3 MB/s |
| 64 KiB | 2000 | 512.3 MB/s | 1117.6 MB/s |
| 1 MiB | 1000 | 1052.1 MB/s | 1284.8 MB/s |

A final targeted 512 x 1 MiB pass reported 955.9 MB/s at `ztx`, 1237.0 MB/s
at `zrx`, and only 24 aggregate Thunderbolt interrupt-count increments on
each host. This rules out the earlier per-4-KiB-frame RX interrupt
amplification for this workload.

## ROCm mapped-pool gate

`tools/rocm-map/tbstream-hip-map` was built with the hosts' TheRock ROCm
toolchain and run independently on both systems. Each reported:

```
PASS: GPU 0 (Radeon 8060S Graphics) mapped and verified 16777216/16777216 TX-pool bytes
```

The test covers GPU write / CPU validation and CPU write / GPU validation over
the entire 16 MiB TX pool. The `hipHostRegister()` path is therefore sufficient
for the current DS4 integration gate; a DMA-BUF fallback is not required.

### Isolation caveat

Both hosts were later confirmed to have booted with `amd_iommu=off`. These
results prove functional GPU/NHI access, but they do not prove the DMA
isolation required for production use and no IOMMU fault test was possible.
Re-enable the IOMMU on both hosts, reboot in a coordinated maintenance window,
and repeat the mapping and transfer gates before treating this as a production
readiness result.

## Deployment verification

- `thunderbolt.ko`, `thunderbolt_stream.ko`, and `thunderbolt_net.ko` load
  from `/lib/modules/7.1.5-101.fc43.x86_64/updates/` on both hosts.
- All three report exact running-kernel vermagic and are present in both
  rebuilt initramfs images.
- `/dev/tbstream0` exists on both systems. Thunderbolt IP ping completed with
  0% loss (0.198 ms average for the final three-packet health check).
- Secure Boot is disabled. The expected unsigned out-of-tree module taint is
  present; no BUG, oops, protection fault, or panic was observed.
