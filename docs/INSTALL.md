# Installing strix-rdma

End-to-end path from a fresh clone to a measured Thunderbolt/USB4 stream
between two hosts. Deep detail lives in [kernel/README.md](../kernel/README.md)
(backport + zero-copy series), [bench/README.md](../bench/README.md)
(benchmark matrix), and
[tools/systemd/tbstream-lifecycle.md](../tools/systemd/tbstream-lifecycle.md)
(endpoint bring-up and teardown rules). This file is the ordered checklist that
ties them together.

## Tested configuration

| Component | Tested with |
|---|---|
| Hosts | 2× Strix Halo systems, direct USB4/Thunderbolt cable |
| Kernel | Linux 7.1.5 (Fedora) + this repo's backport; 7.2+ has USB4STREAM upstream |
| GPU stack | ROCm/HIP, `gfx1151` (only needed for the GPU mapping tools) |
| Userspace | gcc/clang, GNU make, bash, python3 |

Everything except the ROCm tools is plain POSIX C and shell; the kernel steps
must run on the hosts themselves.

## 1. Build the userspace tools

```sh
make            # tools/pingpong/pingpong, tools/dmabuf-probe/tbstream-dmabuf-probe
make rocm       # tools/rocm-map/* (needs hipcc; auto-detects /opt/rocm*)
make check      # no-hardware test suite: device access policy, UAPI contracts
```

## 2. Get USB4STREAM into the kernel (both hosts)

Two options — see [kernel/README.md](../kernel/README.md) for full rationale:

- **Option A — kernel ≥ 7.2:** enable `CONFIG_USB4_STREAM=m` and skip the
  backport. The zero-copy UAPI series in `kernel/zerocopy/` still applies on
  top if you want the mmap/ioctl path rather than read/write.
- **Option B — backport onto 7.1.5 (tested):** module-only, no full kernel
  rebuild. The scripts clone stable v7.1.5, apply `kernel/backport/v7.1/` and
  `kernel/zerocopy/`, and rebuild the three-module set (`thunderbolt`,
  `thunderbolt-net`, `thunderbolt_stream`) with a matching version string:

  ```sh
  ./kernel/scripts/host-build.sh          # clone + patch + build (SRC=~/src/linux-stable)
  ./kernel/scripts/host-sign.sh           # Secure Boot hosts only: sign with enrolled MOK
  ./kernel/scripts/host-install.sh        # install to updates/, depmod, reload stack
  ```

  Verify each module resolves under `updates/`:

  ```sh
  modinfo -n thunderbolt thunderbolt_stream thunderbolt-net
  ```

Optionally validate the zero-copy series without touching the hosts:
`make check-kernel` (needs a Linux checkout with the USB4STREAM backport;
default `KERNEL_SRC=./linux`, 45-case worktree suite). CI runs `make check`
and a sparse v7.1.5 + backport `check-kernel` job.

## 3. Device access policy (both hosts)

`/dev/tbstreamN` defaults to `0600 root:root`. Do not run workloads as root;
use the scoped udev rule and a dedicated group. The stream UAPI mmaps
DMA-mapped pages, so the rule grants `0660 root:tbstream` and must never be
widened to world-writable:

```sh
sudo make install-lifecycle                 # rule + lifecycle doc
sudo groupadd --force --system tbstream
sudo usermod -aG tbstream "$(id -un)"       # new login session required
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match='tbstream*'
```

The rule alone, without the doc install:

```sh
sudo install -m 0644 tools/udev/99-tbstream.rules /etc/udev/rules.d/
```

## 4. Bring up a stream

`tbstream-setup.sh` creates the ConfigFS endpoint, writes ring size and
interrupt throttling before the HopIDs activate the stream, and prints the
device path. The stream name must match on both sides:

```sh
hostA$ sudo tools/scripts/tbstream-setup.sh link    # prints /dev/tbstreamX
hostB$ sudo tools/scripts/tbstream-setup.sh link    # same name on both sides
```

It refuses to create a second endpoint when one already exists, because
automatic HopID allocation can take a path `thunderbolt-net` needs;
`TBSTREAM_SETUP_FORCE=1` overrides. `RING_SIZE=4096` and `THROTTLING=0` env
vars matter at QD1 (see [bench/README.md](../bench/README.md)); the kernel
defaults are already 4096/0 after the zero-copy series.

Read the teardown and wedge rules before running anything long-lived:
[tools/systemd/tbstream-lifecycle.md](../tools/systemd/tbstream-lifecycle.md).
Transmitting toward a peer whose device is not open and enabled can wedge the
whole XDomain connection, Thunderbolt-IP included.

## 5. Smoke test the link

Legacy read/write path:

```sh
hostB$ tools/pingpong/pingpong pong -d /dev/tbstream0 -s 64k
hostA$ bench/scripts/run-pingpong.sh /dev/tbstream0 smoke
```

Zero-copy path (after the `kernel/zerocopy/` series):

```sh
hostB$ tools/pingpong/pingpong zrx -d /dev/tbstream0 -s 1m -V
hostA$ tools/pingpong/pingpong ztx -d /dev/tbstream0 -s 1m -n 1000 -V
```

`ztx` reports full-DMA throughput (waits for TX completions); `zrx -V`
verifies payload geometry and pattern.

## 6. ROCm mapping gate (optional, GPU hosts)

Confirms the GPU can read/write the driver-owned mmap pool without requiring
DMA-BUF for this gate:

```sh
tools/rocm-map/tbstream-hip-map /dev/tbstream0
```

This does not exercise the separate native `hipMalloc` DMA-BUF importer
experiment described in
[GPU_TO_GPU_FEASIBILITY.md](GPU_TO_GPU_FEASIBILITY.md). That path is
diagnostic-gated: it needs `thunderbolt_stream.zc_diagnostic_dmabuf=1` set
explicitly, `CAP_SYS_RAWIO`, and the series-20 module installed under
`/lib/modules/$(uname -r)/updates/` and in the initramfs
(`dracut --force --kver "$(uname -r)"` on the tested Fedora hosts). Keep the
parameter off for ordinary deployments.

## 7. Native verbs path (optional, default-off)

The verbs stack is a separate service with its own bring-up and its own
conflict with the stream setup above — zero-copy mode blacklists
`thunderbolt-net` and claims both ring HopIDs, so the two cannot be enabled on
the same link at the same time:

```sh
sudo tools/scripts/strix-verbs-bringup.sh   # once per boot; read its header first
make -C tools/verbs                         # smoke tool against installed libibverbs
```

Prerequisites, the bounded interface contract and the acceptance gates:
[docs/NATIVE_VERBS.md](NATIVE_VERBS.md). The data plane, its measurements and
the defects they exposed: [docs/ZEROCOPY_VERBS.md](ZEROCOPY_VERBS.md). Kernel
patch order and reproduction: [kernel/verbs/README.md](../kernel/verbs/README.md).

## Troubleshooting

- **`no stream service found`** from `tbstream-setup.sh`: cable not linked or
  the peer hasn't loaded `thunderbolt_stream`. Inspect with `tblist -A` from
  [intel/tbtools](https://github.com/intel/tbtools).
- **Module refuses to load**: version string must match `uname -r` exactly and
  all three modules must come from `updates/` as a set — rerun
  `host-install.sh`. Secure Boot additionally requires `host-sign.sh` with an
  enrolled MOK.
- **`EBUSY` on `TBSTREAM_ZC_ENABLE`**: zero-copy must be enabled on a fresh
  open before any legacy I/O; close, reopen, retry.
- **Stale ConfigFS state**: stop every consumer, then
  `rmdir /sys/kernel/config/thunderbolt/stream/<service>/<name>`. It refuses
  while any fd is open, by design. ConfigFS stream groups also pin
  `thunderbolt_stream`, so remove them before reloading the module.
- **Stalls/diagnostics**: the `TBSTREAM_ZC_GET_STATS` counters separate lost
  interrupts from ring stalls. [docs/TP_TRANSPORT_CONTRACT.md](TP_TRANSPORT_CONTRACT.md)
  lists the ring invariants, and
  [tools/systemd/tbstream-lifecycle.md](../tools/systemd/tbstream-lifecycle.md)
  the recovery sequence for a wedged link.
