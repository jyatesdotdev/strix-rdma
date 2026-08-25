# 2026-08-25 — DS4 Stage-3 `6460eb6` localization window stops at the validation command

## Verdict

**PRE-ARM FAIL-CLOSED. Neither localization arm ran. Stage 3 remains NO-GO.**

Production preflight and quiescence passed, but the mandatory fresh bilateral
`make test-rocm` gate did not execute: the lifecycle invocation supplied
TheRock's runtime library path but omitted its build-time `ROCM_HOME`/`HIPCC`
override. Make selected absent `/opt/rocm/bin/hipcc` and exited 2 on both hosts
while trying to rebuild `ds4_rocm.o`.

No ROCm executable or test ran. The full MXFP4 suite was skipped. Arm A was not
launched, p15 was never loaded, Arm B was not launched, and no dump or NHI
traffic was produced. There was no retry or follow-on.

## Reviewed and staged inputs

- DS4 commit:
  `6460eb6a1704080da44fc6af9ace8d80f8c6a400`
- parent:
  `20182c95e33458e3c50059bb58004c94d700e249`
- unchanged production-source parent:
  `9b383ac47a26df8a18fdaf78aa33ca96cd42a8d2`
- clean Git archive SHA256:
  `4b3bd1b290582dee77b98ff112da57fa6370f1a3ef9449af4f91bc8a4189a9ff`
- canonical 1,488-file source-manifest SHA256:
  `6f01a72e52d4fd69f9cb0f64105edf5e851ace1f06298850cc2c9830e9a4bb95`
- bilateral 17-item linked manifest SHA256:
  `f87d3e0112c1ba74d4443ed854186196be6079ec19d48d59be6dd72af082b7c2`
- bilateral compile-only build-log SHA256:
  `e8f8ac136fcfc5c941b759caaebef6bf7ac2d922e34d25fa6281db7126ff6c57`
- unchanged bilateral runtime binary SHA256:
  `4c20e8117765f912ece00d0e68dd3a22ce91631a8b905a537d542a22848210d7`
- production module SHA256:
  `ccd946330a5612ffe7674246ac387b39d4a2e71562ed585cfc65d8d58e2650e7`
- p15 SHA256, verified but never loaded:
  `8a80396bb27cd3403182ff1b5aef32325bd4f142530151345398d7d07db321e0`
- model size on both hosts: 155,976,458,848 bytes
- hosts/kernel: `max`, `max2`, `7.1.5-101.fc43.x86_64`

All 1,488 archived source files passed on both hosts. The compile-only staging
logs were rc0 and byte-identical. The test-only commits changed only `Makefile`
and `tests/test_tp_combine_rocm.c`; the staged runtime remained exactly the
previous `9b383ac` binary.

The final frozen but **unexecuted** localization inputs were:

- Arm A script SHA256:
  `a7de7c6b74c0cb7da7774a27eb428243f17050069b6c2ba3f7a23babf2c65334`
- Arm B script SHA256:
  `e62a24de997deabee5c0086169f4ec07d4da63e758f640fc1bfb8e859cb129de`
- dump checker SHA256:
  `ca40f332d00bc681e7cce10bbf113f56f0b433bacb6e8f9b9013eb52c82eb202`

A later independent HOLD superseded the earlier script GO while restoration was
already in progress. It identified lifecycle issues for a future proposal,
including trap/preflight ordering, a barrier-failure wait that could defeat the
240-second cap, TERM-only B cleanup without wait/KILL/live proof, and a
one-sided B exit that could wait until the full 900-second deadline. None of
these scripts was executed in this window.

## Production preflight and quiescence

Before shutdown both hosts passed:

- active worker/server/exporter and active reconcile timers;
- exact production `PrivateTmp=yes` lock-path/fd inode equality with live FLOCK;
- exact production module with diagnostic parameter absent;
- exact 9/9, ring-4096, throttling-0 endpoints and no device holder;
- no Stage-3 lock or staged process;
- bilateral TB-IP with zero packet loss;
- `/v1/models`, fresh 5-prompt + 1-completion API inference, and exporter
  scrape.

A host-namespace `/tmp/ds4.lock` on `max` named dead PID 134826. The PID was
absent, the file had no kernel lock or holder, and a nonblocking ownership-aware
FLOCK succeeded before it was removed. This was separate from production's
private lock inode.

Production was stopped worker-first, followed by coordinator/exporter. Both
reconcile timers were stopped. Quiescence was then proven bilaterally: no
production, staged, or test executable; no stream-device holder; production
module still loaded; exact endpoints; and healthy TB-IP.

## Pre-arm validation failure

The intended validation wrapper ran identically on both hosts:

```text
cd /home/jryates/ds4-stage3-build
export LD_LIBRARY_PATH=/opt/rocm-therock/lib
timeout --signal=TERM --kill-after=15s 1500s make test-rocm
```

This set the runtime library path but not the Makefile's build-time ROCm root.
Make therefore expanded:

```text
DS4_LINK="/opt/rocm/bin/hipcc ..."
```

and failed identically:

```text
make[1]: /opt/rocm/bin/hipcc: No such file or directory
make[1]: *** [Makefile:384: ds4_rocm.o] Error 127
make: *** [Makefile:213: test-rocm] Error 2
```

Both bilateral logs are 26 lines and byte-identical, SHA256
`a9ec3bb8274437d093fc03f8e7356ea7228ba10e0b5b805d275943064ec0c3ce`.
The wrapper recorded:

```text
test_rocm=2 mxfp4_build=125 mxfp4=125
```

The C compiler rebuilt several host objects before Make reached the missing
HIP compiler, but the ROCm compiler and every test executable were never
started. The runtime binary and key linked objects retained their frozen
hashes. Under the explicit fail-closed/no-retry contract, correcting the
command and rerunning it in the same outage was not allowed.

## Arms withheld

- standalone Arm A: **not launched**;
- standalone checker: **not run**;
- p15 swap: **not attempted**;
- TP Arm B: **not launched**;
- diagnostic artifacts: **none**;
- NHI traffic: **none**;
- retry/follow-on: **none**.

Consequently this window adds no model-arithmetic or divergence-localization
evidence. The prior wrong-logit result remains the latest Stage-3 model result.

## Restoration

The production module was never swapped or unloaded. After proving that no
staged/test executable or device holder remained, reconcile timers were
restarted bilaterally. Production worker was started first, then coordinator,
then exporter.

The final audit passed on both hosts:

- installed production module hash exact and diagnostic parameter absent;
- exact 9/9, ring-4096, throttling-0 endpoints;
- active timers, worker, server, and exporter;
- production private lock path and fd 3 had identical inodes and live FLOCKs;
- no diagnostic lock, staged process, or stream-device holder;
- staged runtime and key object hashes unchanged;
- bilateral TB-IP zero loss;
- `/v1/models`, fresh 5-prompt + 1-completion API inference, and exporter
  scrape passed;
- marked dmesg on each host contained only the explicit BEGIN and END markers.

The outage opened at `2026-08-25T07:17:36.543578000Z` and was closed at
`2026-08-25T07:21:50.021604000Z`.

## Artifacts

Complete evidence is under
`bench/results/2026-08-25-ds4-stage3-6460eb6-localization-prearm/`, including:

- source, archive, build, linked-object, and script provenance;
- preflight, quiescence, and restored-state audits;
- exact failed validation command and bilateral logs/status;
- pre/post API, exporter, TB-IP, and marked-dmesg evidence;
- frozen but unexecuted scripts/checker;
- `RUN-SUMMARY.txt`, `PROVENANCE.txt`, and `SHA256SUMS`.
