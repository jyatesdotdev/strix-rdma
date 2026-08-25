# LOG
Running log of noteworthy work; newest first.

## 2026-08-24 — Patch 15 validated; the TB link wedge is reproducible
- **What:** Wrote and validated patch 15 (`0015-...Skip-CLOSE-toward-an-
  already-closed-peer.patch`, host tree a48b0ee1f): release skips sending
  CLOSE when the peer's CLOSE already arrived (its RX path is gone; our
  frame could never complete), and the TX flush-timeout warning is
  ratelimited. En route, root-caused the Window-B link wedge to a reliable
  repro: stream TX toward a peer with no active device stalls on zero E2E
  credits and wedges the whole XDomain connection on teardown (TB-IP
  included; config-space timeouts; survives tbnet reload, service rebind,
  and NHI PCI rebind; only reboot heals). New ops rules adopted on both
  sides: never transmit toward an unopened peer, never leave both devices
  closed with the timer stopped, worker-first stop order for future NHI
  prod.
- **Why:** The 78-warning dmesg flood in Window B and the two link wedges
  were the week's only kernel-side blemishes.
- **Impact:** All close-ordering scenarios now exit with zero warnings
  (normal, SIGKILL mid-stream with CLOSE still delivered from the killed
  side, imported-TX dedicated CLOSE frame), dmesg spotless, prod ds4
  served TCP untouched throughout. The flood/ratelimit arm was skipped
  deliberately — generating uncompletable CLOSEs IS the wedge trigger.
  Follow-ups: TX-credit watchdog + firmware-vs-driver wedge
  investigation. Evidence:
  `bench/results/2026-08-24-patch15-close-suppression-and-wedge-repro.md`.

## 2026-08-24 — Gate 7: imported native pools run live DS4 inference bit-exact
- **What:** Window B with the ds4 session: layered an opt-in imported-pool
  mode (`DS4_DIST_NHI_IMPORTED=1`, worker-only) onto their freshly ported
  NHI transport (their `winb-imported-pool` @ 46ea98e). The worker exports a
  dedicated 16 MiB native HIP pool as a DMA-BUF, imports it as the TX pool
  before ring activation, and the output head writes logits straight into
  memory the NHI reads — no hipHostRegister, no CPU touch. Three-arm A/B
  under the acceptance gate: v3-TCP 14.29 t/s, mapped NHI 13.33, imported
  NHI 13.33, all bit-exact (sha df07199e5a292872), 21k prefill 245.85 /
  246.84 (imported best-of-day, all within noise). Kernel stats clean
  (0 failures/drops/CRC). Mid-window the TB-IP link wedged both directions
  with silent dmesg; module reloads didn't heal it, authorized reboot did.
  Window protocol hardened (never both devices closed with the reconcile
  timer stopped); boot-order and tmpfs-/tmp staging footguns found and
  disarmed; loud-failure design validated by an accidental boot flap.
- **Why:** Prove the native DMA-BUF path under real inference and close out
  the 0.065 ms host-registered penalty question.
- **Impact:** Gate 7 passes: bit-exact + wall parity; the penalty removal is
  real but below gate granularity, so the mode's value is structural — the
  zero-CPU-touch substrate for TP exchanges (already measured at 9.8 µs RTT
  / 4 KiB) and the end of host round trips in the result path. Follow-ups
  queued: suppress terminal CLOSE toward a dead peer (2 s retry noise under
  keep-open protocol), earlier+flushed stats logging for SIGTERM teardown.
  Evidence: `bench/results/2026-08-24-gate7-imported-pool-live-inference.md`.
  Both hosts restored to production and re-gated.

## 2026-08-24 — Window A: patch 14 is transparent to real DS4 inference
- **What:** First coordinated test window with the ds4 session (prod stopped,
  restorable, acceptance-gated). Scripted the transient module swap
  (`tools/scripts/p14-swap.sh on|off`: holder-refusal, param verification,
  prod endpoint recreation) and ran the patch-14 module set on both nodes
  under the campaign-lineage DS4 binaries in both existing NHI modes
  (CPU-copy and mapped hipHostRegister), full production config including
  MTP.
- **Why:** Compat prerequisite for gate 7 / Window B: prove patch-14's lazy
  activation is invisible to the deployed open→ZC_ENABLE→GET_INFO→mmap
  transport sequence under real inference load before layering the
  imported-pool mode on it.
- **Impact:** Both arms bit-exact against the production reference sha
  (df07199e5a292872, 200 tok), wall parity (15.38 tok/s gate; 244.96/245.08
  tok/s 21k prefill vs 245.2–245.7 TCP band), dmesg completely clean on both
  nodes, clean teardown, production module+timer restored and verified by
  script. DS4-side code confirmations: single open site, ENABLE immediately
  after open, dispatcher polls only post-mmap. Window B plan agreed: ds4
  session ports the NHI transport onto the live perf-prefill-decode lineage
  (TCP-gated first); I layer the imported-pool worker-logits mode on the
  ported file with an AmbientCapabilities=CAP_SYS_RAWIO drop-in, kernel gate
  kept strict, and transport-shutdown ZC_GET_STATS logging added for gate
  artifacts.

## 2026-08-23 — Pass gate 6; measure sub-5 µs GPU-polled exchange for TP
- **What:** Completed gate 6 on the transient patch-14 module set: 17-case
  import rollback coverage (`tbstream-import-test`), GPU-written imported-TX
  → imported-RX transfer (`tbstream-gpu-tx`, release event/none arms, 64
  wraps word-exact), 5/5 close/reopen cycles, a mid-stream SIGKILL with
  clean CLOSE and recovery after 85,526 verified messages, and zero IOMMU
  faults across ~460 MiB of imported-pool DMA. Physical cable-pull/reboot
  and deliberate IOMMU/GPU-reset faults deferred as unsafe on the
  production pair. Then built `tbstream-tp-pingpong` (both directions
  imported on both hosts) and measured kernel-notified vs persistent-GPU-
  wave detection.
- **Why:** Close the remaining pre-DS4 correctness/fault gates and convert
  the hybrid tensor-parallel proposal from projection to a measured
  transport budget.
- **Impact:** Exchange RTT p50: 4 KiB 20.3 µs reap vs **9.8 µs spin**
  (~4.9 µs one-way); 32 KiB 57.7 vs **45.3 µs** with jitter collapsed (max
  88→47 µs); implied burst bandwidth ~1.6 GB/s/direction. Two findings:
  small `hipMalloc`s share slab BOs (the driver's same-BO rejection caught
  it; tools now pad to dedicated 16 MiB BOs — DS4 integration must verify
  dedicated allocations), and gate-5 coherence is dispatch-scoped — a
  spinning wave never sees NHI writes through GPU L2, so persistent-kernel
  signaling needs uncached flag words (payload stays coarse). At these
  numbers ~120 TP exchanges/token cost ≈2 ms, supporting a ~1.8× hybrid-TP
  decode estimate whose kernel half is now the open question. One polish
  candidate: skip the final CLOSE toward an already-closed peer to silence
  benign flush-timeout warnings. Hosts restored to production and
  smoke-verified. Full evidence:
  `bench/results/2026-08-23-gate6-tp-exchange-probe.md`.

## 2026-08-23 — Pass gate 5: imported native GPU pools carry real NHI traffic
- **What:** Implemented zero-copy patch 14 (`TBSTREAM_ZC_IMPORT`): lazy ring/
  path activation so imports always precede activation, an atomic
  transactional DMA-BUF pool import for either or both directions with
  reverse rollback, guarded (never-synced) imported frames with mmap holes
  and rejected legacy read/write, and a dedicated page-backed CLOSE control
  frame for imported-TX sessions. Strict checkpatch/W=1 clean against the
  exact 7.1.5 tree. Built the `tools/stale-cache` two-host harness (CPU
  sender with generation-stamped patterns; HIP receiver importing a poisoned
  native pool as the live RX ring and verifying every message with GPU
  digest kernels), live-swapped the patch-14 module transiently on both
  hosts (max2's Secure Boot is now disabled — byte-identical module reuse,
  no signing), and ran the full matrix at ring 256 (L2-resident pool) and
  ring 4096 (production geometry).
- **Why:** Gate 5 of the native HIP DMA-BUF experiment: prove NHI DMA into
  reused hot native-GPU RX slots is correct under alternating generations
  before any DS4 integration, and characterize which acquire mechanism is
  actually required.
- **Impact:** First-ever real stream traffic into native `hipMalloc` memory
  with zero CPU copies: 6,656 messages / 448 MiB / 448 pool wraps across
  seven arms (coarse/uncached × event-acquire/none, adversarial prewarm,
  production geometry) — all word-exact with zero event drops or failures.
  No stale-cache failure was producible even with slots deliberately
  cache-resident during DMA writes and no acquire — strong evidence of
  I/O-coherent NHI writes on Strix Halo — but the explicit timing-enabled
  `hipEventReleaseToSystem` acquire remains the production contract. Both
  hosts were restored to the production module set and smoke-verified.
  Gate 6 (imported TX→RX plus fault/rollback coverage) is unblocked;
  gate 7 touches DS4/live inference and awaits explicit operator go-ahead.
  Full evidence: `bench/results/2026-08-23-dmabuf-imported-pool-stale-cache.md`.

## 2026-08-23 — Pass gate 4: live DMA-BUF import probe on the real NHI
- **What:** Wrote the `tbstream-hip-export` companion (dedicated coarse/
  uncached/fine-grain HIP allocation, base/full-range check, GPU fill,
  device quiesce, DMA-BUF export, fork/exec of the probe with an inherited
  fd), fixed the probe's udmabuf fallback (`_IOW` ioctl, `F_SEAL_SHRINK`
  seal), and ran the gate-4 matrix on `max` via a transient live swap of the
  gate-3 13-patch `thunderbolt_stream.ko` with `zc_diagnostic_dmabuf=1`.
  Nothing was installed; the production module was restored, the 9/9 endpoint
  republished, and a live chat completion verified the untouched TCP-v3
  production path. `max2` was never touched.
- **Why:** Gate 4 of the native HIP DMA-BUF experiment: measure real import/
  map geometry against the NHI DMA device before writing any imported-pool
  mode.
- **Impact:** Coarse and uncached native HIP pools map through the enabled
  IOMMU as a **single fully-covering, frame-aligned DMA segment** at 16 MiB
  and 256 MiB in both directions (udmabuf control: uniform 64 KiB segments,
  full coverage) — ideal geometry for the slot flattener. The fine-grain arm
  is unavailable on this platform: the allocation reports coarse-grain
  coherency and XNACK is hard-disabled (`HSA_XNACK=1` has no effect), so
  gate 5 must use coarse+explicit-acquire vs. uncached as its arms. Gate 5
  (stale-cache ownership with real traffic and a GPU payload digest) is now
  unblocked; it needs a MOK-signed two-host deployment, which also covers the
  deferred `max2` replication. Full evidence:
  `bench/results/2026-08-23-dmabuf-import-probe.md`.

## 2026-08-17 — Implement the gate-3 no-traffic DMA-BUF import probe
- **What:** Added zero-copy patch 13 (`TBSTREAM_ZC_DMABUF_PROBE`) with the
  shared `stream-sg.h` segment-validation/frame-flattening helper, the
  `tools/dmabuf-probe` userspace runner (inherited fd or CPU-only udmabuf),
  54 SG geometry unit checks, and series-test coverage for the ABI, privilege
  gate, transactional rollback, and no-descriptor invariants. Verified the
  hosts are reachable and unchanged (production TCP v3, tbstream published,
  IOMMU translated), then compiled the patched module set against the exact
  7.1.5-101.fc43 tree on `max` without installing anything. Two findings were
  caught by that exact-tree gate: 7.1.5 wants the quoted
  `MODULE_IMPORT_NS("DMA_BUF")` form, and the host's `~/src/linux-stable` was
  a stale mid-series working tree (snapshotted, reset, now tracks the patch
  series by `git am`). Strict checkpatch is clean apart from the series-wide
  new-file MAINTAINERS note.
- **Why:** Gate 3 of the native HIP DMA-BUF experiment: build and validate the
  probe against the exact kernel tree without deploying it, so a later
  authorized host run only has to measure, not debug, the import path.
- **Impact:** Nothing was installed or reconfigured on either host; the loaded
  modules and production services are untouched. Gate 4 is now unblocked: set
  `thunderbolt_stream.zc_diagnostic_dmabuf=1`, run `tbstream-dmabuf-probe`
  for udmabuf first and then coarse/uncached/fine-grain HIP exports in both
  directions, and compare covered bytes, entry counts, alignment, and largest
  segment against the flatten rules before any imported-pool mode is written.
  The macOS case-collision phantom files in `linux/` bit once during patch
  authoring; the regenerated patch carries only stream.c, stream-sg.h, and
  the UAPI header.

## 2026-08-14 — Identify and bound a native HIP DMA-BUF path to the USB4 NHI
- **What:** Re-audited the NHI, AMDGPU, ROCm, and current DS4 data paths and ran initial read-only topology/memory checks on both Strix hosts. The GPUs have only 512 MiB of BIOS VRAM and about 124 GiB of GTT, so DS4's large native HIP allocations are system/GTT pages. The installed HIP 7.13 runtime exposes DMA-BUF export; a temporary 4 MiB `hipMalloc` gate on `max` exported successfully, and a diagnostic CPU mmap saw the exact GPU-written pattern. A subsequent importer/synchronization audit used only local Linux, ROCm, and DS4 source and did not access either host. The public gfx1151 wheel manifest resolves the exact HIP build to TheRock `6d2136cd12be` and `rocm-systems` `79e85e1468f9`; the apparent `3309c6114a` suffix is a generated package hash, not the public source commit.
- **Why:** Distinguish physically impossible one-sided RDMA from achievable GPU-addressable DMA message passing, find a way around the measured penalty of NHI-owned `hipHostRegister()` pages, and identify the exact ownership contract before writing a live driver.
- **Impact:** True rkey/remote-VA/atomic/GPU-initiated RDMA remains impossible, but fixed native HIP pools should be addressable by the NHI through a bounded DMA-BUF importer. The source follow-up found two critical constraints: current stream `open()` activates paths before an import ioctl can run, and an idle HIP synchronize is not an RX cache acquire. The design now requires lazy path activation, exact SG/offset validation, reverse rollback, direction-correct DMA-BUF mapping, explicit TX system release, and a source-pinned or newly supported RX system acquire. No-traffic mapping and stale-cache gates must pass before DS4 integration. Full evidence: `docs/GPU_TO_GPU_FEASIBILITY.md`.

## 2026-08-06 — Remove one QKV launch per layer with a bit-exact ROCm fusion
- **What:** Swept MXFP4 routed-MoE rows per wave, retained the existing one-row specialization, then fused DeepSeek V4 Q/KV weighted RMS normalization with KV RoPE for resident ROCm decode. Added a same-binary rollback and a strict differential suite spanning dense, YaRN, inverse, multi-row/head, zero-rotation, exact-256, 257-pair fallback, and invalid shapes. A first YaRN failure exposed fast-math reciprocal hoisting in the fused loop; one-pair-per-thread indexing fixed it without relaxed tolerances.
- **Why:** The event ledger ranked routed MoE, attention output, and QKV preparation as the largest repeated layer groups. The MoE sweep showed that wider row grouping reduces effective bandwidth, while the QKV path had a concrete removable launch boundary.
- **Impact:** The balanced full-model cohort measured 14.57487 tok/s unfused and 14.60320 tok/s fused (+0.1944%); combined two-host GPU stream time fell from 67.30038 to 67.15398 ms/token (-0.2175%, equivalent to +0.2180% throughput). All 40 responses were identical, all 6,656 profiler samples had zero drops, and both order pairs were positive. Final byte-identical binaries are installed side-by-side as `/opt/ds4-qkv-fuse-20260806-r2`; the standard TCP-v3 services were restored and smoked. Full evidence: `bench/results/2026-08-06-ds4-rocm-qkv-fusion.md`.

## 2026-08-06 — Resolve the full-model GPU hot path with in-process ROCm events
- **What:** Added a fail-open, nonblocking ROCm timing backend and a rotating full-model decode profiler that records 14 stream-0 events around one layer per token, then aggregates only after DS4's existing synchronization. Added 198 deterministic backend/stub checks plus a journal analyzer with weighted means and consistency ledgers. Ran balanced full-model profiler OFF/ON arms and same-prompt staged/direct NHI event comparisons on `max`/`max2`.
- **Why:** Boundary profiling had already limited transport/control work to about 0.9 ms of a 68.1 ms token, but synchronized evaluation spans could not distinguish kernels from host launch overhead or show why mapped direct output failed to help.
- **Impact:** ROCm events account for 99.66% of the two host evaluation spans; pooled profiler overhead is approximately -0.046% throughput. Within sampled layer bodies, routed MoE (24.55%), attention output (24.24%), and QKV preparation (20.71%) dominate. The worker output head is 2.51466 ms/token. Direct NHI raises that head to 2.58050 ms (+2.594%) because it writes mapped system/GTT pages, while combined GPU time remains flat; this small penalty exceeds the lease-fence work that direct mode removes and provides a mechanism consistent with the absent RDMA gain. All live correctness and accounting gates passed, and the production TCP-v3 services were restored and smoked. Full evidence: `bench/results/2026-08-06-ds4-rocm-event-profile.md`.

## 2026-08-05 — Repair NHI lost interrupts and fresh-open path activation
- **What:** Added zero-copy patch 10 progress diagnostics, then used live descriptor/IRQ snapshots to separate two failures. Patch 11 flushes posted non-auto-clear MSI-X acknowledgments through a safe NHI readback; patch 12 starts and fully primes RX before enabling XDomain DMA paths and completes partial ring-buffer cleanup. The full twelve-patch series passes 38/38, strict checkpatch, independent review, byte-identical two-host builds, installed/initramfs hash verification, and a clean reboot.
- **Why:** At the original asymmetric stall, hardware and the tail descriptor had completed while software and the interrupt count were one event behind; a diagnostic-only worker kick harvested it. The separate immediate-reopen stall showed posted TX but zero hardware completion on both peers and did not respond to a kick, matching the stream driver's path-before-ring/E2E-credit ordering rather than the interrupt race.
- **Impact:** With kicks disabled, the pair passed 2,880 exact DS4-shaped exchanges across 27 sessions, 26 close/reopen transitions, and 921,600 descriptor completions without kernel transport errors. Required-NHI CPU-copy completed 22 full-model requests and confirmed-mapped NHI completed 12 with zero mismatches or restarts. Five controlled 800-token cohorts measured 10.9421 tok/s CPU-copy NHI, 10.9438 mapped NHI, and 10.9540 TCP v3: no speedup (-0.1081%/-0.0930%). Temporary NHI service overrides were removed and the production TCP-v3 units restored; NHI is now a qualified experimental candidate pending longer soak and active peer-reboot tests. Full evidence: `bench/results/2026-08-05-nhi-msix-rx-prime-fix.md`.

## 2026-08-05 — Keep NHI experimental; select descriptor-framed TCP for production
- **What:** Built, independently reviewed, deployed, and loaded zero-copy patches 6 and 7 on both Strix hosts, then rebuilt both initramfs images with the matching module set. Enabled translated/default IOMMU operation, installed `0660 root:tbstream` access, and installed the allocator/follower lifecycle units with stable `/run/ds4-tbstream/device` publication. Symmetric raw and DS4 backend gates continued to pass, but full-model NHI intermittently lost receive/completion progress in both mapped and CPU-copy modes. A model-independent raw gate using the actual asymmetric request/response geometry reproduced the class of failure after 25 exchanges, and an immediate fresh-open retry failed.
- **Why:** The earlier equal-size bidirectional harness did not cover the model's repeating 17/17/33/65-frame requests followed by 127-frame responses. Reproducing the stall without ROCm or the DS4 TCP control channel isolates it below mapped model I/O and prevents a component-gate pass from being mistaken for production readiness.
- **Impact:** Any configuration that supplies an NHI device is lab-only and reliability-blocked. Production uses `--dist-transport auto` with no `--dist-nhi-device`, which keeps protocol-v3 descriptor framing while selecting TCP; explicit `--dist-transport tcp` is the legacy-v2 compatibility path. No trustworthy NHI tokens/second gain exists because failed or replayed runs are not benchmark samples. The full-model TCP A/B is 11.38 tok/s on legacy v2 versus 11.28 tok/s on descriptor-framed v3, about -0.9%; token-weighted five-run aggregates differ by only +0.005%, so there is no measured increase. Both hosts booted the rebuilt initramfs successfully. A boot-order HopID collision found during that gate was recovered by a clean two-host reset and fixed operationally with a carrier-first, exact-9/9 lifecycle policy; an enabled peer reboot, v3 reconnect, and final full-model smoke now pass.

## 2026-08-04 — Pass the live DS4 backend gate; record production blockers
- **What:** Added an opt-in, model-independent DS4 two-host harness with strict HELLO/control records, v3 bulk descriptors, generation/sequence identity, generation-seeded full-payload verification, required path assertions, and literal physical-ring-wrap accounting. Built identical CPU and ROCm binaries on both Strix hosts with an embedded ROCm RUNPATH, then ran the audit-final sources over `/dev/tbstream0`: both endpoints verified 32 messages each way and 4.01 wraps; CPU used 32 copy messages, while ROCm used 28 mapped plus 4 wrap-fallback copy messages per direction. Also fixed mapped-RX semantic rejection to consume/repost its lease before sending an ordinary WORK error, added a regression, documented least-privilege udev access, and completed the local/remote test matrix.
- **Why:** Raw ping-pong proves the driver, but not DS4's descriptor, generation, sequence, lease, and fallback contract. The new gate exercises that boundary without loading a second 156 GB model beside the production pair or conflating a CPU fallback with a mapped pass.
- **Impact:** The live backend correctness gate and all host-independent tests pass; postflight left the production PIDs, sockets, API, device, modules, boot state, and `/opt` deployment unchanged. Production readiness is still blocked by `amd_iommu=off`, root-only device permissions, the old deployed DS4 binary, and the maintenance-only model equivalence, reconnect/fault, and 8–24 hour soak rows.

## 2026-08-04 — Complete DS4 single-link protocol, NHI, mapped-slot, and generation stages
- **What:** Added DS4 protocol v3 HELLO/ACK/READY negotiation and exact 64-byte bulk descriptors; implemented the persistent `/dev/tbstream` CPU-copy backend and mixed completion dispatcher; integrated contiguous 32-bit ROCm mapped TX/RX leases with explicit ownership synchronization; and added per-connection generations, directional sequences, repeated NHI identity envelopes, single-link enforcement, and fail-closed malformed/stale handling. Automatic negotiation retains descriptor-framed TCP and legacy v2 reconnect fallbacks before NHI activation.
- **Why:** These are integration stages 4–6: they make TCP control and NHI bulk unambiguous, remove heap staging for the mapped 32-bit path, close the HELLO-to-first-WORK activation race, and prevent stale descriptors/events or partial control writes from being accepted in a new or reused generation.
- **Impact:** Host-independent transport/protocol tests pass under strict warnings, ASan/UBSan, TSan, Linux analyzer/ROCm-host compilation, and 100-run repetition. The software path is limited to one remote worker link; reduced-width or wrapping payloads use CPU-copy NHI. Graph tensors are copied directly to/from registered driver slots, but kernels do not yet execute against `lease_device_ptr`. The next gate is the two-Strix `/dev/tbstream` + ROCm end-to-end matrix, including output equivalence, batches 1/2/4/8, prefill/decode/logits, fallback, disconnect/reconnect, and soak.

## 2026-08-04 — Establish persistent DS4 peer transport and TCP-only bulk split
- **What:** Preserved and re-ran the dirty `rocm-mxfp4-experts` baseline, then replaced stack-temporary DS4 transport bindings with opaque, reference-counted peer transports. Registry entries own duplicated sockets; route plans retain transports; inbound worker and forwarder connections own one transport for their lifetime. WORK is now received as typed fixed metadata, tokens, hidden bulk, and route data, while RESULT metadata/telemetry and bulk use the same persistent object. Errors remain on the TCP control path. Added focused ownership, nonempty 16-bit bulk, alignment, disconnect/SIGPIPE, and ordered-prefetch-rejection tests.
- **Why:** These are the first three DS4 integration steps required before a long-lived NHI mapping and completion dispatcher can safely exist. They remove payload-scoped ownership and the whole-WORK temporary buffer without prematurely changing the deployed protocol.
- **Impact:** Valid v2 TCP framing and fallback behavior are unchanged; strict warning builds and focused tests pass. The NHI stub remains intentionally unavailable. Next is a negotiated v3 descriptor with separate control/bulk lengths, followed by a persistent CPU-copy NHI backend for the initial single device-to-device link.

## 2026-08-04 — Deploy five-patch stream stack; pass one-way and ROCm gates
- **What:** Built and deployed the eight-patch USB4STREAM backport plus all five zero-copy patches on `max` and `max2`, installed the exact-vermagic module set, reloaded it live, restored the stream and Thunderbolt IP link, and rebuilt both initramfs images. Added patch 5 after a verified 64 KiB run exposed that last-frame-only RX interrupts could suppress a following CLOSE; patch 5 interrupts both message-boundary descriptors. Ran one-way verified/throughput tests, exclusivity regression, a targeted interrupt check, and full-pool ROCm mapping on both GPUs.
- **Why:** This closes the hardware decision gates before DS4 adopts persistent NHI mappings and records the boundary condition found only under a receive-ring wrap.
- **Impact:** Verified one-way sender rates were 100.5 MB/s at 4 KiB, 512.3 MB/s at 64 KiB, and 1052.1 MB/s at 1 MiB. A final 512 x 1 MiB run reached 955.9 MB/s with only 24 aggregate Thunderbolt interrupts per host. Both Radeon 8060S GPUs read and wrote all 16 MiB of the mapped TX pool correctly. Module paths, vermagic, initramfs contents, stream devices, and IP connectivity all pass; the DS4 CPU-copy backend is cleared to proceed. Full evidence: `bench/results/2026-08-04-usb4stream-zc-oneway-rocm.md`.

## 2026-08-04 — Prepare reproducible RX/ROCm gates; pin DS4 integration boundary
- **What:** Reconciled the missing RX interrupt-suppression work as backward-compatible zero-copy patch 4 (`POST_RX_FLAGS` with final-frame interrupt mode), with automatic userspace fallback to legacy `POST_RX`. Added `tools/rocm-map/tbstream-hip-map`, which safely tests GPU read/write access to TX pool pages without touching NHI-owned RX pages. Fixed `host-build.sh` to incrementally apply all eight backport and four zero-copy patches, and recorded the required persistent-connection/wire-protocol DS4 refactor in `docs/DS4_INTEGRATION.md`.
- **Why:** The logged deployed behavior was not reproducible from the repository, the build script stopped after the base backport, and DS4's current stack-temporary bindings cannot own persistent NHI mappings or safely demultiplex mixed completions.
- **Impact:** All offline-applicable userspace, patch, script, and documentation gates pass. The ROCm binary cannot be compiled on the macOS development host because `hipcc` is unavailable; patches 3/4, `ztx`/`zrx`, and the ROCm mapping gate require an explicitly authorized rollout to the two Linux hosts before DS4 backend implementation should cross the hardware decision gate.

## 2026-08-04 — Add one-way zero-copy benchmark modes and harden slot ownership
- **What:** Added `ztx`/`zrx` to `tools/pingpong` for one-way zero-copy throughput, with full TX-completion draining, message-geometry checks, and optional bit-pattern verification. Fixed batched `REAP` handling that could discard trailing RX events, hardened numeric parsing and sub-8-byte verification, added strict macOS/Linux builds, and documented the implemented zero-copy series. Added patch 3 requiring exclusive zero-copy device ownership so separate opens cannot share and corrupt device-wide slot cursors.
- **Why:** This is the next recorded step after the RTT benchmark. One-way measurement separates NHI transfer time from `zpong`'s intentional whole-message store-and-forward echo, while exclusive ownership makes the proof-of-concept UAPI's cursor model enforceable.
- **Impact:** Offline build and read/write socketpair tests pass. The new zero-copy modes and exclusivity patch still need deployment on the two Linux hosts for hardware measurement. The historical RX-interrupt-suppression note is now represented by backward-compatible patch 4; its exact compatibility with any host-only prototype must be checked during rollout.

## 2026-08-02 19:18 — Ship zero-copy slot mode; CPU and interrupts halve, RTT is an echo artifact
- **What:** Implemented, deployed, and measured the zero-copy UAPI (`kernel/zerocopy/` 2-patch series): `RING_FRAME_NO_INTERRUPT` in nhi.c, mmap'd frame pools + `SUBMIT_TX`/`POST_RX`/`REAP` ioctls + `TBSTREAM_DATA_MORE` message delimiting in stream.c, `zping`/`zpong` in pingpong. Verified bit-exact end-to-end. In parallel, an agent landed the DS4 transport vtable (`ds4_transport.[ch]`, TCP backend byte-identical, NHI stub) on `rocm-mxfp4-experts`; clean build, 98/98 placement tests.
- **Why:** Plan step 3. At 1 MiB the echo side spends **-48% CPU** (0.473→0.245 s/1000 msgs) and **-48% interrupts** (~269→~139/msg) at wire-parity throughput. RTT is parity-to-13%-worse — NOT a transport regression: rw `pong` pipelines within a message via per-frame wakeups while zc REAP is per-message store-and-forward, which matches DS4's real consume-whole-tensor pattern (full analysis in `bench/results/2026-08-02-usb4stream-zc.md`).
- **Impact:** Gotchas: configfs stream groups pin `thunderbolt_stream` — rmdir before module reload (host-install.sh handles it now); stream configfs settings reset on reconfigure (a "tuned" comparison silently ran at default throttling once). v2: `POST_RX` flag to suppress all-but-every-k-th RX interrupt (~139→~2/msg). Next: `ztx`/`zrx` one-way modes, DS4 NHI backend on the new UAPI, `hipHostRegister()` gate (plan step 4). DS4 note: worker WORK-frame receive is a fused control+bulk read; needs restructuring when NHI framing lands.
## 2026-08-02 18:54 — First USB4STREAM data between hosts; decision gate 1 passed
- **What:** Backported stack live on both maxes; stream `ds4` on service 1-2.1 → `/dev/tbstream0`. Verified pingpong: 4 KiB QD1 RTT **22.9 µs p50** (TCP ICMP floor: 64 µs, ~2.8×), bulk ~1.15 GB/s = TCP parity. Ring 4096 / throttling 0 tuning was near-noise → cost is per-4KiB-frame copy/processing (~4.4 µs/frame at 64 KiB), exactly what the zero-copy UAPI targets. Full tables in `bench/results/2026-08-02-usb4stream-stock.md`.
- **Why:** Plan step 2 (stock baseline) and decision gate 1: stock materially beats TCP on latency → zero-copy kernel work is justified.
- **Impact:** Gotchas: max2's post-install boot loaded stock `thunderbolt.ko` (initramfs-era load; broke dependent modules, TB net down) — fixed by live reload + `dracut -f`, but watch both hosts' next reboots. Concurrent readers on one tbstream fd steal frames (incl. CLOSE) — `pkill -f pingpong` before reconfiguring; `ring_size`/`throttling` EBUSY while any fd is open. Next: `RING_FRAME_NO_INTERRUPT` nhi.c change + mmap slot-pool UAPI + `pingpong zc`; DS4 transport abstraction can proceed on the Mac.
## 2026-08-02 18:21 — Backport compiles on hosts; fix cross-module MODPOST
- **What:** First host build of the resolved series: `thunderbolt.ko` + `thunderbolt_stream.ko` compiled and linked clean on both maxes — the hand-resolved conflicts are compile-proven. `thunderbolt-net.ko` failed MODPOST (`tb_ring_throttling` undefined): it's built as a separate `M=` invocation and can't see symbols the freshly built thunderbolt.ko exports. Fixed in `host-build.sh` with `KBUILD_EXTRA_SYMBOLS=$SRC/drivers/thunderbolt/Module.symvers`.
- **Why:** Stock Module.symvers (kernel-devel) predates the backport's new exports; any two-stage external build with cross-module deps needs the first stage's symvers passed explicitly.
- **Impact:** Gotcha to remember: the `ssh ... | tail` pattern masked the build's exit code (background task reported success) — check artifacts, not exit codes, for piped remote builds. Rebuilds running; next is install+reload on max, sign+stage on max2.
## 2026-08-02 18:19 — Identify test hosts, hit max2 Secure Boot blocker, record TCP latency floor
- **What:** Test hosts are `jryates@max` (192.168.1.84) and `jryates@max2`, Fedora 43 (not Ubuntu as STRIXHALO.md suggests), kernel 7.1.5-101.fc43, TB link up as `thunderbolt0` 10.99.0.1/2 MTU 65520. Wrote `kernel/scripts/host-{build,install,sign}.sh`, synced repo to `~/strix-rdma` on both, started module builds. Baseline ICMP RTT over the link: min 37 / avg 64 / max 197 µs.
- **Why:** MODVERSIONS is off on Fedora → only exact vermagic match needed (LOCALVERSION=-101.fc43.x86_64 against vanilla v7.1.5). Gotcha: **max2 has Secure Boot + lockdown=integrity** (max does not) — unsigned modules won't load; needs MOK enrollment (host-sign.sh prepares it) or SB disabled in BIOS, either way a physical-console step.
- **Impact:** max can go end-to-end today; max2 blocks the two-sided stream smoke test until the user does the console step. The 64 µs avg RTT is the small-packet floor the NHI stream must beat at QD1.
## 2026-08-02 16:54 — Confirm NHI core change is required; map Mac-buildable work
- **What:** Verified in source that `ring_write_descriptors()` (`drivers/thunderbolt/nhi.c:245`) hardcodes `RING_DESC_POSTED | RING_DESC_INTERRUPT` on every descriptor — no client control. Also confirmed ds4 builds natively on this Mac (arm64/Metal) and Docker/Colima/Lima are available for Linux kernel compile-checks.
- **Why:** Settles the plan's open question: per-message TX completion interrupts for the zero-copy transport need a small nhi.c core change (a `no_interrupt` flag on `struct ring_frame`); RX side must instead use the existing `throttling` knob/busy-poll since RX descriptors are pre-posted before message boundaries are known.
- **Impact:** Work order while hosts are busy: container compile-verify of the hand-resolved backport series, then DS4 transport abstraction (Mac-native, TCP loopback testable). Gotcha: the kernel tree is subtly broken on macOS's case-insensitive FS (`xt_CONNMARK.h`/`xt_connmark.h` collide → phantom diffs in `linux/`); kernel container builds must clone inside the container FS, never bind-mount the Mac checkout.

## 2026-08-02 16:23 — Scaffold repo and produce verified USB4STREAM backport series
- **What:** Scaffolded the repo (plan doc, kernel/, tools/, bench/), sparse-cloned mainline, and extracted the USB4STREAM work as a 9-commit series ending at `6db21d81`. Test-applied it on v7.1 with `git am -3`, resolved 3 conflicts (property.c `depth` param; xdomain.c hunks from unrelated refactors `4d5fc3f4`/`8b406099` not in the series), dropped the KUnit-only patch, and saved a clean-applying 8-patch series to `kernel/backport/v7.1/`. Wrote and loopback-tested `tools/pingpong` plus ConfigFS setup/bench scripts.
- **Why:** Hosts run 7.1.5; USB4STREAM lands in 7.2. Key finding: `stream.c` needs core APIs absent from 7.1.5 (`tb_ring_flush`, `tb_ring_throttling`, `tb_service_properties_changed`, ConfigFS, `tb_property_merge_dir`, new `tb_service`/`tb_ring` fields), so an out-of-tree module against stock headers is impossible — but the delta is confined to the thunderbolt stack, so rebuilding only `thunderbolt.ko` + `thunderbolt-net.ko` + `thunderbolt_stream.ko` as a set suffices (procedure in `kernel/README.md`).
- **Impact:** Unblocks step 2 of the plan (stock `/dev/tbstreamX` baseline) without a full kernel rebuild. Next: TCP baseline on hosts, then build/install the backported module set and run `bench/scripts/run-pingpong.sh`.
