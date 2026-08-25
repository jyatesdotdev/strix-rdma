#!/usr/bin/env python3
import argparse
import array
import hashlib
import json
import math
from pathlib import Path
import sys

p = argparse.ArgumentParser()
p.add_argument("--prefix", required=True)
p.add_argument("--mode", choices=("standalone", "coordinator", "worker"), required=True)
p.add_argument("--expected-output-sha")
a = p.parse_args()
if a.mode == "standalone" and not a.expected_output_sha:
    p.error("standalone mode requires --expected-output-sha")
if a.mode == "worker" and a.expected_output_sha:
    p.error("worker mode cannot require --expected-output-sha")
if a.expected_output_sha and (
    len(a.expected_output_sha) != 64 or
    any(c not in "0123456789abcdef" for c in a.expected_output_sha)
):
    p.error("--expected-output-sha must be 64 lowercase hexadecimal digits")
prefix = Path(a.prefix)
base = prefix.name
root = prefix.parent
expected = {}
for layer in range(43):
    expected[root / f"{base}_hc_attn_post-{layer}_pos0.bin"] = (65536, "f32")
    expected[root / f"{base}_hc_ffn_post-{layer}_pos0.bin"] = (65536, "f32")
    expected[root / f"{base}_ffn_moe_topk-{layer}_pos0.i32"] = (24, "topk")
    expected[root / f"{base}_ffn_moe_weights_scaled-{layer}_pos0.bin"] = (24, "f32")
output = root / f"{base}_result_output-43_pos0.bin"
if a.mode != "worker":
    expected[output] = (517120, "f32")
actual = set(root.glob(base + "_*"))
failures = []
missing = sorted(str(x) for x in set(expected) - actual)
unexpected = sorted(str(x) for x in actual - set(expected))
if missing:
    failures.append(f"missing {len(missing)} files")
if unexpected:
    failures.append(f"unexpected {len(unexpected)} files")
finite_values = 0
float_values = 0
topk_layers = 0
file_hashes = {}
for path, (size, kind) in sorted(expected.items(), key=lambda x: str(x[0])):
    if not path.is_file():
        continue
    data = path.read_bytes()
    file_hashes[path.name] = hashlib.sha256(data).hexdigest()
    if len(data) != size:
        failures.append(f"bad size {path}: {len(data)} != {size}")
        continue
    if kind == "topk":
        vals = array.array("i")
        vals.frombytes(data)
        if sys.byteorder != "little":
            vals.byteswap()
        if len(vals) != 6 or len(set(vals)) != 6 or any(v < 0 or v > 255 for v in vals):
            failures.append(f"invalid topk {path}: {list(vals)}")
        else:
            topk_layers += 1
    else:
        vals = array.array("f")
        vals.frombytes(data)
        if sys.byteorder != "little":
            vals.byteswap()
        nfinite = sum(math.isfinite(v) for v in vals)
        finite_values += nfinite
        float_values += len(vals)
        if nfinite != len(vals):
            failures.append(f"nonfinite f32 {path}: {len(vals) - nfinite}/{len(vals)}")
if a.expected_output_sha:
    if output.is_file():
        actual_sha = file_hashes.get(output.name)
        if actual_sha != a.expected_output_sha:
            failures.append(f"output SHA {actual_sha} != {a.expected_output_sha}")
    else:
        failures.append("expected output file is absent")
expected_count = 172 if a.mode == "worker" else 173
expected_bytes = 5638160 if a.mode == "worker" else 6155280
actual_bytes = sum(x.stat().st_size for x in actual if x.is_file())
if len(actual) != expected_count:
    failures.append(f"file count {len(actual)} != {expected_count}")
if actual_bytes != expected_bytes:
    failures.append(f"byte count {actual_bytes} != {expected_bytes}")
report = {
    "mode": a.mode,
    "prefix": str(prefix),
    "ok": not failures,
    "file_count": len(actual),
    "expected_file_count": expected_count,
    "byte_count": actual_bytes,
    "expected_byte_count": expected_bytes,
    "finite_f32": finite_values,
    "total_f32": float_values,
    "valid_topk_layers": topk_layers,
    "output_sha256": file_hashes.get(output.name),
    "missing": missing,
    "unexpected": unexpected,
    "failures": failures,
}
print(json.dumps(report, indent=2, sort_keys=True))
sys.exit(0 if not failures else 1)
