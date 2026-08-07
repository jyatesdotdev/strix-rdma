# DS4-shaped zero-copy exchange gate

`tbstream-ds4-gate` isolates the bidirectional NHI data plane from model
loading, ROCm, and DS4's TCP control channel. Every exchange is serialized:

1. The initiator sends a request using the repeating 17, 17, 33, and 65 frame
   geometry. Its final frame is exactly 64 bytes.
2. The responder verifies and reposts the request RX slots, optionally waits
   to mimic model compute, then sends 127 frames with a 1088-byte final frame.
3. The initiator verifies and reposts the response, optionally waits to mimic
   request turnaround, and begins the next exchange.

> **Current hardware result (2026-08-05): PASS with zero-copy patches 11 and
> 12.** On the `max`/`max2` pair, the gate completed 2,880 exchanges across 27
> stream sessions and 26 close/reopen transitions. That covers 921,600 matching
> descriptor completions across the pair with diagnostic kicks disabled and no
> kernel transport errors. Patch 11 flushes posted MSI-X clears; patch 12 primes
> RX before enabling DMA paths. See
> `../../bench/results/2026-08-05-nhi-msix-rx-prime-fix.md` for the deployed
> hashes, full-model qualification, and release stance.

Both sides validate the 64-byte envelope, every deterministic payload byte,
RX cursor and byte geometry, and the exact `TX_DONE` cursor/frame tuple. The
default 32 exchanges advance the responder through 4064 of a 4096-frame ring;
use 33 or more exchanges to exercise ring wrap.

Every transport transition also writes a machine-readable `GATE` line to
standard error. Each line identifies the role, one-based exchange, zero-based
wire sequence, and exact phase (for example `wait-response-rx`). It includes
four monotonic frame totals:

- `tx_submitted_frames`: frames accepted by `TBSTREAM_ZC_SUBMIT_TX`
- `tx_completed_frames`: frames covered by validated `TX_DONE` events
- `rx_received_frames`: frames reported by reaped RX events
- `rx_reposted_frames`: frames successfully returned with `POST_RX`

The line also records the userspace TX/RX cursors, wait count, and elapsed
monotonic time. At a stall, compare the last `GATE` line from both endpoints;
the phase and unequal totals show exactly which transition stopped advancing.

RX slots are returned with the legacy unflagged `TBSTREAM_ZC_POST_RX` ioctl.
This is intentional: request geometry varies, and 127-frame responses do not
divide a 4096-frame ring, so fixed interrupt-boundary reposts become
misaligned after wrap.

Build and run the offline envelope/payload wrap test:

```sh
make -C tools/ds4-shape check
```

Start the responder first and wait for `READY`:

```sh
sudo tools/ds4-shape/tbstream-ds4-gate responder \
  --device /dev/tbstream0 --exchanges 32 --compute-delay-ms 25 \
  --hold-on-timeout
```

Then start the initiator on the peer:

```sh
sudo tools/ds4-shape/tbstream-ds4-gate initiator \
  --device /dev/tbstream0 --exchanges 32 --turnaround-delay-ms 10 \
  --hold-on-timeout
```

The exchange count must match and cannot be less than 32. The per-event
timeout defaults to 30000 ms and can be changed with `--timeout-ms` when a
larger compute delay is intentional.

`--hold-on-timeout` is optional and does not change successful exchanges. On
a timeout it leaves the device file, rings, and mapping open, prints a
`timeout-hold-begin` line containing the process ID, and waits for `SIGINT` or
`SIGTERM`. Capture kernel ring/interrupt state from both machines while the
processes are held, then signal them to release the resources and exit with
the original failure status. Without the option, timeout cleanup remains
immediate as before.

On every timeout or terminal poll error the gate also makes a best-effort
`TBSTREAM_ZC_GET_STATS` request before cleanup or entering the hold. A
diagnostics-capable kernel prints one `GATE_STREAM_STATS` and two
`GATE_RING_STATS` records containing
the stream counters and exact TX/RX software, hardware, descriptor, interrupt,
and workqueue state. The stream record names the first terminal error, if any,
so CRC, overrun, FIFO-drop, and partial enqueue failures remain distinct. An
older kernel reports `event=stats-unavailable`,
`errno=25`, and `reason=kernel_does_not_support_diagnostic_UAPI` rather than
hiding the missing snapshot.

Terminal `POLLERR`/`POLLHUP` takes precedence over `POLLIN` in this release
gate. If the kernel reports both because ownership events were already queued
before a terminal failure, the gate deliberately fails without draining or
counting those events; otherwise a final expected event could hide the failed
session and produce a false pass. The kernel snapshot's `fifo_len` records the
still-queued events for diagnosis.

The `hw_posted` and `hw_completed` values are meaningful only when the same
ring record has `hw_valid=1`, corresponding to
`TBSTREAM_ZC_RING_F_HW_VALID`. When it is clear, the NHI registers could not
be read and their zero values must not be interpreted as ring positions.

For a controlled lost-interrupt experiment, add `--kick-on-timeout`. After
the pre-kick snapshot, the process kicks only the ring relevant to its failed
wait: RX when it expected RX/CLOSE, or TX when it expected `TX_DONE`. The
`kick-selection` record names the selected ring and mask. The gate then waits
10 ms for ordinary ring work and dumps a `timeout-post-kick` snapshot.

Diagnostic kicks are disabled at both layers by default. The gate never
issues one without `--kick-on-timeout`; the kernel additionally requires the
caller to have `CAP_SYS_RAWIO` and the following module parameter to be `1`:

```text
/sys/module/thunderbolt_stream/parameters/zc_diagnostic_kick
```

The kick only schedules the normal ring worker. It does not rewrite
descriptors, reset the path, or turn a failed gate into a pass. Pair it with
`--hold-on-timeout` when the post-kick state also needs manual inspection.
