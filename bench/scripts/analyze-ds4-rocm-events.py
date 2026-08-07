#!/usr/bin/env python3
"""Aggregate DS4's low-overhead ROCm event-profile journal output.

DS4 prints averages over a configurable number of samples.  This analyzer
uses the accompanying ``samples=`` value as a weight, rather than averaging
the already-averaged windows.  It accepts raw stderr, normal journalctl text
formats, and journalctl JSON output.
"""

from __future__ import annotations

import argparse
import collections
import copy
import dataclasses
import json
import math
import re
import sys
from pathlib import Path
from typing import Iterable, Optional, TextIO


OVERALL_MARKER = "ROCm event profile:"
GROUP_MARKER = "ROCm event profile group:"
DISABLED_MARKER = "ROCm event profile disabled after"
GROUP_FIELD = "ratio_group"
KEY_VALUE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)=([^\s]+)")
LAYERS = re.compile(r"([0-9]+):([0-9]+)\Z")

OVERALL_MS_METRICS = (
    "stream0",
    "host",
    "host_minus_stream0",
    "input",
    "unsampled",
    "sampled_layer",
    "qkv",
    "compressor_indexer",
    "attn_core",
    "attn_output",
    "attn_hc",
    "ffn_router",
    "routed_moe",
    "shared_post",
    "output_head",
    "copyback",
)
OVERALL_VALUE_METRICS = ("n_raw", "n_comp")
SAMPLED_SEGMENTS = (
    "qkv",
    "compressor_indexer",
    "attn_core",
    "attn_output",
    "attn_hc",
    "ffn_router",
    "routed_moe",
    "shared_post",
)
TOP_LEVEL_SEGMENTS = (
    "input",
    "unsampled",
    "sampled_layer",
    "output_head",
    "copyback",
)
GROUP_MS_METRICS = ("sampled_layer",) + SAMPLED_SEGMENTS
DIRECT_KEYS = tuple(f"direct{mask}" for mask in range(8))
UINT32_MAX = (1 << 32) - 1
UINT64_MAX = (1 << 64) - 1


class ProfileParseError(ValueError):
    """A line contained a profile marker but not a valid profile record."""


def checked_u64_add(left: int, right: int, field: str) -> int:
    if left < 0 or right < 0 or left > UINT64_MAX - right:
        raise ProfileParseError(f"{field} exceeds uint64 range")
    return left + right


@dataclasses.dataclass
class LineContext:
    message: str
    host: Optional[str] = None
    role: Optional[str] = None


@dataclasses.dataclass
class WeightedMetric:
    weighted_sum: float = 0.0
    weight: int = 0

    def add(self, value: float, weight: int) -> None:
        product = value * weight
        updated_sum = self.weighted_sum + product
        if not math.isfinite(product) or not math.isfinite(updated_sum):
            raise ProfileParseError("weighted metric exceeds finite float range")
        updated_weight = checked_u64_add(self.weight, weight, "weighted sample count")
        self.weighted_sum = updated_sum
        self.weight = updated_weight

    @property
    def mean(self) -> float:
        return self.weighted_sum / self.weight if self.weight else 0.0

    def as_json(self, unit: Optional[str] = None) -> dict[str, object]:
        result: dict[str, object] = {
            "weighted_count": self.weight,
            "weighted_mean": self.mean,
        }
        if unit:
            result["unit"] = unit
        return result


@dataclasses.dataclass
class Consistency:
    formula: str
    weighted_residual_sum: float = 0.0
    sample_count: int = 0
    records: int = 0
    records_outside_tolerance: int = 0
    max_abs_record_residual_ms: float = 0.0

    def add(self, residual_ms: float, samples: int, tolerance_ms: float) -> None:
        product = residual_ms * samples
        updated_sum = self.weighted_residual_sum + product
        if (not math.isfinite(residual_ms) or not math.isfinite(product) or
                not math.isfinite(updated_sum)):
            raise ProfileParseError("consistency residual exceeds finite float range")
        self.weighted_residual_sum = updated_sum
        self.sample_count = checked_u64_add(
            self.sample_count, samples, "consistency sample count"
        )
        self.records = checked_u64_add(self.records, 1, "consistency record count")
        self.max_abs_record_residual_ms = max(
            self.max_abs_record_residual_ms, abs(residual_ms)
        )
        if abs(residual_ms) > tolerance_ms:
            self.records_outside_tolerance = checked_u64_add(
                self.records_outside_tolerance,
                1,
                "consistency outside-tolerance count",
            )

    @property
    def weighted_mean_residual_ms(self) -> float:
        if not self.sample_count:
            return 0.0
        return self.weighted_residual_sum / self.sample_count

    def as_json(self) -> dict[str, object]:
        return {
            "formula": self.formula,
            "weighted_mean_residual_ms": self.weighted_mean_residual_ms,
            "max_abs_record_residual_ms": self.max_abs_record_residual_ms,
            "records_checked": self.records,
            "records_outside_tolerance": self.records_outside_tolerance,
        }


def new_overall_consistency() -> dict[str, Consistency]:
    return {
        "stream0_vs_top_level_segments": Consistency(
            "stream0 - (input + unsampled + sampled_layer + output_head + copyback)"
        ),
        "sampled_layer_vs_segments": Consistency(
            "sampled_layer - (qkv + compressor_indexer + attn_core + "
            "attn_output + attn_hc + ffn_router + routed_moe + shared_post)"
        ),
        "host_minus_stream0": Consistency(
            "host_minus_stream0 - (host - stream0)"
        ),
    }


@dataclasses.dataclass
class OverallAggregate:
    layer_start: int
    layer_end: int
    windows: int = 0
    samples: int = 0
    dropped: int = 0
    metrics: dict[str, WeightedMetric] = dataclasses.field(
        default_factory=lambda: collections.defaultdict(WeightedMetric)
    )
    direct_masks: collections.Counter[int] = dataclasses.field(
        default_factory=collections.Counter
    )
    consistency: dict[str, Consistency] = dataclasses.field(
        default_factory=new_overall_consistency
    )
    direct_count_mismatch_records: int = 0

    def add(
        self,
        values: dict[str, object],
        sample_count: int,
        tolerance_ms: float,
    ) -> None:
        self.windows = checked_u64_add(self.windows, 1, "overall window count")
        self.samples = checked_u64_add(
            self.samples, sample_count, "overall sample count"
        )
        self.dropped = checked_u64_add(
            self.dropped, int(values["dropped"]), "overall dropped count"
        )
        for name in OVERALL_MS_METRICS + OVERALL_VALUE_METRICS:
            self.metrics[name].add(float(values[name]), sample_count)
        direct_total = 0
        for mask, name in enumerate(DIRECT_KEYS):
            count = int(values[name])
            self.direct_masks[mask] = checked_u64_add(
                self.direct_masks[mask], count, f"direct mask {mask} count"
            )
            direct_total += count
        if direct_total != sample_count:
            self.direct_count_mismatch_records = checked_u64_add(
                self.direct_count_mismatch_records,
                1,
                "direct count mismatch records",
            )

        top_residual = float(values["stream0"]) - sum(
            float(values[name]) for name in TOP_LEVEL_SEGMENTS
        )
        sampled_residual = float(values["sampled_layer"]) - sum(
            float(values[name]) for name in SAMPLED_SEGMENTS
        )
        host_residual = float(values["host_minus_stream0"]) - (
            float(values["host"]) - float(values["stream0"])
        )
        self.consistency["stream0_vs_top_level_segments"].add(
            top_residual, sample_count, tolerance_ms
        )
        self.consistency["sampled_layer_vs_segments"].add(
            sampled_residual, sample_count, tolerance_ms
        )
        self.consistency["host_minus_stream0"].add(
            host_residual, sample_count, tolerance_ms
        )

    def as_json(self) -> dict[str, object]:
        dropped_denominator = self.samples + self.dropped
        return {
            "layers": {"start": self.layer_start, "end": self.layer_end},
            "windows": self.windows,
            "samples": self.samples,
            "dropped_samples": self.dropped,
            "dropped_percent": (
                100.0 * self.dropped / dropped_denominator
                if dropped_denominator
                else 0.0
            ),
            "metrics": {
                name: metric.as_json("ms" if name in OVERALL_MS_METRICS else None)
                for name, metric in sorted(self.metrics.items())
            },
            "direct_masks": {
                f"0x{mask:x}": self.direct_masks.get(mask, 0)
                for mask in range(8)
            },
            "direct_mask_count_difference": sum(self.direct_masks.values())
            - self.samples,
            "direct_count_mismatch_records": self.direct_count_mismatch_records,
            "consistency": {
                name: check.as_json()
                for name, check in sorted(self.consistency.items())
            },
        }


def new_group_consistency() -> dict[str, Consistency]:
    return {
        "sampled_layer_vs_segments": Consistency(
            "sampled_layer - (qkv + compressor_indexer + attn_core + "
            "attn_output + attn_hc + ffn_router + routed_moe + shared_post)"
        )
    }


@dataclasses.dataclass
class GroupAggregate:
    ratio: int
    windows: int = 0
    samples: int = 0
    emit: int = 0
    metrics: dict[str, WeightedMetric] = dataclasses.field(
        default_factory=lambda: collections.defaultdict(WeightedMetric)
    )
    consistency: dict[str, Consistency] = dataclasses.field(
        default_factory=new_group_consistency
    )
    emit_count_mismatch_records: int = 0

    def add(
        self,
        values: dict[str, object],
        sample_count: int,
        tolerance_ms: float,
    ) -> None:
        self.windows = checked_u64_add(self.windows, 1, "group window count")
        self.samples = checked_u64_add(
            self.samples, sample_count, "group sample count"
        )
        emitted = int(values["emit"])
        self.emit = checked_u64_add(self.emit, emitted, "group emit count")
        if emitted > sample_count:
            self.emit_count_mismatch_records = checked_u64_add(
                self.emit_count_mismatch_records,
                1,
                "group emit mismatch records",
            )
        for name in GROUP_MS_METRICS:
            self.metrics[name].add(float(values[name]), sample_count)
        residual = float(values["sampled_layer"]) - sum(
            float(values[name]) for name in SAMPLED_SEGMENTS
        )
        self.consistency["sampled_layer_vs_segments"].add(
            residual, sample_count, tolerance_ms
        )

    def as_json(self) -> dict[str, object]:
        return {
            "ratio": self.ratio,
            "windows": self.windows,
            "samples": self.samples,
            "emit_samples": self.emit,
            "emit_percent": 100.0 * self.emit / self.samples if self.samples else 0.0,
            "emit_count_mismatch_records": self.emit_count_mismatch_records,
            "metrics": {
                name: metric.as_json("ms")
                for name, metric in sorted(self.metrics.items())
            },
            "consistency": {
                name: check.as_json()
                for name, check in sorted(self.consistency.items())
            },
        }


@dataclasses.dataclass
class SourceAggregate:
    label: str
    inputs: set[str] = dataclasses.field(default_factory=set)
    hosts: set[str] = dataclasses.field(default_factory=set)
    roles: set[str] = dataclasses.field(default_factory=set)
    overall: dict[tuple[int, int], OverallAggregate] = dataclasses.field(
        default_factory=dict
    )
    groups: dict[int, GroupAggregate] = dataclasses.field(default_factory=dict)
    disable_failures: int = 0

    def as_json(self) -> dict[str, object]:
        overall_samples = sum(item.samples for item in self.overall.values())
        group_samples = sum(item.samples for item in self.groups.values())
        return {
            "label": self.label,
            "inputs": sorted(self.inputs),
            "hosts": sorted(self.hosts),
            "roles": sorted(self.roles),
            "profile_disable_failures": self.disable_failures,
            "overall": [
                self.overall[key].as_json() for key in sorted(self.overall)
            ],
            "ratio_groups": [
                self.groups[key].as_json() for key in sorted(self.groups)
            ],
            "ratio_group_coverage": {
                "overall_samples": overall_samples,
                "ratio_group_samples": group_samples,
                "difference": group_samples - overall_samples,
            },
        }


@dataclasses.dataclass
class Summary:
    tolerance_ms: float
    sources: dict[str, SourceAggregate] = dataclasses.field(default_factory=dict)
    input_lines: int = 0
    overall_records: int = 0
    group_records: int = 0
    malformed_profile_lines: int = 0
    warnings: list[str] = dataclasses.field(default_factory=list)

    def source(self, label: str) -> SourceAggregate:
        if label not in self.sources:
            self.sources[label] = SourceAggregate(label)
        return self.sources[label]

    def as_json(self) -> dict[str, object]:
        return {
            "schema_version": 1,
            "consistency_tolerance_ms": self.tolerance_ms,
            "totals": {
                "input_lines": self.input_lines,
                "overall_records": self.overall_records,
                "ratio_group_records": self.group_records,
                "malformed_profile_lines": self.malformed_profile_lines,
                "sources": len(self.sources),
            },
            "sources": [
                self.sources[label].as_json() for label in sorted(self.sources)
            ],
            "warnings": self.warnings,
        }


def finite_nonnegative(value: str, field: str, suffix: str = "") -> float:
    raw = value
    if suffix:
        if not raw.endswith(suffix):
            raise ProfileParseError(f"{field} must end in {suffix!r}: {raw!r}")
        raw = raw[: -len(suffix)]
    try:
        parsed = float(raw)
    except ValueError as exc:
        raise ProfileParseError(f"invalid {field}: {value!r}") from exc
    if not math.isfinite(parsed) or parsed < 0.0:
        raise ProfileParseError(f"{field} must be finite and nonnegative: {value!r}")
    return parsed


def nonnegative_int(
    value: str,
    field: str,
    allow_zero: bool = True,
    maximum: int = UINT64_MAX,
) -> int:
    if not re.fullmatch(r"[0-9]+", value):
        raise ProfileParseError(f"invalid integer {field}: {value!r}")
    try:
        parsed = int(value)
    except (ValueError, OverflowError) as exc:
        raise ProfileParseError(f"invalid integer {field}: value is too large") from exc
    if not allow_zero and parsed == 0:
        raise ProfileParseError(f"{field} must be greater than zero")
    if parsed > maximum:
        raise ProfileParseError(f"{field} exceeds maximum {maximum}")
    return parsed


def fields_after_marker(message: str, marker: str) -> dict[str, str]:
    payload = message.split(marker, 1)[1]
    pairs = KEY_VALUE.findall(payload)
    fields: dict[str, str] = {}
    for name, value in pairs:
        if name in fields:
            raise ProfileParseError(f"duplicate field {name!r}")
        fields[name] = value.rstrip(",;")
    return fields


def require(fields: dict[str, str], names: Iterable[str]) -> None:
    missing = sorted(set(names) - set(fields))
    if missing:
        raise ProfileParseError("missing fields: " + ", ".join(missing))


def parse_overall(message: str) -> tuple[dict[str, object], tuple[int, int]]:
    fields = fields_after_marker(message, OVERALL_MARKER)
    required = (
        ("samples", "dropped", "layers")
        + OVERALL_MS_METRICS
        + OVERALL_VALUE_METRICS
        + DIRECT_KEYS
    )
    require(fields, required)
    layer_match = LAYERS.fullmatch(fields["layers"])
    if not layer_match:
        raise ProfileParseError(f"invalid layers range: {fields['layers']!r}")
    layer_start, layer_end = (
        nonnegative_int(part, "layer", maximum=UINT32_MAX)
        for part in layer_match.groups()
    )
    if layer_start > layer_end:
        raise ProfileParseError(f"reversed layers range: {fields['layers']!r}")

    values: dict[str, object] = {
        "samples": nonnegative_int(fields["samples"], "samples", allow_zero=False),
        "dropped": nonnegative_int(fields["dropped"], "dropped"),
    }
    for name in OVERALL_MS_METRICS:
        values[name] = finite_nonnegative(fields[name], name, "ms")
    for name in OVERALL_VALUE_METRICS:
        values[name] = finite_nonnegative(fields[name], name)
    for name in DIRECT_KEYS:
        values[name] = nonnegative_int(fields[name], name)
    return values, (layer_start, layer_end)


def parse_group(message: str) -> tuple[dict[str, object], int]:
    marker = GROUP_MARKER if GROUP_MARKER in message else OVERALL_MARKER
    fields = fields_after_marker(message, marker)
    ratio_field = "ratio" if "ratio" in fields else GROUP_FIELD
    require(fields, (ratio_field, "samples", "emit") + GROUP_MS_METRICS)
    ratio = nonnegative_int(
        fields[ratio_field], ratio_field, maximum=UINT32_MAX
    )
    values: dict[str, object] = {
        "samples": nonnegative_int(fields["samples"], "samples", allow_zero=False),
        "emit": nonnegative_int(fields["emit"], "emit"),
    }
    for name in GROUP_MS_METRICS:
        values[name] = finite_nonnegative(fields[name], name, "ms")
    return values, ratio


def detect_role(text: str) -> Optional[str]:
    lowered = text.lower()
    if re.search(r"(?:^|[^a-z0-9])worker(?:[^a-z0-9]|$)", lowered):
        return "worker"
    if re.search(
        r"(?:^|[^a-z0-9])(?:coordinator|coord|server)(?:[^a-z0-9]|$)",
        lowered,
    ):
        return "coordinator"
    return None


def plain_journal_host(line: str) -> Optional[str]:
    patterns = (
        # journalctl -o short-iso[-precise] / short-full
        r"^\s*\d{4}-\d{2}-\d{2}T\S+\s+([A-Za-z0-9_.-]+)\s+",
        # Traditional syslog / journalctl -o short.
        r"^\s*[A-Z][a-z]{2}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2}\s+"
        r"([A-Za-z0-9_.-]+)\s+",
        # journalctl -o short-unix.
        r"^\s*\d{9,}(?:\.\d+)?\s+([A-Za-z0-9_.-]+)\s+",
        # journalctl -o short-monotonic.
        r"^\s*\[\s*\d+(?:\.\d+)?\]\s+([A-Za-z0-9_.-]+)\s+",
    )
    for pattern in patterns:
        match = re.match(pattern, line)
        if match:
            return match.group(1)
    return None


def line_context(raw_line: str) -> LineContext:
    stripped = raw_line.lstrip("\x1e \t")
    if stripped.startswith("{"):
        try:
            record = json.loads(stripped)
        except (json.JSONDecodeError, TypeError):
            record = None
        if isinstance(record, dict) and isinstance(record.get("MESSAGE"), str):
            role_text = " ".join(
                str(record.get(name, ""))
                for name in ("_SYSTEMD_UNIT", "SYSLOG_IDENTIFIER", "_COMM", "MESSAGE")
            )
            host = record.get("_HOSTNAME")
            return LineContext(
                message=record["MESSAGE"],
                host=str(host) if host else None,
                role=detect_role(role_text),
            )

    marker_positions = [
        position
        for marker in (GROUP_MARKER, OVERALL_MARKER, DISABLED_MARKER)
        if (position := raw_line.find(marker)) >= 0
    ]
    prefix = raw_line[: min(marker_positions)] if marker_positions else raw_line
    return LineContext(
        message=raw_line,
        host=plain_journal_host(raw_line),
        role=detect_role(prefix),
    )


def source_label(context: LineContext, fallback: str, explicit: Optional[str]) -> str:
    if explicit:
        return explicit
    if context.host and context.role:
        return f"{context.host}/{context.role}"
    return context.host or context.role or fallback


def consume_lines(
    summary: Summary,
    lines: Iterable[str],
    input_name: str,
    explicit_label: Optional[str],
) -> None:
    for line_number, raw_line in enumerate(lines, 1):
        summary.input_lines += 1
        context = line_context(raw_line.rstrip("\n"))
        message = context.message
        # The current DS4 spelling is ``profile group: ratio=...``.  Accept
        # ``profile: ratio_group=...`` as well so saved logs from the earlier
        # instrumentation branch remain consumable.
        is_group = GROUP_MARKER in message or (
            OVERALL_MARKER in message
            and re.search(rf"(?:^|\s){GROUP_FIELD}=", message) is not None
        )
        is_overall = OVERALL_MARKER in message and not is_group
        is_disabled = DISABLED_MARKER in message
        if not (is_group or is_overall or is_disabled):
            continue
        label = source_label(context, input_name, explicit_label)
        source = summary.source(label)
        source.inputs.add(input_name)
        if context.host:
            source.hosts.add(context.host)
        if context.role:
            source.roles.add(context.role)
        if is_disabled:
            source.disable_failures += 1
            continue
        try:
            if is_group:
                values, ratio = parse_group(message)
                current = source.groups.get(ratio)
                aggregate = copy.deepcopy(current) if current else GroupAggregate(ratio)
                aggregate.add(
                    values, int(values["samples"]), summary.tolerance_ms
                )
                source.groups[ratio] = aggregate
                summary.group_records += 1
            else:
                values, layers = parse_overall(message)
                current = source.overall.get(layers)
                aggregate = (
                    copy.deepcopy(current)
                    if current
                    else OverallAggregate(layers[0], layers[1])
                )
                aggregate.add(
                    values, int(values["samples"]), summary.tolerance_ms
                )
                source.overall[layers] = aggregate
                summary.overall_records += 1
        except ProfileParseError as exc:
            summary.malformed_profile_lines += 1
            summary.warnings.append(f"{input_name}:{line_number}: {exc}")


def open_input(name: str, stdin: TextIO) -> tuple[Iterable[str], Optional[TextIO]]:
    if name == "-":
        return stdin, None
    handle = Path(name).open("r", encoding="utf-8", errors="replace")
    return handle, handle


def print_metric_table(metrics: dict[str, WeightedMetric], names: Iterable[str]) -> None:
    print("    metric                       weighted_n   weighted_mean")
    for name in names:
        metric = metrics[name]
        unit = " ms" if name in OVERALL_MS_METRICS or name in GROUP_MS_METRICS else ""
        print(f"    {name:28} {metric.weight:10d} {metric.mean:15.6f}{unit}")


def print_consistency(consistency: dict[str, Consistency]) -> None:
    print("    consistency (reported minus component sum):")
    for name in sorted(consistency):
        check = consistency[name]
        print(
            f"      {name:35} mean={check.weighted_mean_residual_ms:+.6f} ms "
            f"max_abs={check.max_abs_record_residual_ms:.6f} ms "
            f"outside_tolerance={check.records_outside_tolerance}/{check.records}"
        )


def print_text(summary: Summary) -> None:
    print("DS4 ROCm event profile summary")
    print(
        f"records: overall={summary.overall_records} "
        f"ratio_group={summary.group_records} "
        f"malformed={summary.malformed_profile_lines}; "
        f"consistency_tolerance={summary.tolerance_ms:.6f} ms"
    )
    for label in sorted(summary.sources):
        source = summary.sources[label]
        metadata = []
        if source.hosts:
            metadata.append("hosts=" + ",".join(sorted(source.hosts)))
        if source.roles:
            metadata.append("roles=" + ",".join(sorted(source.roles)))
        metadata.append("inputs=" + ",".join(sorted(source.inputs)))
        print(f"\nsource: {source.label} ({'; '.join(metadata)})")
        if source.disable_failures:
            print(f"  profiler disable failures: {source.disable_failures}")
        for layers in sorted(source.overall):
            aggregate = source.overall[layers]
            denominator = aggregate.samples + aggregate.dropped
            dropped_percent = (
                100.0 * aggregate.dropped / denominator if denominator else 0.0
            )
            print(
                f"  overall layers={layers[0]}:{layers[1]} "
                f"windows={aggregate.windows} samples={aggregate.samples} "
                f"dropped={aggregate.dropped} ({dropped_percent:.3f}%)"
            )
            print_metric_table(
                aggregate.metrics, OVERALL_MS_METRICS + OVERALL_VALUE_METRICS
            )
            direct = ", ".join(
                f"0x{mask:x}={aggregate.direct_masks.get(mask, 0)}"
                for mask in range(8)
                if aggregate.direct_masks.get(mask, 0)
            ) or "none"
            difference = sum(aggregate.direct_masks.values()) - aggregate.samples
            print(
                f"    direct masks: {direct}; count_difference={difference}; "
                f"mismatched_windows={aggregate.direct_count_mismatch_records}"
            )
            print_consistency(aggregate.consistency)

        for ratio in sorted(source.groups):
            aggregate = source.groups[ratio]
            emit_percent = (
                100.0 * aggregate.emit / aggregate.samples
                if aggregate.samples
                else 0.0
            )
            print(
                f"  ratio_group ratio={ratio} windows={aggregate.windows} "
                f"samples={aggregate.samples} emit={aggregate.emit} "
                f"({emit_percent:.3f}%)"
            )
            print_metric_table(aggregate.metrics, GROUP_MS_METRICS)
            print_consistency(aggregate.consistency)

        overall_samples = sum(item.samples for item in source.overall.values())
        group_samples = sum(item.samples for item in source.groups.values())
        print(
            f"  ratio-group coverage: overall_samples={overall_samples} "
            f"group_samples={group_samples} difference={group_samples - overall_samples}"
        )


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Aggregate weighted means from DS4 'ROCm event profile' log lines. "
            "With no INPUT, reads stdin."
        )
    )
    parser.add_argument(
        "inputs",
        metavar="INPUT",
        nargs="*",
        help="log file, or - for stdin (default: -)",
    )
    parser.add_argument(
        "--label",
        action="append",
        default=[],
        metavar="LABEL",
        help=(
            "source label override; use once for all inputs or once per input. "
            "Without it, journal host/role metadata or the input name is used"
        ),
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="emit machine-readable JSON instead of the human-readable report",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return status 2 and suppress the report if a profile line is malformed",
    )
    parser.add_argument(
        "--consistency-tolerance-ms",
        type=float,
        default=0.010,
        metavar="MS",
        help="rounding-aware residual threshold (default: 0.010 ms)",
    )
    return parser


def run(argv: Optional[list[str]] = None, stdin: TextIO = sys.stdin) -> int:
    parser = build_argument_parser()
    args = parser.parse_args(argv)
    if not math.isfinite(args.consistency_tolerance_ms) or args.consistency_tolerance_ms < 0:
        parser.error("--consistency-tolerance-ms must be finite and nonnegative")
    inputs = args.inputs or ["-"]
    if inputs.count("-") > 1:
        parser.error("stdin may be specified at most once")
    if args.label and len(args.label) not in (1, len(inputs)):
        parser.error("use one --label for all inputs or one --label per input")
    labels: list[Optional[str]]
    if not args.label:
        labels = [None] * len(inputs)
    elif len(args.label) == 1:
        labels = [args.label[0]] * len(inputs)
    else:
        labels = list(args.label)
    if any(label == "" for label in labels if label is not None):
        parser.error("--label must not be empty")

    summary = Summary(args.consistency_tolerance_ms)
    try:
        for input_name, label in zip(inputs, labels):
            lines, handle = open_input(input_name, stdin)
            try:
                consume_lines(summary, lines, input_name, label)
            finally:
                if handle is not None:
                    handle.close()
    except OSError as exc:
        parser.exit(2, f"{parser.prog}: error: {exc}\n")

    if summary.warnings:
        for warning in summary.warnings:
            print(f"warning: {warning}", file=sys.stderr)
        if args.strict:
            return 2
    if summary.overall_records == 0 and summary.group_records == 0:
        print("no valid DS4 ROCm event profile records found", file=sys.stderr)
        return 1
    if args.json:
        json.dump(
            summary.as_json(), sys.stdout, indent=2, sort_keys=True, allow_nan=False
        )
        sys.stdout.write("\n")
    else:
        print_text(summary)
    return 0


if __name__ == "__main__":
    raise SystemExit(run())
