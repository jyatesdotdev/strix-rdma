# 2026-08-24 — Gate 7: imported-pool mode in live DS4 inference (Window B)

First live-inference exercise of the native DMA-BUF path. Coordinated with
the ds4 session (their repo: `winb-imported-pool` @ 46ea98e, one commit on
their TCP-gated NHI port `279ee73`). Patch-14 modules transiently swapped
on both nodes via `tools/scripts/p14-swap.sh`; production restored and
re-gated after.

## What was tested

New opt-in mode in the DS4 NHI transport (`DS4_DIST_NHI_IMPORTED=1`,
worker/last-hop only): at startup the worker allocates a dedicated 16 MiB
native HIP pool (dedication proven via `hipMemGetAddressRange`), exports it
as a DMA-BUF, and imports it as the zero-copy TX pool before `ZC_ENABLE`.
The output head's logits readback then targets a device pointer into the
imported pool (hipMemcpyDefault readback at the four lease-reachable
layer-slice sites), so payload is written by the GPU directly into memory
the NHI reads — no hipHostRegister, no CPU touch, no host round trip. The
64-byte envelope and CPU-source control payloads stage via synchronous
H2D copies; visibility rests on copy completion + the commit-time device
synchronize (the gate-5/6 ownership contract). Kernel-side stats are
logged at transport close.

## Arms and results (acceptance gate: 200 tokens, temp 0, sha oracle)

| Arm | Transport | Gate t/s | sha | 21k prefill t/s |
|---|---|---|---|---|
| (a) reference | v3 TCP | 14.29 | df07199e5a292872 | 245.2–245.9 (band) |
| (b) | NHI mapped (hipHostRegister) | 13.33 | df07199e5a292872 | 245.85 |
| (c) | **NHI imported (native GPU pool)** | 13.33 | df07199e5a292872 | **246.84** |

All three arms **bit-exact** against the production reference. Kernel
stats at coordinator close: failures=0, event_drops=0, crc=0, overrun=0,
last_error=0, rings drained (tx 8315/8315 frames across 411 messages).

## Verdict

Gate 7 **passes**: the imported native-pool path is correct under real
inference load end to end, and the hipHostRegister write path is gone.
Wall-clock parity at gate granularity — the 0.065 ms/token penalty this
removes was always below the 1-second wall band (13.33 vs 14.29 t/s
spread across arms is run-to-run noise, not transport signal; prefill
parities at ~245–247 t/s confirm). The mode's value is structural: it is
the zero-CPU-touch substrate the TP-style exchange path needs, and it
removes the last host-memory round trip from the worker's result path.

## Incidents and lessons

1. **TB-IP link wedge mid-window.** Between arms (b) and (c) the
   ThunderboltIP datapath black-holed both directions (ARP INCOMPLETE→
   FAILED, zero frames received either way) with completely silent dmesg.
   thunderbolt_net module reload did not heal it; authorized reboot of
   both nodes did. Correlates with the fully-closed-device + stopped-
   reconcile-timer gap under patch 14, but unproven (code review: patch
   14's activated-close path is narrower than production's). Window
   protocol change adopted: never leave both devices closed with the
   timer stopped — one side stays open across arm gaps.
2. **Boot-order footgun found and disarmed.** On reboot, the winb drop-ins
   outranked prod, so the worker briefly flap-looped on `TBSTREAM_ZC_IMPORT`
   against the production module — validating the loud-failure design
   (no silent degradation) and the ds4 session's boot-safety procedure.
3. **Reboot wiped /tmp on both nodes** (tmpfs), including the staged
   patch-14 module. Re-staged from the persistent build tree
   (`~/src/linux-stable/drivers/thunderbolt/thunderbolt_stream.ko`, md5
   6bb8061db8e54bf0ef23382b58973d74). Swap script now effectively
   requires this re-stage step after any reboot.
4. **Close-ordering noise amplified by keep-open**: the coordinator's
   terminal CLOSE toward an already-exited worker retried flush every 2 s
   for ~5 minutes until systemd stop canceled it (78 warnings, zero
   failures/drops). Same benign mechanism as gate 6; polish item queued:
   suppress/cancel the terminal CLOSE when the peer is already closed.

## Follow-ups queued

- Kernel: suppress/rate-limit terminal CLOSE toward an already-closed
  peer (patch 15 candidate).
- ds4: move the stats log earlier in close + fflush so worker-side stats
  survive systemd SIGTERM teardown.
- Gate 8 (DS4 slot-offset binding / production integration) and the TP
  spike remain open; the TP transport budget is already measured
  (9.8 µs RTT at 4 KiB, ~1.6 GB/s burst).

## Host state

Both nodes: production 12-patch modules, endpoints republished (ring
4096, 9/9), reconcile timers active, prod services restored and re-gated
by the ds4 session (sha df07199e5a292872).
