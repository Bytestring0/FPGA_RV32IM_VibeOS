"""Shape-weighted isolated RTL cost from real graph calls; never board throughput."""
import argparse
import collections
import csv
import hashlib
import io
import json
import subprocess
import time
from pathlib import Path

SYSTEM = Path(__file__).resolve().parents[1]
KEYS = ("opcode", "n", "m", "k", "param0", "param1")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--census", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--scalar-raw", action="store_true", help="Set PARALLEL_RAW=0")
    p.add_argument("--shared-raw-accum", action="store_true", help="Set SHARED_RAW_ACCUM=1 (only used by parallel raw)")
    p.add_argument("--scalar-delta", action="store_true")
    p.add_argument("--scalar-conv", action="store_true")
    p.add_argument("--exclude-raw-reduce", action="store_true", help="Exclude opcode19 ONLY when matrix fusion implements that task elsewhere; excludes fused matrix cost, not a zero-cost claim")
    a = p.parse_args()
    out = a.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    report = {"passed": False, "synthetic_operands": True,
              "scope": "Actual graph shape counts times isolated tensor RTL BFM cycles; excludes CPU/cache/matrix, MMIO submission, physical timing and fused matrix cost.",
              "board_verified": False, "timing_closed": False,
              "architecture": {"parallel_raw": not a.scalar_raw, "shared_raw_accum": a.shared_raw_accum,
                               "parallel_delta": not a.scalar_delta, "parallel_conv": not a.scalar_conv, "split_read_cache": True},
              "exclusion": {"opcodes": [19] if a.exclude_raw_reduce else [],
                            "scope": "Opcode19 tasks are assumed implemented by separately validated matrix fusion; this report measures ONLY remaining tensor work, not fusion correctness or end-to-end throughput." if a.exclude_raw_reduce else "Full supplied tensor census; no task excluded."},
              "source_sha256": {}, "source_snapshot": {}}

    def run(argv, log, limit=60):
        start = time.monotonic()
        with (out / log).open("w") as f:
            r = subprocess.run(list(map(str, argv)), stdout=f, stderr=subprocess.STDOUT, timeout=limit)
        report[log] = {"exit": r.returncode, "seconds": round(time.monotonic() - start, 3)}
        if r.returncode:
            raise RuntimeError((out / log).read_text()[-3000:])

    try:
        # Copy once and compile the snapshot: concurrent integration cannot change this run.
        sources = ["rtl/npu_tensor_v2.sv", "sim/test_tensor_budget.cpp", "rtl/npu_delta_vectors.sv", "sim/test_tensor_v2.cpp",
                   "sw/npu_tensor_reference.c", "sw/npu_tensor_v2.h", "sw/npu_fixed_math.c",
                   "sw/npu_fixed_math.h", "sw/npu_fixed_lut_generated.h", "sw/npu_fixed_lut_q16_generated.h"]
        snapshot = out / "inputs"
        for rel in sources:
            source = SYSTEM / rel
            data = source.read_bytes()
            target = snapshot / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(data)
            report["source_sha256"][str(source)] = hashlib.sha256(data).hexdigest()
            report["source_snapshot"][str(source)] = str(target)
        data = a.census.resolve().read_bytes()
        report["source_sha256"][str(a.census.resolve())] = hashlib.sha256(data).hexdigest()
        census_snapshot = snapshot / "tensor_jobs_original.csv"
        census_snapshot.write_bytes(data)
        report["source_snapshot"][str(a.census.resolve())] = str(census_snapshot)
        reader = csv.DictReader(io.StringIO(data.decode("utf-8")))
        if reader.fieldnames != list(KEYS):
            raise ValueError("unexpected census schema")
        original = [tuple(int(row[k]) for k in KEYS) for row in reader]
        if not original or any(v < 0 or v > 0xffffffff for row in original for v in row):
            raise ValueError("empty census or out-of-range descriptor")
        excluded = collections.Counter(row for row in original if a.exclude_raw_reduce and row[0] == 19)
        active = [row for row in original if not (a.exclude_raw_reduce and row[0] == 19)]
        if not active:
            raise ValueError("no remaining tensor tasks")
        report["original_tensor_jobs"] = len(original)
        report["measured_tensor_jobs"] = len(active)
        report["exclusion"].update(jobs=sum(excluded.values()), shapes=[dict(zip(KEYS, key), calls=count) for key, count in sorted(excluded.items())])
        measured_census = out / "tensor_jobs_measured.csv"
        with measured_census.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(KEYS)
            writer.writerows(active)
        objects = []
        for name in ("npu_tensor_reference", "npu_fixed_math"):
            obj = out / (name + ".o")
            objects.append(obj)
            run(["gcc", "-O2", "-I", snapshot / "sw", "-c", snapshot / "sw" / (name + ".c"), "-o", obj], name + ".log")
        params = ["-G" + name + "=" + str(int(value)) for name, value in
                  (("PARALLEL_RAW", not a.scalar_raw), ("SHARED_RAW_ACCUM", a.shared_raw_accum),
                   ("PARALLEL_DELTA", not a.scalar_delta), ("PARALLEL_CONV", not a.scalar_conv), ("SPLIT_READ_CACHE", True))]
        run(["verilator", "--cc", "--exe", "--build", "-j", "4", "-Wno-fatal", "--top-module", "npu_tensor_v2",
             *params, "--Mdir", out / "obj", snapshot / sources[0], snapshot / sources[1], snapshot / "rtl/npu_delta_vectors.sv",
             "-CFLAGS", "-O2", "-LDFLAGS", " ".join(map(str, objects))], "build.log", 120)
        run([out / "obj/Vnpu_tensor_v2", measured_census], "budget.csv")
        rows = []
        for line in (out / "budget.csv").read_text().splitlines():
            if line and line[0].isdigit():
                fields = line.split("TENSOR_SHAPE_BUDGET_PASS", 1)[0].split(",")
                if len(fields) != 11 or not all(v.isdigit() for v in fields):
                    raise ValueError("invalid budget row")
                rows.append(dict(zip((*KEYS, "calls", "engine_cycles", "reads", "writes", "weighted_cycles"), map(int, fields))))
        counts = collections.Counter(active)
        measured = {tuple(r[k] for k in KEYS): r for r in rows}
        if len(measured) != len(rows) or set(measured) != set(counts) or any(
                row["calls"] != counts[key] or row["engine_cycles"] <= 0 or row["weighted_cycles"] != row["calls"] * row["engine_cycles"]
                for key, row in measured.items()):
            raise ValueError("measured shapes/calls/cycles do not match actual census")
        report.update(passed=True, weighted_cycles=sum(r["weighted_cycles"] for r in rows), rows=rows)
        print("TENSOR_SHAPE_BUDGET_PASS", report["weighted_cycles"], "cycles", len(rows), "shapes", len(active), "jobs", "excluded", sum(excluded.values()))
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
