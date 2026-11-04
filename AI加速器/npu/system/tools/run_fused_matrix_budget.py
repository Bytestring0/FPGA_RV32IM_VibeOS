"""Measure actual-census fused matrix shapes with one RTL build; not board speed."""
import argparse
import collections
import csv
import hashlib
import json
import subprocess
import time
from pathlib import Path

SYSTEM = Path(__file__).resolve().parents[1]
ROOT = SYSTEM.parents[2]


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--census", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--lanes", type=int, choices=(16, 32, 64), default=64)
    p.add_argument("--tile-rows", type=int, default=4096)
    p.add_argument("--tensor-budget", type=Path)
    p.add_argument("--clock-hz", type=int, default=162000000)
    p.add_argument("--delta-ii2-saved-per-head", type=int, default=8158, help="Separately measured n128-head cycle saving; not whole-graph simulation")
    a = p.parse_args()
    if not 1 <= a.tile_rows <= 4096 or a.clock_hz <= 0 or a.delta_ii2_saved_per_head < 0:
        p.error("invalid tile/clock/saved cycles")
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    catalog = SYSTEM / "config/TENSOR_HEAD.JSON"
    report = {"passed": False, "board_verified": False, "timing_closed": False, "synthetic_operands": True,
              "scope": "Actual tensor opcode19 census mapped to exact catalog K, measured in isolated fused matrix RTL BFM; synthetic W4/A8/FP16/max. Engine cycle_count excludes MMIO, CPU, cache, copies and shared DDR contention. Not FPGA or end-to-end throughput.",
              "lanes": a.lanes, "clock_hz_assumed": a.clock_hz, "source_sha256": {}}

    def run(argv, log, limit):
        start = time.monotonic()
        with (out / log).open("w") as f:
            result = subprocess.run(list(map(str, argv)), stdout=f, stderr=subprocess.STDOUT, timeout=limit)
        report[log] = {"exit_code": result.returncode, "seconds": round(time.monotonic() - start, 3)}
        if result.returncode:
            raise RuntimeError((out / log).read_text()[-4000:])

    try:
        # This original model run did not emit raw_jobs.csv. Refuse ambiguous K inference.
        raw_path = a.census.parent / "raw_jobs.csv"
        if raw_path.exists():
            raise ValueError("raw_jobs.csv is available: inspect its schema and use it rather than catalog reconstruction")
        data = a.census.read_bytes(); report["source_sha256"][str(a.census.resolve())] = hashlib.sha256(data).hexdigest()
        (out / "tensor_jobs_original.csv").write_bytes(data)
        with a.census.open(newline="") as f:
            reader = csv.DictReader(f)
            if reader.fieldnames != ["opcode", "n", "m", "k", "param0", "param1"]:
                raise ValueError("unexpected tensor census schema")
            all_jobs = [{key: int(value) for key, value in row.items()} for row in reader]
        actual = collections.Counter((r["n"], r["m"]) for r in all_jobs if r["opcode"] == 19)
        tensors = [t for t in json.loads(catalog.read_text())["tensors"] if t.get("target_dtype") == "signed_W4_zero_point_0"]
        group_to_k = collections.defaultdict(set)
        expected = collections.Counter(); head = collections.Counter(); assignments = []
        for t in tensors:
            n, k = t["shape_export_NK"]; groups = t["groups_per_row"]
            if groups != (k + 127) // 128 or not 1 <= k <= 4096:
                raise ValueError("invalid exact catalog K")
            group_to_k[groups].add(k)
            tiles = collections.Counter(min(a.tile_rows, n - first) for first in range(0, n, a.tile_rows))
            is_head = t["name"] == "output_w4.weight"
            for rows, calls in tiles.items():
                expected[rows, groups] += calls
                if is_head: head[rows, k] += calls
            assignments.append({"name": t["name"], "n": n, "k": k, "groups": groups, "head": is_head, "tiles": dict(tiles)})
        if any(len(ks) != 1 for groups, ks in group_to_k.items() if any(m == groups for n, m in actual)):
            raise ValueError("ambiguous actual K within a raw group count; need raw_jobs.csv")
        if actual != expected:
            raise ValueError("catalog-once tile reconstruction differs from ACTUAL opcode19 census: " + str(actual - expected) + " / " + str(expected - actual))
        shapes = collections.Counter()
        for (n, groups), calls in actual.items(): shapes[n, next(iter(group_to_k[groups]))] += calls
        if sum(1 for t in assignments if t["head"]) != 1:
            raise ValueError("expected exactly one catalog W4 output head")
        report["mapping"] = {"raw_jobs_csv_available": False, "method": "Actual opcode19 n,m call counter equals exact catalog-once projection split into tile_rows; catalog gives unique exact K for every m. No rounding m*128 assumption.",
                             "tile_rows": a.tile_rows, "original_tensor_jobs": len(all_jobs), "actual_raw_calls": sum(actual.values()),
                             "catalog_projection_count": len(assignments), "head_projection_count": 1, "head_hardware_calls": sum(head.values()), "head_catalog": [t for t in assignments if t["head"]],
                             "exact_catalog_assignments": assignments}
        shape_csv = out / "matrix_shapes.csv"
        with shape_csv.open("w", newline="") as f:
            writer = csv.writer(f); writer.writerow(("n", "k", "calls", "head_calls"))
            writer.writerows((n, k, calls, head[n,k]) for (n,k),calls in sorted(shapes.items()))
        sources = [SYSTEM / "rtl" / name for name in ("npu_dma_top.sv", "npu_w4_fused_reduce.sv", "gemv_pipeline16.sv", "gemv_pipeline_v2.sv")]
        sources += [SYSTEM.parents[1] / "rtl/compute" / name for name in ("gemv_core.sv", "group_acc.sv", "mac_lane.sv")]
        bench = SYSTEM / "sim/test_fused_matrix_budget.cpp"
        for source in [*sources, bench, SYSTEM / "sim/test_npu_w4_fused.cpp", catalog, Path(__file__)]:
            report["source_sha256"][str(source)] = hashlib.sha256(source.read_bytes()).hexdigest()
        run(["verilator", "--cc", "--exe", "--build", "-j", "4", "-Wall", "--top-module", "npu_dma_top", "--Mdir", out / "obj",
             "-GLANES=" + str(a.lanes), "-CFLAGS", "-O2 -DNPU_DMA_LANES=" + str(a.lanes), *sources, bench], "build.log", 120)
        run([out / "obj/Vnpu_dma_top", shape_csv], "matrix_budget.log", 60)
        rows = []
        for line in (out / "matrix_budget.log").read_text().splitlines():
            if not line.startswith("MATRIX_BUDGET,"): continue
            values = list(map(int, line.split(",")[1:]))
            rows.append(dict(zip(("n", "k", "calls", "head_calls", "engine_cycles", "reads", "writes", "weighted_cycles"), values)))
        measured = {(r["n"], r["k"]):r for r in rows}
        if len(rows) != len(shapes) or set(measured) != set(shapes) or any(r["calls"] != shapes[key] or r["head_calls"] != head[key] or r["engine_cycles"] <= 0 or r["weighted_cycles"] != r["engine_cycles"] * r["calls"] for key,r in measured.items()):
            raise ValueError("measured matrix rows differ from actual census")
        cycles = sum(r["weighted_cycles"] for r in rows)
        report.update(passed=True, rows=rows, weighted_matrix_cycles=cycles, head_weighted_cycles=sum(r["engine_cycles"] * r["head_calls"] for r in rows),
                      matrix_jobs=sum(shapes.values()), matrix_macs_from_actual_calls=sum(n*k*calls for (n,k),calls in shapes.items()))
        if a.tensor_budget:
            tensor = json.loads(a.tensor_budget.read_text())
            if not tensor.get("passed") or tensor.get("exclusion", {}).get("opcodes") != [19] or tensor.get("original_tensor_jobs") != len(all_jobs):
                raise ValueError("need passing same-census remaining tensor budget with only opcode19 excluded")
            with (Path(tensor["source_snapshot"][str(a.census.resolve())])).open(newline="") as f:
                bound_rows = [{key: int(value) for key,value in row.items()} for row in csv.DictReader(f)]
            if bound_rows != all_jobs: raise ValueError("matrix and tensor bound census differ")
            base = tensor["weighted_cycles"]
            heads = sum(r["calls"] for r in tensor["rows"] if r["opcode"] == 20 and r["n"] == 128)
            saving = heads * a.delta_ii2_saved_per_head
            total = cycles + base; improved = total - saving
            report["combined_serial_budget"] = {"scope": "Sum of separately measured isolated engines; assumes serial scheduling. Delta II2 saving supplied from independent n128-head regression, extrapolated to actual 288-head census. Shared DDR/CPU/cache/MMIO/copies and physical timing excluded.",
                "tensor_baseline_cycles": base, "matrix_measured_cycles": cycles, "baseline_cycles": total, "delta_n128_calls": heads,
                "delta_ii2_measured_saving_per_head": a.delta_ii2_saved_per_head, "delta_ii2_extrapolated_saving": saving, "after_delta_ii2_cycles": improved,
                "clock_scenarios": [{"clock_hz_assumed": hz, "baseline_tokens_per_second_budget_ceiling": hz / total, "after_delta_ii2_tokens_per_second_budget_ceiling": hz / improved,
                                      "after_delta_ii2_cpu_extra_headroom_for_2tps_cycles": hz / 2 - improved} for hz in sorted({100000000, 125000000, 150000000, 162000000, a.clock_hz})]}
        print(json.dumps({k: report[k] for k in ("matrix_jobs", "weighted_matrix_cycles", "head_weighted_cycles", "matrix_macs_from_actual_calls")}, indent=2))
        if "combined_serial_budget" in report: print(json.dumps(report["combined_serial_budget"], indent=2))
    finally:
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
