#!/usr/bin/env python3
"""Executable 24-layer integer SOFTWARE CANDIDATE; no hardware/quality approval.

Online execution uses integer arithmetic only. Decimal is used solely by the
explicit offline nonlinear LUT generator. Original BF16/F32 words are decoded
as exact dyadics; W4 weights are never copied/re-quantized or used as a golden.
BFP32 state/output and layer-specific fixed conversions remain uncalibrated.
"""
from __future__ import annotations

import argparse
from copy import deepcopy
from dataclasses import asdict, dataclass
from decimal import Decimal, ROUND_HALF_EVEN, localcontext
import hashlib
import json
import mmap
from pathlib import Path
import struct
from typing import Sequence

import numpy as np
from agent_integer_reference import (BFP, Stats, checked_range, integer, pack_bfp,
    rescale, residual_bfp, rms_norm, delta_bfp_step, inv_sqrt_ratio_q, softmax_lut,
    load_exp_lut, digest_json, shift_rne, saturate, rne_div, _normalized_qk, _delta_update)
from agent_integer_graph_ops import bf16_decode, bf16_head_dot_row, load_rope_table, rope_apply_q30
from export_agent_integer_scales import verify_export, SOURCE_SHA256, SOURCE_BYTES

VERSION_V1 = "integer-full-24-layer-candidate-v1"
VERSION_V2 = "integer-full-24-layer-candidate-v2-a8-max127-p24"
VERSION = VERSION_V2
A8_MODES = ("v1_power2", "v2_max127_dyadic_p24")
A8_PRECISION_BITS = 24
NL_VERSION = "integer-nonlinear-q8-q30-domain32-v1"
CPU_BYTES = 509561536
CPU_SHA = "168f02aee67afee5b56edb8683c98f5fa97dae5c48dad720984a4c1ff2e5262d"
CATALOG_SHA = "400952e58eb87cc2da45e46d3adfd66035473771eb584fff993e9f17083f0f3d"
SCALE_SHA = "425111fc2c1d82dedb4026309a80fce8e8c330d0657da65f3123b971cd811a62"


class Unsupported(RuntimeError):
    pass


@dataclass(frozen=True)
class Geometry:
    hidden: int = 1024
    ffn: int = 3584
    delta_heads: int = 16
    delta_dim: int = 128
    query_heads: int = 8
    kv_heads: int = 2
    attention_dim: int = 256
    vocab: int = 248320
    layers: int = 24

    def validate(self):
        if self.layers != 24 or self.attention_dim < 64 or self.query_heads % self.kv_heads:
            raise Unsupported("requires complete 24 layers, rotary64 and integral grouped-query mapping")
        for value in asdict(self).values():
            checked_range(value, 1, 1 << 24)


def requirements(g):
    g.validate()
    matrices, vectors = {}, {"output_norm.weight": g.hidden}
    for i in range(24):
        prefix = f"blk.{i}."
        vectors[prefix + "attn_norm.weight"] = g.hidden
        vectors[prefix + "post_attention_norm.weight"] = g.hidden
        for suffix, shape in (("ffn_gate.weight", (g.ffn, g.hidden)),
                              ("ffn_up.weight", (g.ffn, g.hidden)), ("ffn_down.weight", (g.hidden, g.ffn))):
            matrices[prefix + suffix] = shape
        if i % 4 != 3:
            hd = g.delta_heads * g.delta_dim
            for suffix, shape in (("attn_qkv.weight", (3 * hd, g.hidden)), ("attn_gate.weight", (hd, g.hidden)),
                                  ("ssm_alpha.weight", (g.delta_heads, g.hidden)), ("ssm_beta.weight", (g.delta_heads, g.hidden)),
                                  ("ssm_out.weight", (g.hidden, hd))):
                matrices[prefix + suffix] = shape
            for suffix, length in (("ssm_conv1d.weight", 3 * hd * 4), ("ssm_dt.bias", g.delta_heads),
                                   ("ssm_a", g.delta_heads), ("ssm_norm.weight", g.delta_dim)):
                vectors[prefix + suffix] = length
        else:
            for suffix, shape in (("attn_q.weight", (2 * g.query_heads * g.attention_dim, g.hidden)),
                                  ("attn_k.weight", (g.kv_heads * g.attention_dim, g.hidden)),
                                  ("attn_v.weight", (g.kv_heads * g.attention_dim, g.hidden)),
                                  ("attn_output.weight", (g.hidden, g.query_heads * g.attention_dim))):
                matrices[prefix + suffix] = shape
            vectors[prefix + "attn_q_norm.weight"] = g.attention_dim
            vectors[prefix + "attn_k_norm.weight"] = g.attention_dim
    return matrices, vectors


def graph_plan(provider, g, context):
    if context not in (128, 1024, 2048):
        raise Unsupported("context must be explicit 128/1024/2048; no shrinking/wrapping")
    matrices, vectors = requirements(g)
    failures = []
    for name, shape in matrices.items():
        if provider.matrix_shapes.get(name) != tuple(shape):
            failures.append(f"{name}: expected W4 {shape}")
    for name, length in vectors.items():
        if provider.vector_lengths.get(name) != length:
            failures.append(f"{name}: expected vector {length}")
    if provider.embedding_shape != (g.vocab, g.hidden):
        failures.append("tied BF16 embedding/head shape mismatch")
    if failures:
        raise Unsupported("incomplete graph: " + "; ".join(failures))
    return {"version": VERSION, "status": "STRUCTURALLY_COMPLETE_UNCALIBRATED_CANDIDATE",
            "geometry": asdict(g), "context": context, "layers": 24, "delta_layers": 18,
            "attention_layers": 6, "ffn_layers": 24, "w4_matrices": len(matrices),
            "cpu_vectors": len(vectors), "state_mantissa_bits": 32,
            "source_identity": provider.identity, "payload_identity_verified": provider.verified,
            "payload_verification_method": getattr(provider, "verification", "synthetic_declared_identity"),
            "payload_receipt_sha256": getattr(provider, "receipt_sha256", None),
            "gaps": ["not approved integer quality/hardware format", "no real-model differential logits evidence",
                     "per-layer conversions are explicit candidate parameters, not joint calibration",
                     "full BF16 greedy head is potentially slow; selected head is partial logits only"]}


def candidate_parameters(provider, g, a8_scale_mode=None):
    parameters = {"version": "layer-conversions-v1", "status": "UNCALIBRATED_CANDIDATE",
            "source_identity": provider.identity, "geometry": asdict(g),
            "layers": [{"layer": i, "norm_input_frac": 20, "norm_weight_frac": 20,
                        "norm_output_frac": 20, "qkv_frac": 20} for i in range(24)],
            "head_input_frac": 20, "head_output_exponent": -20}
    if a8_scale_mode is not None:
        if a8_scale_mode not in A8_MODES: raise ValueError("explicit known A8 mode required")
        parameters.update({"version": "layer-conversions-v2", "a8_scale_mode": a8_scale_mode,
                           "activation_scale_precision_bits": A8_PRECISION_BITS})
    return parameters


class ModelStats(Stats):
    def __init__(self):
        super().__init__(); self.scales = {}; self.activations = {}
    def scale(self, stage, exponent, bits=32):
        item = self.scales.setdefault(stage, {"bits": bits, "min_exponent": exponent, "max_exponent": exponent, "last_exponent": exponent, "calls": 0})
        item["min_exponent"] = min(item["min_exponent"], exponent)
        item["max_exponent"] = max(item["max_exponent"], exponent)
        item["last_exponent"] = exponent; item["calls"] += 1
    def activation(self, stage, quantized):
        calls = self.activations.get(stage, {}).get("calls", 0) + 1
        self.scale(stage, quantized.exponent, 8)
        self.scales[stage]["last_mantissa"] = quantized.mantissa
        self.activations[stage] = {"mode": quantized.mode, "mantissa": quantized.mantissa,
            "binary_exponent": quantized.exponent, "mantissa_width_bits": quantized.mantissa.bit_length(),
            "mantissa_storage_bits": 64, "group_product_width": "Python arbitrary-width signed integer",
            "scale_axis": "per-input-tensor, shared across N output rows and K groups",
            "rounding": "signed RNE ties-even", "activation_storage_bits": 8,
            "codes": list(quantized.values), "shape": [len(quantized.values)], "calls": calls,
            "capture_policy": "last call per tensor; no claim of prior prefix-code retention",
            "approval": "CANDIDATE_NOT_APPROVED"}
    def report(self):
        result = super().report()
        result["candidate_tensor_scales"] = deepcopy(self.scales)
        result["activation_quantization"] = deepcopy(self.activations)
        result["approval"] = "NONE; dynamic BFP32/explicit INT32 conversion candidates"
        return result


class Scoped:
    def __init__(self, stats, prefix):
        self.stats, self.prefix = stats, prefix
    def hit(self, kind, stage): self.stats.hit(kind, self.prefix + "." + stage)
    def observe(self, value, stage): self.stats.observe(value, self.prefix + "." + stage)
    def scale(self, stage, exponent, bits=32):
        if hasattr(self.stats, "scale"): self.stats.scale(self.prefix + "." + stage, exponent, bits)
    def activation(self, stage, quantized):
        if hasattr(self.stats, "activation"): self.stats.activation(self.prefix + "." + stage, quantized)


@dataclass(frozen=True)
class Activation:
    values: tuple[int, ...]
    mantissa: int
    exponent: int
    mode: str


def quantize_activation(value, mode, stats):
    if not isinstance(value, BFP) or value.bits != 32: raise ValueError("finite BFP32 activation required")
    if mode == "v1_power2":
        old = fast_pack(value.values, value.exponent, 8, stats, "activation.a8")
        return Activation(old.values, 1, old.exponent, mode)
    if mode != "v2_max127_dyadic_p24": raise ValueError("unknown A8 scale mode")
    maximum = max(abs(integer(x)) for x in value.values)
    if not maximum: return Activation((0,) * len(value.values), 1, 0, mode)
    mantissa = rne_div(maximum << A8_PRECISION_BITS, 127, stats, "activation.scale_rne")
    checked_range(mantissa, 1, (1 << 64) - 1)
    exponent = value.exponent - A8_PRECISION_BITS
    checked_range(exponent, -256, 256)
    codes = []
    for x in value.values:
        code = rne_div(x << A8_PRECISION_BITS, mantissa, stats, "activation.a8")
        if not -127 <= code <= 127: raise ValueError("A8 symmetric range overflow")
        codes.append(code)
    return Activation(tuple(codes), mantissa, exponent, mode)


def fast_pack(values, exponent, bits, stats, stage):
    values = [integer(value) for value in values]
    checked_range(exponent, -256, 256); checked_range(bits, 2, 64)
    if not values: raise ValueError("empty tensor")
    largest = max(abs(value).bit_length() for value in values)
    output_exponent = max(-128, min(127, exponent + max(0, largest - bits)))
    low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    while output_exponent < 127:
        if all(low <= shift_rne(value, exponent - output_exponent) <= high for value in values): break
        output_exponent += 1
    packed = tuple(saturate(shift_rne(value, exponent - output_exponent, stats, stage), bits, stats, stage) for value in values)
    return BFP(packed, output_exponent, bits)


def tensor(values, exponent, stats, stage):
    result = fast_pack(list(values), exponent, 32, stats, stage)
    if hasattr(stats, "scale"): stats.scale(stage, result.exponent)
    return result


def delta_state_step(state, q, k, v, decay, beta, frac, stats):
    qn, kn = _normalized_qk(q, k, frac, stats)
    size = len(q)
    if state.bits != 32 or len(state.values) != size * size: raise ValueError("Delta BFP32 square state")
    exponent = min(state.exponent, -frac)
    flat = [x << (state.exponent - exponent) for x in state.values]
    rows = [flat[row * size:(row + 1) * size] for row in range(size)]
    wide_v = [shift_rne(integer(x), -frac - exponent) for x in v]
    updated, output = _delta_update(rows, qn, kn, wide_v, decay, beta, None, stats)
    return (tensor([x for row in updated for x in row], exponent, stats, "delta.bfp.state"),
            tensor(output, exponent, stats, "delta.bfp.output"))


def fixed(value, frac, stats, stage):
    checked_range(frac, 0, 60)
    if hasattr(stats, "scale"): stats.scale(stage, -frac)
    return [saturate(shift_rne(x, value.exponent + frac, stats, stage), 32, stats, stage) for x in value.values]


def segment(value, start, count):
    if start < 0 or start + count > len(value.values) or count <= 0:
        raise ValueError("tensor slice outside shape")
    return BFP(value.values[start:start + count], value.exponent, 32)


def join(values, stats, stage):
    exponent = min(value.exponent for value in values)
    return tensor([x << (value.exponent - exponent) for value in values for x in value.values], exponent, stats, stage)


def multiply(left, right, stats, stage):
    if len(left.values) != len(right.values):
        raise ValueError("pointwise shape mismatch")
    return tensor([a * b for a, b in zip(left.values, right.values)], left.exponent + right.exponent, stats, stage)


class Nonlinear:
    """Versioned offline LUT. Negative exp/softplus tail has explicit underflow."""
    def __init__(self, artifact):
        data = dict(artifact); expected = data.pop("sha256", None)
        if expected != digest_json(data) or data.get("version") != NL_VERSION or set(data) != {"version", "exp", "sigmoid", "softplus"}:
            raise ValueError("nonlinear LUT identity/version mismatch")
        for name in ("exp", "sigmoid", "softplus"):
            if len(data[name]) != 16385 or any(type(x) is not int or x < 0 for x in data[name]):
                raise ValueError("invalid nonlinear Q8 [-32,32] / Q30 LUT")
        self.data = {key: tuple(value) if isinstance(value, list) else value for key, value in data.items()}
        self.sha256 = expected

    @staticmethod
    def generate():
        data = {"version": NL_VERSION, "exp": [], "sigmoid": [], "softplus": []}
        with localcontext() as ctx:
            ctx.prec = 80
            for index in range(-8192, 8193):
                value = Decimal(index) / Decimal(256)
                exponential = value.exp()
                for name, result in (("exp", exponential), ("sigmoid", exponential / (1 + exponential)),
                                     ("softplus", (1 + exponential).ln())):
                    data[name].append(int((result * (1 << 30)).to_integral_value(rounding=ROUND_HALF_EVEN)))
        data["sha256"] = digest_json(data)
        return data

    def scalar(self, name, value, exponent, stats):
        q8 = shift_rne(integer(value), exponent + 8, stats, "nonlinear.input_q8")
        if name == "softplus" and q8 > 20 * 256:
            return shift_rne(value, exponent + 30, stats, "softplus.linear_gt20")
        if q8 < -8192 and name in ("exp", "softplus"):
            stats.hit("domain_underflow", name); return 0
        if not -8192 <= q8 <= 8192:
            raise Unsupported(f"{name}: Q8 input outside versioned [-32,32] LUT: {q8}")
        result = self.data[name][q8 + 8192]
        if result == 0 and name == "exp":
            stats.hit("underflow", "exp.q30")
        return result

    def sigmoid(self, value, stats):
        return BFP(tuple(self.scalar("sigmoid", x, value.exponent, stats) for x in value.values), -30, 32)

    def silu(self, value, stats):
        return multiply(value, self.sigmoid(value, stats), stats, "silu")


def exact_words(raw, dtype, stats, stage):
    values = []
    if dtype == "BF16":
        for (bits,) in struct.iter_unpack("<H", raw):
            item = bf16_decode(bits, stats); values.append((item.mantissa, item.exponent))
    elif dtype == "F32":
        for (bits,) in struct.iter_unpack("<I", raw):
            exp, mantissa = (bits >> 23) & 255, bits & 0x7fffff
            if exp == 255: raise Unsupported("nonfinite CPU F32 resource")
            if exp: mantissa |= 1 << 23
            values.append((-mantissa if bits >> 31 else mantissa, exp - 150 if exp else -149))
    else:
        raise Unsupported("CPU resource dtype must be BF16/F32")
    exponent = min(e for _, e in values)
    return tensor([m << (e - exponent) for m, e in values], exponent, stats, stage)


def hash_file(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for data in iter(lambda: stream.read(4 << 20), b""): result.update(data)
    return result.hexdigest()


def payload_stat(path):
    path = Path(path).resolve(strict=True); item = path.stat()
    return {"path": str(path), "bytes": item.st_size, "mtime_ns": item.st_mtime_ns,
            "device": item.st_dev, "inode": item.st_ino}


def verify_payload_receipt(path, package, cpu, identity):
    receipt = json.loads(Path(path).read_text(encoding="utf-8"))
    checksum = receipt.pop("sha256", None)
    if checksum != digest_json(receipt) or receipt.get("version") != "prior-sha256-stat-receipt-v1":
        raise ValueError("invalid prior verification receipt")
    prior_path = Path(receipt["prior_result_path"]).resolve(strict=True)
    if hash_file(prior_path) != receipt["prior_result_sha256"]:
        raise ValueError("prior verified result identity changed")
    prior = json.loads(prior_path.read_text(encoding="utf-8"))
    if not prior["plan"]["payload_identity_verified"] or prior["identity"]["sources"] != identity:
        raise ValueError("receipt has no matching prior payload SHA verification")
    current = {"package": payload_stat(package), "cpu": payload_stat(cpu)}
    if receipt.get("payload_stats") != current or receipt.get("source_identity") != identity:
        raise ValueError("payload stat/identity changed; prior SHA may not be reused")
    return checksum


class PackageWeights:
    """Read-only mmap loader; run requires SHA or explicit prior-SHA/stat receipt."""
    def __init__(self, package, cpu, catalog, scales, scale_meta, scale_manifest, verify=False, receipt=None):
        paths = [Path(p).resolve(strict=True) for p in (package, cpu, catalog, scales, scale_meta, scale_manifest)]
        if any(paths[i].samefile(paths[j]) for i in range(len(paths)) for j in range(i)):
            raise ValueError("model input alias")
        verify_export(paths[3], paths[4], paths[5])
        meta = json.loads(paths[4].read_text(encoding="utf-8"))
        sources = meta["binding"]["sources"]
        if (sources["package"]["sha256"] != SOURCE_SHA256 or sources["package"]["bytes"] != SOURCE_BYTES or
                sources["tensor_catalog"]["sha256"] != CATALOG_SHA or meta["binary"]["sha256"] != SCALE_SHA):
            raise ValueError("sidecar source/model/static-scale pinned identity mismatch")
        self.identity = {"package_sha256": SOURCE_SHA256, "cpu_sha256": CPU_SHA,
                         "catalog_sha256": meta["binding"]["sources"]["tensor_catalog"]["sha256"],
                         "scale_sha256": meta["binary"]["sha256"], "scale_binding": meta["binding_sha256"]}
        if paths[0].stat().st_size != SOURCE_BYTES or paths[1].stat().st_size != CPU_BYTES:
            raise ValueError("model body/CPU fixed size mismatch")
        if hash_file(paths[2]) != self.identity["catalog_sha256"]:
            raise ValueError("catalog identity mismatch")
        if receipt is not None and verify: raise ValueError("choose full SHA validation or explicit prior receipt")
        if verify and (hash_file(paths[0]) != SOURCE_SHA256 or hash_file(paths[1]) != CPU_SHA):
            raise ValueError("real execution payload SHA mismatch")
        self.receipt_sha256 = verify_payload_receipt(receipt, paths[0], paths[1], self.identity) if receipt is not None else None
        self.verified = verify or receipt is not None
        self.verification = "prior_sha256_plus_matching_stat_receipt" if receipt is not None else ("full_sha256" if verify else "plan_only")
        self.payload_paths, self.payload_stats = paths[:2], [payload_stat(path) for path in paths[:2]]
        self.entries = {entry["name"]: entry for entry in json.loads(paths[2].read_text())["tensors"]}
        self.scales = {entry["name"]: entry for entry in meta["binding"]["tensors"]}
        self.matrix_shapes = {name: tuple(entry["shape_NK"]) for name, entry in self.scales.items()}
        self.vector_lengths = {name: entry["source_raw_bytes"] // (2 if entry["target_dtype"] == "BF16" else 4)
                               for name, entry in self.entries.items() if entry["target_dtype"] != "signed_W4_zero_point_0"}
        embedding = self.entries["token_embd.weight"]
        self.embedding_shape = tuple(embedding["source_shape"])
        if self.embedding_shape != (248320, 1024) or embedding["target_dtype"] != "BF16":
            raise Unsupported("original tied BF16 embedding shape/dtype mismatch")
        self.files = [path.open("rb") for path in (paths[0], paths[1], paths[3])]
        self.maps = [mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) for stream in self.files]
        self.cache = {}
        for name, entry in self.entries.items():
            if entry["target_dtype"] != "signed_W4_zero_point_0":
                offset, size = entry["source_raw_offset"], entry["source_raw_bytes"]
                if offset < 0 or size <= 0 or offset + size > CPU_BYTES:
                    raise ValueError("CPU resource bounds")

    def check_payload_stat(self):
        if self.payload_stats != [payload_stat(path) for path in self.payload_paths]:
            raise ValueError("original payload changed during candidate execution")

    def close(self):
        self.cache.clear()
        for view in self.maps: view.close()
        for stream in self.files: stream.close()

    def vector(self, name, stats):
        if name not in self.cache:
            entry = self.entries[name]; start = entry["source_raw_offset"]
            self.cache[name] = exact_words(self.maps[1][start:start + entry["source_raw_bytes"]], entry["target_dtype"], stats, name)
        return self.cache[name]

    def embedding_row(self, token):
        checked_range(token, 0, self.embedding_shape[0] - 1)
        entry = self.entries["token_embd.weight"]
        offset = entry["source_raw_offset"] + token * self.embedding_shape[1] * 2
        return self.maps[1][offset:offset + self.embedding_shape[1] * 2]

    def rows(self, name, first, count):
        entry = self.scales[name]; n, k = entry["shape_NK"]
        old = entry["original_weight"]; stride = old["row_stride_bytes"]
        raw = np.ndarray((count, stride), dtype=np.uint8, buffer=self.maps[0], offset=old["offset"] + first * stride)
        result = np.empty((count, stride * 2), dtype=np.int32)
        result[:, 0::2] = raw & 15; result[:, 1::2] = raw >> 4
        result[result >= 8] -= 16
        return result[:, :k]

    def row_scales(self, name, row):
        entry = self.scales[name]; n, groups = entry["scale_shape"]
        storage = entry["candidate_scale"]
        offset = storage["offset"] + row * storage["row_stride_bytes"]
        return [struct.unpack_from("<HbB", self.maps[2], offset + g * 4)[:2] for g in range(groups)]


class IntegerModel:
    def __init__(self, provider, geometry, context, parameters, nonlinear, rope, softmax):
        self.provider, self.g, self.context = provider, geometry, context
        self.plan = graph_plan(provider, geometry, context)
        if parameters.get("source_identity") != provider.identity or parameters.get("geometry") != asdict(geometry):
            raise ValueError("per-layer parameter identity mismatch")
        if parameters.get("version") == "layer-conversions-v1":
            if "a8_scale_mode" in parameters: raise ValueError("legacy V1 parameters cannot relabel numerics")
            self.a8_scale_mode = "v1_power2"
        elif parameters.get("version") == "layer-conversions-v2":
            self.a8_scale_mode = parameters.get("a8_scale_mode")
            if self.a8_scale_mode not in A8_MODES or parameters.get("activation_scale_precision_bits") != A8_PRECISION_BITS:
                raise ValueError("V2 requires explicit supported mode and fixed24 precision")
        else: raise ValueError("unknown layer conversions version")
        self.numeric_version = VERSION_V1 if self.a8_scale_mode == "v1_power2" else VERSION_V2
        self.plan.update({"numerics_version": self.numeric_version, "a8_scale_mode": self.a8_scale_mode})
        if parameters.get("status") != "UNCALIBRATED_CANDIDATE" or len(parameters.get("layers", [])) != 24:
            raise ValueError("explicit candidate layer conversions required")
        for index, layer in enumerate(parameters["layers"]):
            if set(layer) != {"layer", "norm_input_frac", "norm_weight_frac", "norm_output_frac", "qkv_frac"} or layer["layer"] != index:
                raise ValueError("missing/duplicate layer conversion")
            for key in ("norm_input_frac", "norm_weight_frac", "norm_output_frac", "qkv_frac"):
                checked_range(layer[key], 0, 24)
        checked_range(parameters["head_input_frac"], 0, 24)
        checked_range(parameters["head_output_exponent"], -60, 60)
        if rope.context_tokens != context:
            raise ValueError("RoPE LUT context must exactly match requested context")
        self.parameters, self.nonlinear, self.rope, self.softmax = deepcopy(parameters), nonlinear, rope, softmax
        self.identity = {"version": self.numeric_version, "a8_scale_mode": self.a8_scale_mode,
                         "activation_scale_precision_bits": A8_PRECISION_BITS,
                         "sources": deepcopy(provider.identity), "parameters_sha256": digest_json(parameters),
                         "nonlinear_sha256": nonlinear.sha256, "rope_sha256": rope.artifact()["sha256"],
                         "softmax_sha256": softmax.artifact()["sha256"], "geometry": asdict(geometry), "context": context,
                         "implementation_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                         "kernel_source_sha256": {name: hashlib.sha256(Path(__file__).with_name(name).read_bytes()).hexdigest()
                             for name in ("agent_integer_reference.py", "agent_integer_graph_ops.py", "export_agent_integer_scales.py")}}
        self.reset()

    def reset(self):
        g = self.g
        self.tokens, self.needs_reset, self.stats = [], False, ModelStats()
        self.conv = {i: [BFP((0, 0, 0), -128, 32) for _ in range(3 * g.delta_heads * g.delta_dim)] for i in range(24) if i % 4 != 3}
        self.delta = {i: [BFP((0,) * (g.delta_dim * g.delta_dim), -128, 32) for _ in range(g.delta_heads)] for i in self.conv}
        self.kv = {i: [] for i in range(3, 24, 4)}
        self.layer_outputs, self.progress = [], None

    def norm(self, value, name, layer, centered, stats):
        cfg = self.parameters["layers"][min(layer, 23)]
        weights = self.provider.vector(name, stats)
        x = fixed(value, cfg["norm_input_frac"], stats, "norm.input")
        w = fixed(weights, cfg["norm_weight_frac"], stats, "norm.weight")
        result = rms_norm(x, w, cfg["norm_input_frac"], cfg["norm_weight_frac"], cfg["norm_output_frac"], centered,
                          output_bits=32, stats=stats)
        return tensor(result, -cfg["norm_output_frac"], stats, "norm.result")

    def matrix(self, name, value, stats):
        stats = Scoped(stats, name)
        n, k = self.provider.matrix_shapes[name]
        if len(value.values) != k: raise ValueError("matrix input shape")
        a8 = quantize_activation(value, self.a8_scale_mode, stats)
        stats.activation("activation.a8", a8)
        activation = np.asarray(a8.values, dtype=np.int32)
        group_count = (k + 127) // 128
        results, output_exponent = [], None
        for first in range(0, n, 32):
            count = min(32, n - first)
            weights = self.provider.rows(name, first, count)
            if weights.dtype != np.int32 or weights.shape != (count, k) or np.any(weights < -8) or np.any(weights > 7):
                raise ValueError("W4 decoded dtype/range")
            partial = np.empty((count, group_count), dtype=np.int32)
            for group in range(group_count):
                start, stop = group * 128, min(k, (group + 1) * 128)
                partial[:, group] = weights[:, start:stop] @ activation[start:stop]
            for row in range(count):
                scales = self.provider.row_scales(name, first + row)
                if len(scales) != group_count: raise ValueError("exact dyadic scale group count")
                for m, e in scales:
                    checked_range(m, 1, 2047); checked_range(e, -24, 5)
                exponent = min(e for m, e in scales) + a8.exponent
                total = sum((int(partial[row, group]) * m * a8.mantissa) << (e + a8.exponent - exponent)
                            for group, (m, e) in enumerate(scales))
                stats.observe(total, "w4.wide_group_sum")
                if output_exponent is None: output_exponent = exponent
                if exponent < output_exponent:
                    results = [x << (output_exponent - exponent) for x in results]; output_exponent = exponent
                results.append(total << (exponent - output_exponent))
        return tensor(results, output_exponent, stats, "matrix.output")

    def linear(self, i, normal, stats):
        g, prefix = self.g, f"blk.{i}."
        qkv = self.matrix(prefix + "attn_qkv.weight", normal, stats)
        z = self.matrix(prefix + "attn_gate.weight", normal, stats)
        a = self.matrix(prefix + "ssm_alpha.weight", normal, stats)
        b = self.matrix(prefix + "ssm_beta.weight", normal, stats)
        conv_weight = self.provider.vector(prefix + "ssm_conv1d.weight", stats)
        converted = []
        for channel, current in enumerate(qkv.values):
            history = self.conv[i][channel]
            common = min(history.exponent, qkv.exponent)
            inputs = [x << (history.exponent - common) for x in history.values] + [current << (qkv.exponent - common)]
            coeff = conv_weight.values[channel * 4:channel * 4 + 4]
            result = tensor([sum(x * w for x, w in zip(inputs, coeff))], common + conv_weight.exponent, stats, "conv.sum")
            converted.append(self.nonlinear.silu(result, stats))
            self.conv[i][channel] = tensor(inputs[1:], common, stats, "conv.raw_state")
        qkv = join(converted, stats, "conv.output")
        dt, al = self.provider.vector(prefix + "ssm_dt.bias", stats), self.provider.vector(prefix + "ssm_a", stats)
        a_dt = residual_bfp(a, dt, 32, stats)
        heads, hd = [], g.delta_heads * g.delta_dim
        frac = self.parameters["layers"][i]["qkv_frac"]
        for head in range(g.delta_heads):
            plus = self.nonlinear.scalar("softplus", a_dt.values[head], a_dt.exponent, stats)
            alpha = self.nonlinear.scalar("exp", al.values[head], al.exponent, stats)
            decay = self.nonlinear.scalar("exp", -(plus * alpha), -60, stats)
            beta = self.nonlinear.scalar("sigmoid", b.values[head], b.exponent, stats)
            q, k, v = [fixed(segment(qkv, offset + head * g.delta_dim, g.delta_dim), frac, stats, "delta.qkv")
                       for offset in (0, hd, 2 * hd)]
            self.delta[i][head], result = delta_state_step(self.delta[i][head], q, k, v, decay, beta, frac, stats)
            stats.scale(f"delta.head.{head}.state", self.delta[i][head].exponent)
            result = self.norm(result, prefix + "ssm_norm.weight", i, False, stats)
            gate = self.nonlinear.silu(segment(z, head * g.delta_dim, g.delta_dim), stats)
            heads.append(multiply(result, gate, stats, "delta.gate"))
        return self.matrix(prefix + "ssm_out.weight", join(heads, stats, "delta.concat"), stats)

    def attention(self, i, normal, stats):
        g, prefix = self.g, f"blk.{i}."
        qraw = self.matrix(prefix + "attn_q.weight", normal, stats)
        kraw = self.matrix(prefix + "attn_k.weight", normal, stats)
        vraw = self.matrix(prefix + "attn_v.weight", normal, stats)
        position = len(self.tokens)
        keys, values = [], []
        for head in range(g.kv_heads):
            key = self.norm(segment(kraw, head * g.attention_dim, g.attention_dim), prefix + "attn_k_norm.weight", i, True, stats)
            key = tensor(rope_apply_q30(key.values, position, self.rope, 32, stats), key.exponent, stats, "rope.key")
            keys.append(key); values.append(segment(vraw, head * g.attention_dim, g.attention_dim))
            stats.scale(f"kv.head.{head}.key", key.exponent)
            stats.scale(f"kv.head.{head}.value", values[-1].exponent)
        self.kv[i].append((keys, values))
        outputs = []
        inverse_dimension = inv_sqrt_ratio_q(g.attention_dim, 1, 30, stats)
        for head in range(g.query_heads):
            raw = segment(qraw, head * g.attention_dim * 2, g.attention_dim * 2)
            query = self.norm(segment(raw, 0, g.attention_dim), prefix + "attn_q_norm.weight", i, True, stats)
            query = tensor(rope_apply_q30(query.values, position, self.rope, 32, stats), query.exponent, stats, "rope.query")
            kv_head = head // (g.query_heads // g.kv_heads)
            scores = []
            for saved_keys, saved_values in self.kv[i]:
                key = saved_keys[kv_head]
                total = sum(a * b for a, b in zip(query.values, key.values)) * inverse_dimension
                score = shift_rne(total, query.exponent + key.exponent - 30 + 8, stats, "attention.score_q8")
                scores.append(saturate(score, 32, stats, "attention.score_q8"))
            probabilities = softmax_lut(scores, self.softmax, 8, 30, stats)
            saved = [entry[1][kv_head] for entry in self.kv[i]]
            exponent = min(value.exponent for value in saved)
            combined = [sum((value.values[j] << (value.exponent - exponent)) * probability
                            for value, probability in zip(saved, probabilities)) for j in range(g.attention_dim)]
            result = tensor(combined, exponent - 30, stats, "attention.value_sum")
            gate = self.nonlinear.sigmoid(segment(raw, g.attention_dim, g.attention_dim), stats)
            outputs.append(multiply(result, gate, stats, "attention.gate"))
        return self.matrix(prefix + "attn_output.weight", join(outputs, stats, "attention.concat"), stats)

    def token_step(self, token, head_ids=None, greedy=False):
        if self.needs_reset: raise Unsupported("reset required after failed token; partial state is not resumable")
        if not self.provider.verified: raise Unsupported("payload identity unverified; plan-only provider cannot execute")
        checked_range(token, 0, self.g.vocab - 1)
        if len(self.tokens) >= self.context: raise Unsupported("context full; no token loss/context shrinking")
        if greedy and head_ids is not None: raise ValueError("greedy requires full head, not selected rows")
        if head_ids is not None:
            if not head_ids or len(set(head_ids)) != len(head_ids): raise ValueError("unique selected head IDs required")
            for row in head_ids: checked_range(row, 0, self.g.vocab - 1)
        self.layer_outputs = []
        try:
            if hasattr(self.provider, "check_payload_stat"): self.provider.check_payload_stat()
            value = exact_words(self.provider.embedding_row(token), "BF16", self.stats, "embedding")
            for i in range(24):
                stats = Scoped(self.stats, f"layer.{i}")
                prefix = f"blk.{i}."
                normal = self.norm(value, prefix + "attn_norm.weight", i, True, stats)
                output = self.attention(i, normal, stats) if i % 4 == 3 else self.linear(i, normal, stats)
                value = residual_bfp(value, output, 32, stats)
                normal = self.norm(value, prefix + "post_attention_norm.weight", i, True, stats)
                gate = self.nonlinear.silu(self.matrix(prefix + "ffn_gate.weight", normal, stats), stats)
                up = self.matrix(prefix + "ffn_up.weight", normal, stats)
                down = self.matrix(prefix + "ffn_down.weight", multiply(gate, up, stats, "ffn.product"), stats)
                value = residual_bfp(value, down, 32, stats)
                self.layer_outputs.append({"layer": i, "values": list(value.values), "exponent": value.exponent, "bits": 32})
                if self.progress is not None: self.progress(len(self.tokens), i)
            result = {"status": "CANDIDATE_TOKEN_COMPUTED_NOT_QUALITY_APPROVED", "position": len(self.tokens),
                      "token": token, "layers_executed": 24, "selected_logits": None, "greedy": None}
            if head_ids is not None or greedy:
                normal = self.norm(value, "output_norm.weight", 23, True, Scoped(self.stats, "final"))
                activation = fixed(normal, self.parameters["head_input_frac"], self.stats, "head.input")
                logits, best = [], None
                for row in (range(self.g.vocab) if greedy else head_ids):
                    score = bf16_head_dot_row(self.provider.embedding_row(row), activation,
                        self.parameters["head_input_frac"], self.parameters["head_output_exponent"], 64, self.stats)
                    if greedy:
                        if best is None or score > best[1]: best = (row, score)
                    else: logits.append({"row": row, "value": score})
                result["selected_logits"] = logits if not greedy else None
                result["greedy"] = best[0] if greedy else None
                result["logit_exponent"] = self.parameters["head_output_exponent"]
            if hasattr(self.provider, "check_payload_stat"): self.provider.check_payload_stat()
            self.tokens.append(token)
            return result
        except Exception:
            self.needs_reset = True
            raise

    def prefill(self, tokens):
        if not tokens or len(tokens) > self.context - len(self.tokens):
            raise Unsupported("prefix exceeds context or is empty; tokens never truncated")
        for token in tokens: checked_range(token, 0, self.g.vocab - 1)
        return [self.token_step(token) for token in tokens]

    def snapshot(self):
        if self.needs_reset: raise Unsupported("cannot snapshot partial failed token")
        def encode(value):
            if isinstance(value, BFP): return {"values": list(value.values), "exponent": value.exponent, "bits": 32}
            if isinstance(value, dict): return {str(key): encode(item) for key, item in value.items()}
            if isinstance(value, (tuple, list)): return [encode(item) for item in value]
            return value
        payload = {"version": self.numeric_version, "identity": deepcopy(self.identity), "tokens": self.tokens[:],
                   "conv": encode(self.conv), "delta": encode(self.delta), "kv": encode(self.kv)}
        return {"payload": payload, "sha256": digest_json(payload)}

    def restore(self, document):
        payload = document.get("payload")
        if document.get("sha256") != digest_json(payload) or payload.get("identity") != self.identity or payload.get("version") != self.numeric_version:
            raise ValueError("prefix full-state identity/hash mismatch")
        tokens = payload["tokens"]
        if type(tokens) is not list or len(tokens) > self.context: raise ValueError("prefix tokens outside context")
        for token in tokens: checked_range(token, 0, self.g.vocab - 1)
        def read(value, length):
            if set(value) != {"values", "exponent", "bits"} or value["bits"] != 32 or len(value["values"]) != length:
                raise ValueError("prefix state shape/bits mismatch")
            return BFP(tuple(value["values"]), value["exponent"], 32)
        g = self.g
        linear, attention = {str(i) for i in self.conv}, {str(i) for i in self.kv}
        if set(payload["conv"]) != linear or set(payload["delta"]) != linear or set(payload["kv"]) != attention:
            raise ValueError("missing full layer states")
        conv, delta, kv = {}, {}, {}
        for key in linear:
            if len(payload["conv"][key]) != 3 * g.delta_heads * g.delta_dim or len(payload["delta"][key]) != g.delta_heads:
                raise ValueError("prefix conv/delta head count")
            conv[int(key)] = [read(value, 3) for value in payload["conv"][key]]
            delta[int(key)] = [read(value, g.delta_dim * g.delta_dim) for value in payload["delta"][key]]
        for key in attention:
            if len(payload["kv"][key]) != len(tokens): raise ValueError("KV cache/prefix length mismatch")
            kv[int(key)] = []
            for pair in payload["kv"][key]:
                if len(pair) != 2 or any(len(part) != g.kv_heads for part in pair): raise ValueError("KV head shape")
                kv[int(key)].append(tuple([read(value, g.attention_dim) for value in part] for part in pair))
        self.tokens, self.conv, self.delta, self.kv, self.needs_reset = tokens[:], conv, delta, kv, False


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("nonlinear-lut", "plan", "candidate-parameters", "run"))
    parser.add_argument("--out", required=True, type=Path)
    for name in ("package", "cpu", "catalog", "scales", "scale-meta", "scale-manifest", "parameters", "nonlinear", "rope", "softmax", "tokens", "prefix-in", "prefix-out", "payload-receipt"):
        parser.add_argument("--" + name, type=Path)
    parser.add_argument("--context", type=int, default=2048)
    parser.add_argument("--head-ids", help="comma-separated selected head rows; never a full greedy claim")
    parser.add_argument("--greedy", action="store_true")
    parser.add_argument("--progress", action="store_true", help="integer token/layer progress to stderr")
    parser.add_argument("--a8-scale-mode", choices=A8_MODES, help="required only for candidate-parameters; run mode comes from bound config")
    args = parser.parse_args(argv)
    if args.command == "candidate-parameters" and args.a8_scale_mode is None:
        raise ValueError("candidate-parameters requires explicit --a8-scale-mode")
    if args.command != "candidate-parameters" and args.a8_scale_mode is not None:
        raise ValueError("no CLI run override: numeric mode must be in parameters")
    inputs = [value for key, value in vars(args).items() if isinstance(value, Path) and key not in ("out", "prefix_out")]
    outputs = [path for path in (args.out, args.prefix_out) if path is not None]
    for index, output in enumerate(outputs):
        for source in inputs + outputs[:index]:
            if output.resolve() == source.resolve() or (output.exists() and source.exists() and output.samefile(source)):
                raise ValueError("output aliases original model/input/prefix evidence")
    provider, model = None, None
    try:
        if args.command == "nonlinear-lut": result = Nonlinear.generate()
        else:
            required = (args.package, args.cpu, args.catalog, args.scales, args.scale_meta, args.scale_manifest)
            if not all(required): raise ValueError("all original body/CPU/catalog/static-scale sidecar inputs required")
            provider = PackageWeights(*required, verify=args.command == "run" and args.payload_receipt is None, receipt=args.payload_receipt)
            geometry = Geometry()
            result = graph_plan(provider, geometry, args.context)
            if args.command == "candidate-parameters": result = candidate_parameters(provider, geometry, args.a8_scale_mode)
            if args.command == "run":
                if not all((args.parameters, args.nonlinear, args.rope, args.softmax, args.tokens)):
                    raise Unsupported("execution requires explicit per-layer parameters and all versioned LUTs/token IDs")
                load = lambda path: json.loads(path.read_text(encoding="utf-8"))
                model = IntegerModel(provider, geometry, args.context, load(args.parameters), Nonlinear(load(args.nonlinear)),
                                     load_rope_table(load(args.rope)), load_exp_lut(load(args.softmax)))
                if args.prefix_in: model.restore(load(args.prefix_in))
                if args.progress:
                    model.progress = lambda token, layer: print(f"integer candidate token_position={token} layer={layer}", flush=True)
                tokens = load(args.tokens)
                if type(tokens) is not list or not tokens: raise ValueError("explicit nonempty token ID array required")
                if len(tokens) > args.context - len(model.tokens): raise Unsupported("requested token sequence exceeds context; no truncation")
                for token in tokens: checked_range(token, 0, geometry.vocab - 1)
                head_ids = [int(row) for row in args.head_ids.split(",")] if args.head_ids else None
                model.prefill(tokens[:-1]) if len(tokens) > 1 else None
                last = model.token_step(tokens[-1], head_ids, args.greedy)
                if args.prefix_out:
                    args.prefix_out.parent.mkdir(parents=True, exist_ok=True)
                    args.prefix_out.write_text(json.dumps(model.snapshot()) + "\n", encoding="utf-8")
                result = {"plan": model.plan, "result": last, "statistics": model.stats.report(), "identity": model.identity,
                          "layer_outputs": model.layer_outputs}
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        return 0
    except Unsupported as error:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        failure = {"status": "UNSUPPORTED", "reason": str(error), "version": VERSION}
        if model is not None:
            failure.update({"processed_tokens": model.tokens, "needs_reset": model.needs_reset,
                            "statistics": model.stats.report(), "identity": model.identity,
                            "layer_outputs": model.layer_outputs})
        args.out.write_text(json.dumps(failure, indent=2) + "\n", encoding="utf-8")
        return 3
    finally:
        if provider is not None: provider.close()


if __name__ == "__main__":
    raise SystemExit(main())
