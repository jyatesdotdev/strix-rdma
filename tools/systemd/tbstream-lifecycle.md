# USB4STREAM endpoint lifecycle

What a host needs to create, access, and tear down a `/dev/tbstreamN` endpoint,
and the ordering rules that keep the link alive. Everything here is host-local
configuration plus `tools/scripts/tbstream-setup.sh`; no systemd units are
involved. The native verbs path does not use this interface at all — it drives
its own NHI rings (see `../../docs/NATIVE_VERBS.md` and
`../../docs/ZEROCOPY_VERBS.md`).

## Prerequisites

- `thunderbolt_stream` loaded: Linux 7.2+ with `CONFIG_USB4_STREAM=m`, or the
  backport plus zero-copy series in `kernel/` (see `../../kernel/README.md`).
- ConfigFS mounted at `/sys/kernel/config`.
- The cable linked and the peer's `thunderbolt_stream` loaded. A peer running
  the driver advertises an XDomain service with `key=stream`; inspect the link
  with `tblist -A` from [intel/tbtools](https://github.com/intel/tbtools).
- When the same link also carries Thunderbolt-IP, `thunderbolt-net` must be
  present and its carrier up before the stream endpoint is created: both draw
  HopIDs from the same adapter pool, and `thunderbolt-net` reserves HopID 8
  during its login. The zero-copy verbs mode is the deliberate exception — it
  blacklists `thunderbolt-net` and claims both HopIDs.

## Device access policy

`/dev/tbstreamN` defaults to `0600 root:root`. The stream UAPI mmaps
DMA-mapped pages, so never widen it to world-writable; use the scoped rule and
a dedicated group instead of running workloads as root:

```sh
sudo groupadd --force --system tbstream
sudo usermod -aG tbstream "$(id -un)"   # then start a new login session
sudo make install-lifecycle             # installs 99-tbstream.rules + this doc
sudo udevadm control --reload-rules
sudo udevadm trigger --action=change --subsystem-match=misc \
    --sysname-match='tbstream*'
```

`make install-lifecycle` installs `tools/udev/99-tbstream.rules` as
`/etc/udev/rules.d/99-tbstream.rules` (`0660 root:tbstream`) and this document
under the package docdir.

## Creating the endpoint

`tools/scripts/tbstream-setup.sh` creates the ConfigFS child, writes
`ring_size`/`throttling` **before** the HopIDs activate the stream, and prints
the resulting device path. The stream name must match on both hosts:

```sh
hostA$ sudo tools/scripts/tbstream-setup.sh link   # prints /dev/tbstreamX
hostB$ sudo tools/scripts/tbstream-setup.sh link
```

Each side requests automatic HopID allocation (`-1`) and the kernel protocol
pairs the two ends. `RING_SIZE=4096` and `THROTTLING=0` are the values that
matter at QD1 and are already the kernel defaults after the zero-copy series;
both are overridable by environment. `SERVICE=<xdomain>.<service>` skips
service autodetection. The script is lab-scoped: it refuses to create a second
endpoint when one already exists (`TBSTREAM_SETUP_FORCE=1` overrides) and
errors out when no peer `stream` service is visible.

## Teardown and the wedge rules

Hard-won ordering rules. Violating the first one can cost a reboot.

1. **Never transmit toward a peer whose device is not open and enabled.** TX
   toward an inactive peer stalls on zero E2E credits and can wedge the entire
   XDomain connection, Thunderbolt-IP included. Reproduced 2026-08-24; it
   survives a `thunderbolt-net` reload, a service rebind, and an NHI PCI
   rebind.
2. Sequence startup as: both sides open (and enable zero-copy) → out-of-band
   barrier → first submit.
3. Keep one side's device open across restarts of the other; do not leave both
   ends closed for the duration of a session gap.
4. Stop order is writer first. Keep the survivor open until it observes the
   peer's CLOSE; patch 15 then skips the impossible reciprocal CLOSE toward an
   already-closed peer.
5. ConfigFS stream groups pin `thunderbolt_stream`: `rmdir` the ConfigFS child
   before reloading the module. `ring_size`/`throttling` writes return `EBUSY`
   while any fd is open.
6. Concurrent readers on one fd steal frames, CLOSE included. Stop every
   consumer (`pkill -f pingpong` in the lab) before reconfiguring.

Recovery when the link is wedged: stop every holder, remove the ConfigFS
child, reload `thunderbolt_stream` on **both** hosts, verify the
Thunderbolt-IP link, then recreate the endpoint. A full coordinated reload
healed the 2026-08-30 incident without a reboot; use a coordinated reboot only
if the reload does not restore bidirectional traffic. Never attempt the reload
while a process still holds `/dev/tbstream*`.

## Offline verification

```sh
make -C tools/tests test
```

Static assertions on the installed device-access policy. It never touches live
sysfs, ConfigFS, or `/dev`.
