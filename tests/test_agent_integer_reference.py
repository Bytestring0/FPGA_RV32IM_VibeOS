"""Numerical tests; float/NumPy calculations here are independent test oracles.

No model/evaluation corpus is consumed. All fixtures are synthetic. Set
INTEGER_REF_TEST_OUT to an E-drive directory; Python must run with -B.
"""
from __future__ import annotations

import ast
from copy import deepcopy
from decimal import Decimal, ROUND_HALF_EVEN, localcontext
from fractions import Fraction
import hashlib
import inspect
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import agent_integer_reference as ref

OUT = Path(os.environ.get("INTEGER_REF_TEST_OUT",
           r"E:\FPGA_RV32IM_Work\deepseek_agent_accel_20261102\integer_reference\test_tmp"))


def clip_signed(value, bits):
    return min((1 << (bits - 1)) - 1, max(-(1 << (bits - 1)), int(value)))


def dyadic_fraction(scale):
    return Fraction(scale.mantissa << scale.exponent) if scale.exponent >= 0 else Fraction(scale.mantissa, 1 << -scale.exponent)


def delta_oracle(state, q, k, v, decay, beta):
    """Direct float64 equation, independent of integer update implementation."""
    q = q / np.sqrt(np.dot(q, q) + 1e-6) / np.sqrt(len(q))
    k = k / np.sqrt(np.dot(k, k) + 1e-6)
    decayed = state * decay
    error = (v - k @ decayed) * beta
    updated = decayed + np.outer(k, error)
    return updated, q @ updated


class IntegerArithmeticTests(unittest.TestCase):
    def test_signed_rne_exhaustive_and_reports(self):
        for numerator in range(-200, 201):
            for denominator in range(1, 33):
                self.assertEqual(ref.rne_div(numerator, denominator),
                                 round(Fraction(numerator, denominator)))
        stats = ref.Stats()
        self.assertEqual(ref.rne_div(-1, 2, stats), 0)
        self.assertEqual(ref.saturate(32768, 16, stats), 32767)
        self.assertEqual(ref.saturate(-32769, 16, stats), -32768)
        self.assertEqual(stats.counts["underflow"], 1)
        self.assertEqual(stats.counts["saturation"], 2)
        for value in (1.0, True, np.int64(1)):
            with self.assertRaises(ValueError):
                ref.rne_div(value, 2)
        with self.assertRaises(ValueError):
            ref.rne_div(1, 0)

    def test_integer_inverse_sqrt_exact_midpoints(self):
        self.assertEqual(ref.inv_sqrt_ratio_q(4, 25, 0), 2)  # 2.5 -> even 2
        self.assertEqual(ref.inv_sqrt_ratio_q(4, 49, 0), 4)  # 3.5 -> even 4
        rng = np.random.default_rng(101)
        with localcontext() as context:
            context.prec = 100
            for _ in range(200):
                n, d = (int(x) for x in rng.integers(1, 1 << 40, 2))
                expected = int((Decimal(1 << 30) * (Decimal(d) / Decimal(n)).sqrt())
                               .to_integral_value(rounding=ROUND_HALF_EVEN))
                self.assertEqual(ref.inv_sqrt_ratio_q(n, d), expected)

    def test_binary16_exact_scale_decode(self):
        rng = np.random.default_rng(102)
        cases = [1, 2, 0x3ff, 0x400, 0x3c00, 0x7bff] + rng.integers(1, 0x7c00, 1000).tolist()
        for bits in cases:
            decoded = float(np.array([bits], dtype=np.uint16).view(np.float16)[0])
            self.assertEqual(dyadic_fraction(ref.binary16_scale(bits)), Fraction.from_float(decoded))
        for bits in (0, 0x8000, 0xbc00, 0x7c00, 0x7e00):
            with self.assertRaises(ValueError):
                ref.binary16_scale(bits)

    def test_w4_group_dot_padding_and_extremes(self):
        rng = np.random.default_rng(103)
        for columns in (1, 2, 127, 128, 129, 255, 257, 4096):
            groups = (columns + 127) // 128
            weights = rng.integers(-8, 8, (3, groups * 128), dtype=np.int64)
            activations = rng.integers(-128, 128, columns, dtype=np.int64)
            rows = [bytes(((row[::2] & 15) | ((row[1::2] & 15) << 4)).tolist()) for row in weights]
            actual = ref.w4_g128_a8_dot(rows, activations.tolist())
            expected = [[int(np.dot(row[g * 128:min(columns, (g + 1) * 128)],
                                    activations[g * 128:min(columns, (g + 1) * 128)]))
                         for g in range(groups)] for row in weights]
            self.assertEqual(actual, expected)
        self.assertEqual(ref.w4_g128_a8_dot([bytes([0x88] * 64)], [-128] * 128), [[131072]])
        with self.assertRaises(ValueError):
            ref.w4_g128_a8_dot([b"\x01"], [1])
        with self.assertRaises(ValueError):
            ref.w4_g128_a8_dot([bytes(64)], [128])

    def test_group_requantization_rounding_and_saturation(self):
        # Each group is rounded BEFORE summation: two half-even zeros stay zero.
        self.assertEqual(ref.requantize_groups([[1, 1]], [[ref.Dyadic(1, -1)] * 2],
                                               ref.Dyadic(1, 0), 0), [0])
        rng = np.random.default_rng(104)
        partials = rng.integers(-131072, 131073, (5, 4)).tolist()
        scales = [[ref.binary16_scale(int(h)) for h in row] for row in
                  rng.integers(0x1000, 0x6000, (5, 4)).tolist()]
        activation = ref.Dyadic(13, -11)
        stats = ref.Stats()
        actual = ref.requantize_groups(partials, scales, activation, -12, stats=stats)
        expected = [clip_signed(sum(round(Fraction(p) * dyadic_fraction(w) *
                                            dyadic_fraction(activation) * (1 << 12))
                                    for p, w in zip(row, sw)), 16) for row, sw in zip(partials, scales)]
        self.assertEqual(actual, expected)
        self.assertGreater(stats.counts.get("saturation", 0), 0)

    def test_int16_residual_different_scales(self):
        rng = np.random.default_rng(105)
        left, right = rng.integers(-32768, 32768, (2, 300)).tolist()
        stats = ref.Stats()
        actual = ref.residual_int16(left, right, 4, 9, 7, stats)
        expected = [clip_signed(round((Fraction(a, 16) + Fraction(b, 512)) * 128), 16)
                    for a, b in zip(left, right)]
        self.assertEqual(actual, expected)
        self.assertGreater(stats.counts.get("saturation", 0), 0)
        self.assertEqual(ref.residual_int16([32767], [-32767], 0, 0, 0), [0])

    def test_bfp_exponent_growth_underflow_and_overflow(self):
        stats = ref.Stats()
        block = ref.pack_bfp([1 << 20, 1, -(1 << 20)], -10, 16, stats)
        self.assertEqual(block.exponent, -4)
        self.assertEqual(block.values, (16384, 0, -16384))
        self.assertGreater(stats.counts.get("underflow", 0), 0)
        self.assertEqual(ref.pack_bfp([-32768], 0).exponent, 0)
        self.assertEqual(ref.pack_bfp([32768], 0).exponent, 1)
        left, right = ref.BFP((100, -100), -8), ref.BFP((50, 25), -6)
        result = ref.residual_bfp(left, right)
        self.assertEqual(result, ref.BFP((300, 0), -8))
        ref.pack_bfp([1 << 200], 127, 32, stats)
        self.assertGreater(stats.counts.get("saturation", 0), 0)

    def test_rms_epsilon_and_one_plus_weight(self):
        rng = np.random.default_rng(106)
        for centered in (False, True):
            x = np.rint(rng.normal(0, 0.5, 128) * 4096).astype(np.int64)
            w = np.rint(rng.normal(0, 0.1, 128) * 16384).astype(np.int64)
            xf, wf = x.astype(np.float64) / 4096, w.astype(np.float64) / 16384
            expected = xf / np.sqrt(np.mean(xf * xf) + 1e-6) * (wf + int(centered))
            actual = ref.rms_norm(x.tolist(), w.tolist(), 12, 14, 20, centered, output_bits=32)
            self.assertLessEqual(np.max(np.abs(np.asarray(actual) - np.rint(expected * (1 << 20)))), 1)
        self.assertEqual(ref.rms_norm([0, 0], [0, 0], 20, 14, 20), [0, 0])
        self.assertEqual(ref.rms_norm([100], [0], 10, 14, 20, False, output_bits=32), [0])
        tiny = ref.rms_norm([1], [0], 20, 14, 20, True, output_bits=32)[0]
        self.assertAlmostEqual(tiny / (1 << 20), (2 ** -20) / np.sqrt(2 ** -40 + 1e-6), delta=2 ** -20)
        stats = ref.Stats()
        ref.rms_norm([32767, 0], [32767, 0], 0, 0, 15, stats=stats)
        self.assertGreater(stats.counts.get("saturation", 0), 0)

    def test_a8_quantization_integer_oracle(self):
        values = [-1000000, -257, -128, -1, 0, 1, 128, 257, 1000000]
        scale = ref.Dyadic(1, -12)
        stats = ref.Stats()
        actual = ref.quantize_a8(values, 20, scale, stats)
        self.assertEqual(actual, [clip_signed(round(Fraction(v, 256)), 8) for v in values])
        self.assertEqual(stats.counts["saturation"], 2)
        self.assertGreater(stats.counts["underflow"], 0)

    def test_integer_case_runner_reports_and_rejects_float_shell(self):
        case = {"schema": "agent-integer-case-v1", "kernel": "quantize_a8", "parameters": {
            "values": [1000000, 1], "input_frac": 20, "scale": {"mantissa": 1, "exponent": -12}}}
        report = ref.run_integer_case(case)
        self.assertEqual(report["output"], [127, 0])
        self.assertEqual(report["statistics"]["counts"]["saturation"], 1)
        self.assertEqual(report["statistics"]["counts"]["underflow"], 1)
        case["parameters"]["values"] = [1.0]
        with self.assertRaises(ValueError):
            ref.run_integer_case(case)
        with self.assertRaises(ValueError):
            ref.run_integer_case({"schema": "agent-integer-case-v1", "kernel": "unknown", "parameters": {}})

    def test_execution_kernels_contain_no_float_constants(self):
        names = ("rne_div", "saturate", "shift_rne", "w4_g128_a8_dot", "requantize_groups",
                 "rms_norm", "_normalizer", "inv_sqrt_ratio_q", "softmax_lut", "silu_integer",
                 "causal_conv_step", "_normalized_qk", "_delta_update", "delta_int32_step", "delta_bfp_step")
        for name in names:
            tree = ast.parse(inspect.getsource(getattr(ref, name)))
            self.assertFalse(any(isinstance(node, ast.Constant) and type(node.value) is float
                                 for node in ast.walk(tree)), name)


class LUTAndStateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lut = ref.generate_exp_lut()

    def test_lut_identity_domain_and_offline_oracle(self):
        expected = np.rint(np.exp(-np.arange(4097, dtype=np.float64) / 256) * (1 << 30))
        self.assertLessEqual(np.max(np.abs(np.asarray(self.lut.entries) - expected)), 1)
        artifact = self.lut.artifact()
        self.assertEqual(ref.load_exp_lut(artifact), self.lut)
        corrupted = deepcopy(artifact)
        corrupted["entries"][20] -= 1
        with self.assertRaises(ValueError):
            ref.load_exp_lut(corrupted)
        corrupted = deepcopy(artifact)
        corrupted["version"] = "unknown-version"
        fields = dict(corrupted)
        fields.pop("sha256")
        corrupted["sha256"] = ref.digest_json(fields)
        with self.assertRaises(ValueError):
            ref.load_exp_lut(corrupted)

    def test_global_softmax_across_tiles_translation_and_tails(self):
        rng = np.random.default_rng(107)
        scores = rng.integers(-2000, 2000, 97).tolist()
        actual = np.asarray(ref.softmax_lut(scores, self.lut), dtype=np.float64) / 32768
        shifted = np.asarray(scores, dtype=np.float64) / 256
        expected = np.exp(shifted - max(shifted))
        expected /= expected.sum()
        self.assertLessEqual(np.max(np.abs(actual - expected)), 2 / 32768)
        self.assertEqual(ref.softmax_lut(scores, self.lut),
                         ref.softmax_lut([v + 12345 for v in scores], self.lut))
        # A tile-wise softmax would produce roughly three times the total mass.
        self.assertAlmostEqual(float(actual.sum()), 1.0, delta=97 / 65536)
        stats = ref.Stats()
        self.assertEqual(ref.softmax_lut([0, -4097], self.lut, stats=stats), [32768, 0])
        self.assertEqual(stats.counts["domain_underflow"], 1)
        self.assertEqual(ref.softmax_lut([-(1 << 31), (1 << 31) - 1], self.lut), [0, 32768])

    def test_integer_silu_oracle(self):
        for value in range(-4096, 4097, 29):
            x = value / 256
            expected = x / (1 + np.exp(-x))
            actual = ref.silu_integer(value, 8, 12, self.lut, output_bits=32)
            self.assertAlmostEqual(actual / 4096, expected, delta=1 / 4096)

    def test_causal_convolution_impulse_and_raw_history(self):
        history = [[0, 0, 0]]
        result = []
        for value in [1, 0, 0, 0, 0]:
            history, output = ref.causal_conv_step(history, [value], [[1, 2, 3, 4]], 0, 0, 0)
            result += output
        self.assertEqual(result, [4, 3, 2, 1, 0])
        rng = np.random.default_rng(108)
        history = rng.integers(-200, 200, (4, 3)).tolist()
        taps = rng.integers(-100, 100, (4, 4)).tolist()
        sample = rng.integers(-200, 200, 4).tolist()
        original = deepcopy(history)
        new_state, actual = ref.causal_conv_step(history, sample, taps, 8, 8, 12,
                                                activation_lut=self.lut)
        self.assertEqual(history, original)
        self.assertEqual(new_state, [row[1:] + [x] for row, x in zip(history, sample)])
        # Oracle includes the documented intermediate Q12 conv quantization.
        pre = np.rint(np.sum(np.column_stack([history, sample]) * np.asarray(taps), axis=1) / 16) / 4096
        expected = pre / (1 + np.exp(-pre))
        # SiLU LUT rounds its exponent input to Q8: bounded approximation error.
        self.assertLess(np.max(np.abs(np.asarray(actual) / 4096 - expected)), 0.002)

    def test_deltanet_int32_multistep_against_float_equations(self):
        rng = np.random.default_rng(109)
        dim, frac = 8, 20
        state = np.rint(rng.normal(0, 0.1, (dim, dim)) * (1 << frac)).astype(np.int64).tolist()
        oracle = np.asarray(state, dtype=np.float64) / (1 << frac)
        decay, beta = round(0.97 * (1 << 30)), round(0.31 * (1 << 30))
        stats = ref.Stats()
        for _ in range(20):
            q, k, v = [np.rint(rng.normal(0, 0.3, dim) * (1 << frac)).astype(np.int64).tolist() for _ in range(3)]
            previous = deepcopy(state)
            actual_state, actual_output = ref.delta_int32_step(state, q, k, v, decay, beta, stats=stats)
            self.assertEqual(state, previous)
            oracle, expected_output = delta_oracle(oracle, np.asarray(q) / (1 << frac),
                np.asarray(k) / (1 << frac), np.asarray(v) / (1 << frac), decay / (1 << 30), beta / (1 << 30))
            self.assertLess(np.max(np.abs(np.asarray(actual_state) / (1 << frac) - oracle)), 3e-5)
            self.assertLess(np.max(np.abs(np.asarray(actual_output) / (1 << frac) - expected_output)), 3e-5)
            state = actual_state
        self.assertEqual(stats.counts.get("saturation", 0), 0)
        self.assertGreater(stats.peak_magnitude_bits["delta.state.updated"], 0)

    def test_deltanet_zero_and_saturation_gates(self):
        state, output = ref.delta_int32_step([[0, 0], [0, 0]], [0, 0], [0, 0], [100, -100],
                                            1 << 30, 1 << 30)
        self.assertEqual(state, [[0, 0], [0, 0]])
        self.assertEqual(output, [0, 0])
        stats = ref.Stats()
        huge = [[(1 << 31) - 1] * 2 for _ in range(2)]
        state, output = ref.delta_int32_step(huge, [1 << 20] * 2, [1 << 20] * 2,
                                            [0, 0], 1 << 30, 0, stats=stats)
        self.assertEqual(state, huge)
        self.assertGreater(stats.counts.get("saturation", 0), 0)
        with self.assertRaises(ValueError):
            ref.delta_int32_step(huge, [1, 1], [1, 1], [0, 0], (1 << 30) + 1, 0)

    def test_deltanet_bfp32_large_state_oracle(self):
        rng = np.random.default_rng(110)
        dim, frac = 4, 20
        mantissas = rng.integers(-(1 << 28), 1 << 28, dim * dim).tolist()
        state = ref.BFP(tuple(mantissas), -10, 32)
        q, k, v = [np.rint(rng.normal(0, 0.3, dim) * (1 << frac)).astype(np.int64).tolist() for _ in range(3)]
        decay, beta = round(0.99 * (1 << 30)), round(0.2 * (1 << 30))
        actual_state, output = ref.delta_bfp_step(state, q, k, v, decay, beta)
        expected_state, expected_output = delta_oracle(np.asarray(mantissas).reshape(dim, dim) / 1024,
            np.asarray(q) / (1 << frac), np.asarray(k) / (1 << frac), np.asarray(v) / (1 << frac),
            decay / (1 << 30), beta / (1 << 30))
        decoded = np.asarray(actual_state.values).reshape(dim, dim) * (2.0 ** actual_state.exponent)
        decoded_output = np.asarray(output.values) * (2.0 ** output.exponent)
        self.assertLess(np.max(np.abs(decoded - expected_state)), 4 * 2.0 ** actual_state.exponent)
        self.assertLess(np.max(np.abs(decoded_output - expected_output)), 4 * 2.0 ** output.exponent)


class CalibrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.cal = next(f"synthetic-{i}" for i in range(100) if ref.split_for_sample(f"synthetic-{i}") == "calibration")
        cls.held = next(f"synthetic-{i}" for i in range(100) if ref.split_for_sample(f"synthetic-{i}") == "heldout")

    def capture(self, held_value=Decimal("0.5")):
        return {"schema": "agent-raw-numeric-trace-v1", "source": {
                    "dataset_role": "calibration_candidates", "capture_kind": "raw_model_inputs_and_state",
                    "model_sha256": "1" * 64, "source_sha256": "2" * 64},
                "records": [{"sample_id": sample, "token": 0, "layer": 0, "ordinal": 0,
                             "tag": "linear.delta.decay", "shape": [1], "values": [value]}
                            for sample, value in ((self.cal, Decimal("0.5")), (self.held, held_value))]}

    def test_frozen_sequence_split_no_heldout_scale_leakage(self):
        baseline = ref.calibrate_trace(self.capture())
        extreme = ref.calibrate_trace(self.capture(Decimal("1000000")))
        key = "0:linear.delta.decay"
        self.assertEqual(baseline["groups"][key]["scale"], extreme["groups"][key]["scale"])
        self.assertEqual(baseline["split_manifest_sha256"], extreme["split_manifest_sha256"])
        self.assertEqual(extreme["groups"][key]["heldout"]["counts"]["saturation"], 1)
        self.assertEqual(extreme["status"], "INCOMPLETE_RAW_COVERAGE")
        self.assertIn("norm.attn.input", extreme["missing_raw_tags"]["calibration"])
        self.assertNotIn("linear.delta.decay", extreme["missing_raw_tags"]["calibration"])

    def test_projection_and_evaluation_rejected_for_scale_selection(self):
        document = self.capture()
        document["source"]["dataset_role"] = "independent_evaluation_frozen"
        with self.assertRaises(ValueError):
            ref.calibrate_trace(document)
        document = self.capture()
        document["source"]["capture_kind"] = "projection_ranges"
        with self.assertRaises(ValueError):
            ref.calibrate_trace(document)
        document = self.capture()
        for record in document["records"]:
            record["tag"] = "linear.gated"
        result = ref.calibrate_trace(document)
        self.assertEqual(result["groups"], {})
        self.assertIn("linear.delta.decay", result["missing_raw_tags"]["calibration"])
        document = self.capture()
        document["records"][0]["tag"] = "norm.attn.input"
        with self.assertRaisesRegex(ValueError, "projection"):
            ref.calibrate_trace(document)

    def test_trace_identity_shape_and_finite_validation(self):
        for mutation in ("duplicate", "shape", "nan", "identity"):
            document = self.capture()
            if mutation == "duplicate":
                document["records"].append(deepcopy(document["records"][0]))
            elif mutation == "shape":
                document["records"][0]["shape"] = [2]
            elif mutation == "nan":
                document["records"][0]["values"] = [Decimal("NaN")]
            else:
                document["source"]["source_sha256"] = "HEAD-only"
            with self.assertRaises(ValueError, msg=mutation):
                ref.calibrate_trace(document)

    def test_cli_metadata_lut_calibration_and_artifact_identity(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            path = Path(folder)
            self.assertEqual(ref.main(["metadata", "--out", str(path / "formats.json")]), 0)
            metadata = json.loads((path / "formats.json").read_text())
            self.assertEqual(metadata["status"], "EXPERIMENTAL_CANDIDATE_NOT_APPROVED")
            self.assertEqual(set(metadata["calibration"]["raw_value_counts"]), ref.RAW_TAGS)
            self.assertEqual(ref.main(["lut", "--out", str(path / "lut.json")]), 0)
            lut = ref.load_exp_lut(json.loads((path / "lut.json").read_text()))
            self.assertEqual(lut.version, ref.LUT_VERSION)
            integer_case = path / "integer_case.json"
            integer_case.write_text(json.dumps({"schema": "agent-integer-case-v1", "kernel": "softmax_lut",
                "parameters": {"scores": [0, -4097]}}), encoding="utf-8")
            self.assertEqual(ref.main(["run", "--case", str(integer_case), "--lut", str(path / "lut.json"),
                                       "--out", str(path / "execution.json")]), 0)
            execution = json.loads((path / "execution.json").read_text())
            self.assertEqual(execution["output"], [32768, 0])
            self.assertEqual(execution["statistics"]["counts"]["domain_underflow"], 1)
            self.assertEqual(len(execution["integer_case_sha256"]), 64)
            document = self.capture()
            document["records"][0]["values"] = [0]  # JSON integer fixture; no float kernel wrapper.
            document["records"][1]["values"] = [1]
            trace = path / "synthetic_raw_capture.json"
            trace.write_text(json.dumps(document), encoding="utf-8")
            self.assertEqual(ref.main(["calibrate", "--trace", str(trace), "--out", str(path / "report.json")]), 2)
            result = json.loads((path / "report.json").read_text())
            self.assertEqual(len(result["capture_file_sha256"]), 64)
            self.assertEqual(result["status"], "INCOMPLETE_RAW_COVERAGE")


class BinaryCalibrationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)

    def fixture(self, folder, heldout_value=1000000, all_tags=False):
        root = Path(folder)
        paths = {key: root / name for key, name in (
            ("index", "trace_index.jsonl"), ("values", "trace_values.f32le.bin"),
            ("split", "SPLIT_FROZEN.json"), ("source", "trace_source.json"))}
        # Deliberately reverse the generic SHA split: external frozen split wins.
        cal = next(f"binary-{i}" for i in range(100) if ref.split_for_sample(f"binary-{i}") == "heldout")
        held = next(f"binary-{i}" for i in range(100) if ref.split_for_sample(f"binary-{i}") == "calibration")
        samples = [{"sample_id": sample, "split": split, "token_count": 1,
                    "input_sha256": str(i) * 64, "token_ids_sha256": str(i + 2) * 64,
                    "format": "u32le(count)+u32le(ids)"}
                   for i, (sample, split) in enumerate(((cal, "calibration_seed"),
                     (held, "heldout_reporting_only")), 1)]
        manifest = {"format": "agent-calibration-seeds-v1", "samples": samples,
                    "freeze_sha256": ref.digest_json(samples)}
        paths["split"].write_text(json.dumps(manifest), encoding="utf-8")
        paths["source"].write_text(json.dumps({"dataset_role": "calibration_candidates",
            "capture_kind": "raw_model_inputs_and_state", "model_sha256": "a" * 64,
            "source_sha256": "b" * 64, "samples": [{key: sample[key] for key in
                ("sample_id", "input_sha256", "token_ids_sha256")} for sample in samples]}), encoding="utf-8")
        records, chunks, offset = [], [], 0
        for sample in samples:
            tags = sorted(ref.RAW_TAGS) if all_tags else ["linear.delta.decay"]
            for ordinal, tag in enumerate(tags):
                value = 0.5 if sample["split"] == "calibration_seed" else heldout_value
                count = ref.RAW_COUNTS[tag]
                raw = np.full(count, value, dtype="<f4").tobytes()
                records.append({"sample_id": sample["sample_id"], "token": 0, "layer": 0,
                    "head_seq": ordinal, "tag": tag, "shape": [count], "count": count,
                    "offset_bytes": offset, "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()})
                chunks.append(raw)
                offset += len(raw)
        paths["values"].write_bytes(b"".join(chunks))
        paths["index"].write_text("".join(json.dumps(record) + "\n" for record in records), encoding="utf-8")
        paths["expected"] = hashlib.sha256(paths["split"].read_bytes()).hexdigest()
        return paths, records

    def run_capture(self, paths):
        return ref.calibrate_binary(paths["index"], paths["values"], paths["split"],
                                    paths["source"], paths["expected"])

    def test_vectorized_f32_integer_statistics_matches_exact_scalar(self):
        rng = np.random.default_rng(111)
        words = rng.integers(0, 1 << 32, 5000, dtype=np.uint32)
        words = words[((words >> 23) & 255) != 255]
        edges = [0, 0x80000000, 1, 0x80000001, 0x7f7fffff, 0xff7fffff,
                 0x3f000000, 0xbf000000, 0x3fc00000, 0xbfc00000, 0x47000000, 0xc7000000]
        words = np.concatenate([words, np.array(edges, dtype=np.uint32)]).astype("<u4")
        for exponent in (-128, -30, 0, 100, 127):
            stats = ref.Stats()
            for word in words.tolist():
                ref._quantize_fraction(ref._f32_bits_fraction(int(word)), exponent, stats, "scalar")
            actual = ref._f32_quantization_counts(words.tobytes(), exponent)
            for kind in ("saturation", "underflow", "inexact"):
                self.assertEqual(actual[kind], stats.counts.get(kind, 0), (exponent, kind))
            self.assertEqual(actual["negative_zero"], 1)

    def test_binary_external_frozen_split_and_heldout_no_scale_leakage(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            paths, records = self.fixture(folder, 0.5)
            baseline = self.run_capture(paths)
            paths, records = self.fixture(folder, 1000000)
            extreme = self.run_capture(paths)
            key = "0:linear.delta.decay"
            self.assertEqual(baseline["groups"][key]["scale"], extreme["groups"][key]["scale"])
            self.assertEqual(extreme["groups"][key]["heldout"]["counts"]["saturation"], 1)
            self.assertEqual(extreme["binary_identity"]["sha256"], hashlib.sha256(paths["values"].read_bytes()).hexdigest())
            self.assertEqual(extreme["frozen_split"]["manifest_file_sha256"], paths["expected"])
            self.assertEqual(extreme["status"], "INCOMPLETE_RAW_COVERAGE")

    def test_binary_full_required_raw_tags_is_range_evidence_only(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            paths, records = self.fixture(folder, 0.75, all_tags=True)
            report = self.run_capture(paths)
            self.assertEqual(report["status"], "CANDIDATE_RAW_RANGE_ONLY")
            self.assertEqual(report["binary_identity"]["records"], 2 * len(ref.RAW_TAGS))
            self.assertEqual(set(report["groups"]), {"0:" + tag for tag in ref.RAW_TAGS})
            self.assertFalse(report["missing_samples"])
            self.assertIn("not full layer/head", report["coverage_limit"])

    def test_binary_corruption_alias_shape_identity_rejected(self):
        for mutation in ("record_hash", "overlap", "tail", "shape", "nan", "sample_identity",
                         "frozen_sha", "freeze_digest", "logical_alias", "count", "position"):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory(dir=OUT) as folder:
                paths, records = self.fixture(folder)
                if mutation == "record_hash":
                    records[0]["sha256"] = "0" * 64
                elif mutation == "overlap":
                    records[1]["offset_bytes"] = 0
                elif mutation == "tail":
                    paths["values"].write_bytes(paths["values"].read_bytes() + b"tail")
                elif mutation == "shape":
                    records[0]["tag"] = "norm.attn.input"
                elif mutation == "nan":
                    raw = np.array([0x7fc00000], dtype="<u4").tobytes()
                    paths["values"].write_bytes(raw + paths["values"].read_bytes()[4:])
                    records[0]["sha256"] = hashlib.sha256(raw).hexdigest()
                elif mutation == "sample_identity":
                    source = json.loads(paths["source"].read_text())
                    source["samples"][0]["input_sha256"] = "f" * 64
                    paths["source"].write_text(json.dumps(source))
                elif mutation == "frozen_sha":
                    paths["expected"] = "0" * 64
                elif mutation == "freeze_digest":
                    manifest = json.loads(paths["split"].read_text())
                    manifest["freeze_sha256"] = "0" * 64
                    paths["split"].write_text(json.dumps(manifest))
                    paths["expected"] = hashlib.sha256(paths["split"].read_bytes()).hexdigest()
                elif mutation == "logical_alias":
                    duplicate = dict(records[0])
                    duplicate["offset_bytes"] = 8
                    records.append(duplicate)
                    paths["values"].write_bytes(paths["values"].read_bytes() + paths["values"].read_bytes()[:4])
                elif mutation == "count":
                    records[0]["count"] = 2
                elif mutation == "position":
                    records[0]["position"] = 1
                paths["index"].write_text("".join(json.dumps(record) + "\n" for record in records))
                with self.assertRaises(ValueError):
                    self.run_capture(paths)

    def test_binary_cli_and_output_alias_guard(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            paths, records = self.fixture(folder)
            args = ["calibrate", "--index", str(paths["index"]), "--values", str(paths["values"]),
                    "--split-manifest", str(paths["split"]), "--split-sha256", paths["expected"],
                    "--source", str(paths["source"]), "--out", str(Path(folder) / "result.json")]
            self.assertEqual(ref.main(args), 2)
            args[-1] = str(paths["values"])
            with self.assertRaisesRegex(ValueError, "aliases"):
                ref.main(args)
            with self.assertRaisesRegex(ValueError, "alias"):
                ref.calibrate_binary(paths["index"], paths["index"], paths["split"], paths["source"], paths["expected"])


if __name__ == "__main__":
    unittest.main()
