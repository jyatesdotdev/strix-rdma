# 2026-08-25 — DS4 Stage-3 preflight: hardened S2 passes; engine arm held before NHI

## Scope and state

Transport/lifecycle review of DS4 Stage 3 found and fixed, before node
time, four ownership bugs across commits `e4266a5..8fb6977`: TX_DONE's
UAPI-defined zero byte count, producer overwrite before wrap credit,
healthy-shutdown service drain, and gate-construction/CLOSE races. Final
review approved `8fb6977` for bounded testing.

Identical staged artifacts:

```text
ds4                 md5 65075b5e7135168826a5f5bf987fa39b
test_tp_nhi_live    md5 dfcec5a6a3c903d3e2299b6c00fd718d
p15 module          md5 18f21563e2c6051e06070c8765a20f2e
```

Production was stopped worker-first, patch-15 modules were transiently
swapped on max2 then max, reconcile timers stopped, and `STAGE3WIN`
dmesg markers written. TB-IP remained healthy. No module was installed.

## Arm 1 — hardened S2, 4000 exchanges (~8 ring wraps): PASS

Both ranks used imported, dedicated MTYPE_UC TX+RX pools, role-tagged
in-band stamps, producer-side TX reservation before every GPU fill,
strict TX_DONE/RX geometry validation, final-reader + RX-event joined
repost, and exact ownership quiesce before the teardown barrier.

| Rank | Result | Sustained | 86-gate equivalent | Kernel ownership |
|---|---|---:|---:|---|
| max2 / rank 0 | bit-exact PASS, 4000/4000 | 94.1 µs/exchange | 8.09 ms | TX 32000/32000 |
| max / rank 1 | bit-exact PASS, 4000/4000 | 93.4 µs/exchange | 8.03 ms | TX 32000/32000 |

Both close snapshots:

```text
flags=0x18 TX_IMPORTED RX_IMPORTED
failures=0 event_drops=0 crc=0 overrun=0
tx descriptors=32000/32000
rx descriptors=36095 posted / 32000 completed
```

The ~94 µs harness result includes the already-accounted Stage-2 costs
(per-exchange host staging, dispatch/sync, and timeout readback); it is a
correctness/wrap result, not the Stage-3 performance prediction. It
improves the previous harness result (~106 µs) and leaves the direct probe
ceiling (35.2 µs with reduce, ~3.0 ms/86 gates) unchanged.

Dmesg after arm 1: zero tbstream/thunderbolt/IOMMU/error/warn/fault
lines on both hosts. No CLOSE flush warning.

Raw logs at window close:

```text
max2:/tmp/stage3-arm1-r0.{log,rc}
max:/tmp/stage3-arm1-r1.{log,rc}
```

## Arm 2 — one-token Stage-3 engine reference: NOT RUN

The first process attempt never reached NHI open/READY or any TP gate:

1. max2 coordinator loaded the model and stopped at
   `waiting for worker on 10.99.0.2:5601`;
2. max worker exited immediately, before connect/model/NHI, because the
   root-run test encountered a jryates-owned mode-0600 host-namespace
   `/tmp/ds4.lock` left by an earlier manual run under Fedora's
   sticky-directory `fs.protected_regular` policy;
3. while the coordinator was still only waiting on TCP, the DS4 session
   found a semantic rank-1 attention bug: compact owned heads live at
   offset zero, but the Q8 TP path shifted the head input by `group0`;
4. the urgent hold was honored. Coordinator timed out without a peer; no
   retry was made.

Therefore the attempt is **not transport evidence and not an engine
result**. It produced no stream traffic and no imported-pool stats. The
semantic fix exists in DS4 `0ce04c5` with a targeted rank-1 regression but
was not part of this window and requires review/build testing before a new
one-token arm.

Future root-run windows must set, for example,
`DS4_LOCK_FILE=/run/ds4-stage3.lock` on each host instead of reusing a
host `/tmp` path. Follow-up audit showed production itself was healthy:
both systemd units have `PrivateTmp=yes`, and each service's namespace
path device/inode exactly matched fd 3 with a live kernel FLOCK. Those
private locks cannot exclude a manual process in the host mount namespace,
so the operational prod-stop + process audit remains mandatory. A future
shared `/run` lock configured in both systemd and manual runs would make
the singleton guard global.

## Restoration audit

After the hold:

- stale host-namespace test locks removed only after confirming no DS4
  test process; post-start inode audit verified both production-private
  lock paths remained present and locked inside their mount namespaces;
- production modules restored both hosts; diagnostic DMA-BUF parameter
  absent; endpoints `/dev/tbstream0` republished;
- reconcile timers active both;
- `ds4-mxfp4-worker`, `ds4-mxfp4-server`, and exporter active;
- `/v1/models` healthy and a fresh chat-completions request executed
  (8 prompt + 1 completion token);
- TB-IP 0.327 ms average;
- final dmesg audit since `STAGE3WIN`: spotless on both hosts.

## Verdict

The hardened DS4 transport core is approved by a real 8-wrap,
bit-exact, exact-quiesce test. The Stage-3 model path remains **unrun**;
the next gate is local/remote validation of DS4 `0ce04c5`, followed by a
fresh bounded one-token window. No full decode should run before that
one-token reference is bit-exact and its transport stats/dmesg are clean.
