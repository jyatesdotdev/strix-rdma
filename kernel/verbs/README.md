# Experimental native NHI kernel verbs

**Implementation/build milestone, not runtime acceptance or complete RC
conformance. Do not deploy this alongside production traffic without a separate
operator-authorized hardware qualification.** No module has been loaded by the
implementation workflow. The ordinary rdma-core provider is in
[`providers/strix_nhi`](../../providers/strix_nhi/README.md); its Linux compile
and offline tests do not establish live discovery. See the integrated
[status and unrun gates](../../docs/NATIVE_VERBS.md).

This is a real Linux `ib_device`/uverbs backend, not a libibverbs replacement.
It implements bounded connected SEND/RECV, WRITE and READ in software over new
NHI rings. CPU pages are pinned with `ib_umem`; CPU copies stage data into/out of
separate coherent DMA buffers. No GPU/DMA-BUF, zero-copy, RoCE interoperability,
RNIC offload, CM, atomics or performance claim is made.

## Source and build

The C/headers, `Kconfig` and `Makefile` here are canonical. Regenerate the
follow-on patch after changes with:

```sh
sh kernel/verbs/generate-patch.sh > kernel/verbs/0002-RDMA-add-opt-in-native-NHI-software-verbs.patch
make -C kernel/tests check-native-verbs
# Optional executed sanitizer tests:
make -C kernel/tests check-native-verbs CC=clang CFLAGS='-fsanitize=address,undefined -g'
```

Target: Linux stable **v7.2.8** (the v7.2 API: `tb_nhi->dev`,
`ib_umem_get_va`), with the twenty `kernel/zerocopy` patches rebased onto it
(the v7.1 backport series is upstream in v7.2 and no longer applied), then both
patches in this directory in order.
Patch1 adds the separately approved, opt-in native path-lifetime core API;
patch2 adds the native driver and RDMA Kconfig/Makefile entries. Existing path
API signatures/behavior, USB4STREAM, thunderbolt-net, existing patches, lifecycle
units, permissions and default build/install paths are unchanged. The new core
fields/APIs are conditional on CONFIG_INFINIBAND_STRIX_NHI; default-off builds
still compile three behavior-preserving pieces of patch1 (the
`__tb_approve_xdomain_paths` lock split, a `tb_cm_ops` forward declaration and
a call to the empty `tb_xdomain_remove_native_paths` stub). Default-off
equivalence with a pure 28-patch build is not separately tested. Enabling that symbol
changes the XDomain structure: rebuild **all Thunderbolt modules together**, not
just the native module, against the same generated configuration. Use `git apply`
for these patches in an owned prepared source tree.

`CONFIG_INFINIBAND_STRIX_NHI=m` depends on RDMA user access/memory and
USB4/USB4_NET. It defaults off. `strix_nhi.ko` has **no autoload device alias**
and refuses initialization without its explicit `enable=1` parameter. Loading
or installing it is not part of these build instructions. System suspend is
rejected while the service is bound; resume is not an implemented operation.

## Reproduce on the lab nodes (max / max2)

End-to-end bring-up, assuming the nodes run Fedora 43 / 7.2.8 with
the rebased zero-copy series and patch1 (`0001-thunderbolt-native-path-lifetime`)
already installed (they are; see `kernel/README.md` for the kernel-side recipe):

```sh
# On each node, in the prepared tree ~/src/linux-stable (7.2.8 + zc01..20 + 0001):
git am ~/strix-rdma/kernel/verbs/0002-RDMA-add-opt-in-native-NHI-software-verbs.patch
KREL=$(uname -r); LOCALVER=-${KREL#*-}
make -j"$(nproc)" M=drivers/infiniband/hw/strix_nhi modules \
  LOCALVERSION="$LOCALVER" KBUILD_EXTRA_SYMBOLS="$PWD/drivers/thunderbolt/Module.symvers"
sudo make M=drivers/infiniband/hw/strix_nhi modules_install \
  INSTALL_MOD_DIR=updates LOCALVERSION="$LOCALVER"
sudo depmod -a

# Load on BOTH nodes. Two modes, and they differ in what thunderbolt_net must do:
#
#   zero-copy (the fast path; both DMA path tuples, data rings live, zdata=1)
#     thunderbolt_net must be ABSENT. With a netdev present the driver gates ring
#     bring-up on TB-IP carrier, which no-TB-IP operation never establishes.
#     Use the script -- it blacklists tbnet, reloads the custom core over the
#     stock initramfs module, loads strix_nhi once, and waits for the handshake:
sudo ~/strix-rdma/tools/scripts/strix-verbs-bringup.sh
#
#   staged-only (legacy; small messages and CPU-copy fallback)
#     thunderbolt_net up, and strix_nhi gates on its carrier:
sudo modprobe strix_nhi enable=1

# Each side goes PORT_ACTIVE once the wire handshake completes:
cat /sys/class/infiniband/strix_nhi0/ports/1/state   # -> 4: ACTIVE

# Two-host exchange smoke (both sides; listener first). In zero-copy mode TB-IP
# is down, so the out-of-band metadata channel runs over the LAN addresses
# instead of 10.99.0.x:
cd ~/strix-rdma/tools/verbs && make OUTPUT=strix-verbs-smoke
# max (192.168.1.84):
sudo sh -c 'ulimit -l unlimited; exec sudo -u jryates \
  ./strix-verbs-smoke --live strix_nhi0 listen 192.168.1.84 4789'
# max2:
sudo sh -c 'ulimit -l unlimited; exec sudo -u jryates \
  ./strix-verbs-smoke --live strix_nhi0 connect 192.168.1.84 4789'

# Ping-pong RTT (256 B staged path; connector prints avg/min/max):
#   ... listen  192.168.1.84 4789 ping 2000
#   ... connect 192.168.1.84 4789 ping 2000
# Verified RDMA WRITE / READ (window 4 x 2 MiB, bidirectional, every byte
# checked; both sides print the placed byte count and the native counter delta):
#   ... listen  192.168.1.84 4789 write 4
#   ... connect 192.168.1.84 4789 write 4
#   ... listen  192.168.1.84 4789 read 4
#   ... connect 192.168.1.84 4789 read 4
# Throughput at the reference workload's shape (window 4 x 2 MiB, bidirectional):
#   ... listen  192.168.1.84 4789 200
#   ... connect 192.168.1.84 4789 200
# Live driver state (data-ring occupancy, zc state, poll/credit counters):
cat /sys/class/infiniband/strix_nhi0/native_stats
```

Port 4789 must be open on both nodes for the metadata channel
(`firewall-cmd --add-port=4789/tcp`; add `--permanent` to keep it across
reboots).

Running a consumer over the provider: create a systemd drop-in on each node that
overrides ExecStart to `--transport rdma --rdma-device strix_nhi0
--rdma-gid-index 0` plus `LimitMEMLOCK=infinity` (the verbs MR path checks
memlock). The proven tensor-parallel run used node-local drop-ins for that
workload's server (max2, coordinator) and worker (max) units; those drop-ins live
on the nodes, not in this repository. The API was on the coordinator at
`http://10.99.0.2:8080`.

**Do not load `thunderbolt_stream` while strix_nhi and thunderbolt-net are both
bound** — the Strix NHI has only 3 usable ring HopIDs per direction and the
stream would fail to allocate (harmless, but confusing).

```sh
# Reads source; applies the patch only to a fresh temporary fixture:
sh kernel/tests/test-native-patch.sh /explicit/unmodified-infiniband-source
sh kernel/tests/test-native-core-contract.sh /explicit/prepared/linux-7.1.5
# Linux only; input source must already contain the exact generated patch.
# Output directory must not exist. No downloads, installs or module actions.
sh kernel/verbs/build-check.sh /explicit/prepared/linux-7.1.5 /input/config /new/scratch/output
```

Real compile evidence in this pass is **arm64 only**, not Strix x86_64 binary
qualification: GCC 14.4.0, Linux v7.1.5, generated configuration and actual
RDMA/Thunderbolt dependencies, `vmlinux`, successful MODPOST and linked
`strix_nhi.ko`, `thunderbolt.ko`, `thunderbolt_net.ko`, `thunderbolt_stream.ko`.
`W=1` additionally emits upstream arm64 syscall-table override-initialization
warnings; inherited stream.c and lib/rhashtable.c kernel-doc warnings also occur. These are
not new native-driver/core-extension warnings. Public build dependencies and
base image pins are in `build-dependencies.txt`. The helper script is for future
out-of-tree reproduction; the executed build used an owned in-container tree.

## Provider interface and exact limits

- Device name `strix_nhi%d`; service driver name `strix_nhi`; uverbs ABI **1**;
  `RDMA_DRIVER_UNKNOWN`, no borrowed vendor ID. Match both actual parent driver
  identity and device name/ABI, not a fixed GUID or all UNKNOWN devices.
- Standard uverbs commands, including POST_SEND, POST_RECV, POLL_CQ and
  REQ_NOTIFY_CQ. **No private command payloads**, mmap queues or doorbells.
  Provider command structures must not append private input/output bytes.
- One context, one PD, one RC QP, up to two CQs of 1..64 entries (the reference
  workload uses a shared
  CQ8). QP SQ/RQ depths 1..4, **exactly one SGE** per WR, including zero-length
  transfers. CQ overflow emits CQ_ERR, fails the QP and never overwrites unread
  CQEs. Unsignaled successful SQ work has no CQE; errors/flushes do.
- Up to four CPU MRs, each 1..8 MiB; aggregate **page-rounded** pins <=32 MiB,
  additionally subject to normal memlock accounting. Local/rkey is a monotonic
  nonreused device-lifetime key; exhaustion fails instead of wrapping. Keys are
  checked with PD, range and access; deregistration returns EBUSY while a WQE or
  incomplete WRITE retains the MR. QP destruction joins copying under the device
  mutex, flushes owned work and compacts its unread CQEs. A CQE never points
  at the QP: `ib_uverbs_poll_cq` reads `wc->qp->qp_num` after `poll_cq` drops
  the mutex, possibly after DESTROY_QP freed the QP, so `wc->qp` is one of two
  device-owned identities holding only `qp_num` (freed after uverbs drains).
  Only a poll stalled across two destroy/create cycles could report a newer
  QPN; that is cosmetic, not a use-after-free.
- SEND, RECV, WRITE, READ <=2 MiB. One serialized active SQ op, one actual READ
  requester and one responder snapshot. SEND_SIGNALED and FENCE supported;
  FENCE follows naturally from serialization. Inline, solicited, IMM,
  invalidate, atomics, SRQ, MW, ODP, relaxed ordering and all other ops fail.
- RESET->INIT->RTR->RTS; ERR and RESET teardown supported. Other transitions
  fail. INIT: port1, PKey0, access subset LOCAL_WRITE/REMOTE_READ/REMOTE_WRITE.
  RTR: MTU4096 **only**, 24-bit QPN/PSN, global AH with peer GID, port1, SGID0,
  hop_limit64, flow_label/traffic_class/SL/static_rate0, read depth0..1,
  min_rnr_timer12 **only**. RTS: PSN24, timeout14 **only**, retry/rnr_retry0..6,
  max_rd_atomic0..1. Infinite retries and other timer/MTU policies are rejected,
  not silently reinterpreted. Query QP preserves accepted setup attributes.
- Timeout14 = 67,108,864 ns; timer12 = 640,000 ns. Polling rounds delays upward.
  An independent 30-second staging/binding/assembly watchdog bounds local
  stalls. Standard posted but unmatched receives can remain pending until
  matched, failed or explicitly destroyed. Internal operation serial starts at
  the negotiated PSN, increments per operation and never wraps; header identity
  includes QP generations so the visible 24-bit PSN is not the replay key.
- `native_stats` under the RDMA device reports native TX/RX frames, rejected
  decoded frames and SEND/WRITE/READ placement byte counts. These are kernel
  counters, not evidence of measured hardware throughput.

The reference consumer is a Linux ROCm tensor-parallel RoCE transport header:
two 8 MiB LOCAL_WRITE-only MRs, window4/2 MiB, shared CQ8, access_flags0 and read
depth1. **Its MRs remain remotely inaccessible**, so it exercises SEND/RECV only.
The separate generic Linux adapter for that workload requires RoCEv2 GIDs and much
larger queues; it is unsupported. Provider startup must not fabricate ACTIVE:
operators must observe bilateral port readiness before launching a consumer,
which fails immediately on a DOWN port.

## Native addressing and platform safety

Port1 is Ethernet-associated only through the real `thunderbolt-net` service
whose parent is the **same XDomain object**. Probe defers if absent or ambiguous;
no dummy netdev, name-prefix identity or RX-handler takeover. Netdev references
and notifications handle carrier loss/unregistration. The RDMA core does not
drop a port netdev on NETDEV_UNREGISTER, so the notifier releases both the
driver's and the core's references at once (otherwise thunderbolt-net
unregistration would wait forever). The device then stays dead until the
service is reprobed; it never re-associates another netdev. Initial association is
in the initial network namespace. GID0 is `fe80::` plus the low 64 bits of the
validated local XDomain UUID; the remote advertised GID must match the remote
UUID-derived value. One PKey (0xffff), one GID. Core protocol flags and MAD size
are zero; speed/width are unknown (zero), and the cached GID type is the core's
non-RoCE default, **not RoCEv2**. This metadata path still needs real uverbs tests.

Only PCI **1022:158d / 1022:158e** are admitted, before netdev lookup, allocation
or RDMA registration. These Strix Halo IDs were observed at c4:00.5/c4:00.6 in the
2026-08-25 compile-oracle dmesg capture on max2 (lines536/541 of its
`max2-dmesg-prefix.txt`); they are not inferred from DMI/netdev names. At the pinned kernel,
`nhi.c:nhi_select_cm` selects software CM for native ACPI control, otherwise tries
`icm_probe` and falls back to `tb_probe`. `icm.c:2463..2589` switches on PCI device
ID (not vendor!) and neither admitted ID has an ICM case; absent `is_supported`
returns NULL. Thus both paths choose software CM. `tb.c:2368..2411` disconnects
only the exact DMA tunnel tuple; contrast `icm.c:613..631`, which disconnects a
physical port and is deliberately excluded. The core-contract test evaluates
actual ICM case constants and asserts the early guard/exact teardown call.
This is a **pinned-source restriction**, not a generic AMD/NHI compatibility claim.

Approved core exception: `uverbs_cmd.c:copy_ah_attr_from_uverbs` uses
`rdma_ah_set_dlid`/`rdma_ah_set_path_bits`; the pinned `ib_verbs.h` discards these
IB-only fields for UNDEFINED AH type. The driver cannot reject their original
raw values. They have **no native routing effect**, are not supported routing
features, and the reference workload sets both zero. All visible native route fields are checked.

## Source-associated startup and teardown proof

Series20 property refresh was checked against pinned stable source plus
backport patches1/2: `tb_service_properties_changed` increments the property
block generation and queues notification. `PROPERTIES_CHANGED_REQUEST` queues
state work; ENUMERATED queues a property fetch; a newer generation goes through
`enumerate_services` -> `update_service` -> locked `remote_properties` replacement.
`update_property_block` merges service-local properties into an XDomain-specific
copy; the static template intentionally has no `session` field, because merge
uses replace=false. Notifications can fail and generation comparisons can reject
stale updates: failure never authorizes DMA. There is no legacy global callback.

Invalid remote snapshots are gone entirely: the dynamic session handshake is
**wire-carried** (HELLO/HELLO_ACK for reciprocal readiness, BIND/BIND_ACK for
QP pairing), and the property channel carries only static inline immediates
(prtc* plus a write-once `rxhop` rendezvous per service incarnation) — the
property class that round-trips reliably. Torn pulls, DATA-section corruption
and frozen parses therefore cannot affect the session. A core-side pull that
validated per-chunk generations (the documented B follow-up) is no longer on
this driver's critical path.

The service key is `strixv1`, UUID `9dfdfb88-eaa4-4d26-9892-59f208455101`,
protocol1.

### Hardware bring-up lessons (found on the live nodes)

- Frame `sof/eof` must be `sof=FRAME_START(1), eof=FRAME_END(2)` with masks
  `BIT(1)/BIT(2)` (thunderbolt-net's exact scheme). `sof=0` breaks the NHI's
  multi-packet frame reassembly: >252-byte frames arrive as CRC-flagged pieces
  whose content is intact (the software CRC passes every one) but whose NHI
  E2E CRC fails per piece.
- `RING_FLAG_E2E` is required on **both** rings (unlike tbnet's RX-only
  pattern). TX without E2E stalls large transfers: the sender's NHI flow-
  controls against the receiver's E2E credits and the accounting wedges.
- The XDomain in-HopID must stay clear of thunderbolt-net's login (HopID 8):
  allocate from 10 up. Holding 8 breaks TB-IP re-login; its carrier never
  rises, which also wedges us (our rings gate on the TB-IP carrier).
- The Strix NHI has only 3 usable ring HopIDs per direction (0=ctl, 1=tbnet,
  2=strix_nhi) — thunderbolt-stream cannot be loaded alongside both.

Its static property block carries only prtc* metadata plus a write-once
`rxhop` immediate (our receive XDomain HopID) — the session state is
**wire-carried** by the HELLO/BIND handshake instead of a dynamic property.
Values are converted explicitly to/from byte offsets, not memcpy'd across host
endianness. Snapshot reads/replacements hold only `svc->lock`;
allocation, path operations and publication notifications occur outside that
lock.

Rings start only with the TB-IP carrier present and both directions proven by
the wire handshake. QP operations require reciprocal QPN/generation/PSN
binding. Carrier loss stops DMA and invalidates the session; a down/up
transition between worker passes is latched until local DMA/path teardown
completes, rather than silently reusing old paths. Service removal stops local
rings, joins cancellation callbacks and obtains proof of exact path cleanup
before releasing DMA buffers/rings/HopIDs. Core-originated removal can retire
paths first and publishes this only after completing cleanup. Standard uverbs
context-disassociation cleans objects without waiting for a peer ACK.

### Nonblocking core ownership extension (patch1)

The original APIs are insufficient for a worker-driven service: hotplug and
`tb_domain_remove` can hold `tb->lock` while synchronously unbinding a service;
a worker blocked in `tb_approve_xdomain_paths` then deadlocks with remove's
object lock/cancel_sync. Merely moving work to the ordered domain queue does not
fix domain removal from another thread. The new APIs solve this in the owning
software CM; the native worker remains independent and never waits for tb->lock.

- `tb_xdomain_try_enable_native_paths` uses mutex_trylock, rejects firmware CM,
  unplug/removal, and admits at most two exact native tuples per XDomain (a
  control ring pair and a zero-copy data ring pair). EAGAIN
  changes no core state. Existing clients keep the original blocking API.
- The core retains the exact tuple, not a raw tunnel pointer. Driver ring/HopID
  ownership prevents tuple reuse until release. Other core invalid-tunnel cleanup
  can remove the tuple first; a later exact lookup safely finds nothing.
- `tb_xdomain_try_disable_native_paths`: 0 means exact local cleanup; EAGAIN
  retains all ownership; ENODEV is acquire-observed proof that removing core
  already cleaned it. No timeout is treated as permission to free anything.
- Every software-CM `tb_xdomain_remove` entry performs native cleanup **before**
  handshake cancellation/service callbacks. All audited software callers hold
  tb->lock: scan-port replacement, hotplug, switch-subtree removal, resume cleanup
  and domain stop. ICM calls are explicitly a no-op and cannot admit native paths.
  Admission-stop is release-published before cleanup, completion after it.
- Domain stop recursively retires native tuples before generic tunnel destruction
  and tb_switch_remove. The optional empty-list reset prevents traversal of freed
  generic nodes by subsequent XDomain cleanup. This covers the non-workqueue
  `tb_domain_remove -> cm_ops->stop -> tb_switch_remove` path too.
- `tb_service_try_native_properties_changed` serializes notification queueing
  with the same core lock and admission-stop marker. EAGAIN retains a dirty bit
  for retry; ENODEV queues nothing. This prevents a service worker from queueing
  property work after core stop_handshake canceled it. A flag check alone would
  have a check-before-queue race. Publication never holds svc->lock here.
- Worker path calls return EAGAIN without waiting under the object mutex. Carrier
  loss stops DMA and retries retained path ownership on later bounded passes.
  Removal marks dead/flushes, releases the object mutex, joins worker and ring
  callbacks, then retries EAGAIN **in the removing process**, not on a queue core
  is draining. Core-originated removal sees its completed marker immediately;
  ordinary sysfs unbind/module removal waits for exact local release without
  releasing IDs on an arbitrary timeout. Unexpected ownership errors are warned
  and retained, never converted to unsafe release.

There is no lock-state guessing or unlocked tunnel mutation. Static source gates
check nonwaiting calls, exact cleanup, early central retirement and EAGAIN
retention. Tests also execute the actual prepared core API bodies with modeled
locks/path operations, including busy-core removal and notification-after-cancel
prevention; full Linux compilation checks real integration. Concurrency/lockdep,
all teardown races and hardware behavior still require reviewer and runtime
qualification; these source checks are not that qualification.

`nhi.c:tb_ring_stop` disables descriptor DMA then `flush_work`s callbacks before
returning. That local stop contract, not timeout expiry, permits staging free.
`xdomain.c:2109..2115` keeps runtime PM active while the XDomain exists.
**Abrupt peer loss can still hit the known inactive-peer NHI wedge despite this
handshake.** Neither automatic recovery nor coexistence has been hardware-proven.

## Data wire version1

All fields are big endian. `native_wire.h` is the shared tested codec; its C
structure is never transmitted. Frames are exactly 112+payload bytes, <=4096;
payload <=3984 and total operation <=2 MiB. Unsupported/noncanonical fields,
reserved bits, length overflow and CRC mismatch are rejected before copying.

| Byte offset | Field |
|---|---|
| 0 | magic `0x534e4831` (SNH1) u32 |
| 4/5/6/7 | version1 / reserved0 / header-length high0 / low112 |
| 8/9..11 | opcode / flags and reserved (all zero) |
| 12/16/20 | total length / source QPN / destination QPN (u32) |
| 24/32 | source/destination QP generation (u64) |
| 40/48 | source/destination link epoch (u64) |
| 56/64 | operation serial / ACK serial (u64) |
| 72/76 | fragment offset / payload length (u32) |
| 80/88/92 | remote address u64 / rkey u32 / status u32 |
| 96/100..111 | CRC32C u32 / reserved zeros |

CRC32C Castagnoli uses initial/final complement and treats bytes96..99 as zero,
covering the complete header and payload. It detects corruption, not an
unauthorized peer. Opcodes: SEND1, WRITE2, READ3, READ_REPLY4, ACK5, READ_ACK6,
CREDIT7, HELLO8, HELLO_ACK9, BIND10, BIND_ACK11, ZC_SEND12, ZC_READY13,
ZC_WRITE14. Status: OK0, RNR1, ACCESS2, LENGTH3, PROTOCOL4. Unused fields must be zero.
CREDIT total is available application RQ WQEs, serial its revision; it is only a
hint. Actual SEND admission checks RQ ownership and returns RNR without consuming
an absent WQE. Preposted transport RX slots are never application credits.

The ZC_* opcodes are the zero-copy data-plane burst protocol and are only ever
exchanged between peers that both advertised `SN_CAP_ZDATA` in HELLO. A burst
descriptor carries the whole message in `total` and its payload follows as
header-less frames on the data ring; `ZC_READY` carries the operation serial in
the ACK serial field. `ZC_SEND` and `ZC_WRITE` differ exactly as their staged
counterparts do: `ZC_SEND` forbids remote address/rkey because the payload lands
in a receive WQE the responder posted, while `ZC_WRITE` requires them because the
responder places the payload in the requester's named MR. A `ZC_WRITE` therefore
consumes no receive WQE, produces no receive CQE and can never be RNR; a WRITE
whose length is at or below the zero-copy threshold, or whose local source or
remote destination is not frame aligned, stays on the staged path instead.

A single ordered assembly accepts only the expected serial; reordered fragments
are dropped, already assembled duplicates do not advance it. Complete SEND/WRITE
is placed once before completion/ACK. A replay record retains the outcome; ACK
loss never consumes another receive or rewrites memory. READ captures one stable
snapshot and replays that snapshot until acknowledged/advanced. Requester READ
uses its retained local SGE, copies once, completes once and re-ACKs duplicates.
FIFO TX slot ownership is reclaimed only on local ring callback, independently
of WR success. Eight of 63 usable TX slots are reserved from outgoing request
data for control/READ response progress. ACKs precede replies, which precede SQ
requests. The control queue holds 16 coalesced ACKs; if a single RX batch
overflows it, the QP fails closed and no caller completes the flushed work
afterward. The mutex orders placement before CQ visibility; it does not promise
atomicity against concurrent CPU access to registered pages.

## Validation boundaries and provenance

Portable ASan/UBSan tests execute the production codec/order helpers and actual
`protocol.c` with explicitly modeled MR/CQ/time/ring adapters: malicious frames,
0/1/3984/2 MiB segmentation, generation/key-range predicates, RNR/ACK timeouts,
SEND ACK loss, WRITE replay, READ snapshot replay and distinct four-slot
bidirectional 2 MiB payloads/WR identities. **Those adapters do not test kernel
locking, `ib_umem`, CQ notification races, uverbs object lifetimes or real DMA.**
The separate real kernel build is not replaced by these tests. Added object
behavior tests execute extracted production MR lookup/refcount/deregister,
post/partial-list, CQ/notify/overflow and error-flush functions with explicitly
modeled kernel types/locks/umem. They cover keys/PDs/ranges/SGEs/sizes, EBUSY until
flush, destroyed-QP CQE compaction and CQE QP identity that stays valid after
the QP is freed, but **not real kernel concurrency or
page pinning**. Repository
`make check` and 45 existing kernel patch-contract tests passed in owned scratch.

Still required: reviewer gate; the CI-like source checks; the x86_64
deployment build; **PROVEN live**: full two-host SEND/RECV exchange (0/1/3984B
and 2 MiB windows, 18.9 MB), repeated unload/reload cycles, and unmodified
ROCm tensor-parallel inference over the provider (`--transport rdma
--rdma-device strix_nhi0`, 377 MB placed, zero bad frames, working inference).

**Also PROVEN live since (2026-10-10, zero-copy data plane):** sustained
throughput and latency at the reference workload's message shape, which closes the A/B item below.
Window-4 x 2 MiB bidirectional at **1182-1238 MiB/s (9.9-10.4 Gbps) per
direction**, 15/15 consecutive runs consistent, including a 6.4 GiB long run;
256 B ping-pong RTT **35.1 us avg / 31 us min / 345 us max**. Both are at or
above the USB4STREAM zero-copy figures (1146 MB/s, 3837 us bidirectional p50)
and at the ~9.5 Gbit/s per-direction link ceiling, so the keep-native-vs-stream
decision now favours native on both axes. These figures are quoted here rather
than linked: the dated result files that originally recorded the runs were
removed when this repository stopped being workload-specific, so the numbers
below are the surviving record of what was measured on the lab host pair.

**RDMA READ and WRITE on hardware (2026-10-10, first live exercise).** After a
clean reboot plus bring-up on both hosts, with `data_throttle_ns` at its 128000
boot default:

- `strix-verbs-smoke ... read 200` **passes**: 200 iters x window 4 x 2 MiB =
  1,677,721,600 bytes placed and verified byte-for-byte on both sides,
  `placed_read_bytes` non-zero for the first time, `bad_frames 0`, 61.96 MiB/s.
- `... write 4` **passes 3/3**: 33,554,432 bytes verified byte-for-byte per run
  at ~425 MiB/s, `placed_write_bytes` non-zero for the first time,
  `bad_frames 0`, `zc_retries` unchanged.
- `... write 200` **fails intermittently** on payload verification. It is
  retry-correlated and reproducible: every failing run incremented
  `zc_retries`, the failing iteration moved (22, then 32), and the failing side
  flipped between hosts. Short runs that finish without a retry always pass.
- The default CPU SEND/RECV smoke still passes (18,878,353 bytes, `zc_bursts 9`,
  `bad_frames 0`), so this is specific to zero-copy WRITE.

**Zero-copy WRITE is opt-in (`zc_write`, off by default) because of that
failure.** The mechanism is a replayed burst: when a ZC WRITE is retried, stale
payload can land in the requester's MR *after* that WRITE already completed and
the application reused the buffer. A replayed SEND is harmless because it lands
in a receive WQE the peer already consumed; a replayed WRITE corrupts live user
memory and still reports `IBV_WC_SUCCESS`. Silent corruption, so eligible WRITEs
stay on the staged path until the replay ordering is root-caused. The offline
suites cover the happy path, lost `ZC_READY`, lost burst and the gate itself,
but they model a *lost* burst rather than a *late* one arriving after
completion, which is why they did not catch this.

**Gated build re-validated after a second clean reboot (same day).** With
`zc_write=N` (the default) all three modes pass on both sides:

- SEND/RECV smoke: 18,878,353 bytes, byte-for-byte, `zc_bursts` exercised.
- `write 200`: **PASS** — 1,677,721,600 bytes placed and verified byte-for-byte
  in 27.2 s (58.81 MiB/s). This is the run that failed intermittently at
  iteration 22–32 before the gate. `zc_bursts` rose by only 9 across the whole
  sequence, confirming WRITE stayed on the staged control-ring path.
- `read 200`: **PASS** — 1,677,721,600 bytes verified byte-for-byte in 25.6 s
  (62.62 MiB/s).
- `bad_frames 0` throughout.

So the gate does what it exists to do: WRITE and READ are correct and safe today,
at staged-path speed. **The gate costs roughly 7x WRITE throughput** — ~425 MiB/s
for zero-copy WRITE in the short runs that completed, against 58.81 MiB/s staged —
which is the size of the prize for root-causing the replay ordering.

Bulk SEND throughput after the clean reboot was **1127–1191 MiB/s** over 5 runs
(mean 1158), `bad_frames 0`, retry rate 462/6000 bursts = 7.7% (the recorded
endurance run was 9.4%, so this is normal). That is at or above the 1070–1156
MiB/s this stack recorded for previous script-brought-up post-reboot runs; the
1182–1238 MiB/s headline band was measured pre-reboot and is not the like-for-like
comparison.

Also not yet run:
hot-unplug/malformed-peer injection; sustained multi-hour endurance; an
end-to-end inference re-run over the zero-copy data plane (the earlier proven run
predates it). Do not describe this bounded experimental implementation as
complete RC or production-ready.

**Mixed-version pairs fail rather than degrade.** `SN_ZC_WRITE` is opcode 14 and
reuses the existing `SN_CAP_ZDATA` negotiation instead of adding a capability
bit. That is deliberate: `SN_CAPS_KNOWN` is enforced during HELLO/HELLO_ACK
validation, so a new bit would make a pre-change peer reject the handshake
outright and the session would never form at all — strictly worse than the
alternative. The consequence is that a peer running the older module still
advertises ZDATA, accepts the descriptor, and then rejects opcode 14 at
`sn_decode`: the frame is dropped, `bad_frames` increments, and the requester
eventually fails with retry-exceeded. There is no misinterpretation and no
memory-safety hazard, but **both hosts must run the same module build** before
exercising zero-copy WRITE. Real mixed-version safety would need an `SN_VERSION`
bump negotiated in HELLO; that is a larger wire-ABI decision and has not been
taken.

All new code here is GPL-2.0-only, authored for this implementation against the
pinned Linux API. No Hellas source was copied. The experimental Hellas repository
at `76ba39b630a70accb72f19388eefe48844b50eb8` was an architecture reference only;
no legacy control callback, fake line rate or RoCE capability was imported.
