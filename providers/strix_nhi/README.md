# strix_nhi rdma-core provider (experimental)

This is a normal rdma-core plugin for the opt-in native kernel backend in
[`kernel/verbs`](../../kernel/verbs/README.md). It is **not** a replacement
libibverbs, LD_PRELOAD shim, Soft-RoCE provider, or TCP payload transport. It
forwards standard uverbs commands. No private input/output bytes, doorbells,
mmap queues, completions, link state, GID type or capabilities are invented here.

The supported source is **rdma-core v62.0**, commit
`31af04ec84378724cb6256814d4ffde359a7123b`, private provider ABI **59**. The
kernel native uverbs ABI is **1** (these are different version numbers).
`rdma-core-v62.patch` adds only the provider CMake subdirectory. `rdma_provider`
generates the ordinary `strix_nhi.driver` configuration and
`libstrix_nhi-rdmav59.so`. Both are needed for normal discovery. Do not copy the
plugin into a distribution with a different private ABI or replace a system
libibverbs with a mismatched scratch build.

## Matching and behavior

- Match only ABI1, `RDMA_DRIVER_UNKNOWN`, `strix_nhi` followed by decimal digits,
  and a resolved `device/driver` path ending in
  `/bus/thunderbolt/drivers/strix_nhi`. The kernel registers the real service as
  parent. UNKNOWN alone, name alone, GUIDs, netdev names and Hellas devices do
  not match. Renaming the RDMA device outside this prefix is unsupported.
- Query device/port/QP, PD, CPU MR, CQ, RC QP, state transitions, notification,
  SEND/RECV/READ/WRITE and polling all use rdma-core `ibv_cmd_*` helpers.
  `query_gid` uses the normal core interface. Unsupported context operations
  retain rdma-core's explicit unsupported-operation defaults.
- One command per WR under the direction's QP mutex bounds rdma-core's stack
  allocation and preserves the accepted prefix. Only SGE count/pointer safety
  is checked before marshalling; the kernel validates opcodes, flags, lengths,
  lkeys/rkeys, PDs, permissions and state. The caller's WR chain is unchanged;
  `bad_wr` refers to the original first rejected WR, not a stack copy.
- Polling is bounded to64 entries per command and rejects negative counts.
  No queued local operation is treated as a successful completion. CQEs come
  solely from the kernel peer-placement/retry/flush engine.
- Pointer-returning failures set errno; verbs integer methods return the command
  error code (poll returns negative on error). Failed destroy/deregistration
  retains the userspace object, including kernel EBUSY for owned MRs. Successful
  synchronous QP destroy precedes deregistration/free. Like normal verbs,
  applications exclude concurrent destruction/use of the same object.

## Build without installation

Linux compiler, CMake, make, Python3 (upstream build generator), pkg-config and
libnl3/libnl-route development files are prerequisites. No dependency installer
is provided. Exact executed arm64 image/package hashes and the pinned source
archive hash are in [`build-dependencies.txt`](build-dependencies.txt).

Obtain the public archive independently from its pinned URL there. The build
helper checks its SHA256 **before extraction**, requires a new output directory,
patches only that owned copy, builds the actual plugin, libibverbs and stock
`ibv_devices`/`ibv_devinfo`, compiles the standard-verbs smoke and executes offline
tests. It does not fetch, install, enumerate/open devices, load modules, modify
system configuration, or access another source checkout.

```sh
sh providers/strix_nhi/build-check.sh /input/rdma-core-pinned.tar.gz /scratch/new-verbs-build
# Extra actual-provider ASan/UBSan command-boundary tests:
CFLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer' \
  sh providers/strix_nhi/test-provider.sh /scratch/new-verbs-build/build /scratch/new-sanitizer-tests
```

`IN_PLACE=1` generates scratch-local libibverbs config/library paths and rpaths,
so the built stock tools can discover the plugin without installing anything.
For a separately authorized deployment, package the matching provider and its
`.driver` config with the matching rdma-core private ABI using that project's
normal packaging conventions. This workflow has not installed either. Building
from these pins is not authorization to change a production host's RDMA stack.

Tests compile the **actual provider against actual rdma-core headers/library**,
mocking only selected command boundaries: negative matching, bounded WRs and
partial accepted lists, ordering, retained failed-destroy objects and bounded
poll errors. A separate executable dlopens the real plugin with immediate
symbol resolution/registration. None of these tests proves sysfs/uverbs device
discovery, a kernel command, DMA, concurrency, or hardware behavior. See
[`docs/NATIVE_VERBS.md`](../../docs/NATIVE_VERBS.md) for those unrun gates.

## Provenance

New provider, tests and build helper are GPL-2.0-only, authored against the
pinned rdma-core `libibverbs/driver.h`, command implementations and provider
integration conventions. No Hellas code was copied. Its pinned experimental
repository (`76ba39b630a70accb72f19388eefe48844b50eb8`) was an architecture
reference, not a dependency; its protocol, provider and licenses are not being
repackaged. The upstream rdma-core source and its own per-file licenses remain
unchanged except for the one-line build integration patch. The native source
has no compatibility claim with Hellas, RoCE or InfiniBand packets.
