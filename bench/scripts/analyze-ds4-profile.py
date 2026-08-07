#!/usr/bin/env python3
"""Summarize DS4 distributed decode and NHI trace logs.

The profiler logs are deliberately qualification-only: emitting one line per
token perturbs timing. This tool turns those lines into a compact critical-path
ledger and correlates NHI wait/synchronization pairs by request and sequence.
"""

from __future__ import annotations

import argparse
import collections
import math
import re
import statistics
from pathlib import Path


FLOAT = r"([0-9]+(?:\.[0-9]+)?)"

COORDINATOR_PATTERNS = {
    "coord.local": re.compile(rf"span request=\d+ .* local={FLOAT}ms"),
    "coord.remote": re.compile(rf"span request=\d+ .* remote={FLOAT}ms"),
    "coord.total": re.compile(rf"span request=\d+ .* total={FLOAT}ms"),
    "coord.control_send": re.compile(rf"remote request=\d+ .* send={FLOAT}ms"),
    "coord.wait_result": re.compile(rf"remote request=\d+ .* wait_result={FLOAT}ms"),
    "coord.output_head": re.compile(rf"remote request=\d+ output_head={FLOAT}ms"),
    "coord.copy_logits": re.compile(rf"remote request=\d+ copy_logits={FLOAT}ms"),
    "mtp.draft": re.compile(rf"mtp timing distributed .* draft={FLOAT} ms"),
    "mtp.verify": re.compile(rf"mtp timing distributed .* verify={FLOAT} ms"),
    "mtp.total": re.compile(rf"mtp timing distributed .* total={FLOAT} ms"),
}

WORKER_PATTERN = re.compile(
    rf"worker request=(\d+) .* input_decode={FLOAT}ms lock_wait={FLOAT}ms "
    rf"eval={FLOAT}ms send={FLOAT}ms total={FLOAT}ms .* direct=0x([0-9a-fA-F]+)"
)

COORDINATOR_SPAN_ID = re.compile(r"dist decode profile: span request=(\d+)")
DIRECT_PATTERN = re.compile(r"direct=0x([0-9a-fA-F]+)")
MTP_ACCEPT_PATTERN = re.compile(r"mtp timing distributed drafted=(\d+) committed=(\d+)")
NHI_PREFIX = re.compile(r"NHI trace: ([0-9]+\.[0-9]+) .*? ")
NHI_EVENT = re.compile(
    r"(TX|RX) (commit sync begin|commit sync done|acquire wait|acquire ready|"
    r"submit begin|submit done|repost begin|repost done) "
    r"seq=(\d+) request=(\d+)"
)
NHI_SYNC_SKIPPED = re.compile(
    r"(TX|RX) commit sync skipped .* seq=(\d+) request=(\d+)"
)


def percentile(values: list[float], q: float) -> float:
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    index = (len(ordered) - 1) * q
    lo = math.floor(index)
    hi = math.ceil(index)
    if lo == hi:
        return ordered[lo]
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (index - lo)


def add_patterns(lines: list[str], patterns: dict[str, re.Pattern[str]], out: dict[str, list[float]]) -> None:
    for line in lines:
        for name, pattern in patterns.items():
            match = pattern.search(line)
            if match:
                out[name].append(float(match.group(1)))


def add_worker(
    lines: list[str],
    out: dict[str, list[float]],
    masks: collections.Counter[int],
    request_ids: set[str],
) -> None:
    names = ("worker.input_decode", "worker.lock_wait", "worker.eval", "worker.send", "worker.total")
    for line in lines:
        match = WORKER_PATTERN.search(line)
        if not match:
            continue
        request_ids.add(match.group(1))
        for name, value in zip(names, match.groups()[1:6]):
            out[name].append(float(value))
        masks[int(match.group(7), 16)] += 1


def add_nhi_pairs(
    role: str,
    lines: list[str],
    out: dict[str, list[float]],
    request_ids: set[str],
    diagnostics: collections.Counter[str],
) -> None:
    starts: dict[tuple[str, str, str, str], float] = {}
    pair_names = {
        "commit sync done": "lease_sync",
        "acquire ready": "rx_acquire_wait",
        "submit done": "submit_ioctl",
        "repost done": "repost_ioctl",
    }
    for line in lines:
        prefix = NHI_PREFIX.search(line)
        skipped = NHI_SYNC_SKIPPED.search(line)
        if prefix and skipped:
            direction, _sequence, request = skipped.groups()
            if request in request_ids:
                diagnostics[f"{role}.nhi.{direction.lower()}.lease_sync_skipped"] += 1
            continue
        event = NHI_EVENT.search(line)
        if not prefix or not event:
            continue
        timestamp = float(prefix.group(1))
        direction, action, sequence, request = event.groups()
        if request not in request_ids:
            continue
        if action == "commit sync begin":
            starts[(direction, "commit", sequence, request)] = timestamp
        elif action == "acquire wait":
            starts[(direction, "acquire", sequence, request)] = timestamp
        elif action == "submit begin":
            starts[(direction, "submit", sequence, request)] = timestamp
        elif action == "repost begin":
            starts[(direction, "repost", sequence, request)] = timestamp
        else:
            kind = {
                "commit sync done": "commit",
                "acquire ready": "acquire",
                "submit done": "submit",
                "repost done": "repost",
            }[action]
            start = starts.pop((direction, kind, sequence, request), None)
            if start is not None and timestamp >= start:
                out[f"{role}.nhi.{direction.lower()}.{pair_names[action]}"].append(
                    (timestamp - start) * 1000.0
                )
            else:
                diagnostics[f"{role}.nhi.unmatched_end"] += 1
    if starts:
        diagnostics[f"{role}.nhi.unmatched_start"] += len(starts)


def read_lines(path: Path) -> list[str]:
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--coordinator", required=True, type=Path)
    parser.add_argument("--worker", required=True, type=Path)
    args = parser.parse_args()

    coordinator = read_lines(args.coordinator)
    worker = read_lines(args.worker)
    metrics: dict[str, list[float]] = collections.defaultdict(list)
    coordinator_masks: collections.Counter[int] = collections.Counter()
    worker_masks: collections.Counter[int] = collections.Counter()
    mtp = collections.Counter()
    diagnostics: collections.Counter[str] = collections.Counter()
    coordinator_request_ids = {
        match.group(1)
        for line in coordinator
        if (match := COORDINATOR_SPAN_ID.search(line))
    }
    worker_request_ids: set[str] = set()

    add_patterns(coordinator, COORDINATOR_PATTERNS, metrics)
    add_worker(worker, metrics, worker_masks, worker_request_ids)
    add_nhi_pairs(
        "coordinator", coordinator, metrics, coordinator_request_ids, diagnostics
    )
    add_nhi_pairs("worker", worker, metrics, worker_request_ids, diagnostics)

    for line in coordinator:
        direct = DIRECT_PATTERN.search(line)
        if direct and "dist decode profile: span" in line:
            coordinator_masks[int(direct.group(1), 16)] += 1
        accepted = MTP_ACCEPT_PATTERN.search(line)
        if accepted:
            mtp["drafted"] += int(accepted.group(1))
            mtp["committed"] += int(accepted.group(2))
            mtp["cycles"] += 1

    print("metric                              count      mean       p50       p95       max")
    for name in sorted(metrics):
        values = metrics[name]
        print(
            f"{name:34} {len(values):5d} "
            f"{statistics.fmean(values):9.3f} {percentile(values, 0.50):9.3f} "
            f"{percentile(values, 0.95):9.3f} {max(values):9.3f} ms"
        )
    if coordinator_masks:
        print(
            "coordinator direct masks: "
            + ", ".join(f"0x{k:x}={v}" for k, v in sorted(coordinator_masks.items()))
        )
    if worker_masks:
        print(
            "worker direct masks: "
            + ", ".join(f"0x{k:x}={v}" for k, v in sorted(worker_masks.items()))
        )
    if diagnostics:
        print(
            "NHI diagnostics: "
            + ", ".join(f"{name}={count}" for name, count in sorted(diagnostics.items()))
        )
    if mtp["cycles"]:
        rate = 100.0 * mtp["committed"] / mtp["drafted"] if mtp["drafted"] else 0.0
        print(
            f"MTP: cycles={mtp['cycles']} drafted={mtp['drafted']} "
            f"committed={mtp['committed']} slot_acceptance={rate:.2f}%"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
