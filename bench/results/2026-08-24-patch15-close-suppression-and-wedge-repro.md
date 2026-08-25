# 2026-08-24 — Patch 15 (CLOSE suppression) and the NHI wedge repro

Two connected outcomes: patch 15 validated, and the Window-B link wedge
root-caused to a reproducible trigger.

## The wedge repro (root cause of the Window-B 16:10 event)

**Trigger: stream TX toward a peer whose device is not active.** With
patch-14 lazy activation, a device with no opener has no rings or paths.
A sender that transmits into that state stalls on zero E2E credits
(initial credit pool ~16 frames); teardown then hits `timeout reading
config space` / `hop deactivation failed`, and the whole XDomain
connection wedges — ThunderboltIP included (100% packet loss both
directions). Recovery escalation, all failed: tbnet module reload,
service-device unbind/rebind, NHI PCI-function (c4:00.6) unbind/rebind
(max2's bind probe hung). Only reboot heals it.

Timeline this session: reboot healed the link at 16:26; a botched tool
invocation (receiver exited on a usage error, sender transmitted alone)
wedged it again ~17:05; software recovery attempts 17:05–17:10 all
failed; second reboot healed it. The pattern matches Window B exactly
(systemd-respawned worker transmitting while the coordinator's device
was stale). Not introduced by patch 14/15 — the exposure predates them
(DS4's normal flow never transmits pre-negotiation); lazy activation
only widens the unopened-peer window slightly.

**Operational rules adopted** (both sessions): never transmit toward an
unopened peer; never leave both devices closed with the reconcile timer
stopped; worker-first stop order when NHI is in prod someday.

**Kernel hardening candidates** (future): TX submit watchdog when
credits never arrive; determine whether the wedge is NHI firmware state
or a driver path-table bug (dmesg goes silent until teardown, which
points at firmware/flow-control state, not driver bookkeeping).

## Patch 15: skip CLOSE toward an already-closed peer

Host tree commit `a48b0ee1f` (on top of ebb4db209), file
`kernel/zerocopy/0015-thunderbolt-stream-Skip-CLOSE-toward-an-already-closed-peer.patch`.

- Release path: if the peer's CLOSE was already received
  (`sdev->closed`), skip sending our CLOSE — the peer's receive path is
  gone, our frame could never complete, and the teardown flush would
  stall 500 ms and warn over expected behavior.
- The TX flush timeout warning in `tbstream_dev_stop` is now
  ratelimited: userspace generation churn against a dead peer produced
  78 warnings at 2 s cadence in Window B.

Checkpatch 0/0/0, W=1 clean.

## Validation (patch-15 module live on both nodes, prod ds4 untouched)

| Test | Result |
|---|---|
| Normal close ordering (512 msgs, 32 MiB, CPU TX → imported HIP RX) | word-exact, receiver exits on CLOSE, **0 flush warnings** (1 pre-patch) |
| SIGKILL sender mid-stream | 115,025 msgs word-exact then CLOSE from the killed side's release (CLOSE still sent when peer alive — regression check), **0 warnings both nodes** |
| Imported-TX dedicated CLOSE frame (1024 GPU-written msgs, 16 MiB) | word-exact, CLOSE delivered, **0 warnings** |
| dmesg audit across all tests | completely clean, both nodes |

The ratelimit flood test was deliberately NOT run: generating
uncompletable CLOSEs requires a dead peer, which is the wedge trigger
above. The ratelimit is hygiene verified by inspection.

## Host state

Both nodes: production 12-patch modules restored, endpoints published
(ring 4096, 9/9), reconcile timers active, prod ds4 (TCP) active and
serving throughout the validation window.
