# DS4 NHI reliability result and TCP-v3 release decision — 2026-08-05

> **Historical failure report.** The measurements below remain the pre-fix
> baseline. Zero-copy patches 11 and 12 subsequently repaired the lost MSI-X
> notification and fresh-open path ordering; the post-fix raw and full-model
> qualification is in
> [`2026-08-05-nhi-msix-rx-prime-fix.md`](2026-08-05-nhi-msix-rx-prime-fix.md).

Hosts: `max` and `max2`, Fedora kernel `7.1.5-101.fc43.x86_64`, direct
Thunderbolt/USB4 link. This report supersedes the readiness interpretation—not
the historical measurements—in the 2026-08-04 component-gate reports.

## Host and deployment state

- Zero-copy patches 6 and 7 were independently built, reviewed, deployed, and
  loaded with the existing eight-patch USB4STREAM backport and zero-copy patches
  1–5. Their repository SHA-256 values are
  `dce920682a3a97094b45da9d78128216a142109b9d97d0171491fb7908eed2a1`
  and `8b29b7bb8879026fe41953d202d92b533157324ddcbdd11b801fa7574fe0a5ad`.
  The matching module set is installed in both rebuilt initramfs images. Both
  hosts subsequently booted those images and loaded byte-identical modules from
  `/lib/modules/7.1.5-101.fc43.x86_64/updates`. The live SHA-256 values are
  `78e31a21b77d336851d9eded63a312ff7091873d81f57cba5b759e16749e864f`
  (`thunderbolt`),
  `217e80fd2d3b695a3f3523de0159f0a44c6505001650fc411975b68ab38e00f1`
  (`thunderbolt_stream`), and
  `d004406445d65934a62ecd2083e499ea352402ad7f9655307ca6df1d81d68343`
  (`thunderbolt_net`).
- Source-only patch 8 repairs receive-HopID ownership in `thunderbolt-net`; its
  SHA-256 is
  `49736fb182b4df154ca4e6a60bd50804516e58604bc8ca2eb374319346396caf`.
  Patch 9 makes stream-side exact attachment, replacement, and rollback
  transactional; its SHA-256 is
  `178e21b76616fab691f669f1e5adbf8449aa8134b410185c1f37b97b70a375b2`.
  Together the full source series passes a 19-case ownership audit, strict
  checkpatch, independent review, and isolated `main.o`/`stream.o` builds. They
  are deliberately not described as deployed. The carrier-first lifecycle
  policy below prevents the observed collision on the live single-link pair.
- Both hosts run with translated/default IOMMU operation enabled. ROCm and the
  NPU remain healthy and no IOMMU fault was observed during these checks.
- The stream device policy is `0660 root:tbstream`. The lifecycle units are
  installed and live with `max2` as allocator and `max` as follower, publishing
  the validated endpoint at `/run/ds4-tbstream/device`.
- The new DS4 applications build independently and byte-identically on both
  hosts. Host-independent transport, protocol-v3, distributed-framing, and ROCm
  gates pass.

The first coordinated reboot exposed a deterministic startup collision: the
stream endpoint consumed HopID 8 before `thunderbolt-net`, leaving the network
interface without carrier. Both hosts were quiesced, their ConfigFS items were
cleanly removed, and both were rebooted with lifecycle triggers disabled to
reset the adapter allocators. The deployed reconciler now waits for
`thunderbolt0` carrier before creating a ConfigFS child and requires the stream
to acquire HopID 9 in both directions, with no automatic fallback. Its offline
suite passes 20/20. A subsequent normal reboot of `max2` with the lifecycle
fully enabled converged both peers to 9/9, retained carrier and bidirectional
ping, and automatically reconnected the full-model v3 TCP generation.

## Why the earlier gates were insufficient

The fixed-size raw transfers and the model-independent DS4 harness from
2026-08-04 still pass. The latter sends equal-size traffic in both directions,
verifies 32 messages per endpoint, crosses 4.01 physical ring wraps, and covers
both CPU-copy and explicitly enabled mapped leases. Those are useful component
checks, but they do not reproduce the model's asymmetric traffic sequence.

Full-model NHI testing exposed intermittent loss of receive/completion progress:

- With `DS4_DIST_NHI_MAPPED=1`, the coordinator could submit a request and reap
  its local TX completion while the worker never observed the corresponding RX
  event.
- With mapped leases disabled—the normal NHI CPU-copy path—the model could make
  progress, then time out and reconnect/replay when a later request failed to
  arrive. Disabling ROCm registration therefore did not remove the failure.
- No matching CRC, overrun, IOMMU, or kernel warning explained the missing
  progress. A local TX completion proves local ring completion, not remote
  delivery.

## Exact asymmetric raw reproduction

`tools/ds4-shape/tbstream-ds4-gate` removes model loading, ROCm, and DS4's TCP
control channel while retaining the observed transfer geometry. It repeats
17-, 17-, 33-, and 65-frame requests; every request is followed by a 127-frame
response whose final frame is 1088 bytes. It verifies the envelope, every
payload byte, RX/TX cursor, event geometry, and exact TX completion before
advancing.

On the deployed seven-patch stack the pair completed 25 exchanges and then
timed out waiting for NHI progress. An immediate fresh-open retry also failed.
This is a production-blocking result: the failure is reproducible below mapped
model I/O and below the DS4 TCP control protocol, and the endpoint does not
recover reliably merely by closing and reopening it.

## Production transport selection

Production uses:

```text
--dist-transport auto
```

No `--dist-nhi-device` is supplied. With no local NHI candidate, current peers
negotiate descriptor-framed protocol-v3 TCP and retain its generation,
sequence, and typed bulk-descriptor checks. Explicit `--dist-transport tcp`
selects legacy protocol v2 and is retained only as the compatibility oracle.

Any configuration supplying an NHI device is lab-only and
reliability-blocked, including `auto` plus `--dist-nhi-device` and required
`--dist-transport nhi`. The pre-activation fallback in `auto` is not a recovery
guarantee once an NHI generation has active or ambiguous traffic. Mapped leases
remain an additional explicit experiment enabled only with
`DS4_DIST_NHI_MAPPED=1` on both peers.

## Full-model performance result

The clean pre-prefill-fix TCP comparison used five full-model runs per cohort:

| Transport/protocol | Full-model generation rate |
|---|---:|
| Explicit TCP, legacy v2 | 11.38 tok/s |
| `auto` without NHI device, descriptor-framed v3 TCP | 11.28 tok/s |

The measured change is -0.10 tok/s, or -0.88%; there is no measured increase.
That difference is within run-to-run noise and is not evidence of a meaningful
regression either. Weighting all completion tokens and wall time across the five
runs gives 11.2244 tok/s for v2 and 11.2249 tok/s for v3, a +0.005% difference;
that is also effectively zero. The raw rows are in
[`2026-08-05-ds4-full-model-tps.csv`](2026-08-05-ds4-full-model-tps.csv).

The scheduler-correctness repair uses canonical 128-token absolute prefill graph
boundaries. On the repaired `ds4-server` (`ea6894e...df751`), cold, warm,
streaming, and sequential full-model gates completed 46 requests with zero
same-seed mismatches: 20 cold concurrent requests at 7.14 aggregate tok/s,
eight warm at 7.12, eight streaming at 6.69, and ten sequential at 10.94. The
post-reboot sequential pass completed 10 more requests and 800 completion tokens
at 11.05 tok/s; the final enabled-peer-reboot smoke completed two requests at
10.93 tok/s. These confirm current-build correctness and its roughly 11 tok/s
single-request operating point, but they are not a new same-build v2/v3 A/B and
therefore do not create a speedup claim. The old graph-history cache remains
recoverable at `/var/cache/ds4-mxfp4/kv.pre-prefill-graph-20260805`; production
uses the fresh `/var/cache/ds4-mxfp4/kv` namespace.

All full-model rows used the 155,976,458,848-byte GGUF with SHA-256
`0e3a161b670f686128ec5f92a601dfde616a37bf5e7e48999fa2d32471b57ec6`.
The repeated post-reboot command was:

```sh
python3 tests/test_server_batching.py --url http://127.0.0.1:8080 \
    --pairs 1 --workers 1 --rounds 5 --timeout 1800 \
    --nonce postreboot-r2-single-20260805
```

There is **no trustworthy NHI tokens/second gain** to report. NHI model runs
that time out, reconnect, or replay are failed correctness/reliability samples,
not performance measurements. An NHI throughput claim requires a failure-free
full-model correctness run and repeated controlled A/B measurements after the
asymmetric delivery and reopen defect is fixed.

## Release conclusion

Descriptor-framed v3 TCP is the release-safe transport. The NHI work remains a
valuable experimental implementation and diagnostic target, but fixed-size,
symmetric, mapping, and local-completion passes cannot override the exact raw
failure. NHI qualification remains blocked on root-cause repair, reliable
fresh-open recovery, repeated asymmetric passes through ring wrap, full-model
equivalence, disconnect/reconnect behavior, and soak testing.
