# DS4 USB4STREAM lifecycle units

These files make the ephemeral USB4STREAM ConfigFS endpoint converge after a
local boot, peer reboot, or service-ordinal swap. They are templates: installing
them is an explicit production change and is not performed by the test suite.
As of 2026-08-05 they are installed and live on the current pair, with `max2`
as allocator and `max` as follower; the repository copies remain the source for
future installation or rollback. After deployment, an enabled `max2` reboot
preserved Thunderbolt-net carrier and converged both endpoints to the configured
9/9 pair before the production v3 TCP model reconnected.

The helper owns only ConfigFS children named `ds4`. It resolves the current
Thunderbolt service by `key=stream`, derives the misc-device index from that
ConfigFS child, verifies the device number and `0660 root:tbstream` policy, and
then atomically publishes `/run/ds4-tbstream/device`. DS4 should use this stable
path rather than `/dev/tbstream0`. The stable path is withdrawn before every
reset; direct raw-device consumers must be stopped because they can bypass that
publication boundary and the helper's advisory lock.

Exactly one endpoint allocates HopIDs. For the current pair, install
`tools/sysconfig/ds4-tbstream.allocator` as `/etc/sysconfig/ds4-tbstream` on
`max2`, and install the follower sample on `max`. A follower removes its own
unpublished attempt and retries until the allocator's reversed HopIDs are
visible; both endpoints must never be configured as followers.

The current single-link samples pin each local direction to HopID 9 with the
paired `TBSTREAM_IN_HOPID` and `TBSTREAM_OUT_HOPID` settings. The allocator
requests those exact positive values instead of automatic `-1` allocation; if
either value is unavailable, it deactivates and removes the partial child and
retries the same pair without falling back. The follower still does not
allocate independently: it learns the allocator's advertised pair, reverses it
through the kernel protocol, and requires the resulting local values to match
its configured pair before publication. Consequently, asymmetric settings on
the follower must be the allocator's `OUT/IN` values in reverse local order.
Both variables must be empty (automatic lab behavior) or both must be integers
from 8 through the architectural 11-bit maximum of 2047. Hardware may expose a
smaller maximum; an unsupported exact value fails closed at the kernel write.

Both role samples set `TBSTREAM_NETDEV=thunderbolt0`. Before creating a new
ConfigFS child, the reconciler waits for that interface's
`/sys/class/net/thunderbolt0/carrier` value to become `1`. This lets
`thunderbolt-net` allocate its HopIDs first and keeps the TCP control/fallback
link available when USB4STREAM draws from the same adapter pool. Set the value
to the actual Thunderbolt netdev name on each host, or leave it empty only when
that physical link intentionally has no Thunderbolt network interface. The
wait shares `TBSTREAM_WAIT_SECONDS`; a single-pass watchdog checks once and
returns immediately. Carrier is a creation-order gate, not an endpoint health
condition: after a valid managed endpoint exists, carrier loss does not reset
it or add work to the healthy fast path.

## Install layout

Install the files as root-owned, non-writable production artifacts:

```sh
groupadd --force --system tbstream
usermod -aG tbstream jryates
install -d -o root -g root -m 0755 /usr/local/libexec \
    /usr/local/share/doc/strix-rdma
install -o root -g root -m 0755 tools/scripts/ds4-tbstream-reconcile.sh \
    tools/scripts/ds4-tbstream-cleanup.sh /usr/local/libexec/
install -o root -g root -m 0644 tools/systemd/ds4-tbstream-reconcile.service \
    tools/systemd/ds4-tbstream-reconcile-watchdog.service \
    tools/systemd/ds4-tbstream-reconcile.timer /etc/systemd/system/
install -o root -g root -m 0644 tools/modules-load/ds4-tbstream.conf \
    /etc/modules-load.d/ds4-tbstream.conf
install -o root -g root -m 0644 tools/udev/98-ds4-tbstream-reconcile.rules \
    /etc/udev/rules.d/98-ds4-tbstream-reconcile.rules
install -o root -g root -m 0644 tools/udev/99-tbstream.rules \
    /etc/udev/rules.d/99-tbstream.rules
install -o root -g root -m 0644 tools/systemd/tbstream-lifecycle.md \
    /usr/local/share/doc/strix-rdma/tbstream-lifecycle.md
```

Install exactly one role sample as `/etc/sysconfig/ds4-tbstream`, reload udev
and systemd, then enable both the service and timer. Start a new login before DS4
so its process has the `tbstream` supplementary group. The service's initial
start waits for a peer; the timer and udev add event converge later generations.
The timer remains necessary for property changes and because a remove event
cannot reliably read the already-removed service's `key` attribute. Udev starts
reconcile immediately on stream-service add; the staggered timer is a 10–11
second fallback for removal and missed events. It targets a single-pass service
that disables bootstrap, holder, and device waits and skips if a stronger pass
already owns the lifecycle lock. A healthy pass verifies the generation,
ConfigFS attributes, device identity, metadata, stable link, and ready manifest
without replacing files or logging another ready transition. Exit 75 is a
normal transient result for this single-pass watchdog (for example while a peer
is rebooting), so its unit accepts that status without recording a systemd
failure; configuration errors remain failures.

```sh
systemctl daemon-reload
udevadm control --reload-rules
udevadm trigger --action=change --subsystem-match=misc \
    --sysname-match='tbstream*'
udevadm settle --timeout=10
systemctl enable --now ds4-tbstream-reconcile.service \
    ds4-tbstream-reconcile.timer
```

Do not add ConfigFS deletion to `ExecStop=`. A normal service stop or shutdown
must not race an active DS4 mapping. The explicit cleanup helper first withdraws
the stable link, waits for every `/dev/tbstream*` holder to drain, and then uses
ConfigFS `rmdir`; it never deletes a `/dev` node.

## DS4 ordering and rollout

Keep this least-privilege service setting in both versioned DS4 units:

```ini
[Service]
SupplementaryGroups=tbstream
```

The layer-slice v3 transport baseline is:

```text
--dist-transport auto
```

With no `--dist-nhi-device`, two current peers negotiate descriptor-framed v3
TCP. Explicit `--dist-transport tcp` selects the legacy-v2 compatibility path;
it is not the v3 production spelling.

For layer-slice deployments, any configuration that supplies
`/run/ds4-tbstream/device` remains an explicit qualification mode while
extended soak and active peer-reboot testing continue; the repaired candidate
also has no measured full-model speed advantage. That includes either:

```text
--dist-transport auto --dist-nhi-device /run/ds4-tbstream/device
--dist-transport nhi --dist-nhi-device /run/ds4-tbstream/device
```

The independently enabled lifecycle unit remains useful for hardware diagnosis
and normally finishes long before DS4 loads its model. Production DS4 must not
be ordered after the blocking reconciler and does not depend on the stable
device. `auto` can fall back before NHI activation, but it cannot safely replay
an ambiguous in-flight NHI generation; therefore pre-activation fallback is not
a substitute for explicit NHI qualification. Do not add
`Requires`, `After`, `ExecStartPre`, or `BindsTo` dependencies from the
production DS4 service to the stream device.

## DS4 tensor-parallel NHI drop-ins

The imported DMA-BUF pool path used by DS4 tensor parallelism needs two
additional, deliberately explicit pieces on both hosts:

1. The patch-14/15 `thunderbolt_stream` module must be installed under
   `/lib/modules/$(uname -r)/updates/` and selected by `modinfo
   thunderbolt_stream`. Install `tools/modprobe.d/ds4-tbstream-zc.conf` as
   `/etc/modprobe.d/ds4-tbstream-zc.conf`, run `depmod -a`, then reload the
   module. `zc_diagnostic_dmabuf=1` exposes a diagnostic capability and must
   not be enabled for ordinary TCP or page-pool NHI deployments.
2. Install the versioned DS4 drop-in examples after adjusting their paths,
   addresses, ports, and ROCm library path:

   ```sh
   install -D -m 0644 tools/systemd/ds4-mxfp4-worker.tp-nhi.conf.example \
       /etc/systemd/system/ds4-mxfp4-worker.service.d/99-tp-nhi.conf
   install -D -m 0644 tools/systemd/ds4-mxfp4-server.tp-nhi.conf.example \
       /etc/systemd/system/ds4-mxfp4-server.service.d/99-tp-nhi.conf
   systemctl daemon-reload
   ```

The examples use `/run/ds4-tbstream/device`, require the service user to be
in the `tbstream` group, and require the base units to select a non-root
`User=`. The drop-ins reset any inherited capability lists and then grant only
`CAP_SYS_RAWIO` through `CapabilityBoundingSet` plus `AmbientCapabilities`.
That capability is the kernel's current gate for `TBSTREAM_ZC_IMPORT`.
`NoNewPrivileges=true` remains enabled; ambient capabilities are assigned by
systemd before `execve` and do not require later privilege elevation.

Startup order is worker first, coordinator second. Stop order is also worker
first: keep the coordinator's NHI device open until it observes the worker
close, then stop the coordinator. Keep the reconcile timer enabled on both
hosts; do not leave both NHI devices closed with the timer stopped during a
test gap.

The production pair validated this configuration with DS4 commit `c18296e`
on ROCm/gfx1151: 50/50 expert ownership bound on both ranks, a 23-token
prompt with 4096 generated tokens completed at 15.64 tok/s, and a 3373-token
cold prefill completed at 15.97 tok/s. Both nodes closed with zero NHI
failures, event drops, CRC errors, or overruns.

## Maintenance cleanup and rollback

First prevent both timer and udev activation, stop or switch DS4 away from NHI,
and confirm no process holds any `/dev/tbstream*`. Then run:

```sh
systemctl mask --runtime --now ds4-tbstream-reconcile.service \
    ds4-tbstream-reconcile-watchdog.service
systemctl stop ds4-tbstream-reconcile.timer
/usr/local/libexec/ds4-tbstream-cleanup.sh
```

The command exits 75 and leaves ConfigFS intact if a holder remains. Do not
force it, do not remove `/dev/tbstream*`, and do not unload the module while a
holder exists. Keep both lifecycle services runtime-masked throughout the
maintenance window. To resume, unmask them and start the main service and timer.
For complete rollback, restore the prior TCP-only versioned DS4 units, run
`systemctl disable --now` for the lifecycle service and timer, runtime-mask the
main and watchdog services, remove and reload the udev event rule, daemon-reload
systemd, run the explicit cleanup, and optionally unload
`thunderbolt_stream`. A coordinated local reboot is the final ConfigFS reset.

## Offline verification

The mock suite never touches live ConfigFS, sysfs, or `/dev`:

```sh
make -C tools/tests test
```

It covers exact allocator/follower HopID pinning, invalid pairs, unavailable or
mismatched exact allocations with no automatic fallback, netdev-carrier
ordering, carrier wait/timeout and single-pass behavior, active idempotence,
ambiguous service discovery, device verification, holder refusal,
service-generation changes, and the observed stale `tbstream0` plus new
`tbstream1` ordinal-swap state.
