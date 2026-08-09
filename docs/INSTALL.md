# Installing strix-rdma

End-to-end path from a fresh clone to a measured Thunderbolt/USB4 stream
between two hosts. Deep detail lives in [kernel/README.md](../kernel/README.md)
(backport + zero-copy series), [bench/README.md](../bench/README.md)
(benchmark matrix), and
[tools/systemd/tbstream-lifecycle.md](../tools/systemd/tbstream-lifecycle.md)
(managed lifecycle). This file is the ordered checklist that ties them
together.

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
make            # tools/pingpong/pingpong, tools/ds4-shape/tbstream-ds4-gate
make rocm       # tools/rocm-map/* (needs hipcc; auto-detects /opt/rocm*)
make check      # no-hardware test suite: selftest, lifecycle mocks, python
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
`make check-kernel` (needs a Linux checkout; default `KERNEL_SRC=./linux`,
38-case worktree suite).

## 3. Device access policy (both hosts)

`/dev/tbstreamN` defaults to `0600 root:root`. Do not run workloads as root;
use the scoped udev rule and a dedicated group:

```sh
sudo groupadd --force --system tbstream
sudo usermod -aG tbstream "$(id -un)"       # new login session required
sudo install -m 0644 tools/udev/99-tbstream.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change --subsystem-match=misc --sysname-match='tbstream*'
```

(Installed automatically by `make install-lifecycle` in step 4b.)

## 4a. Bring up a stream — quick/manual

With the cable connected and `thunderbolt_stream` loaded on both ends:

```sh
hostA$ sudo tools/scripts/tbstream-setup.sh ds4    # prints /dev/tbstreamX
hostB$ sudo tools/scripts/tbstream-setup.sh ds4    # same name on both sides
```

`RING_SIZE=4096` and `THROTTLING=0` env vars matter at QD1; see
[bench/README.md](../bench/README.md).

## 4b. Bring up a stream — managed lifecycle (recommended for repeated use)

The fail-closed systemd/udev lifecycle converges the endpoint at boot and on
peer hotplug, and publishes a stable path at `/run/ds4-tbstream/device`.
Exactly one endpoint is the allocator:

```sh
hostA$ sudo make install-lifecycle ROLE=allocator
hostB$ sudo make install-lifecycle ROLE=follower
```

Review `/etc/sysconfig/ds4-tbstream` on each host (HopIDs, `TBSTREAM_NETDEV`),
then on both:

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now ds4-tbstream-reconcile.timer
sudo systemctl start ds4-tbstream-reconcile.service
cat /run/ds4-tbstream/device        # e.g. /dev/tbstream0
```

Semantics, holder locking, and cleanup rules:
[tools/systemd/tbstream-lifecycle.md](../tools/systemd/tbstream-lifecycle.md).

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

Confirms the GPU can read/write the mmap-ed DMA pool without a DMA-BUF
fallback:

```sh
tools/rocm-map/tbstream-hip-map /dev/tbstream0
```

## 7. DS4 integration

The transport contract for the DS4 inference stack is
[docs/DS4_INTEGRATION.md](DS4_INTEGRATION.md). Production currently ships
descriptor-framed protocol-v3 TCP (`--dist-transport auto`, no
`--dist-nhi-device`); supplying an NHI device selects the qualified
experimental path.

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
- **Stale ConfigFS state**: `sudo /usr/local/libexec/ds4-tbstream-reconcile.sh
  --cleanup` (managed installs) — it refuses while any holder has the device
  open, by design.
- **Stalls/diagnostics**: `TBSTREAM_ZC_GET_STATS` counters and the
  `tools/ds4-shape/README.md` timeout-capture workflow localize lost
  interrupts vs. ring stalls.
