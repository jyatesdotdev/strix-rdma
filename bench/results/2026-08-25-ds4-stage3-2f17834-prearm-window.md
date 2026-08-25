# 2026-08-25 — DS4 Stage-3 `2f17834` pre-arm window: exact provenance, CLI rejects zero-token cap

## Verdict

**No Stage-3 engine arm ran. Stage 3 and full decode remain NO-GO.**

The corrected `2f17834` source and binary provenance passed independent
verification on both hosts. A transient p15 window was then opened, but the
single allowed coordinator invocation exited at CLI validation:

```text
ds4: invalid value for -n: 0
```

Exit status was 2. This happened before lock creation, model load, TCP listen,
NHI open, or worker launch. No retry was attempted. There was no tensor-parallel
traffic and no logit dump, so this window is neither engine nor transport
evidence.

## Corrected provenance

A staging audit before the window invalidated an earlier candidate binary hash:
max's build tree had retained an older tracked `ds4_transport_nhi.c`. That
binary was never used in this window.

Both `/home/jryates/ds4-stage3-build` trees were replaced from
`git archive 2f178345bf67ef99d4406b0fb4e94bfe50dfaaf3` and rebuilt independently.
The local reviewer then generated a canonical SHA256 manifest for all 1,488
tracked files at that Git tree and hashed the same paths on each host. Both
remote manifests matched without a differing or missing file:

```text
canonical manifest SHA256 3603d1856fe6e0df6c46d749808deb318bbeb722fb4e8d45f1eb2f4c48bd824b
tracked files              1488 / 1488 on max and max2
```

Bilateral build artifacts were bit-identical:

```text
ds4                 dafe4d4b4ae19682029839aefca442dfc6a407a641c4fb3525fe4a00d13bfc3a
ds4_rocm.o          38769a8c1bf7a79a6c072f22fb62225e96098356b231d3a6f8eae9f692afd4e5
ds4_transport_nhi.o d72d393ed93842bcdb9f2540dc8b57e6691ba0dc839b3efac58b8c932481c553
```

The corrected source hash of `ds4_transport_nhi.c` was
`ae7b9b52bd9325f38cda8f57ba7cb6b6151b44547b46a92f85197ad2e49a24d4`.
Fresh exact-source evidence passed independently on both hosts:

- `make strix-halo`;
- `make test-rocm`, including TP 9/9 and CLI 49/0;
- the full real-shape MXFP4 suite, including strict rank-local cached halves,
  rank recombination, span exactness, and path variants.

The p15 module was identical on both hosts:

```text
SHA256 8a80396bb27cd3403182ff1b5aef32325bd4f142530151345398d7d07db321e0
MD5    18f21563e2c6051e06070c8765a20f2e
vermagic 7.1.5-101.fc43.x86_64 SMP preempt mod_unload
```

## Preflight

Before changing state, both hosts passed:

- exact staged source, binary, object, p15, and swap-script hashes;
- production module SHA256
  `ccd946330a5612ffe7674246ac387b39d4a2e71562ed585cfc65d8d58e2650e7`;
- production diagnostic parameter absent;
- exact HopID 9/9, ring 4096, throttling 0 endpoints;
- active reconcile timers and no `/dev/tbstream0` holders;
- model size 155,976,458,848 bytes on both hosts;
- bilateral TB-IP with zero loss;
- active worker, server, and exporter;
- `/v1/models`, exporter metrics, and a fresh 11-prompt + 1-completion API
  inference;
- production `PrivateTmp=yes` lock path equal to fd 3 by device/inode, with a
  live kernel write FLOCK.

Fresh test-created host `/tmp/ds4.lock` files were removed only after proving
that their recorded PIDs were dead, acquiring them nonblocking, and proving
their inodes differed from the active production-private locks.

Production was stopped worker-first. Timers and exporter were stopped, and both
hosts were re-audited process-, holder-, and test-lock-free. P15 was swapped
coordinator/allocator host max2 first, then worker/follower host max. Both
parameters read `Y`; endpoints remained exact 9/9 and holder-free; TB-IP stayed
healthy; marked dmesg was clean.

## Single invocation: rejected before the arm

The reviewed intent was one raw prompt token and zero evaluated decode tokens.
Rank 0 was invoked first with `Hello`, `--raw-prompt`, `--temp 0`, and `-n 0`.
The exact `2f17834` CLI uses `parse_int()` for `-n`; it rejects values `<= 0`.
The coordinator therefore exited 2 immediately. The worker-listen barrier was
never reached, so rank 1 was never launched.

Observed state after the exit:

- no `/run/ds4-stage3.lock` on either host;
- no staged DS4 process;
- no stream-device holder;
- no rank-0 or rank-1 result dump;
- no rank-1 log or rc file;
- p15 endpoints still exact and healthy;
- marked dmesg contained only the explicit window markers.

The only arm log is:

```text
ds4: invalid value for -n: 0
```

## Existing-CLI correction for a future review

Post-restoration source review found that no CLI change is needed. With `-n 1`
and `DS4_MTP_SPEC_DISABLE=1`, the existing sampled CLI:

1. unconditionally executes the one-token `ds4_session_sync()` prefill;
2. samples and prints one token from the resulting logits;
3. increments `generated` to the cap;
4. exits the loop before `ds4_session_eval()` can evaluate that sampled token.

That is also the shape used by the frozen standalone oracle and the earlier
`0a2cf21` TP attempt: one `pos0` prefill graph and no `pos1` graph. It is a
candidate only for a **separately reviewed and explicitly authorized future
window**. It was not retried here.

## Restoration

After capturing the CLI failure, both hosts were confirmed process-, lock-, and
holder-free. Production modules were restored worker host first and coordinator
host second; worker was started before server, then exporter.

Final bilateral audit passed:

- installed production module hash restored exactly;
- diagnostic parameter absent;
- endpoints `/dev/tbstream0` and `/run/ds4-tbstream/device` healthy at exact
  9/9, ring 4096, throttling 0;
- reconcile timers active;
- worker, server, and exporter active;
- no host or `/run` test lock;
- production private lock path and fd 3 matched with a live FLOCK;
- TB-IP zero loss;
- `/v1/models` and a fresh 11-prompt + 1-completion API inference passed;
- exporter reported zero active requests;
- marked dmesg contained only the window markers, with no warning, error,
  timeout, fault, or transport line.

## Artifacts

Durable evidence is under
`bench/results/2026-08-25-ds4-stage3-2f17834-prearm/`:

- canonical source manifest;
- bilateral corrected test logs;
- rank-0 CLI log and rc;
- bilateral in-window and restoration states;
- pre/post production API smokes;
- `SHA256SUMS`.
