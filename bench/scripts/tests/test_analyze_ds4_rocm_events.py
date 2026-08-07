#!/usr/bin/env python3
"""Deterministic CLI tests for analyze-ds4-rocm-events.py."""

from __future__ import annotations

import json
import subprocess
import sys
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[1] / "analyze-ds4-rocm-events.py"


def overall(
    samples: int,
    scale: float = 1.0,
    dropped: int = 0,
    direct_mask: int = 0,
) -> str:
    base = {
        "stream0": 34.0,
        "host": 34.1,
        "host_minus_stream0": 0.1,
        "input": 1.0,
        "unsampled": 10.0,
        "sampled_layer": 20.0,
        "qkv": 1.0,
        "compressor_indexer": 2.0,
        "attn_core": 3.0,
        "attn_output": 4.0,
        "attn_hc": 1.0,
        "ffn_router": 2.0,
        "routed_moe": 5.0,
        "shared_post": 2.0,
        "output_head": 3.0,
        "copyback": 0.0,
    }
    metrics = " ".join(f"{name}={value * scale:.3f}ms" for name, value in base.items())
    directs = " ".join(
        f"direct{mask}={samples if mask == direct_mask else 0}" for mask in range(8)
    )
    return (
        f"ds4: ROCm event profile: samples={samples} dropped={dropped} layers=0:21 "
        f"{metrics} n_raw={8.0 * scale:.1f} n_comp={2.0 * scale:.1f} {directs}"
    )


def group(ratio: int, samples: int, scale: float = 1.0, emit: int = 0) -> str:
    base = {
        "sampled_layer": 20.0,
        "qkv": 1.0,
        "compressor_indexer": 2.0,
        "attn_core": 3.0,
        "attn_output": 4.0,
        "attn_hc": 1.0,
        "ffn_router": 2.0,
        "routed_moe": 5.0,
        "shared_post": 2.0,
    }
    metrics = " ".join(f"{name}={value * scale:.3f}ms" for name, value in base.items())
    return (
        f"ds4: ROCm event profile group: ratio={ratio} samples={samples} "
        f"emit={emit} {metrics}"
    )


def run_cli(input_text: str, *arguments: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *arguments],
        input=input_text,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


class AnalyzerCliTests(unittest.TestCase):
    def test_journal_metadata_and_weighted_aggregation(self) -> None:
        lines = [
            "2026-08-06T12:00:00-0700 max2 ds4-server[10]: " + overall(32),
            "2026-08-06T12:00:01-0700 max2 ds4-server[10]: " + group(4, 20, emit=5),
            "2026-08-06T12:00:02-0700 max2 ds4-server[10]: " + group(128, 12),
            "2026-08-06T12:00:03-0700 max2 ds4-server[10]: " + overall(8, 2.0, 2, 2),
            "2026-08-06T12:00:04-0700 max2 ds4-server[10]: " + group(4, 8, 2.0, 2),
            "2026-08-06T12:00:05-0700 max ds4-worker[11]: " + overall(4),
        ]
        result = run_cli("\n".join(lines) + "\n", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        sources = {source["label"]: source for source in data["sources"]}

        coordinator = sources["max2/coordinator"]
        aggregate = coordinator["overall"][0]
        self.assertEqual(aggregate["samples"], 40)
        self.assertEqual(aggregate["dropped_samples"], 2)
        self.assertAlmostEqual(
            aggregate["metrics"]["stream0"]["weighted_mean"], 40.8
        )
        self.assertAlmostEqual(
            aggregate["metrics"]["n_raw"]["weighted_mean"], 9.6
        )
        self.assertEqual(aggregate["direct_masks"]["0x0"], 32)
        self.assertEqual(aggregate["direct_masks"]["0x2"], 8)
        for check in aggregate["consistency"].values():
            self.assertAlmostEqual(check["weighted_mean_residual_ms"], 0.0)
            self.assertEqual(check["records_outside_tolerance"], 0)
        self.assertEqual(coordinator["ratio_group_coverage"]["difference"], 0)

        ratio4 = next(item for item in coordinator["ratio_groups"] if item["ratio"] == 4)
        self.assertEqual(ratio4["samples"], 28)
        self.assertEqual(ratio4["emit_samples"], 7)
        self.assertAlmostEqual(
            ratio4["metrics"]["sampled_layer"]["weighted_mean"],
            (20.0 * 20 + 40.0 * 8) / 28,
        )
        self.assertIn("max/worker", sources)

    def test_explicit_label_for_raw_stdin_and_human_output(self) -> None:
        result = run_cli(overall(2) + "\n", "--label", "coordinator-A")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("source: coordinator-A", result.stdout)
        self.assertIn("weighted_n", result.stdout)
        self.assertIn("stream0_vs_top_level_segments", result.stdout)

    def test_journal_json_metadata(self) -> None:
        record = {
            "_HOSTNAME": "max",
            "_SYSTEMD_UNIT": "ds4-mxfp4-worker.service",
            "MESSAGE": overall(3),
        }
        result = run_cli(json.dumps(record) + "\n", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["sources"][0]["label"], "max/worker")
        self.assertEqual(data["sources"][0]["overall"][0]["samples"], 3)

    def test_ratio_group_field_alias(self) -> None:
        alias = group(4, 3).replace(
            "ROCm event profile group: ratio=4",
            "ROCm event profile: ratio_group=4",
        )
        result = run_cli(overall(3) + "\n" + alias + "\n", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["sources"][0]["ratio_groups"][0]["ratio"], 4)
        self.assertEqual(data["sources"][0]["ratio_groups"][0]["samples"], 3)

    def test_strict_rejects_malformed_profile_line(self) -> None:
        result = run_cli(
            "ds4: ROCm event profile: samples=32 dropped=0\n",
            "--strict",
        )
        self.assertEqual(result.returncode, 2)
        self.assertEqual(result.stdout, "")
        self.assertIn("missing fields", result.stderr)

    def test_numeric_overflow_is_rejected_without_partial_aggregation(self) -> None:
        huge_samples = overall(2).replace(
            "samples=2", "samples=" + ("9" * 5000), 1
        )
        huge_result = run_cli(huge_samples + "\n", "--strict")
        self.assertEqual(huge_result.returncode, 2)
        self.assertEqual(huge_result.stdout, "")
        self.assertNotIn("Traceback", huge_result.stderr)
        self.assertIn("too large", huge_result.stderr)

        overflow = overall(32).replace(
            "stream0=34.000ms", "stream0=1e308ms", 1
        )
        result = run_cli(overall(2) + "\n" + overflow + "\n", "--json")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Infinity", result.stdout)
        self.assertNotIn("Traceback", result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["totals"]["malformed_profile_lines"], 1)
        self.assertEqual(data["sources"][0]["overall"][0]["samples"], 2)

        strict_result = run_cli(overflow + "\n", "--strict")
        self.assertEqual(strict_result.returncode, 2)
        self.assertEqual(strict_result.stdout, "")
        self.assertNotIn("Traceback", strict_result.stderr)
        self.assertIn("finite float range", strict_result.stderr)


if __name__ == "__main__":
    unittest.main()
