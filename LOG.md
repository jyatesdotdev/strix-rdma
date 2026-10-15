# LOG
Running log of noteworthy work; newest first.

## 2026-10-14 — control-ring E2E A/B: no effect; reboot hang; shutdown hook
- **E2E A/B (round seventeen).** `ctl_e2e=0` runs the control rings without
  E2E, leaving one E2E pair per host. Two boots per arm (A, B, A, B), 11 runs
  each, all passed. READ runs with a natural stall: 4 of 12 with control-ring
  E2E on, 3 of 12 off. Throughput was the same, with no bad frames or QP
  errors. So a second E2E pair is not why the NHI withholds a burst's last
  frame. `ctl_e2e` stays on.
- **The A/B runner never read the arm back** (a quoting bug). The arm is
  supported by the journal, the modprobe route and the throttle readback.
  `bench/scripts/verbs-e2e-ab.sh` now reads it back and stops on a mismatch.
- **A reboot hung on max** (1 in 22 that day): kernel alive, no user space,
  Thunderbolt domain still answering 6.8 minutes in. The watchdog did not fire,
  so it needed a power cycle. The cause was not captured.
- **`sn_shutdown()`:** stops the poller, then the rings, before the NHI's
  shutdown tears the domain down. The wait is bounded. It is one candidate
  removed, not a demonstrated fix. Source contract added (67 green, three
  mutations caught).
- **Capture:** `tools/scripts/arm-shutdown-capture.sh` (netconsole plus
  `initcall_debug`) and `netconsole-listen.py`. One simultaneous reboot with
  sessions up was clean. The record shows the hook (0.5 ms) and the NHI's
  domain teardown (about 1 ms). Gates before and after it passed: `write 200`,
  `read 500`, the SEND smoke, `send 2000` ×5 at 411.8–418.6 MiB/s.

## 2026-10-12 — wire version 2: mixed pairs refused at the first frame; READ_ACK probe
- **`SN_VERSION` is 2.** Correlation is that version 2 adds the `SN_STAGED`
  status and the `SN_READ_PROBE` frame. A frame from another version is refused
  at the first frame, counted as `version_mismatch` (never as corruption) and
  reported: "peer speaks wire version N, this build speaks 2". Previously a
  mixed pair formed a session and failed mid-operation with a bad_frames trail.
- **`SN_READ_PROBE` (opcode 16)** closes ok 24: a zero-copy READ response is
  never re-burst, so a READ_ACK lost while the connection was idle failed the
  responder's QP and could strand a crossing operation. The responder now asks
  and the requester answers from its recorded completions.
- Zero-copy READ completions now record `last_read`/`have_last_read` like the
  staged path always did; without that a probe had nothing to answer from.
- Offline: 39/39 isolated, 66 green, and three mutations each caught by the
  intended test. Wire tests cover the new opcode and the version check.

## 2026-10-12 — zero-copy SEND into an unaligned receive buffer no longer fails both QPs
- **Bug:** a zero-copy SEND (aligned source, more than 64 KiB) matched to a
  receive buffer that is not frame aligned failed both QPs. That is legal in
  verbs and common, because `malloc` returns +16. Reproduced with the new
  `--recv-off`: the first iteration failed with "remote invalid request".
- **Fix:** the receiver answers with the new ACK status `STAGED` (5),
  consuming nothing. The sender resends the same operation staged. `SEND` and
  `ZC_SEND` replay as one class, so a late duplicate is harmless.
- **Tests:** ok 37 failed first (37/37 isolated), and the identity mutation
  is caught.
- **Hardware:** +16 and +4097, on CPU and GPU memory, all byte-for-byte, with
  400 refusals and 400 staged resends per side per run. Regressions pass and
  throughput is unchanged.
- **Tool:** the overrun check now scans only the bytes outside the range. It
  had been costing default-geometry runs about 15% of their wall rate.
- `docs/ZEROCOPY_VERBS.md`, round fifteen.

## 2026-10-12 — GPU-direct G3 and G4: staged paths through GPU memory; the release is required
- **G3:** `strix-verbs-smoke --len/--off` sets per-slot transfer geometry.
  WRITE, READ and SEND on CPU and GPU memory, at 3,000 B (staged), 1 MiB at
  offset 100 (staged) and 2,093,055 B (zero-copy with a partial last frame):
  all 18 combinations pass byte-for-byte with no overrun. This was also the
  first hardware run of the staged paths at volume.
- **G4:** `--gpu-kernels` runs GPU kernels that produce and check the data,
  with a positive control and a "hot" mode that keeps the probed lines
  resident in L2.
  - Release and acquire on: 0 mismatches.
  - Acquire off: 0 mismatches.
  - **Release off: 5–97% of iterations stale**, so the producer's
    system-scope release is required.
  - The first, non-hot G4 run had passed every arm because its clears evicted
    the lines under test.
- `docs/GPU_DIRECT_VERBS.md`, "Results". Remaining: hot-unplug with GPU MRs.

## 2026-10-12 — GPU-direct v1: GPU memory as verbs MRs, GPU-to-GPU byte-for-byte
- **What:** `ibv_reg_dmabuf_mr` on `hipMalloc` memory now works.
  - **Kernel:** `reg_user_mr_dmabuf` imports through
    `ib_umem_dmabuf_get_pinned_with_dma_device`, attached to the NHI (this IB
    device has no DMA device of its own). The data path uses the exporter's
    mapping as is, with no re-map and no sync. A `dma_buf_vmap` gives the
    staged paths CPU access.
  - **Provider:** `reg_dmabuf_mr` forwards to `ibv_cmd_reg_dmabuf_mr`.
  - **Smoke tool:** `--gpu` mode, with HIP `dlopen`ed.
  - Design in `docs/GPU_DIRECT_VERBS.md`.
- **Validated:**
  - GPU-to-GPU `write`/`read`/`send 2000` byte-for-byte.
  - Forced READ fallbacks through GPU memory: 18 fallbacks, all byte-for-byte.
  - Three NHI attachments per host while live, zero after about 30 runs (no
    leaks).
  - `kill -9` mid-transfer releases cleanly.
  - Offline: object tests ok 5/6 with mutation checks, and the provider test
    on the hosts' rdma-core 58.
- **Throughput:** transfer-only is now printed by every verified mode. GPU
  memory WRITE median is 1,289 MiB/s per direction against 1,008 for CPU
  memory (5 runs each, non-overlapping ranges); SEND is 1,183 against 1,052
  (overlapping).
- **Finding:** earlier verified-mode wall rates (410–490 MiB/s) were bound by
  the test tool's own pattern work. The link moves about 1.0–1.3 GB/s per
  direction.
- **Open:** G4 (GPU-kernel stale-cache gate), hot-unplug with GPU MRs, and
  small/unaligned operations on GPU MRs. All are in `TODO.md`.

## 2026-10-12 — first hot-unplug test: two pulls, both recovered automatically
- **Idle pull:** both hosts removed the verbs device cleanly. Replugging,
  the XDomain was found again and the driver re-probed and reached ACTIVE about
  1.5 s later, with every mode then passing at full rate.
- **Mid-transfer pull** (bidirectional zero-copy `read 20000`, about 420 MiB/s
  each way): both applications got `EIO` and exited within about a second. Both
  kernels stayed clean. After replugging the link was ACTIVE about 1 s later,
  and full `read`/`write`/`send 2000` runs passed byte-for-byte. No wedge.
- **Driver bug:** `sn_rings_stop()` re-stopped already-stopped data rings when
  retried on EAGAIN (hot unplug makes that routine), so the core warned "TX ring
  2 already stopped". Fixed with a `data_rings_started` guard and a
  source-contract test; the mid-transfer pull was then warning-free.
- **Cosmetic:** the idle poller slept uninterruptibly, showing as D state and
  adding 1 to the load average. It now uses `wait_event_idle_timeout`.
- **Process error:** a remote `pkill -f hp-monitor` matched its own shell and
  aborted a build-and-reboot step. The step-1 monitor logs were then deleted;
  the kernel log was recovered from the persistent journal.
- Remaining unplug cases are in `TODO.md`.

## 2026-10-12 — 90-minute endurance: 120/120
- 40 rounds of verified `write 2000`, `send 2000` and `read 2000` on one boot,
  all fault injection off. All 120 runs passed byte-for-byte in 5,445 s, about
  1.83 TiB per direction, with `bad_frames 0` and no QP error.
- Medians: write 438.70, send 452.78 and read 412.23 MiB/s.
- Natural timeouts, all recovered: 18 READ runs (25 fallbacks, all served) and
  4 WRITE runs (5 ACK probes).
- `docs/ZEROCOPY_VERBS.md`, round fourteen.

## 2026-10-12 — ACK loss injected at volume
- `ack_drop=N` drops every Nth ACK or READ_ACK. With `ack_drop=100`, 3 runs
  each of `write 2000`, `send 2000` and `read 2000` passed byte-for-byte, about
  80 drops per side per run. SEND/WRITE recover through exactly one ACK probe
  each, and READ through the next request's implicit acknowledgement. No QP
  error. Known limit kept: a lost READ_ACK followed by an idle connection fails
  the responder (ok 24). `docs/ZEROCOPY_VERBS.md`, round thirteen.

## 2026-10-12 — natural timeouts traced to a withheld last frame at the NHI
- **What:** a new `stall_snap` `tmo_*` snapshot, taken at every zero-copy
  timeout, plus `since_land_us` and `max_pass_gap_us`. On a fresh boot, 6 of
  10 `read 2000` runs each had one natural fallback.
- **Finding:** the requester is always missing exactly the burst's last 4 KiB
  frame and has a buffer posted for it. The responder's NHI holds that frame
  posted but untransmitted for more than 67 ms, and then sends it. Both pollers
  are current, and core and hardware ring indices agree. This is consistent with
  a withheld E2E credit grant, and matches upstream reports of E2E credit-return
  trouble on ASMedia and on Strix Halo with multiple E2E rings.
- **Impact:** about one 100–200 ms stall per 8,000 operations on a bad boot,
  handled correctly by recovery, under 1% of throughput. Poller gaps of up to
  30 ms were also measured; they are not the cause.
- **Open (a design decision):** running the control rings without E2E, or a
  software credit scheme on the data ring. Recorded in
  `docs/ZEROCOPY_VERBS.md`, round twelve.

## 2026-10-12 — zero-copy SEND/WRITE: the same E2E starvation, reproduced and removed
- **What:** zero-copy SEND and WRITE (both on by default) still restarted the
  receiver's data RX ring on a duplicate burst descriptor, the one-sided restart
  that round nine showed starves the peer's data TX. A lost or late ZC_READY
  reaches it.
- **Reproduced:** `zc_send_fault=100` (new diagnostic parameter) failed a
  `write 2000` with RETRY_EXC at iteration 52 on both hosts. `stall_snap` showed
  posted TX frames against matching posted RX buffers in both directions, with
  nothing moving. The same boot passed with no injection.
- **Fix:** a sender bursts each operation once and after a drained timeout
  only probes for the ACK. A receiver answers a duplicate with ZC_READY when
  nothing has landed and otherwise ignores it. `sn_data_recv_restart()` is
  deleted.
- **Validated:** fresh boot with `zc_send_fault=100`: `write 2000` 20/20 and a
  new verified `send 2000` mode 20/20 byte-for-byte, 1,600 injected faults per
  side per mode, no QP error. READ with its fault injection and every mode with
  faults off passed on the same boot.
- **Also:** ok 10 and ok 14 ("lost burst") had never lost a burst. They were
  rewritten to really drop it. `bench/scripts/` gained the smoke and streak
  runners, and `strix-verbs-smoke` gained `send N`, a SEND/RECV mode with
  byte-for-byte verification.
- **Evidence:** `docs/ZEROCOPY_VERBS.md`, round eleven.

## 2026-10-12 — zero-copy RDMA READ on by default
- **What:** `SN_ZC_READ = 15` (added 2026-10-11) is now the default for eligible
  READs. It runs at 408–418 MiB/s against 62.6 MiB/s staged, ~6.6×.
  `zc_read=0` is the kill-switch. The bar it cleared on a fresh boot of both
  hosts: 20 consecutive `read 2000` runs (16,777,216,000 B per direction each)
  passing byte-for-byte with the timeout recovery forced and firing in every
  run. In those 20 runs, 92 fallbacks fired and all 92 were served, with no
  QP error. A 3-run stress addendum at 10× the fault rate also passed.
- **The path there:** the first hardware runs failed once retries started. The
  original recovery re-burst on a duplicate request and restarted the
  requester's data RX ring under a live burst, and both turned out to be
  unsound. Recovery was rebuilt as a requester-driven staged-`SN_READ` fallback
  for the same operation. That fallback worked, but a residual remained: the
  zero-copy READ *after* a served fallback sometimes stalled. The best streak
  was 5 runs.
- **Root cause, found by instrumentation:** `native_stats` now exports ring
  and QP state, and a new `stall_snap` attribute freezes the driver's ring
  indices, the QP state and `tb_ring_get_stats()` for both data rings at the
  moment a stall is detected. 15 natural runs produced no timeout, so a
  `zc_read_fault=N` parameter forces one. Two induced stalls out of two showed
  the same thing: the requester's NHI held 63 posted RX descriptors, the
  responder's held 63 posted TX descriptors it never sent, and TX completed
  equalled RX completed exactly. This was E2E credit starvation, beginning at
  the fallback completion's one-sided `tb_ring_stop`/`tb_ring_start` of the
  requester's data RX ring. Restarting both ends cleared it.
- **Fix (in this driver; the thunderbolt core is untouched):** a data RX ring
  is never restarted one-sided on this path. The responder always posts and
  drains its zero-copy reply in full before serving the staged one, reverting
  7977ae2's truncation. The requester completes only after both the staged
  reply and the abandoned burst have landed, so every posted buffer is consumed
  and nothing needs reclaiming. A reply that never lands, which lossless E2E
  cannot produce, now fails loudly instead.
- **Tests:** the harness models E2E starvation (`e2e_model`). ok 31
  (`zc_read_fault`) and ok 32 (round nine replayed) failed first. ok 19, 28,
  29 and 30 were rewritten to the new contract. In isolation the engine suite
  passes 32/32 with the fix; with the fix reverted, exactly those five fail.
  `make check-native-verbs` is 54/54.
- **After the flip:** both hosts rebooted on the default-on build with no
  override and report `zc_read=Y`. The SEND smoke, `write 200` (374.80 MiB/s)
  and `read 2000` (411.12 MiB/s) all passed byte-for-byte. The `read 2000`
  rode out one natural timeout per side through the fallback, uninjected.
- **Open:** a retried zero-copy SEND/WRITE descriptor still restarts the
  receiver's data RX ring (`sn_receive_zc`). It has not misbehaved in
  validation, but it is the same pattern.
- **Evidence:** `docs/ZEROCOPY_VERBS.md`, rounds one to ten and "Outcome";
  commits 2ea27f0, ebec8f0, 1ab0a0b, 65bab6b and this one.

## 2026-10-10 — repository generalised; RDMA READ/WRITE hardware-validated
- **What:** The repository stopped being a single-workload tensor transport and
  became general RDMA support between two Strix Halo hosts. Every artifact of
  the original workload-specific framing was deleted rather than renamed — 620
  files, including all of `bench/results/`, the workload-shaped traffic tool,
  the managed-lifecycle reconcile subsystem with its systemd units, udev rule,
  modules-load and sysconfig files, the patch-swap helper, the profile-analysis
  scripts and the workload integration contract doc. Docs were reframed, and figures whose
  only provenance was a deleted result file are now quoted inline with their
  measurement dates. `git grep -i` for the old workload names is empty;
  `make`, `make check` and CI are green.
- **Kept deliberately:** `tools/tests/` and `tools/systemd/tbstream-lifecycle.md`
  in reduced form. What remains is workload-free general infra — one
  least-privilege assertion that `99-tbstream.rules` keeps `MODE=0660` and
  `GROUP=tbstream` on a DMA-mappable character device, and a doc that now states
  plainly that no systemd units are involved and that the native verbs path does
  not use this interface at all.
- **Zero-copy RDMA WRITE:** `SN_ZC_WRITE = 14` added. An eligible WRITE now
  places payload straight from MR pages into the peer's remote MR instead of
  staging through `sn_mr_copy()`. On by default; `zc_write=0` is the
  kill-switch. Unlike `SN_ZC_SEND`, which clears address/rkey because payload
  lands in a receive WQE the peer posted, `SN_ZC_WRITE` carries both so the
  responder can resolve the landing zone.
- **The bug hardware found:** `write 200` failed 3/3 on payload verification —
  retry-correlated, with both the failing iteration (22, then 32) and the
  failing host moving between runs. Cause: restarting a burst re-sends the
  descriptor, the peer zeroes `zc_received` for it, and frames from the
  superseded attempt that were still queued in the data TX ring then get counted
  into the new one. `sn_data_recv_restart()` flushes frames already received but
  cannot recall frames still in flight, and a 2 MiB burst is 512 frames against
  a 64-slot ring, so partial posting is the normal case. The byte count
  overshot, the for-CPU sync that keyed on exact equality was skipped, and a
  buffer assembled from two attempts completed as `IBV_WC_SUCCESS`.
- **Fix:** the retry path defers the restart until `sn_data_tx_drained()`,
  bounded by the retry budget so a ring that never drains fails the QP with
  `IB_WC_RETRY_EXC_ERR`; plus an overshoot guard that fails the QP with
  `IB_WC_GENERAL_ERR` instead of completing. The offline suites could not catch
  this because their `sn_data_send_burst` is atomic — one `memcpy` plus an
  immediate byte count — so a partial burst, a stale in-flight frame and an
  overshooting count were all unrepresentable. `ok 17` now models it by losing
  the completion ACK and refusing to drain. Reaching the transient `SN_ZC_SENT`
  state needs hand-driven stepping: `sn_ack()` only queues into the control ring,
  so the responder must be progressed before its `ZC_READY` can be delivered.
- **Validated:** after a clean reboot and bring-up, `write 200` three times and
  `write 2000` once all passed — 15.6 GiB (16,000 MiB) per direction verified byte-for-byte
  at 356–456 MiB/s, `bad_frames` 0, 10,400 zero-copy bursts, exact
  `placed_write_bytes` accounting. `read 200` passed over 1,677,721,600 bytes
  byte-for-byte. Zero-copy WRITE is ~7× the staged path (449.78 vs 58.81 MiB/s).
  `check-native-verbs` went 31 → 38 assertions over the day.
- **Baseline correction:** bulk SEND after the clean reboot measured 1127–1191
  MiB/s (mean 1158 over 5 runs), at or above the 1070–1156 MiB/s previously
  recorded for script-brought-up post-reboot runs. The 1182–1238 MiB/s headline
  band was measured pre-reboot and is not the like-for-like comparison — worth
  remembering before calling a post-reboot number a regression.
- **Operational notes:** `strix_nhi` must still be loaded once per boot; two
  re-probes survived this session but re-probing after heavy bursts can wedge the
  router's DMA path config space, clearable only by power cycle, so it is a
  gamble and not a tuning loop. Set module parameters via `modprobe.d` and take
  one boot per value. The smoke tool's TCP rendezvous port must be open in
  firewalld — a blocked port surfaces as `errno=113` on the RDMA connect and
  reads exactly like a link failure.

## 2026-10-06 — native verbs: full two-host bring-up over the provider (hardware-validated)
- **What:** The experimental native NHI verbs backend (`kernel/verbs/`,
  `providers/strix_nhi/`) passed its full bring-up on the lab nodes (max/max2,
  Fedora 43 / 7.2.8). Two-host SEND/RECV exchange PASS (0/1/3984-byte and 2 MiB
  windows, 18.9 MB, ERR flush), repeated module reload cycles, and an
  unmodified ROCm tensor-parallel inference workload over the provider
  (`--transport rdma --rdma-device strix_nhi0 --rdma-gid-index 0`): both ranks
  bound, 377 MB placed through SEND/RECV, ~140k frames per direction, zero bad
  frames, working 128-token inference, no kernel warnings.
- **The three hardware bugs found in bring-up** (documented in
  `kernel/verbs/README.md`): (1) `sof=0` broke multi-packet frame reassembly —
  >252-byte frames arrived as CRC-flagged pieces with intact content; the fix
  is tbnet's framing `sof=FRAME_START(1), eof=FRAME_END(2)`. (2)
  `RING_FLAG_E2E` is required on both rings (unlike tbnet's RX-only) or large
  transfers wedge on E2E credit accounting. (3) The XDomain in-HopID must stay
  clear of thunderbolt-net's login (HopID 8) — allocate from 10 up — or TB-IP
  re-login breaks and its carrier (which our rings gate on) never rises.
- **Also in this effort:** kernel migration to 7.2.8 (the 20-patch zero-copy
  series rebased; the 7.1 backport series is upstream in 7.2), and a redesign
  of the session handshake to a wire-carried dual HELLO/HELLO_ACK +
  BIND/BIND_ACK exchange (the property channel now carries only static prtc* +
  a write-once `rxhop` rendezvous), which eliminated the stale-snapshot class.
- **Next:** throughput/latency A/B against the stream path (the keep-native-vs
  -stream decision data); hardware RDMA READ/WRITE (the validated consumer
  path is SEND-only); hot-unplug and malformed-peer injection.

## 2026-08-25 — TP exchange probe: 3.2 ms/token — tensor parallelism clears the bar
- **What:** Built `tbstream-tp-exchange` (full-duplex simultaneous 28 KiB
  partial-sum exchanges through imported pools both directions, TCP start
  barrier honoring the never-transmit-toward-inactive-peer rule, in-wave
  float reduce with exact analytic verification) and ran the {spin, reap}
  × {transport-only, +reduce} matrix at sustained cadence on the node
  pair: spin 29.0 µs/exchange (2.61 ms/90-exchange token), spin+reduce
  35.2 µs (3.17 ms), reap 52.3 µs (4.71 ms), reap+reduce 75.6 µs
  (6.80 ms). Zero drops, dmesg spotless, reduce sums bit-exact, and the
  production TCP-served workload untouched throughout. Follow-up
  `hip-event-before-spin`
  verified on both gfx1151 hosts that a service-thread event sync returns
  while a later same-default-stream one-CU spin remains active — the
  exact deadlock-sensitive record-event-then-spin enqueue shape.
- **Why:** The consumer workload's go/no-go criterion for building TP mode was
  sustained sync ≤ 5 ms/token at the real exchange shape.
- **Impact:** **TP clears the bar with 1.8 ms of margin** in the realistic
  spin+reduce arm → projected ~27 t/s vs today's 14.3 (≈1.9×). Full
  duplex costs ~2 µs over half-RTT (rings don't contend); reap loses to
  per-exchange dispatch+sync, not interrupts; the single-workgroup
  UC-payload reduce is conservative. Fidelity notes recorded: in-band
  stamps force whole-pool UC in spin mode (one MTYPE per imported pool)
  at no cost for streaming payloads; RX repost requires both the driver
  event and completion of the final GPU slot reader; and a consumer must
  event-sync, never stream-sync, before host submission. The remaining work is
  the consumer's (ROCm gate ops + NHI TP backend). Measured 2026-08-25 on the
  lab host pair.

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
  side, imported-TX dedicated CLOSE frame), dmesg spotless, and the production
  TCP-served workload untouched throughout. The flood/ratelimit arm was skipped
  deliberately — generating uncompletable CLOSEs IS the wedge trigger.
  Follow-ups: TX-credit watchdog + firmware-vs-driver wedge
  investigation. Validated 2026-08-24 on the lab host pair.

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
- **Why:** Close the remaining pre-integration correctness/fault gates and convert
  the hybrid tensor-parallel proposal from projection to a measured
  transport budget.
- **Impact:** Exchange RTT p50: 4 KiB 20.3 µs reap vs **9.8 µs spin**
  (~4.9 µs one-way); 32 KiB 57.7 vs **45.3 µs** with jitter collapsed (max
  88→47 µs); implied burst bandwidth ~1.6 GB/s/direction. Two findings:
  small `hipMalloc`s share slab BOs (the driver's same-BO rejection caught
  it; tools now pad to dedicated 16 MiB BOs — consumers must verify
  dedicated allocations), and gate-5 coherence is dispatch-scoped — a
  spinning wave never sees NHI writes through GPU L2, so persistent-kernel
  signaling needs uncached flag words (payload stays coarse). At these
  numbers ~120 TP exchanges/token cost ≈2 ms, supporting a ~1.8× hybrid-TP
  decode estimate whose kernel half is now the open question. One polish
  candidate: skip the final CLOSE toward an already-closed peer to silence
  benign flush-timeout warnings. Hosts restored to production and
  smoke-verified. Measured 2026-08-23 on the lab host pair.

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
  before any workload integration, and characterize which acquire mechanism is
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
  gate 7 touches a live inference workload and awaits explicit operator
  go-ahead. Measured 2026-08-23 on the lab host pair.

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
  deferred `max2` replication. Measured 2026-08-23 on the lab host pair.

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
- **What:** Re-audited the NHI, AMDGPU, ROCm, and consumer data paths and ran initial read-only topology/memory checks on both Strix hosts. The GPUs have only 512 MiB of BIOS VRAM and about 124 GiB of GTT, so a workload's large native HIP allocations are system/GTT pages. The installed HIP 7.13 runtime exposes DMA-BUF export; a temporary 4 MiB `hipMalloc` gate on `max` exported successfully, and a diagnostic CPU mmap saw the exact GPU-written pattern. A subsequent importer/synchronization audit used only local Linux, ROCm, and consumer source and did not access either host. The public gfx1151 wheel manifest resolves the exact HIP build to TheRock `6d2136cd12be` and `rocm-systems` `79e85e1468f9`; the apparent `3309c6114a` suffix is a generated package hash, not the public source commit.
- **Why:** Distinguish physically impossible one-sided RDMA from achievable GPU-addressable DMA message passing, find a way around the measured penalty of NHI-owned `hipHostRegister()` pages, and identify the exact ownership contract before writing a live driver.
- **Impact:** True rkey/remote-VA/atomic/GPU-initiated RDMA remains impossible, but fixed native HIP pools should be addressable by the NHI through a bounded DMA-BUF importer. The source follow-up found two critical constraints: current stream `open()` activates paths before an import ioctl can run, and an idle HIP synchronize is not an RX cache acquire. The design now requires lazy path activation, exact SG/offset validation, reverse rollback, direction-correct DMA-BUF mapping, explicit TX system release, and a source-pinned or newly supported RX system acquire. No-traffic mapping and stale-cache gates must pass before workload integration. Full evidence: `docs/GPU_TO_GPU_FEASIBILITY.md`.

## 2026-08-05 — Repair NHI lost interrupts and fresh-open path activation
- **What:** Added zero-copy patch 10 progress diagnostics, then used live descriptor/IRQ snapshots to separate two failures. Patch 11 flushes posted non-auto-clear MSI-X acknowledgments through a safe NHI readback; patch 12 starts and fully primes RX before enabling XDomain DMA paths and completes partial ring-buffer cleanup. The full twelve-patch series passes 38/38, strict checkpatch, independent review, byte-identical two-host builds, installed/initramfs hash verification, and a clean reboot.
- **Why:** At the original asymmetric stall, hardware and the tail descriptor had completed while software and the interrupt count were one event behind; a diagnostic-only worker kick harvested it. The separate immediate-reopen stall showed posted TX but zero hardware completion on both peers and did not respond to a kick, matching the stream driver's path-before-ring/E2E-credit ordering rather than the interrupt race.
- **Impact:** With kicks disabled, the pair passed 2,880 exact workload-shaped exchanges across 27 sessions, 26 close/reopen transitions, and 921,600 descriptor completions without kernel transport errors. Required-NHI CPU-copy completed 22 full-model requests and confirmed-mapped NHI completed 12 with zero mismatches or restarts. Five controlled 800-token cohorts measured 10.9421 tok/s CPU-copy NHI, 10.9438 mapped NHI, and 10.9540 TCP v3: no speedup (-0.1081%/-0.0930%). Temporary NHI service overrides were removed and the production TCP-v3 units restored; NHI is now a qualified experimental candidate pending longer soak and active peer-reboot tests. Measured 2026-08-05 on the lab host pair.

## 2026-08-04 — Deploy five-patch stream stack; pass one-way and ROCm gates
- **What:** Built and deployed the eight-patch USB4STREAM backport plus all five zero-copy patches on `max` and `max2`, installed the exact-vermagic module set, reloaded it live, restored the stream and Thunderbolt IP link, and rebuilt both initramfs images. Added patch 5 after a verified 64 KiB run exposed that last-frame-only RX interrupts could suppress a following CLOSE; patch 5 interrupts both message-boundary descriptors. Ran one-way verified/throughput tests, exclusivity regression, a targeted interrupt check, and full-pool ROCm mapping on both GPUs.
- **Why:** This closes the hardware decision gates before consumers adopt persistent NHI mappings and records the boundary condition found only under a receive-ring wrap.
- **Impact:** Verified one-way sender rates were 100.5 MB/s at 4 KiB, 512.3 MB/s at 64 KiB, and 1052.1 MB/s at 1 MiB. A final 512 x 1 MiB run reached 955.9 MB/s with only 24 aggregate Thunderbolt interrupts per host. Both Radeon 8060S GPUs read and wrote all 16 MiB of the mapped TX pool correctly. Module paths, vermagic, initramfs contents, stream devices, and IP connectivity all pass; the CPU-copy backend is cleared to proceed. Measured 2026-08-04 on the lab host pair.

## 2026-08-04 — Prepare reproducible RX/ROCm gates
- **What:** Reconciled the missing RX interrupt-suppression work as backward-compatible zero-copy patch 4 (`POST_RX_FLAGS` with final-frame interrupt mode), with automatic userspace fallback to legacy `POST_RX`. Added `tools/rocm-map/tbstream-hip-map`, which safely tests GPU read/write access to TX pool pages without touching NHI-owned RX pages. Fixed `host-build.sh` to incrementally apply all eight backport and four zero-copy patches.
- **Why:** The logged deployed behavior was not reproducible from the repository, the build script stopped after the base backport, and the consumer's stack-temporary bindings cannot own persistent NHI mappings or safely demultiplex mixed completions.
- **Impact:** All offline-applicable userspace, patch, script, and documentation gates pass. The ROCm binary cannot be compiled on the macOS development host because `hipcc` is unavailable; patches 3/4, `ztx`/`zrx`, and the ROCm mapping gate require an explicitly authorized rollout to the two Linux hosts before backend implementation should cross the hardware decision gate.

## 2026-08-04 — Add one-way zero-copy benchmark modes and harden slot ownership
- **What:** Added `ztx`/`zrx` to `tools/pingpong` for one-way zero-copy throughput, with full TX-completion draining, message-geometry checks, and optional bit-pattern verification. Fixed batched `REAP` handling that could discard trailing RX events, hardened numeric parsing and sub-8-byte verification, added strict macOS/Linux builds, and documented the implemented zero-copy series. Added patch 3 requiring exclusive zero-copy device ownership so separate opens cannot share and corrupt device-wide slot cursors.
- **Why:** This is the next recorded step after the RTT benchmark. One-way measurement separates NHI transfer time from `zpong`'s intentional whole-message store-and-forward echo, while exclusive ownership makes the proof-of-concept UAPI's cursor model enforceable.
- **Impact:** Offline build and read/write socketpair tests pass. The new zero-copy modes and exclusivity patch still need deployment on the two Linux hosts for hardware measurement. The historical RX-interrupt-suppression note is now represented by backward-compatible patch 4; its exact compatibility with any host-only prototype must be checked during rollout.

## 2026-08-02 19:18 — Ship zero-copy slot mode; CPU and interrupts halve, RTT is an echo artifact
- **What:** Implemented, deployed, and measured the zero-copy UAPI (`kernel/zerocopy/` 2-patch series): `RING_FRAME_NO_INTERRUPT` in nhi.c, mmap'd frame pools + `SUBMIT_TX`/`POST_RX`/`REAP` ioctls + `TBSTREAM_DATA_MORE` message delimiting in stream.c, `zping`/`zpong` in pingpong. Verified bit-exact end-to-end.
- **Why:** Plan step 3. At 1 MiB the echo side spends **-48% CPU** (0.473→0.245 s/1000 msgs) and **-48% interrupts** (~269→~139/msg) at wire-parity throughput. RTT is parity-to-13%-worse — NOT a transport regression: rw `pong` pipelines within a message via per-frame wakeups while zc REAP is per-message store-and-forward, which matches a real consume-whole-buffer pattern.
- **Impact:** Gotchas: configfs stream groups pin `thunderbolt_stream` — rmdir before module reload (host-install.sh handles it now); stream configfs settings reset on reconfigure (a "tuned" comparison silently ran at default throttling once). v2: `POST_RX` flag to suppress all-but-every-k-th RX interrupt (~139→~2/msg). Next: `ztx`/`zrx` one-way modes, an NHI backend on the new UAPI, and the `hipHostRegister()` gate (plan step 4).
## 2026-08-02 18:54 — First USB4STREAM data between hosts; decision gate 1 passed
- **What:** Backported stack live on both maxes; a stream on service 1-2.1 → `/dev/tbstream0`. Verified pingpong: 4 KiB QD1 RTT **22.9 µs p50** (TCP ICMP floor: 64 µs, ~2.8×), bulk ~1.15 GB/s = TCP parity. Ring 4096 / throttling 0 tuning was near-noise → cost is per-4KiB-frame copy/processing (~4.4 µs/frame at 64 KiB), exactly what the zero-copy UAPI targets. Measured 2026-08-02 on the lab host pair.
- **Why:** Plan step 2 (stock baseline) and decision gate 1: stock materially beats TCP on latency → zero-copy kernel work is justified.
- **Impact:** Gotchas: max2's post-install boot loaded stock `thunderbolt.ko` (initramfs-era load; broke dependent modules, TB net down) — fixed by live reload + `dracut -f`, but watch both hosts' next reboots. Concurrent readers on one tbstream fd steal frames (incl. CLOSE) — `pkill -f pingpong` before reconfiguring; `ring_size`/`throttling` EBUSY while any fd is open. Next: `RING_FRAME_NO_INTERRUPT` nhi.c change + mmap slot-pool UAPI + `pingpong zc`.
## 2026-08-02 18:21 — Backport compiles on hosts; fix cross-module MODPOST
- **What:** First host build of the resolved series: `thunderbolt.ko` + `thunderbolt_stream.ko` compiled and linked clean on both maxes — the hand-resolved conflicts are compile-proven. `thunderbolt-net.ko` failed MODPOST (`tb_ring_throttling` undefined): it's built as a separate `M=` invocation and can't see symbols the freshly built thunderbolt.ko exports. Fixed in `host-build.sh` with `KBUILD_EXTRA_SYMBOLS=$SRC/drivers/thunderbolt/Module.symvers`.
- **Why:** Stock Module.symvers (kernel-devel) predates the backport's new exports; any two-stage external build with cross-module deps needs the first stage's symvers passed explicitly.
- **Impact:** Gotcha to remember: the `ssh ... | tail` pattern masked the build's exit code (background task reported success) — check artifacts, not exit codes, for piped remote builds. Rebuilds running; next is install+reload on max, sign+stage on max2.
## 2026-08-02 18:19 — Identify test hosts, hit max2 Secure Boot blocker, record TCP latency floor
- **What:** Test hosts are `jryates@max` (192.168.1.84) and `jryates@max2`, Fedora 43 (not Ubuntu as STRIXHALO.md suggests), kernel 7.1.5-101.fc43, TB link up as `thunderbolt0` 10.99.0.1/2 MTU 65520. Wrote `kernel/scripts/host-{build,install,sign}.sh`, synced repo to `~/strix-rdma` on both, started module builds. Baseline ICMP RTT over the link: min 37 / avg 64 / max 197 µs.
- **Why:** MODVERSIONS is off on Fedora → only exact vermagic match needed (LOCALVERSION=-101.fc43.x86_64 against vanilla v7.1.5). Gotcha: **max2 has Secure Boot + lockdown=integrity** (max does not) — unsigned modules won't load; needs MOK enrollment (host-sign.sh prepares it) or SB disabled in BIOS, either way a physical-console step.
- **Impact:** max can go end-to-end today; max2 blocks the two-sided stream smoke test until the user does the console step. The 64 µs avg RTT is the small-packet floor the NHI stream must beat at QD1.
## 2026-08-02 16:54 — Confirm NHI core change is required; map Mac-buildable work
- **What:** Verified in source that `ring_write_descriptors()` (`drivers/thunderbolt/nhi.c:245`) hardcodes `RING_DESC_POSTED | RING_DESC_INTERRUPT` on every descriptor — no client control. Also confirmed Docker/Colima/Lima are available for Linux kernel compile-checks.
- **Why:** Settles the plan's open question: per-message TX completion interrupts for the zero-copy transport need a small nhi.c core change (a `no_interrupt` flag on `struct ring_frame`); RX side must instead use the existing `throttling` knob/busy-poll since RX descriptors are pre-posted before message boundaries are known.
- **Impact:** Work order while hosts are busy: container compile-verify of the hand-resolved backport series, then the consumer transport abstraction (Mac-native, TCP loopback testable). Gotcha: the kernel tree is subtly broken on macOS's case-insensitive FS (`xt_CONNMARK.h`/`xt_connmark.h` collide → phantom diffs in `linux/`); kernel container builds must clone inside the container FS, never bind-mount the Mac checkout.

## 2026-08-02 16:23 — Scaffold repo and produce verified USB4STREAM backport series
- **What:** Scaffolded the repo (plan doc, kernel/, tools/, bench/), sparse-cloned mainline, and extracted the USB4STREAM work as a 9-commit series ending at `6db21d81`. Test-applied it on v7.1 with `git am -3`, resolved 3 conflicts (property.c `depth` param; xdomain.c hunks from unrelated refactors `4d5fc3f4`/`8b406099` not in the series), dropped the KUnit-only patch, and saved a clean-applying 8-patch series to `kernel/backport/v7.1/`. Wrote and loopback-tested `tools/pingpong` plus ConfigFS setup/bench scripts.
- **Why:** Hosts run 7.1.5; USB4STREAM lands in 7.2. Key finding: `stream.c` needs core APIs absent from 7.1.5 (`tb_ring_flush`, `tb_ring_throttling`, `tb_service_properties_changed`, ConfigFS, `tb_property_merge_dir`, new `tb_service`/`tb_ring` fields), so an out-of-tree module against stock headers is impossible — but the delta is confined to the thunderbolt stack, so rebuilding only `thunderbolt.ko` + `thunderbolt-net.ko` + `thunderbolt_stream.ko` as a set suffices (procedure in `kernel/README.md`).
- **Impact:** Unblocks step 2 of the plan (stock `/dev/tbstreamX` baseline) without a full kernel rebuild. Next: TCP baseline on hosts, then build/install the backported module set and run `bench/scripts/run-pingpong.sh`.
