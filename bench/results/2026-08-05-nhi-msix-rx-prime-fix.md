# NHI interrupt and reopen fixes — 2026-08-05

Hosts: `max` and `max2`, Fedora kernel `7.1.5-101.fc43.x86_64`, direct
Thunderbolt/USB4 link, USB4STREAM HopIDs 9/9, ring size 4096, interrupt
throttling disabled.

## Result

The two failures isolated by the zero-copy progress diagnostics no longer
reproduce with patches 11 and 12. The patched pair completed 2,880 consecutive
DS4-shaped request/response exchanges across 27 stream sessions, including 26
close/reopen transitions. That is 460,800 wire descriptors and 921,600 matching
TX/RX descriptor completions across the pair. Diagnostic kicks remained
disabled, all NHI model cohorts ran with zero service restarts, and the current
boot contains no NHI, stream, CRC, ring-flush, or DMA-path error.

Required-NHI full-model runs now complete in both CPU-copy and explicitly
verified mapped mode. They do not show a tokens/second improvement over
descriptor-framed TCP v3, so the production units were restored to TCP-v3
selection after the qualification run.

## Failure localization and repairs

Patch 10 first separated descriptor, interrupt, workqueue, FIFO, and ownership
progress. At the original asymmetric stall, the responder's RX descriptor at
the software tail was complete and the hardware completion index was one ahead
of software, while the interrupt count did not advance. A directional RX kick
harvested exactly that completion without changing the interrupt count. That
localized the first defect to completion notification rather than DMA, CRC,
the fabric, or userspace ownership.

Patch 11, `thunderbolt: Flush posted MSI-X interrupt clears`, flushes the
non-auto-clear NHI's W1C interrupt acknowledgment through a read of the
side-effect-free interrupt-enable register. This prevents a delayed posted
clear from erasing the only notification for a completion that arrived after
the worker drained the prior descriptor. Its repository SHA-256 is
`25b1bd9453a12e065700558bd8ac4f74bb96ebdb50ada84afe9c36dacf6e5013`.

A later immediate-reopen failure had a different signature: the initiator had
posted TX descriptors but both hardware completion positions remained zero,
and a workqueue kick made no progress. USB4STREAM enabled its XDomain DMA paths
before starting either ring and before posting RX buffers. Patch 12,
`thunderbolt: stream: Prime Rx ring before enabling DMA paths`, now follows the
ThunderboltIP ordering: start both rings, fully populate RX, then expose the
paths. It also completes partial TX/RX allocation unwinds and clears freed
pointers so a failed start can be retried safely. Its repository SHA-256 is
`61b40c127b5c29d64fc0b87620aaf72716aa872ba46ebefff5537c3f67d4dbff`.

Forcing the controller onto shared MSI was tested only as a diagnostic and
failed NHI probe on both hosts with configuration-space timeouts and RX-ring
overflow. That experiment was reverted and is not part of the release series.

## Build and deployment evidence

- The isolated patch-series and ABI suite passes 38/38 locally and on both
  hosts. Patch 12 passes strict `checkpatch.pl` with zero errors, warnings, or
  checks and received an independent lifetime, locking, and unwind review.
- Both hosts built byte-identical modules with matching
  `7.1.5-101.fc43.x86_64 SMP preempt mod_unload` vermagic:
  - `thunderbolt.ko`:
    `dfc5ebc1ee35719436afcbbb87309a2fbfa98a152f906ab980ae030c8122cd1f`
  - `thunderbolt_stream.ko`:
    `ccd946330a5612ffe7674246ac387b39d4a2e71562ed585cfc65d8d58e2650e7`
  - `thunderbolt_net.ko`:
    `c642d1fc467de1df585913511338a5c30db912239c484a49f70e87d4315f03bd`
- Those exact files were installed, embedded in both rebuilt initramfs images,
  extracted back out for hash verification, and booted. The post-deployment
  boot IDs are `7fc510d7-913d-4fdb-8ca7-a11bdbc68340` (`max`) and
  `fdd86be1-750b-42ea-8eb6-b87ce3545749` (`max2`).
- Both lifecycle timers converged with `/run/ds4-tbstream/device` pointing to
  `/dev/tbstream0`, carrier up, HopIDs 9/9, `0660 root:tbstream`, ring size
  4096, and throttling zero.

## Hardware qualification

All raw sessions used the exact repeating 17/17/33/65-frame request and
127-frame response geometry, validated every byte and ownership cursor, and
ran with `zc_diagnostic_kick=N`:

| Phase | Sessions | Exchanges | Pair descriptor completions | Result |
|---|---:|---:|---:|---:|
| Initial delayed soak | 1 | 256 | 81,920 | PASS |
| Immediate reopen loop | 20 | 1,280 | 409,600 | PASS |
| Long delayed soak | 1 | 1,024 | 327,680 | PASS |
| Post-soak reopen loop | 5 | 320 | 102,400 | PASS |
| **Total** | **27** | **2,880** | **921,600** | **PASS** |

The 1,024-exchange initiator completed in 38.452 seconds. No session needed a
kick, timeout recovery, replay, or endpoint reset. The long-run logs are on the
hosts at `/tmp/strix-rxprime-soak1024-{initiator,responder}.log`.

## Full-model result

All rows used the 155,976,458,848-byte model with SHA-256
`0e3a161b670f686128ec5f92a601dfde616a37bf5e7e48999fa2d32471b57ec6`,
coordinator binary `ea6894e56c...df751`, and worker binary
`7623dd3825...fe0b`. Required NHI mode prevented TCP fallback. CPU-copy NHI
completed an excluded warm-up, a ten-request mixed-shape run, and ten measured
fixed-shape requests. Mapped NHI completed an excluded warm-up and ten measured
fixed-shape requests; a traced startup on both hosts recorded
`mapped_requested=1 mapped=1`. Every repeated-seed pair matched and neither
service restarted.

The controlled cohorts each contain five two-request runs and exactly 800
completion tokens:

| Transport | Wall time | Token-weighted rate | Change vs TCP v3 |
|---|---:|---:|---:|
| TCP v3 | 73.033 s | 10.953952 tok/s | baseline |
| Required NHI, CPU copy | 73.112 s | 10.942116 tok/s | -0.1081% |
| Required NHI, mapped | 73.101 s | 10.943763 tok/s | -0.0930% |

The differences are noise-scale: there is no measured full-model throughput
increase. Raw JSON remains on `max2` under
`/home/jryates/ds4-maint-20260805/release-nhi-rxprime-20260805/`, and the
curated rows are in `2026-08-05-ds4-full-model-tps.csv`.

## Release stance

The exact lost-notification and fresh-open failures are repaired by the current
candidate, and required-NHI full-model correctness now passes. Production stays
on `--dist-transport auto` without an NHI device for now: the controlled model
cohort shows no speed benefit, while a longer soak and active peer-reboot test
are still appropriate before making NHI the default. NHI can now proceed as a
qualified experimental transport rather than a known-failing one.
