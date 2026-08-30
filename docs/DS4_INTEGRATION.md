# DS4 NHI integration contract

This is the implementation boundary between the zero-copy USB4STREAM UAPI in
this repository and `~/Repositories/ds4`. It covers the coordinator-to-worker
layer-slice transport. The separate `ds4-server --tensor-parallel --transport
nhi` deployment is governed by `docs/TP_TRANSPORT_CONTRACT.md` and the systemd
examples in `tools/systemd/`; that imported-pool TP path is now the current
host pair's production configuration. For this layer-slice transport, the
baseline remains descriptor-framed v3 TCP unless NHI is explicitly selected.

Software stages 1–6 are implemented for one coordinator-to-worker device link.
Stage 7 now includes the host safety, lifecycle, mapped-pool, asymmetric
raw/reopen, and full-model NHI gates. Progress diagnostics isolated a lost
MSI-X notification and a separate path-before-ring fresh-open failure;
zero-copy patches 11 and 12 repair them. The post-fix pair passes 2,880
DS4-shaped exchanges across 27 stream sessions, plus required-NHI CPU-copy
and mapped full-model correctness.

## Current architecture

DS4 owns one opaque, reference-counted `ds4_transport` for each long-lived peer
connection. Registry entries, retained route plans, inbound worker connections,
and worker forwarders retain that object, so an NHI device fd, mmap, completion
dispatcher, and directional cursors live for the whole connection generation.

TCP continues to carry discovery, negotiation, frame headers, WORK/RESULT
metadata, tokens, routes, errors, snapshots, and health traffic. Only boundary
hidden states and successful logit payloads are eligible for NHI. The existing
v2 TCP layout remains available for old peers. Two v3 peers use descriptor
framing even when negotiation selects TCP, which makes v3 TCP the byte/output
equivalence oracle and automatic fallback.

The layer-slice baseline selection is `--dist-transport auto` with no
`--dist-nhi-device`. With no local NHI candidate, two current peers negotiate
descriptor-framed v3 TCP. Explicit `--dist-transport tcp` is the legacy-v2
compatibility path. Supplying an NHI device to `auto`, or selecting required
`nhi`, opts into the qualified single-link candidate; required mode is preferred
for measurements because it cannot silently produce a TCP sample.

The initial implementation deliberately permits only one remote worker link in
a v3/NHI route. Multi-hop NHI routing and pipelined mapped leases are later work.

## Protocol v3 framing

Each TCP frame begins with the existing 12-byte magic/type/length header. A v3
WORK or RESULT then carries a fixed 64-byte bulk descriptor. The descriptor
contains:

- transport mode (`NONE`, `TCP_INLINE`, or `NHI_OOB`) and payload kind;
- nonzero connection generation and directional sequence;
- session/request identity;
- exact payload bytes, element width, and NHI frame count; and
- zeroed flags/reserved fields for forward compatibility.

For `TCP_INLINE`, the TCP frame length and body include the tensor bytes. For
`NHI_OOB`, the TCP length covers control data only; the receiver validates the
descriptor before waiting for the NHI completion. There is intentionally no
wire slot number: each endpoint owns an independent local ring cursor.

The NHI message starts with a second 64-byte envelope that repeats generation,
sequence, identity, kind, width, byte count, and frame count. The receiver
accepts an event only when its local FIFO geometry and envelope both match the
already validated TCP descriptor.

Negotiation is `HELLO + v3 offer -> HELLO_ACK -> HELLO_READY`:

1. Both peers advertise protocol/capability bits and local NHI geometry.
2. The coordinator selects NHI only when the complete capability bundle and
   geometry match; otherwise `auto` selects descriptor-framed TCP.
3. Both peers configure the selected transport with the coordinator-issued
   generation.
4. The worker sends `HELLO_READY` only after its selected link is installed.
   The coordinator does not publish the worker or dispatch WORK before that
   barrier arrives and validates.

An `auto` worker may reconnect with v3 TCP if local NHI activation fails after
ACK, and may reconnect with exact v2 framing for a legacy peer. A required-NHI
configuration fails instead of silently downgrading.

That pre-activation fallback is not a recovery guarantee for an active NHI
generation. Once an NHI descriptor may be visible or DMA may be in flight, the
generation is abandoned rather than silently replayed in place. The kernel
repairs do not weaken this fail-closed rule; they remove the two observed causes
of lost progress and unreliable fresh opens.

## NHI backend and ownership

The Linux backend opens one exclusive `/dev/tbstreamX`, enables the zero-copy
UAPI, mmaps both fixed pools once, posts/reaps through the existing ioctls, and
runs one dispatcher for mixed TX, RX, CLOSE, and control-fd failure events. Its
CPU-copy path copies the descriptor envelope and tensor to/from mapped slots but
still bypasses TCP for the bulk data.

Mapped ROCm handoff is available only when both peers explicitly set
`DS4_DIST_NHI_MAPPED=1` and the complete mmap can be registered with
`hipHostRegister(..., hipHostRegisterMapped)`. A lease retains the transport and
exposes one contiguous payload span. There may be one active lease per
direction:

```text
TX: acquire -> direct graph output or GPU copy into mapped slot
    -> write all TCP control -> mark control visible
    -> HIP synchronize -> SUBMIT_TX -> release

RX: validate event + envelope -> acquire
    -> direct graph input or GPU copy from mapped slot
    -> HIP synchronize -> POST_RX -> release
```

DS4 uses leases only for contiguous 32-bit tensors. The ordinary staged mapped
path copies directly between the graph tensor and registered driver slot with
HIP—there is no intermediate application heap buffer. In the separately opt-in
`DS4_DIST_NHI_DIRECT_SLOTS` mode, `ds4_session_eval_layer_slice_device_io()`
wraps eligible lease aliases as external ROCm tensors, and graph kernels consume
the RX alias and/or write hidden-state or logit output directly into the TX
alias. The 2026-08-06 event profile confirms those direct masks were exercised.
Reduced-width activation packing, ineligible shapes, and ring-wrapping payloads
retain their staged or CPU-copy fallback.

These aliases still refer to NHI-owned pages registered with
`hipHostRegister()`, not native `hipMalloc` storage. Native HIP DMA-BUF pools are
a separate, unimplemented experiment. They require a bounded NHI DMA-BUF
importer and a verified system-scope RX acquire; see
`GPU_TO_GPU_FEASIBILITY.md`.

TX abort is recoverable only before any TCP control write is attempted, and
only for the immediately outstanding prepared sequence. RX abort, post-control
TX abort, partial control write, submit/repost error, malformed v3 framing, and
descriptor/event mismatch abandon the generation. A message is never silently
resent over TCP after its control descriptor might be visible or its NHI
submission might be ambiguous.

## Reconnect and stale-data rules

The coordinator issues a new nonzero generation for every accepted v3 link.
TX and RX sequences start at one and advance independently. Core descriptor
validation checks the selected generation and exact next directional sequence;
the NHI envelope repeats those fields. A stale descriptor, queued event, or
event/envelope mismatch poisons and shuts down that link rather than consuming
data as part of the new connection.

Fallback is therefore allowed only before activation or after abandoning the
old generation and reconnecting. It is never an in-place retry of a possibly
submitted WORK.

## Rollout status

1. **Complete:** `tools/rocm-map/tbstream-hip-map` passed on both hosts.
2. **Complete:** persistent, reference-counted transports preserve v2 TCP.
3. **Complete:** typed WORK/RESULT control/bulk split and TCP equivalence tests.
4. **Complete in software:** v3 descriptors, HELLO/ACK/READY negotiation,
   persistent CPU-copy NHI, single-link guard, and TCP/legacy fallback.
5. **Complete in software:** contiguous 32-bit mapped TX/RX leases and explicit
   HIP ownership synchronization, with a CPU-copy alternative for unsupported
   shapes. Both paths pass the initial post-fix full-model correctness gate.
6. **Complete in software:** per-link generations, directional sequences,
   repeated NHI identity envelope, and fail-closed stale/malformed handling.
7. **Initial hardware reliability gate passed:** zero-copy patch 11 flushes
   posted MSI-X clears, and patch 12 primes RX before enabling DMA paths. The
   exact 17/17/33/65-frame request and 127-frame response gate completed 2,880
   exchanges, 921,600 pair descriptor completions, and 26 close/reopen
   transitions with kicks disabled and no transport errors. Required-NHI
   CPU-copy completed 22 full-model requests and mapped mode completed 12, all
   with zero repeated-seed mismatches or service restarts. The controlled
   fixed-shape rates were 10.9421 tok/s (CPU copy), 10.9438 (mapped), and
   10.9540 (TCP v3), so qualification does not establish a speedup. Longer soak
   and active peer-reboot behavior remain release follow-ups.

## Host safety and operational gates

Both hosts now run with translated/default IOMMU operation enabled, and their
ROCm and NPU devices remain healthy. The scoped udev policy publishes the stream
device as `0660 root:tbstream`; DS4 runs as an unprivileged member rather than as
root. The allocator/follower lifecycle is installed and live, with
`/run/ds4-tbstream/device` published only after the generation and device policy
validate. On endpoint creation, its configured Thunderbolt netdev must first
reach carrier so `thunderbolt-net` reserves the TCP control/fallback HopIDs;
the gate is not reapplied to an existing valid endpoint. The single-link role
files then require local input/output HopID 9 exactly on both peers. Allocation
failure or a follower-side mismatch removes the unpublished ConfigFS child and
never falls back to an arbitrary HopID. These gates were active throughout the
post-fix raw and full-model qualification.

The twelve-patch module set is loaded live and included in both rebuilt
initramfs images. Both hosts booted those images successfully; the enabled
carrier-first/exact-9 lifecycle also passed a subsequent peer reboot, including
carrier, bidirectional ping, endpoint convergence, v3 reconnect, and a
full-model API smoke check.

The split model leaves only 28–32 GiB available per host. Do not load a second
pair concurrently. Preserve the old versioned deployment for rollback and use
a coordinated stop/restart window for model tests.

Host-independent verification covers strict warnings-as-errors builds, v3
serialization/negotiation tests, transport sequence and lease-state tests,
WORK/RESULT TCP equivalence, sanitizers, and repeated runs. The live asymmetric
and full-model gates now complement those checks. TCP v3 remains the safest
layer-slice interoperability baseline because it is mature and the controlled
NHI cohort shows no throughput gain; it is no longer the only DS4 production
shape because the separate TP/NHI server stack has been promoted.
