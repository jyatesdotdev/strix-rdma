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
p.add_argument("--log", required=True)
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
if array.array("f").itemsize != 4 or array.array("i").itemsize != 4:
    p.error("native array f/i items must both be exactly 4 bytes")
prefix = Path(a.prefix)
base = prefix.name
root = prefix.parent
log_path = Path(a.log)
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
try:
    log_data = log_path.read_bytes()
except OSError as e:
    log_data = b""
    failures.append(f"cannot read log {log_path}: {e}")
if not log_data:
    failures.append(f"empty log {log_path}")
for marker in (
    b"failed to synchronize before dumping",
    b"failed to resume Metal command batch after dumping",
):
    if marker in log_data:
        failures.append(f"dump error in log: {marker.decode()}")
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
expected_f32 = 1409282 if a.mode == "worker" else 1538562
if float_values != expected_f32 or finite_values != expected_f32:
    failures.append(
        f"decoded finite f32 count {finite_values}/{float_values} != {expected_f32}"
    )
if topk_layers != 43:
    failures.append(f"valid topk layer count {topk_layers} != 43")
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
    "log": str(log_path),
    "log_sha256": hashlib.sha256(log_data).hexdigest(),
    "missing": missing,
    "unexpected": unexpected,
    "failures": failures,
}
print(json.dumps(report, indent=2, sort_keys=True))
sys.exit(0 if not failures else 1)
