"""Synthetic complete 24-layer candidate graph; no real-model quality claim."""
import ast
from copy import deepcopy
from fractions import Fraction
import inspect
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import agent_integer_model as graph
from agent_integer_reference import BFP, Stats, generate_exp_lut, load_exp_lut, digest_json
from agent_integer_graph_ops import generate_rope_table

OUT = Path(os.environ.get("AGENT_FULL_GRAPH_TEST_OUT",
           r"E:\FPGA_RV32IM_Work\deepseek_agent_accel_20261102\integer_reference\full_graph\test_tmp"))


class SyntheticWeights:
    """Real complete layer layout at low dimensions, deterministic signed W4."""
    def __init__(self, geometry, zero=False):
        self.identity = {"kind": "SYNTHETIC_ONLY", "seed": 2026, "zero": zero}
        self.verified = True
        self.matrix_shapes, self.vector_lengths = graph.requirements(geometry)
        self.embedding_shape = (geometry.vocab, geometry.hidden)
        rng = np.random.default_rng(2026)
        self.weights = {name: np.zeros(shape, dtype=np.int32) if zero else
                        rng.integers(-2, 3, shape, dtype=np.int32) for name, shape in self.matrix_shapes.items()}
        self.vectors = {}
        for name, length in self.vector_lengths.items():
            if name.endswith("ssm_norm.weight"):
                self.vectors[name] = BFP((1,) * length, 0, 32)
            elif name.endswith("ssm_conv1d.weight"):
                self.vectors[name] = BFP((1,) * length, -2, 32)
            elif name.endswith("ssm_dt.bias"):
                self.vectors[name] = BFP((-1,) * length, 0, 32)
            else:
                self.vectors[name] = BFP((0,) * length, 0, 32)
    def vector(self, name, stats): return self.vectors[name]
    def rows(self, name, first, count): return self.weights[name][first:first + count]
    def row_scales(self, name, row): return [(1024, -18)] * ((self.matrix_shapes[name][1] + 127) // 128)
    def embedding_row(self, token):
        codes = [0x3e80, 0xbf00, 0x3f00, 0x3f40]
        return b"".join(struct.pack("<H", codes[(token + i) % len(codes)]) for i in range(self.embedding_shape[1]))


class FullGraphTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        cls.geometry = graph.Geometry(hidden=8, ffn=16, delta_heads=2, delta_dim=4,
                                      query_heads=2, kv_heads=1, attention_dim=64, vocab=11)
        cls.nonlinear_artifact = graph.Nonlinear.generate()
        cls.nonlinear = graph.Nonlinear(cls.nonlinear_artifact)
        cls.rope = generate_rope_table(128)
        cls.softmax = load_exp_lut(generate_exp_lut().artifact())

    def model(self, zero=False):
        provider = SyntheticWeights(self.geometry, zero)
        params = graph.candidate_parameters(provider, self.geometry)
        return graph.IntegerModel(provider, self.geometry, 128, params,
                                  self.nonlinear, self.rope, self.softmax)

    def test_complete_24_layers_nonzero_state_and_selected_head(self):
        model = self.model()
        result = model.token_step(1, [0, 1, 2])
        self.assertEqual(result["layers_executed"], 24)
        self.assertEqual(len(model.conv), 18)
        self.assertEqual(len(model.delta), 18)
        self.assertEqual(len(model.kv), 6)
        self.assertEqual(model.tokens, [1])
        self.assertEqual(len(result["selected_logits"]), 3)
        self.assertIsNone(result["greedy"])
        self.assertTrue(all(len(cache) == 1 for cache in model.kv.values()))
        self.assertTrue(any(any(x for x in state.values) for heads in model.delta.values() for state in heads))
        self.assertTrue(any(any(x for x in state.values) for channels in model.conv.values() for state in channels))
        stages = model.stats.peak_magnitude_bits
        for i in range(24):
            self.assertTrue(any(name.startswith(f"layer.{i}.") for name in stages), i)
        self.assertEqual(model.plan["w4_matrices"], 186)
        scales = model.stats.report()["candidate_tensor_scales"]
        self.assertIn("layer.0.blk.0.attn_qkv.weight.activation.a8", scales)
        self.assertIn("layer.0.delta.head.0.state", scales)
        self.assertIn("layer.3.kv.head.0.key", scales)
        self.assertEqual(len(model.identity["implementation_sha256"]), 64)
        self.assertIn("NOT_QUALITY_APPROVED", result["status"])

    def test_full_prefix_restore_next_token_identical_and_greedy(self):
        first = self.model()
        first.prefill([1, 2])
        snapshot = first.snapshot()
        continued = first.token_step(3, [0, 2, 4])
        second = self.model()
        second.restore(snapshot)
        self.assertEqual(second.tokens, [1, 2])
        restored = second.token_step(3, [0, 2, 4])
        self.assertEqual(continued, restored)
        self.assertEqual(first.snapshot(), second.snapshot())
        greedy_model = self.model()
        greedy = greedy_model.token_step(1, greedy=True)
        selected_model = self.model()
        selected = selected_model.token_step(1, list(range(self.geometry.vocab)))
        best = max(selected["selected_logits"], key=lambda item: item["value"])["row"]
        self.assertEqual(greedy["greedy"], best)
        self.assertIsNone(greedy["selected_logits"])

    def test_zero_projection_graph_reduces_to_tied_embedding_norm_oracle(self):
        model = self.model(True)
        result = model.token_step(0, [0, 1, 2])
        embedding = np.array([0.25, -0.5, 0.5, 0.75] * 2)
        # Float is an independent TEST oracle only; complete zero-projection graph is identity residual.
        normal = embedding / np.sqrt(np.mean(embedding * embedding) + 1e-6)
        for item in result["selected_logits"]:
            codes = [0.25, -0.5, 0.5, 0.75]
            row = np.array([codes[(item["row"] + i) % 4] for i in range(8)])
            expected = np.dot(row, normal)
            self.assertAlmostEqual(item["value"] * (2.0 ** result["logit_exponent"]), expected, delta=2e-5)
        self.assertTrue(all(not any(state.values) for heads in model.delta.values() for state in heads))

    def test_integer_matrix_group_scaling_exact_fraction_oracle(self):
        model = self.model()
        name = "blk.0.ffn_gate.weight"
        value = BFP((100, -110, 120, -3, 5, 9, -22, 66), -8, 32)
        actual = model.matrix(name, value, Stats())
        weights = model.provider.weights[name]
        for row, mantissa in enumerate(actual.values):
            exact = sum(Fraction(int(w) * a, 1 << 16) for w, a in zip(weights[row], value.values))
            represented = Fraction(mantissa) * (Fraction(1 << actual.exponent) if actual.exponent >= 0 else Fraction(1, 1 << -actual.exponent))
            self.assertEqual(exact, represented)

    def test_checkpoint_corruption_missing_state_and_version_fail_atomically(self):
        model = self.model(); model.prefill([1, 2]); snapshot = model.snapshot()
        for mutation in ("sha", "identity", "conv", "delta", "kv", "prefix"):
            with self.subTest(mutation=mutation):
                broken = deepcopy(snapshot)
                if mutation == "sha": broken["sha256"] = "0" * 64
                elif mutation == "identity": broken["payload"]["identity"]["context"] = 1024
                elif mutation == "conv": broken["payload"]["conv"]["0"].pop()
                elif mutation == "delta": broken["payload"]["delta"]["0"][0]["values"].pop()
                elif mutation == "kv": broken["payload"]["kv"]["3"].pop()
                else: broken["payload"]["tokens"].append(99999)
                if mutation != "sha": broken["sha256"] = digest_json(broken["payload"])
                previous = model.snapshot()
                with self.assertRaises(ValueError): model.restore(broken)
                self.assertEqual(model.snapshot(), previous)
        snapshot["payload"]["identity"]["context"] = 0
        self.assertEqual(model.identity["context"], 128)

    def test_unsupported_gate_partial_state_requires_reset_and_no_context_truncation(self):
        model = self.model()
        model.provider.vectors["blk.0.ssm_a"] = BFP((1 << 20,) * 2, 0, 32)
        with self.assertRaises(graph.Unsupported): model.token_step(1)
        self.assertTrue(model.needs_reset)
        self.assertEqual(model.tokens, [])
        with self.assertRaises(graph.Unsupported): model.token_step(2)
        with self.assertRaises(graph.Unsupported): model.snapshot()
        model.provider.vectors["blk.0.ssm_a"] = BFP((0,) * 2, 0, 32)
        model.reset(); model.token_step(1)
        self.assertEqual(model.tokens, [1])
        with self.assertRaises(graph.Unsupported): model.prefill([1] * 128)
        self.assertEqual(model.tokens, [1])
        model.provider.verified = False
        with self.assertRaises(graph.Unsupported): model.token_step(1)

    def test_plan_shape_parameters_lut_and_payload_identity_fail_closed(self):
        model = self.model()
        provider = SyntheticWeights(self.geometry)
        provider.matrix_shapes.pop("blk.23.attn_q.weight")
        with self.assertRaises(graph.Unsupported): graph.graph_plan(provider, self.geometry, 128)
        params = graph.candidate_parameters(model.provider, self.geometry)
        params["layers"].pop()
        with self.assertRaises(ValueError):
            graph.IntegerModel(model.provider, self.geometry, 128, params, self.nonlinear, self.rope, self.softmax)
        params = graph.candidate_parameters(model.provider, self.geometry)
        valid = deepcopy(params)
        isolated = graph.IntegerModel(model.provider, self.geometry, 128, params, self.nonlinear, self.rope, self.softmax)
        params["layers"][0]["qkv_frac"] = 0
        self.assertEqual(isolated.parameters, valid)
        with self.assertRaises(graph.Unsupported): graph.graph_plan(model.provider, self.geometry, 64)

    def test_nonlinear_independent_oracle_domains_and_exact_cpu_bits(self):
        stats = Stats()
        for index in (-4096, -256, 0, 256, 4096):
            x = index / 256
            for name, oracle in (("exp", math.exp(x)), ("sigmoid", 1 / (1 + math.exp(-x))),
                                 ("softplus", math.log1p(math.exp(x)))):
                actual = self.nonlinear.scalar(name, index, -8, stats)
                self.assertLessEqual(abs(actual - round(oracle * (1 << 30))), max(2, int(oracle * 0.01)))
        self.assertEqual(self.nonlinear.scalar("exp", -33, 0, stats), 0)
        self.assertEqual(self.nonlinear.scalar("softplus", 21, 0, stats), 21 << 30)
        with self.assertRaises(graph.Unsupported): self.nonlinear.scalar("sigmoid", 33, 0, stats)
        self.assertGreater(stats.counts["domain_underflow"], 0)
        result = graph.exact_words(struct.pack("<III", 0x3f800000, 0xbf000000, 0), "F32", stats, "test")
        self.assertEqual(graph.fixed(result, 8, stats, "test"), [256, -128, 0])
        with self.assertRaises(graph.Unsupported):
            graph.exact_words(struct.pack("<I", 0x7fc00000), "F32", stats, "test")

    def test_fast_pack_and_delta_core_exact_against_existing_kernels(self):
        from agent_integer_reference import pack_bfp, delta_bfp_step
        import random
        rng = random.Random(77)
        for bits in (8, 16, 32):
            for exponent in (-149, -128, -60, 0, 120, 127, 200):
                for width in (1, 20, 60, 100):
                    values = [rng.randrange(-(1 << width), 1 << width) for _ in range(7)]
                    old = pack_bfp(values, exponent, bits)
                    new = graph.fast_pack(values, exponent, bits, Stats(), "test")
                    self.assertEqual(old, new)
        for exponent in (-128, -48, -20, 0):
            state = BFP(tuple(rng.randrange(-(1 << 25), 1 << 25) for _ in range(16)), exponent, 32)
            q, k, v = [[rng.randrange(-1000000, 1000000) for _ in range(4)] for _ in range(3)]
            args = (state, q, k, v, 1000000000, 300000000, 20)
            self.assertEqual(delta_bfp_step(*args), graph.delta_state_step(*args, Stats()))

    def test_v2_a8_fixed24_fraction_oracle_and_v1_compatibility(self):
        from agent_integer_reference import pack_bfp
        rng = np.random.default_rng(818)
        cases = [[0] * 5, [-2147483648, 2147483647, -1, 1, 0], [-127, -64, 0, 64, 127]]
        cases += [rng.integers(-10000000, 10000001, 64).tolist() for _ in range(20)]
        for exponent in (-128, -20, 0, 127):
            for values in cases:
                value = BFP(tuple(values), exponent, 32)
                old = graph.quantize_activation(value, "v1_power2", Stats())
                expected_old = pack_bfp(values, exponent, 8)
                self.assertEqual((old.values, old.mantissa, old.exponent), (expected_old.values, 1, expected_old.exponent))
                new = graph.quantize_activation(value, "v2_max127_dyadic_p24", Stats())
                maximum = max(abs(x) for x in values)
                if maximum:
                    expected_m = round(Fraction(maximum << 24, 127))
                    self.assertEqual(new.mantissa, expected_m)
                    self.assertEqual(new.exponent, exponent - 24)
                    self.assertEqual(list(new.values), [round(Fraction(x << 24, expected_m)) for x in values])
                else: self.assertEqual((new.mantissa, new.exponent, new.values), (1, 0, tuple(values)))
                self.assertTrue(all(-127 <= x <= 127 for x in new.values))
        for value in (BFP((1,), 0, 64), [1.0], [float("nan")], [float("inf")]):
            with self.assertRaises(ValueError): graph.quantize_activation(value, "v2_max127_dyadic_p24", Stats())
        with self.assertRaises(ValueError): graph.quantize_activation(BFP((1,), 0, 32), "unknown", Stats())

    def test_v2_full24_prefix_modes_and_actual_codes(self):
        provider = SyntheticWeights(self.geometry)
        params = graph.candidate_parameters(provider, self.geometry, "v2_max127_dyadic_p24")
        model = graph.IntegerModel(provider, self.geometry, 128, params, self.nonlinear, self.rope, self.softmax)
        first = model.token_step(1, [0, 1])
        self.assertEqual(model.numeric_version, graph.VERSION_V2)
        self.assertEqual(first["layers_executed"], 24)
        snap = model.snapshot()
        later = model.token_step(2, [0, 1])
        restored = graph.IntegerModel(provider, self.geometry, 128, params, self.nonlinear, self.rope, self.softmax)
        restored.restore(snap)
        self.assertEqual(later, restored.token_step(2, [0, 1]))
        entries = model.stats.report()["activation_quantization"]
        self.assertEqual(len(entries), 186)
        item = entries["layer.0.blk.0.attn_qkv.weight.activation.a8"]
        self.assertEqual(item["mode"], "v2_max127_dyadic_p24")
        self.assertEqual(len(item["codes"]), 8)
        self.assertGreater(item["mantissa"], 1)
        self.assertEqual(item["mantissa_storage_bits"], 64)
        legacy = self.model()
        self.assertEqual(legacy.a8_scale_mode, "v1_power2")
        with self.assertRaises(ValueError): legacy.restore(snap)
        for bad in (None, "unknown"):
            broken = deepcopy(params); broken["a8_scale_mode"] = bad
            with self.assertRaises(ValueError):
                graph.IntegerModel(provider, self.geometry, 128, broken, self.nonlinear, self.rope, self.softmax)
        broken = deepcopy(params); broken["activation_scale_precision_bits"] = 16
        with self.assertRaises(ValueError):
            graph.IntegerModel(provider, self.geometry, 128, broken, self.nonlinear, self.rope, self.softmax)

    def test_v2_group_projection_against_C_fp32_reconstruction(self):
        provider = SyntheticWeights(self.geometry)
        params = graph.candidate_parameters(provider, self.geometry, "v2_max127_dyadic_p24")
        model = graph.IntegerModel(provider, self.geometry, 128, params, self.nonlinear, self.rope, self.softmax)
        rng = np.random.default_rng(891)
        name = "blk.0.attn_qkv.weight"
        provider.matrix_shapes[name] = (8, 256)
        provider.weights[name] = rng.integers(-8, 8, (8, 256), dtype=np.int32)
        mantissas = rng.integers(-5000000, 5000001, 256).tolist(); mantissas[0] = -5605000
        value = BFP(tuple(mantissas), -20, 32)
        normal = np.asarray(mantissas, dtype=np.float32) * np.float32(2 ** -20)
        c_sx = np.float32(np.max(np.abs(normal)) / np.float32(127))
        c_codes = np.rint(np.float32(normal / c_sx)).astype(np.int32)
        quant = graph.quantize_activation(value, "v2_max127_dyadic_p24", Stats())
        self.assertEqual(list(quant.values), c_codes.tolist())
        expected = np.zeros(8, dtype=np.float32)
        for group in range(2):
            partial = provider.weights[name][:, group * 128:(group + 1) * 128] @ c_codes[group * 128:(group + 1) * 128]
            expected = np.float32(expected + np.float32(np.float32(partial.astype(np.float32) * np.float32(2 ** -8)) * c_sx))
        output = model.matrix(name, value, Stats())
        actual = np.asarray(output.values, dtype=np.float64) * (2.0 ** output.exponent)
        self.assertLess(np.max(np.abs(actual - expected)), 2e-6)
        for row in range(8):
            dot = sum(Fraction(int(w) * code * 1024 * quant.mantissa) *
                      Fraction(1, 1 << -(quant.exponent - 18))
                      for w, code in zip(provider.weights[name][row], quant.values))
            represented = Fraction(output.values[row], 1 << -output.exponent)
            self.assertLessEqual(abs(represented - dot), Fraction(1, 1 << -output.exponent))

    def test_prior_identity_stat_receipt_rejects_changed_payload(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            root = Path(folder)
            package, cpu, prior, receipt_path = [root / name for name in ("package.bin", "cpu.bin", "prior.json", "receipt.json")]
            package.write_bytes(b"package"); cpu.write_bytes(b"cpu")
            identity = {"synthetic": "known-prior-proof"}
            prior.write_text(json.dumps({"plan": {"payload_identity_verified": True}, "identity": {"sources": identity}}))
            receipt = {"version": "prior-sha256-stat-receipt-v1", "prior_result_path": str(prior),
                "prior_result_sha256": graph.hash_file(prior), "source_identity": identity,
                "payload_stats": {"package": graph.payload_stat(package), "cpu": graph.payload_stat(cpu)}}
            receipt["sha256"] = digest_json(receipt); receipt_path.write_text(json.dumps(receipt))
            graph.verify_payload_receipt(receipt_path, package, cpu, identity)
            cpu.write_bytes(b"changed")
            with self.assertRaises(ValueError): graph.verify_payload_receipt(receipt_path, package, cpu, identity)

    def test_lut_identity_and_cli_input_alias_rejected(self):
        bad = deepcopy(self.nonlinear_artifact)
        bad["sigmoid"][8192] += 1
        with self.assertRaises(ValueError): graph.Nonlinear(bad)
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            path = Path(folder) / "original.bin"
            path.write_bytes(b"original")
            with self.assertRaisesRegex(ValueError, "aliases"):
                graph.main(["plan", "--package", str(path), "--out", str(path)])
            self.assertEqual(path.read_bytes(), b"original")

    def test_online_kernels_do_not_use_float_or_decimal(self):
        for function in (graph.IntegerModel.matrix, graph.IntegerModel.linear, graph.IntegerModel.attention,
                         graph.IntegerModel.token_step, graph.exact_words, graph.Nonlinear.scalar):
            import textwrap
            tree = ast.parse(textwrap.dedent(inspect.getsource(function)))
            self.assertFalse(any(isinstance(node, ast.Constant) and type(node.value) is float for node in ast.walk(tree)))
            self.assertFalse(any(isinstance(node, ast.Call) and isinstance(node.func, ast.Name) and
                node.func.id in ("float", "Decimal") for node in ast.walk(tree)))


if __name__ == "__main__":
    unittest.main()
