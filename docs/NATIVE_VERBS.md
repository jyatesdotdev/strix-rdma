# Experimental Linux native NHI verbs

**Hardware-validated on the lab nodes (Strix Halo pair, Fedora 43 / 7.2.8):
two-host SEND/RECV exchange passes (0/1/3984-byte and 2 MiB windows, 18.9 MB),
repeated unload/reload cycles pass, and unmodified ROCm DS4 tensor parallelism
runs over the provider (`--transport rdma --rdma-device strix_nhi0
--rdma-gid-index 0`): 377 MB placed through SEND/RECV, ~140k frames per
direction, zero bad frames, working inference. Not yet: throughput/latency A/B
against the stream path (the keep-vs-stream decision data), hardware
READ/WRITE (DS4's RoCE path is SEND-only), hot-unplug and malformed-peer
injection.** Still not complete RC conformance, production-ready, or a
performance claim. Existing series20
USB4STREAM production behavior/configuration is unchanged. This separate service
must not be tried alongside production traffic without operator-authorized
maintenance and an independent safety review.

The NHI is not an RNIC: it does not offload QPs, rkeys, remote virtual-address
placement, RDMA reads/writes, or verbs reliability. Those semantics can be
implemented in software over its DMA rings. This implementation is a real Linux
`ib_device`/uverbs device plus ordinary rdma-core provider, **not** a verbs-lookalike
API or library interposer. The first path is **CPU-copy staging**: pinned CPU MR
pages -> coherent NHI staging -> cable -> staging -> pinned CPU MR pages. There
is no IP/TCP payload fallback, GPU-direct, DMA-BUF registration, or zero-copy
claim. TCP in the smoke/DS4 adapter is metadata/barriers only.

## Compatibility and limits

See the canonical [kernel wire/ABI/lifetime contract](../kernel/verbs/README.md)
and [provider interface/build](../providers/strix_nhi/README.md).

| Interface | Explicit bounded implementation |
|---|---|
| Device | `strix_nhiN`, kernel ABI1, experimental `RDMA_DRIVER_UNKNOWN`; provider also validates actual Thunderbolt service driver |
| Platform | NHI PCI1022:158d/158e only, pinned software-CM path; all other controllers rejected |
| Port | one; ACTIVE only with same-XDomain thunderbolt-net carrier and reciprocal native readiness; Ethernet metadata, zero protocol capability bits, unknown speed/width |
| Addressing | native nonzero GID0, cached type IB by core classification, **not RoCEv2/IB packet interoperability**; port1/PKey0 |
| Objects | one context/PD/RC QP, SQ4/RQ4, exactly one SGE per WR; two CQs up to64 each |
| CPU memory | four MRs, each <=8MiB, page-rounded aggregate32MiB plus normal memlock; fresh nonreused keys |
| Operations | SEND/RECV, bounded software WRITE/READ <=2MiB; one real requester/responder READ resource; signaled/unsignaled and serialized FENCE |
| Setup | MTU4096 only, 24-bit PSNs, global exact peer GID, SGID0/hop64; timeout14, min_rnr_timer12, retry/rnr_retry0..6, read depths0..1 |
| Unsupported | GPU/DMA-BUF/ODP/relaxed ordering, inline/solicited send, IMM/invalidation, atomics, SRQ/MW, other QP types, CM/MAD, alternate routes, infinite retries, other MTUs/timer policies |

Unsupported operations are rejected; they are not reported as successes. Local
NHI TX completion returns only staging ownership. Successful verbs SEND/WRITE
completion requires peer-placement ACK; READ uses a retained stable response
snapshot and ACK protocol. Preposted NHI buffers are **not** receive-WQE credits.
There are finite RNR/retry/progress budgets, fresh link/QP generations, replay
records, key/PD/access/range checks, CQ error/flush handling and synchronous local
ownership teardown. Kernel/runtime/hardware validation of those semantics is
still mandatory. A trusted physical link is not authentication: epochs prevent
stale sessions, not a malicious authorized peer. Keep IOMMU and existing device
access policy enabled; no widened permissions are required or authorized here.

### Unchanged DS4 target

The target is **Linux ROCm `ds4_tp_roce.h`**, not every Linux DS4 build. The
inspected read-only DS4 HEAD is `1bcd62093b5307a3758d90c3875d86ca758b08fb`.
It dlopens standard `libibverbs.so.1` and uses normal device/port/GID queries,
CQ8, RC QP4/4, one SGE, two8MiB host MRs, 2MiB chunks, INIT/RTR/RTS and CQ polling.
It requests one READ requester/responder resource even though its transfers are
SEND/RECV; these are real implemented resources, not ignored attributes.
Its MRs have LOCAL_WRITE only and QP access0: **they remain remotely inaccessible**.
No DS4 source change, transport hook or fake RoCE capability was introduced.

DS4's existing adapter calls the transport “RoCE” in logs/option descriptions;
that string is not a protocol claim by this native provider. It accepts active
Ethernet metadata and nonzero GID0 without requiring RoCEv2 type. Select the
explicit native device and GID0 only after bilateral readiness. The separate
generic non-ROCm Linux adapter explicitly requires RoCEv2 GIDs and larger
queues, so **it is unsupported**. Do not relabel the native GID to bypass it.

## Build and offline gates

Neither root `make`, `make check`, nor existing install/lifecycle targets build,
install or activate native verbs. New gates are opt-in:

```sh
# Portable shared-code and extracted real object-body tests, no hardware:
make check-native-verbs
make check-native-verbs CC=clang CFLAGS='-fsanitize=address,undefined -g'
# Linux, explicit pinned public archive + new owned output; no download/install:
sh providers/strix_nhi/build-check.sh /input/rdma-core-pinned.tar.gz /scratch/new-build
# Kernel prerequisites/patch order and full compile helper:
# See kernel/verbs/README.md; all native + Thunderbolt modules must be rebuilt.
```

Executed evidence is **aarch64**, not Strix x86_64 qualification:

- Previous kernel stage: pinned stable v7.1.5
  (`155b42bec9cbb6b8cdc47dd9bd09503a81fbe493`) +unchanged8 backport/20 zerocopy
  patches +native core/driver patches; full `W=1 vmlinux modules`, MODPOST,
  enabled and default-off builds. This provider stage did not modify that kernel
  production source. Base v7.1 (non-stable) was not separately build-qualified.
- Provider stage: pinned rdma-core v62.0, real libibverbs, plugin, stock
  ibv_devices/ibv_devinfo and ordinary smoke compile with GCC14.4.0; actual plugin
  `dlopen`/constructor registration; command-boundary tests with ASan/UBSan.
- Shared production codec/assembly/protocol tests: malformed/reserved/CRC/length
  rejection, 0/1/3984/2MiB segmentation, generations, local TX vs placement,
  missing receive/RNR exhaustion, ACK loss/duplicate replay, permission failure,
  stable READ snapshot, WRITE replay and bidirectional4x2MiB with distinct IDs.
- Added exact production MR/post/CQ/flush function-body tests: SGE/size/key/PD/
  range/flags rejection, partial WR lists, MR EBUSY/refcount release after flush,
  unsignaled errors, CQ ordering/notification/missed events/overflow,
  destroyed-QP CQE compaction and CQE QP identity that stays valid after the QP
  is freed between poll and copy. Locks, time, scheduler and umem are **models**;
  these do not execute kernel pinning, uverbs, races, callbacks or DMA.
- Provider tests use actual v62 types/library with mocked command responses:
  strict identity, accepted prefixes, bad_wr, error return conventions, failed
  object destruction and poll bounds. They do not prove actual device discovery.

Exact public image/source/package hashes are in each component's
`build-dependencies.txt`. Build helpers consume explicit inputs, create scratch
outputs, never run an upstream dependency installer, and do not install into the
host. Normal compile containers have network disabled, no devices, host mounts
or privilege. Missing optional upstream libudev/systemd/pyverbs/manpage features
are not native-provider prerequisites.

## Live smoke gate (PASSING on the lab nodes)

**Executed on the lab nodes (max/max2, Fedora 43 / 7.2.8): bilateral ACTIVE,
full exchange PASS on both ends (0/1/3984-byte and 2 MiB windows, 18.9 MB
placed, ERR flush), repeated across module reload cycles.** The gate below
remains the required procedure for any future environment. Build the tool against
normal installed libibverbs with `make -C tools/verbs`. With no arguments it
prints usage and opens no device; `--selftest` is metadata-only. It will not
select another device automatically.

First verify real `/sys/class/infiniband/strix_nhiN` and associated
`/dev/infiniband/uverbsN`, then use the matching stock `ibv_devices` and
`ibv_devinfo -d strix_nhiN`. Confirm actual driver, ABI, limits, non-RoCE GID0,
unknown line rate and ACTIVE state; these are **unrun** acceptance checks.

Example only, after authorization (use actual native device/address on each
endpoint; the listener binds the explicit address, not all interfaces):

```sh
# Endpoint A: IP is this host's approved TCP metadata address.
./strix-verbs-smoke --live strix_nhi0 listen 10.99.0.1 18515
# Endpoint B: IP is endpoint A's approved metadata address.
./strix-verbs-smoke --live strix_nhi0 connect 10.99.0.1 18515
```

The tool performs DS4-shaped queries/registration/transitions and read-depth
checks, rejects oversized/multiple-SGE/invalid-key/inline WRs, preposts4 receives,
checks deregistration returns EBUSY, then exchanges boundary-size and two full
4x2MiB windows **in both directions**, checking each byte, WR identity, WC opcode,
receive length, duplicates and per-direction completion order. Control/polling
has30-second deadlines; TCP has no payload send path. Before/after kernel
`native_stats` must show increasing native frame counts, exactly the expected
SEND placement bytes and no READ/WRITE fallback/extra placement or bad frames.
It tests explicit ERR's ordered receive flush, then destroys QP before MR/free.
A failed teardown exits without manually freeing registered memory. Any error
is a failed gate, never a benchmark sample. This is **not a DS4 model run**.

## Remaining acceptance gates and deployment restrictions

Independent review is required, especially native core path/property lifetime,
MR/WR ownership, peer protocol and malformed input. No production acceptance
is implied by compilation or the above models. Still unrun:

1. Strix x86_64 deployment-config compile/signing/whole-stack ABI packaging;
   real Linux registration, uverbs/provider discovery, normal memlock isolation,
   honest zero-protocol-bits/non-RoCE-GID/undefined-AH INIT/RTR/RTS path.
2. Above two-host smoke; live READ/WRITE permission/replay and remote-key denial,
   real partial posting, CQ notification races/overflow/unsignaled ordering,
   short/unposted receives, memlock failures and unsupported-operation checks.
3. Packet corruption/reordering/ACK drops/exhausted retries; bidirectional
   saturation/control reserve, epoch/QP churn, restart, carrier loss/hot unplug,
   concurrent reset/destroy/deregistration/context close, lockdep and pin/DMA
   lifetime checks. No peer ACK may be needed to safely release local ownership.
4. Hardware qualification of separate HopIDs/rings and exact tunnel cleanup,
   coexistence with TB-IP/series20, abrupt peer-loss/unload and the **known
   inactive-peer NHI wedge**. READY means primed/started/locally activated with
   reciprocal fresh epochs; it does not prove loss/unload races cannot wedge
   hardware. No recovery or non-disruption guarantee exists yet.
5. Actual **unchanged Linux ROCm DS4** build against normal headers/library,
   provider selection/GID0, TP exchange, model correctness and output equivalence.
   Source-call-pattern matching and a smoke binary are not this test. No
   throughput, latency, inference-performance or GPU-direct claims until measured.

The native kernel configuration defaults off; its module has no autoload alias
and requires explicit enable. Its approved core lifetime extension is conditional
on the native config (apart from three behavior-preserving refactors listed in
the kernel README) and changes XDomain layout when enabled: rebuild all
Thunderbolt modules together. Suspend is rejected while bound. Do not change
existing series20 deployment, configfs, networking, IOMMU, permissions, or host
services as part of a provider build/test. The native and existing stream wire
protocols are different, and neither endpoint can silently interoperate with
the other's application protocol.

Known limitations, not fixed in this implementation:

- MR registration pins user pages (`ib_umem_get`) while holding the device
  mutex, so a slow-faulting mapping stalls the worker and removal meanwhile.
- A receive-side CQ overflow is reported to the peer as ACCESS and locally as
  LOC_PROT_ERR; the QP fails either way, but the status is misleading.
- Posting to a QP in ERR returns EINVAL instead of flushing the WR.
- Host-router NVM authentication (root only) disconnects all XDomain paths
  through the wildcard path teardown, which bypasses native retirement: the
  service keeps reporting ready with no path. **Operator exclusion:** do not
  start NVM authentication while the native service is bound.
- After netdev unregistration the native device stays dead until the service
  is reprobed.
- The XDomain properties pull reassembles multi-chunk responses without
  checking per-chunk generations, and the DATA section of the property block
  does not round-trip reliably at runtime (observed live as garbage `session`
  snapshots; inline VALUE properties are unaffected, which is why TB-IP never
  shows it). The driver no longer depends on dynamic property content at all:
  the rendezvous is one write-once `rxhop` immediate and the session handshake
  is wire-carried (HELLO/BIND). The core-side fix (**B**: per-chunk generation
  validation with whole-read retry in the properties pull, and the DATA-section
  corruption itself) remains a documented upstream follow-up, not a dependency.
- The provider build helper's scratch output embeds a build-environment
  RUNPATH; packaging must not ship those scratch binaries.
