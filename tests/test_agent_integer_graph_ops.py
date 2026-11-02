"""Independent synthetic/NumPy/Fraction graph-op tests; do not read evaluation."""
from __future__ import annotations

import ast
from copy import deepcopy
from fractions import Fraction
import inspect
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import agent_integer_graph_ops as graph
from agent_integer_reference import Stats

OUT = Path(os.environ.get("AGENT_GRAPH_OPS_TEST_OUT",
           r"E:\FPGA_RV32IM_Work\deepseek_agent_accel_20261102\integer_reference\graph_ops\test_tmp"))


def bf16_float(bits):
    return float(np.array([int(bits) << 16], dtype=np.uint32).view(np.float32)[0])


def quantized(frac, exponent, bits):
    scalar = frac * (1 << -exponent) if exponent < 0 else frac / (1 << exponent)
    return min((1 << (bits - 1)) - 1, max(-(1 << (bits - 1)), round(scalar)))


class RoPETests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tables = {context: graph.generate_rope_table(context) for context in graph.ROPE_CONTEXTS}

    def test_offline_table_domains_and_float_oracle(self):
        rng = np.random.default_rng(11)
        original = rng.integers(-30000, 30001, 256, dtype=np.int64).tolist()
        for context, table in self.tables.items():
            self.assertEqual(len(table.rows), context)
            self.assertEqual(graph.rope_apply_q30(original, 0, table, 16), original)
            for position in sorted({0, 1, min(context - 1, 127), context - 1}):
                result = graph.rope_apply_q30(original, position, table, 32)
                self.assertEqual(result[64:], original[64:])
                for i in range(32):
                    angle = position * 10000000.0 ** (-2 * i / 64)
                    cs, sn = np.cos(angle), np.sin(angle)
                    a, b = original[i], original[i + 32]
                    self.assertLessEqual(abs(result[i] - round(a * cs - b * sn)), 1)
                    self.assertLessEqual(abs(result[i + 32] - round(b * cs + a * sn)), 1)
                    self.assertLessEqual(abs(table.rows[position][i][0] - round(cs * graph.Q30)), 1)
                    self.assertLessEqual(abs(table.rows[position][i][1] - round(sn * graph.Q30)), 1)
            for bad_position in (-1, context, context + 1, True, 0.0):
                with self.assertRaises(ValueError):
                    graph.rope_apply_q30(original, bad_position, table)
        self.assertEqual(self.tables[128].rows[-1], self.tables[2048].rows[127])
        self.assertEqual(self.tables[1024].rows[-1], self.tables[2048].rows[1023])
        with self.assertRaises(ValueError):
            graph.generate_rope_table(64)
        with self.assertRaises(ValueError):
            graph.generate_rope_table(2050)

    def test_rope_coefficients_mutation_version_and_saturation(self):
        artifact = self.tables[128].artifact()
        self.assertEqual(graph.load_rope_table(artifact), self.tables[128])
        corrupt = deepcopy(artifact)
        corrupt["rows"][1][0][0] -= 1
        with self.assertRaises(ValueError):
            graph.load_rope_table(corrupt)
        corrupt = deepcopy(artifact)
        corrupt["theta"] = 10000
        corrupt["sha256"] = graph.digest_json({k: v for k, v in corrupt.items() if k != "sha256"})
        with self.assertRaises(ValueError):
            graph.load_rope_table(corrupt)
        corrupt = deepcopy(artifact)
        corrupt["rows"].pop()
        corrupt["max_position"] = 126
        corrupt["context_tokens"] = 127
        corrupt["sha256"] = graph.digest_json({k: v for k, v in corrupt.items() if k != "sha256"})
        with self.assertRaises(ValueError):
            graph.load_rope_table(corrupt)
        stats = Stats()
        data = [0] * 64
        data[0], data[32] = 32767, -32768
        result = graph.rope_apply_q30(data, 1, self.tables[128], 16, stats)
        self.assertEqual(result[0], 32767)
        self.assertGreater(stats.counts.get("saturation", 0), 0)

    def test_integer_execution_not_decimal_float_wrapper(self):
        for name in ("rope_apply_q30", "bf16_decode", "bf16_embedding_slice",
                     "bf16_embedding_row", "bf16_head_dot_row", "bf16_head_selected",
                     "export_bfp_candidate", "verify_bfp_candidate"):
            tree = ast.parse(inspect.getsource(getattr(graph, name)))
            self.assertFalse(any(isinstance(node, ast.Constant) and type(node.value) is float
                                 for node in ast.walk(tree)), name)
            self.assertFalse(any(isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and
                                 node.func.id in ("float", "Decimal") for node in ast.walk(tree)), name)


class BF16Tests(unittest.TestCase):
    def test_bf16_bit_decode_exact_fraction_and_nonfinite(self):
        rng = np.random.default_rng(12)
        cases = [0, 0x8000, 1, 0x8001, 0x007f, 0x0080, 0x3f80, 0xbf80,
                 0x4000, 0x7f7f, 0xff7f] + rng.integers(0, 0x10000, 5000).tolist()
        for bits in cases:
            exponent = (bits >> 7) & 255
            if exponent == 255:
                with self.assertRaises(ValueError):
                    graph.bf16_decode(bits)
                continue
            value = graph.bf16_decode(bits)
            actual = Fraction(value.mantissa) * (Fraction(1 << value.exponent) if value.exponent >= 0
                         else Fraction(1, 1 << -value.exponent))
            self.assertEqual(actual, Fraction.from_float(bf16_float(bits)))
        stats = Stats()
        self.assertEqual(graph.bf16_decode(0x8000, stats), graph.BF16Value(0, 0))
        self.assertEqual(stats.counts["negative_zero"], 1)
        for bad in (65536, -1, 1.0, True):
            with self.assertRaises(ValueError):
                graph.bf16_decode(bad)

    def test_integer_embedding_and_selected_tied_head_dot(self):
        rng = np.random.default_rng(13)
        vocab, hidden = 5, 32
        codes = rng.choice([0x3f80, 0xbf80, 0x4000, 0xc000, 0x3e80,
                            0xbe80, 0x0080, 1, 0x7f00], size=vocab * hidden)
        packed = np.asarray(codes, dtype="<u2").tobytes()
        stats = Stats()
        for token_id in (0, 2, vocab - 1):
            result = graph.bf16_embedding_row(packed, token_id, vocab, hidden, -12, 32, stats)
            sliced = graph.bf16_embedding_slice(packed[token_id * hidden * 2:(token_id + 1) * hidden * 2],
                                                -12, 32, stats)
            self.assertEqual(sliced, result)
            expected = [quantized(Fraction.from_float(bf16_float(bits)), -12, 32)
                        for bits in codes[token_id * hidden:(token_id + 1) * hidden]]
            self.assertEqual(result, expected)
        activation = rng.integers(-10000, 10001, hidden).tolist()
        rows = [4, 0, 3]
        logits = graph.bf16_head_selected(packed, rows, activation, vocab, 14, -6, 32, stats)
        for entry, row in zip(logits, rows):
            oracle = sum(Fraction.from_float(bf16_float(bits)) * Fraction(x, 1 << 14)
                         for bits, x in zip(codes[row * hidden:(row + 1) * hidden], activation))
            self.assertEqual(entry, {"row_id": row, "value": quantized(oracle, -6, 32)})
        self.assertGreater(stats.peak_magnitude_bits["bf16.head.accumulator"], 0)
        with self.assertRaises(ValueError):
            graph.bf16_embedding_row(packed, vocab, vocab, hidden, 0)
        with self.assertRaises(ValueError):
            graph.bf16_embedding_row(packed[:-1], 1, vocab, hidden, 0)
        with self.assertRaises(ValueError):
            graph.bf16_head_selected(packed, [0, 0], activation, vocab, 14, 0)
        with self.assertRaises(ValueError):
            graph.bf16_head_selected(packed, [0], activation[:-1], vocab, 14, 0)
        with self.assertRaises(ValueError):
            graph.bf16_head_dot_row(packed[:hidden * 2], activation[:1], 14, 0)
        with self.assertRaises(ValueError):
            graph.bf16_head_dot_row(packed[:hidden * 2], [0.1] * hidden, 14, 0)

    def test_wide_integer_head_not_w4_parallel_export(self):
        weights = np.array([0x7f7f, 0x007f, 0xbf80], dtype="<u2").tobytes()
        activations = [1, -1, 1]
        stats = Stats()
        value = graph.bf16_head_dot_row(weights, activations, 0, 30, 32, stats)
        exact = sum(Fraction.from_float(bf16_float(bits)) * x for bits, x in
                    zip([0x7f7f, 0x007f, 0xbf80], activations))
        self.assertEqual(value, quantized(exact, 30, 32))
        self.assertGreater(stats.counts.get("saturation", 0), 0)
        self.assertIn("not_w4_head", graph.metadata()["bf16"])


class BFPExportTests(unittest.TestCase):
    @staticmethod
    def identity():
        return {"model_sha256": "a" * 64, "source_sha256": "b" * 64, "trace_sha256": "c" * 64}

    def test_per_head_export_roundtrip_bytes_and_scale(self):
        values = [1, 0, -3, 8, 1000000, -1000000,
                  1 << 34, -1 << 34, 7, -8, 33, -66]
        for bits in (16, 32):
            with self.subTest(bits=bits):
                stats = Stats()
                blob, manifest = graph.export_bfp_candidate(values, [2, 2, 3], -20, bits, self.identity(),
                    "linear.delta.state.before", stats)
                decoded = graph.verify_bfp_candidate(blob, manifest)
                self.assertEqual(len(blob), 20 + 32 + 2 + len(values) * (bits // 8))
                self.assertEqual(manifest["total_bytes"], len(blob))
                self.assertEqual(manifest["binding_bytes"], 32)
                self.assertNotEqual(manifest["heads"][0]["exponent"], manifest["heads"][1]["exponent"])
                for i, row in enumerate(decoded):
                    exponent = manifest["heads"][i]["exponent"]
                    expected = [quantized(Fraction(v) *
                               (Fraction(1, 1 << 20)), exponent, bits)
                               for v in values[i * 6:(i + 1) * 6]]
                    self.assertEqual(row, expected)
                self.assertEqual(manifest["heads"][0]["end"], manifest["heads"][1]["start"])
                self.assertGreater(stats.counts.get("underflow", 0), 0)
                self.assertIn("NOT-APPROVED", manifest["version"])

    def test_identity_alias_corruption_and_overflow_reporting(self):
        stats = Stats()
        blob, manifest = graph.export_bfp_candidate([1 << 250, -1 << 250], [2, 1, 1], 127, 16,
                   self.identity(), "attention.key.state", stats)
        self.assertGreater(stats.counts.get("saturation", 0), 0)
        for mutation in ("identity", "alias", "hash", "tail", "tag", "input_exponent", "header"):
            with self.subTest(mutation=mutation):
                m = deepcopy(manifest); data = bytearray(blob)
                if mutation == "identity":
                    m["identity"]["source_sha256"] = "f" * 64
                elif mutation == "alias":
                    m["heads"][1]["start"] = m["heads"][0]["start"]
                elif mutation == "hash":
                    data[-1] ^= 1
                elif mutation == "tail":
                    data += b"\0"
                elif mutation == "tag":
                    m["tensor_tag"] = "linear.delta.state.after"
                elif mutation == "input_exponent":
                    m["input"]["exponent"] = 126
                else:
                    data[0] = 0
                with self.assertRaises(ValueError):
                    graph.verify_bfp_candidate(bytes(data), m)
        for kwargs in ({"identity": {"source_sha256": "HEAD"}},
                       {"tag": "unknown.projection"}, {"shape": [1, 3]}, {"bits": 8}):
            params = {"values": [1, 2], "shape": [2, 1, 1], "input_exponent": 0,
                      "bits": 16, "identity": self.identity(), "tag": "attention.key.state"}
            params.update(kwargs)
            with self.assertRaises(ValueError):
                graph.export_bfp_candidate(**params)


class CLITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)

    def test_cli_rope_bf16_export_artifacts_and_alias_guard(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            root = Path(folder)
            lut = root / "rope128.json"
            self.assertEqual(graph.main(["rope-lut", "--context", "128", "--out", str(lut)]), 0)
            raw = json.loads(lut.read_text(encoding="utf-8"))
            case = root / "case.json"; result = root / "result.json"
            graph.write_json(case, {"schema": "agent-graph-integer-case-v1", "kernel": "rope",
                "expected_lut_sha256": raw["sha256"],
                "parameters": {"values": [0, 100] + [0] * 62, "position": 127, "bits": 16}})
            self.assertEqual(graph.main(["run", "--case", str(case), "--rope-lut", str(lut),
                                         "--out", str(result)]), 0)
            self.assertEqual(json.loads(result.read_text())["rope_lut_sha256"], raw["sha256"])
            with self.assertRaisesRegex(ValueError, "aliases"):
                graph.main(["run", "--case", str(case), "--rope-lut", str(lut), "--out", str(lut)])
            graph.write_json(case, {"schema": "agent-graph-integer-case-v1", "kernel": "bf16_embedding",
                "parameters": {"matrix_hex": np.array([0x3f80, 0xbf80, 0x4000, 0], dtype="<u2").tobytes().hex(),
                               "token_id": 1, "vocab": 2, "width": 2, "output_exponent": 0}})
            graph.main(["run", "--case", str(case), "--out", str(result)])
            self.assertEqual(json.loads(result.read_text())["output"], [2, 0])
            export_case = root / "export.json"; blob = root / "state.ibfp"; manifest = root / "state.json"
            graph.write_json(export_case, {"schema": "agent-graph-bfp-export-v1",
                "parameters": {"values": [5, 6, 7, 8], "shape": [2, 1, 2],
                    "input_exponent": -12, "bits": 16, "identity": BFPExportTests.identity(),
                    "tag": "linear.delta.state.after"}})
            self.assertEqual(graph.main(["export-bfp", "--case", str(export_case),
                "--out", str(blob), "--manifest", str(manifest)]), 0)
            self.assertEqual(graph.verify_bfp_candidate(blob.read_bytes(), json.loads(manifest.read_text())),
                             [[5, 6], [7, 8]])


if __name__ == "__main__":
    unittest.main()
