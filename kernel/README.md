# Kernel work

Two ways to get USB4STREAM onto the 7.1.5 Strix Halo hosts. Pick one:

## Option A — boot Linux 7.2

If a 7.2(-rc) kernel boots cleanly on the hosts, nothing here is needed for
step 2 of the plan: enable `CONFIG_USB4_STREAM=m` (needs `CONFIGFS_FS` /
`USB4_CONFIGFS`) and go straight to benchmarking.

## Option B — backport to 7.1.5 (module-only, no kernel rebuild)

`backport/v7.1/` holds the 8-patch USB4STREAM series, conflict-resolved so it
applies cleanly on v7.1 (verified with `git am` on the v7.1 tag):

1. `tb_property_merge_dir()`
2. service-driver local/remote properties (`tb_service_properties_changed()`)
3. `ring_frame_size()` moved to `<linux/thunderbolt.h>`
4. service-driver interrupt throttling (`tb_ring_throttling()`)
5. `tb_ring_size()` helper
6. `tb_ring_flush()`
7. thunderbolt ConfigFS support (`tb_configfs_register_group()`)
8. USB4STREAM itself (`thunderbolt_stream.ko`, `/dev/tbstreamX`)

`backport/upstream/` is the pristine 9-patch series from mainline for
provenance (patch 2 there is a KUnit test that needs an unrelated test
refactor; it is intentionally dropped from the v7.1 series).

Resolution notes (deviations from mainline, all deliberate):

- `property.c`: v7.1 still has the `depth` recursion parameter on
  `__tb_property_parse_dir()`; kept.
- `xdomain.c`: mainline's base had debugfs-remove moved to unregister
  (`4d5fc3f4`) and an xdomain ref held per service (`8b406099`). Neither is in
  the series, so the backport keeps v7.1 semantics: `tb_service_debugfs_remove()`
  stays in `tb_service_release()` (it is idempotent), `svc->dev.parent` is
  assigned without `get_device()`, and the unused `__unregister_service()`
  helper is dropped.

### Build on a host

The patches change `struct tb_service`/`struct tb_ring` layout and
`include/linux/thunderbolt.h`, so `thunderbolt.ko` and `thunderbolt_net.ko`
must be rebuilt and replaced together with the new `thunderbolt_stream.ko`.

```sh
# exact source of the running kernel
git clone --depth 1 --branch v7.1.5 \
    https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git
cd linux
git am /path/to/strix-rdma/kernel/backport/v7.1/*.patch
git am /path/to/strix-rdma/kernel/zerocopy/*.patch

# match the running kernel's config and symbol CRCs
cp /boot/config-"$(uname -r)" .config          # or zcat /proc/config.gz
scripts/config --module CONFIG_USB4_STREAM
make olddefconfig
make modules_prepare
cp /lib/modules/"$(uname -r)"/build/Module.symvers .   # if CONFIG_MODVERSIONS

# version string must match `uname -r` exactly for module load
# (set CONFIG_LOCALVERSION / disable LOCALVERSION_AUTO as needed)

make -j"$(nproc)" M=drivers/thunderbolt modules
make -j"$(nproc)" M=drivers/net/thunderbolt modules
sudo make M=drivers/thunderbolt modules_install INSTALL_MOD_DIR=updates
sudo make M=drivers/net/thunderbolt modules_install INSTALL_MOD_DIR=updates
sudo depmod -a
```

`scripts/host-build.sh` performs the clone, incrementally applies both patch
series by exact commit subject, and builds this module set. It is safe to rerun
after adding a later zero-copy patch, but intentionally refuses a kernel source
tree with uncommitted changes.

Reboot (simplest) or unload/reload the whole stack
(`thunderbolt-net`, `thunderbolt_stream`, then `thunderbolt`). `updates/`
takes precedence over the stock modules via depmod search order.

Caveats:

- Secure Boot hosts need the modules signed with an enrolled MOK.
- If `CONFIG_MODVERSIONS` is off, the version-string match alone gates
  loading; the struct layout changes are only used inside the three rebuilt
  modules, which is why they must be replaced as a set.
- After boot: `modprobe thunderbolt_stream`, then
  `tools/scripts/tbstream-setup.sh` (see ../bench/README.md).

### Least-privilege device access

The misc device defaults to `0600 root:root`. Do not run DS4 as root merely to
open a DMA-mapped stream. Install the repository's scoped udev rule and use a
dedicated group instead:

```sh
sudo groupadd --force --system tbstream
sudo usermod -aG tbstream "$(id -un)"
sudo install -m 0644 tools/udev/99-tbstream.rules \
    /etc/udev/rules.d/99-tbstream.rules
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change --subsystem-match=misc \
    --sysname-match='tbstream*'
```

Start a new login session so the supplementary group is active, then verify
that the intended account can read and write `/dev/tbstream0`. The rule matches
only `SUBSYSTEM=="misc"` devices named `tbstreamN`; it intentionally does not
grant world access.

On the current `max`/`max2` pair this policy is installed and the lifecycle
publishes `0660 root:tbstream` devices through
`/run/ds4-tbstream/device`.

## Zero-copy UAPI (step 3 of the plan)

`zerocopy/` contains the twelve-patch follow-on series to apply after the v7.1
backport:

1. Add `RING_FRAME_NO_INTERRUPT` so a ring client can suppress completion
   interrupts for non-final frames.
2. Add mmap-able TX/RX frame pools plus `TBSTREAM_ZC_ENABLE`, `GET_INFO`,
   `SUBMIT_TX`, `POST_RX`, and `REAP` ioctls. Multi-frame messages use
   `TBSTREAM_DATA_MORE`, and TX completion reporting is per message.
3. Require exclusive device ownership while zero-copy mode is enabled so
   independent file descriptions cannot corrupt shared slot cursors.
4. Add a backward-compatible flagged RX repost operation that fixed-size
   streams use to request message-boundary interrupts.
5. Keep interrupts on both the first and last RX frame so a CLOSE immediately
   following a fixed-size message cannot land on a suppressed descriptor.
6. Observe streaming-DMA ownership for the complete TX mapping: require an
   idle TX ring at enable, synchronize every mapped page for CPU access before
   exposing it, return it to the NHI on submit, and return it to userspace
   before publishing TX completion. Unmap TX and RX pages at their original
   mapping size.
7. Harden the zero-copy transition against same-file legacy I/O and RX
   callbacks, rebase the idle TX cursor to the UAPI's zero-based starting
   position, and reclaim an unsent TX slot if ring enqueue fails. If an
   enqueue failure splits a multi-frame message, latch a terminal zero-copy
   error so later submit/post/reap calls return `EIO` instead of merging data
   across the lost boundary. Event reaping peeks and copies before dequeue, so
   an `EFAULT` leaves the ownership event available for retry.
8. Fix ThunderboltIP receive-HopID ownership when USB4STREAM and
   `thunderbolt-net` share an XDomain. A positive preferred HopID can return a
   different free ID; the network driver now releases that mismatch
   immediately, records only an exact successful allocation, and releases only
   the recorded ID during setup rollback or teardown. This prevents both a
   leaked allocation and teardown freeing a stream-owned HopID.
9. Make USB4STREAM attachment transactional. Peer-requested HopIDs remain local
   until exact input and output allocations succeed; any later failure releases
   only the directions actually acquired, clears both ownership fields, and is
   propagated to ConfigFS creation or reported during device reattachment.
   Replacement writes retain the prior allocation until the exact new ID is
   acquired, creation is serialized against service detach, pre-registration
   cleanup skips `misc_deregister()`, and manual recovery wakes blocked openers.
10. Add zero-copy progress diagnostics and terminal-error reporting. The stream
    UAPI can snapshot cumulative ownership counters plus TX/RX software and NHI
    descriptor positions, interrupt/workqueue progress, and the first terminal
    error. Hardware positions carry an explicit validity flag and are read only
    while runtime PM confirms that the controller is active. A privileged,
    default-off diagnostic kick can schedule the ordinary ring worker without
    mutating descriptors or interrupt state.
11. Flush non-auto-clear MSI-X interrupt acknowledgments with a safe NHI
    register read. This orders the posted W1C before deferred ring work and
    prevents a later completion from losing its only notification.
12. Start both rings and fully prime RX before enabling the XDomain DMA paths,
    matching ThunderboltIP's ordering. This restores reliable close/reopen E2E
    credits and completes partial TX/RX allocation cleanup.
13. Add `TBSTREAM_ZC_DMABUF_PROBE`, a privileged, default-off, no-traffic
    DMA-BUF import probe, and the shared `stream-sg.h` segment validation
    and frame-flattening rules the future imported-pool mode will reuse.

The original five-patch zero-copy implementation was built, deployed, and
benchmarked on both test hosts; see
`../bench/results/2026-08-02-usb4stream-zc.md` and
`../bench/results/2026-08-04-usb4stream-zc-oneway-rocm.md`. The userspace
`pingpong` tool provides `zping`/`zpong` for RTT and `ztx`/`zrx` for one-way
throughput. The complete twelve-patch series was independently reviewed, built
byte-identically, deployed, embedded in both initramfs images, and booted on the
two test hosts. The isolated ABI and source-contract suite passes 40/40.

The progress counters localized the asymmetric stall to a lost MSI-X
notification, while the separate fresh-open failure showed zero hardware
progress and exposed the path-before-ring ordering. With patches 11 and 12,
the exact DS4-shaped gate passes 2,880 exchanges across 27 sessions and 26
close/reopen transitions: 921,600 descriptor completions across the pair with
diagnostic kicks disabled and no kernel transport errors. Required-NHI
full-model CPU-copy and mapped correctness runs also pass. See
`../bench/results/2026-08-05-nhi-msix-rx-prime-fix.md`.

`TBSTREAM_ZC_GET_STATS` is safe to call after a terminal zero-copy failure and
reports already-queued ownership events before `REAP` starts returning `EIO`.
NHI register positions are meaningful only when the corresponding ring includes
`TBSTREAM_ZC_RING_F_HW_VALID`. `TBSTREAM_ZC_KICK` is a lab diagnostic, not a
recovery API: it requires `CAP_SYS_RAWIO` and remains disabled unless
`thunderbolt_stream.zc_diagnostic_kick=1` (or the equivalent sysfs module
parameter) is explicitly set. The DS4-shaped gate documents the timeout capture
and directional-kick workflow in `../tools/ds4-shape/README.md`.

Validate the complete series without changing the local kernel checkout:

```sh
make -C kernel/tests test
```

The 40-case test creates an isolated worktree, applies every zero-copy patch
in order, compares the final UAPI with the userspace mirror, verifies its
32/64-bit ABI, compiles and runs the SG-flatten geometry unit tests, and
checks the ownership, failure, diagnostic, and teardown invariants.

`TBSTREAM_ZC_ENABLE` is intended to run immediately after opening a fresh
stream. It returns `EBUSY` if legacy TX is still in flight or any RX frame has
already completed or been consumed; this keeps the mapped slot cursors and
zero-copy event stream unambiguous. A terminal partial-submit error requires
closing and reopening the stream before zero-copy I/O can resume.

This is still a proof-of-concept UAPI. It has ordered fixed slots and bounded
in-flight frames, but not generation numbers, eventfd/io_uring notification,
or a production protocol for peer credit negotiation and reconnect recovery.
Those belong with the DS4 transport integration after the ROCm mapping gate.

The ROCm gate in `../tools/rocm-map/` registers the driver-owned mapped TX pool
and verifies GPU writes and reads without handing any frame to the NHI. It
passed for the full 16 MiB pool on both test hosts, so DS4 can use this mapping
without DMA-BUF for that mode. Native `hipMalloc` DMA-BUF ownership is a
separate, unimplemented importer/coherency experiment; see
`../docs/GPU_TO_GPU_FEASIBILITY.md`.

## DMA-BUF import probe (gate 3 of the native-pool experiment)

Patch 13 adds the first piece of the bounded native-allocation experiment:
a no-traffic diagnostic that answers "can the NHI DMA device map this
DMA-BUF, and with what segment geometry?" before any imported-pool mode
is built. Given one DMA-BUF fd, a direction, and a frame-aligned range,
it attaches the buffer to the stream ring's DMA device, pins it, maps it,
validates every mapped segment against the production geometry rules
(frame-aligned, overflow-free, frame addresses never crossing a segment),
tears the mapping down, and only then reports aggregate statistics:
requested/covered bytes, original and mapped SG entry counts, tightest
segment alignment, and largest mapped segment. It never programs a ring
descriptor, never changes path state, and never exposes a DMA address.

The probe requires `CAP_SYS_RAWIO` and the default-off
`thunderbolt_stream.zc_diagnostic_dmabuf=1` module parameter (writable at
runtime by root), and refuses configured zero-copy sessions. Ranges are
bounded to 1 GiB per call. `../tools/dmabuf-probe/` runs it from
userspace, sourcing the DMA-BUF either from an inherited fd (`--fd`, for
example a HIP export) or from a CPU-only `/dev/udmabuf` allocation
(`--udmabuf`) that smokes the import path without a GPU.

The SG-flatten rules in `drivers/thunderbolt/stream-sg.h` are pure
arithmetic shared verbatim with the future imported-pool mode; the series
test compiles the same header in userspace and exercises one/many/coalesced
segments, exact boundaries, zero and unaligned segments, short and overlong
coverage, address and cumulative-length overflow, excessive frame counts,
and final partial segments.
