# Stage 3 `93f75b9` bilateral compile/runtime-oracle window (v10 PASS)

Date: 2026-08-25. Launch: 2026-08-25T20:51:01Z. Result: **PASS**.
Model/prefill/decode/TP-NHI-diagnostic windows remained **HOLD** throughout and
no model invocation occurred in this window.

## What ran

The independently reviewed, exact-digest frozen bundle
`/tmp/stage3-93f75b9-compile-oracle-v10` (SHA256SUMS
`9bb2c0b5d488851d8d5c6375549c17987c886f74d82ed4875b064543b9120f1a`, orchestrator
`243233d6299c9f1603c1542ab53f76c698189f3baece18ba4a6250e70750805d`) ran the
stage-3 no-model compile/runtime-oracle sequence on `max` then `max2`,
fail-fast, from exact DS4 commit
`93f75b98bcfcb6a44ca739754472bf767f5433dc` (tree
`1da8f8f1d776b455d46a679a3a6a6e06e06acc70`, 1,490-file manifest
`5bdea6ff306961d198f341815082d72c9903f25349cd679fb9bb768fa8d83bf2`).

Per host, exactly once each, in a clean scrubbed `execve` environment:

1. `make clean`
2. `make -B strix-halo` (ds4/ds4-server/ds4-bench/ds4-eval/ds4-agent)
3. `test-tp-combine-rocm` — PASS (bitexact TP combines)
4. `test-mxfp4-rocm` — PASS
5. `test-graph-deferred-dump-rocm` — PASS, exact chronology: 3 control
   negatives, subset armed (8 entries), full armed (172 entries), 172 unique
   deferred dumps, one PASS line, no plan/failure lines

The umbrella `make test-rocm`, GGUF/model loading, prefill, decode, TP/NHI
diagnostics, and module swaps were forbidden and did not occur.

## Bilateral results

- Both hosts compiled byte-identical production binaries, test binaries,
  toolchain identities, and runtime-library manifests (bilateral checker PASS).
- Graph oracle identical on both hosts (172 unique deferred captures each).
- Wrapper/settle return codes: `max=0 settle_max=0 max2=0 settle_max2=0`.
- Final receipt `SUCCESS`: `phase=COMPILE_ORACLES_PASS`,
  `restoration=PASS`, `model_window=HOLD`, nonce
  `34cc83dd6e4c3896d83ca13ad0fff39a`, evidence manifest SHA256
  `8ce422b786569de9d8e1e9d42692d0441f1d58bfbdc852f253dbd10878b2d9e2`.

## Production baseline discovered and bound in v10

The v9 attempt found the production baseline had drifted from the earlier
frozen expectation; authorized read-only inventory established the current
exact baseline, which v10 binds:

- worker: `/opt/ds4-perf-nhi-20260824/ds4` SHA
  `866411734f0fa0cdeb3eda88ee501970ce50787d32a50e833d1a742af5da9803`
- server: `/opt/ds4-perf-nhi-20260824/ds4-server` SHA
  `2aa8837ee388bb5110af2832d0bc8a1d01ab302d56826444933efab0720ca9ba`
- exporter runtime: `/usr/bin/python3.14` SHA
  `ea4bc0ec462c5f76a1ce00e8899ec29c3f04098860733aca153cf9e030b5a052`
- production API: `127.0.0.1:8080` (8000 closed)
- module: `/lib/modules/7.1.5-101.fc43.x86_64/updates/thunderbolt_stream.ko`
  SHA `ccd946330a5612ffe7674246ac387b39d4a2e71562ed585cfc65d8d58e2650e7`,
  build-id `5b89774a824d13a6e22b6f418a3981ff36216bd9`, installed srcversion
  empty + loaded sysfs srcversion absent (dual-absence bound)
- pinned systemd 258 no-job serialization is exact empty `Job=\n` (any
  nonempty value, including `0`, fails closed)

## Restoration

RESTORATION=PASS: services/timers restored via replace-mode transactions,
pre/post static + runtime identity byte-identical, lifecycle health PASS,
module/endpoint/network/API/exporter audits PASS, private FLOCKs verified,
bilateral ping zero-loss, dmesg deltas empty, no nonce processes, global
`/tmp/ds4.lock` absent throughout.

## History of the bundle lineage

v1–v7: built and frozen, each rejected/held in review, never executed.
v8: approved and attempted once; failed closed before READY/host contact
(macOS has no local `flock` prerequisite). Retired.
v9: approved and attempted once; reached read-only bilateral preflight, found
the stale worker baseline, then its cleanup hit a Bash nounset same-line
derived-local bug. No owner/staging/stop/mutation occurred; local capture and
mutex retained as incident evidence in `/tmp`. Retired.
v10: split all derived locals with nounset harness coverage, exact
worker/server/exporter baselines, API 8080, module dual-absence srcversion,
exact-empty Job fencing. **PASS** (this window).

## Evidence

Sealed capture: `2026-08-25-ds4-stage3-93f75b9-compile-oracle-v10-capture/`
(188 files, `EVIDENCE-SHA256SUMS` verified after archival). Includes both host
reports, bilateral report, wrapper logs with full build/test output, pre/post
static and runtime identity, lifecycle health, module/endpoint/network/lock
audits, restore fences and receipts, dmesg before/after/delta, readiness and
exporter scrapes, and the final `SUCCESS` receipt.
