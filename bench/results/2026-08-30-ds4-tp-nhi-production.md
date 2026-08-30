# 2026-08-30 — DS4 TP/NHI production promotion

## Deployment

Promoted DS4 commit `c18296eff2152654327bb06f5bc6c968bfd922de` on both
Strix Halo hosts using `ds4-server --tensor-parallel --transport nhi` over
patch-15 `thunderbolt_stream` (`zc_diagnostic_dmabuf=1`). The deployment
examples and lifecycle documentation are in `tools/modprobe.d/` and
`tools/systemd/`.

Validated workloads before promotion:

- 23-token prompt, 4096 generated tokens: 261.934 s, 15.64 tok/s generation.
- 3373-token cold prefill plus 64 generated tokens: 211.221 s prefill,
  15.97 tok/s.
- Both nodes closed with `failures=0 event_drops=0 crc=0 overrun=0`.

## Operational incident and recovery

While applying the final hardened systemd drop-ins, the worker was found in
a connection retry loop and Thunderbolt-IP pings were black-holing in both
directions. The worker stop timed out and systemd aborted it; the coordinator
was then stopped cleanly while it still held `/dev/tbstream0`.

Recovery succeeded without reboot:

1. Stop both DS4 services and the reconcile timers.
2. Run the managed ConfigFS cleanup on both hosts.
3. `rmmod thunderbolt_stream` and `modprobe thunderbolt_stream` on both
   hosts.
4. Start the reconcile service and timer on both hosts.
5. Verify bidirectional `10.99.0.x` ping, then start the DS4 worker followed
   by the coordinator.

The link recovered immediately, the hardened units bound rank 0/rank 1 over
NHI, `/v1/models` became ready, and a 16-token production smoke request
returned HTTP 200. This does not weaken the worker-first close rule; it adds
a documented non-reboot recovery attempt for a link that has already wedged.

## Reboot persistence correction

The first coordinated reboot after promotion exposed that the initramfs still
carried the older 895192-byte `thunderbolt_stream.ko`; the patched 962880-byte
module was present under `/lib/modules/.../updates/`, but the early boot module
had no `zc_diagnostic_dmabuf` parameter, so both DS4 services failed
`TBSTREAM_ZC_IMPORT` as intended. Rebuilding both images with `dracut --force
--kver "$(uname -r)"`, verifying the patched module and
`etc/modprobe.d/ds4-tbstream-zc.conf` were present in the image, and rebooting
again restored the persistent configuration.

The post-reboot production pair then passed the full performance run:

- 23-token prompt + 4096 generated tokens: HTTP 200 in 261.740 s,
  15.65 generated tok/s.
- 3373-token cold prefill + 64 generated tokens: HTTP 200 in 215.346 s,
  15.97 prefill tok/s.
- Both services had zero restarts after the corrected boot.
