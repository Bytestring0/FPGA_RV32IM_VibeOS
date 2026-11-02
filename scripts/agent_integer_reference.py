#!/usr/bin/env python3
"""Independent integer numerical/state reference; experimental, NOT a hardware ABI.

Real values are integer * 2**exponent (or integer / 2**fraction_bits).
Kernels use integer arithmetic only, including normalization reciprocal roots.
Python unbounded intermediates specify the mathematical integer reference; only
explicit saturation sites assert a width. No fixed hardware accumulator width is
claimed. Decimal arithmetic is restricted to offline LUT generation and exact
capture ingestion, never an execution kernel. NumPy handles integer bit fields
in streaming binary calibration and independent floating test oracles only.

Commands: metadata --out FILE; lut --out FILE;
          run --case INTEGER_CASE.json [--lut LUT.json] --out FILE;
          calibrate --trace RAW_CAPTURE.json --out REPORT.json;
          calibrate --index TRACE.jsonl --values VALUES.bin --split-manifest SPLIT.json
                    --split-sha256 EXPECTED_SHA --source SOURCE.json --out REPORT.json
No evaluation corpus is read or used to choose scales.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from decimal import Decimal, ROUND_HALF_EVEN, localcontext
from fractions import Fraction
import hashlib
import json
from math import isqrt, prod
from pathlib import Path
import re
from typing import Sequence

REFERENCE_VERSION = "agent-integer-numerics-v1"
LUT_VERSION = "exp-neg-q8-q30-domain16-v1"
SPLIT_VERSION = "agent-calibration-sample-sha256-mod5-v1"
RAW_TAGS = frozenset({
    "norm.attn.input", "norm.ffn.input", "norm.delta.input", "norm.key.input",
    "norm.query.input", "norm.final.input", "norm.sumsq",
    "residual.attn.left", "residual.attn.right", "residual.ffn.left",
    "residual.ffn.right", "linear.qkv", "linear.conv.state.before",
    "linear.conv.state.after", "linear.delta.state.before",
    "linear.delta.state.after", "linear.delta.decay", "linear.delta.beta",
    "linear.delta.q", "linear.delta.k", "linear.delta.v",
    "attention.key.state", "attention.value.state",
})
RAW_COUNTS = {
    **{tag: 1024 for tag in ("norm.attn.input", "norm.ffn.input", "norm.final.input",
                            "residual.attn.left", "residual.attn.right", "residual.ffn.left", "residual.ffn.right")},
    "norm.delta.input": 128, "norm.key.input": 256, "norm.query.input": 256,
    "norm.sumsq": 1, "linear.qkv": 6144,
    "linear.conv.state.before": 6144 * 3, "linear.conv.state.after": 6144 * 3,
    "linear.delta.state.before": 128 * 128, "linear.delta.state.after": 128 * 128,
    "linear.delta.decay": 1, "linear.delta.beta": 1,
    "linear.delta.q": 128, "linear.delta.k": 128, "linear.delta.v": 128,
    "attention.key.state": 256, "attention.value.state": 256,
}


def integer(value: int) -> int:
    if type(value) is not int:
        raise ValueError("kernel operands must be Python integers, never floats/bools")
    return value


def checked_range(value: int, low: int, high: int) -> int:
    value = integer(value)
    if not low <= value <= high:
        raise ValueError(f"integer outside [{low}, {high}]")
    return value


def vector(values: Sequence[int], bits: int = 32) -> list[int]:
    checked_range(bits, 2, 64)
    if not values:
        raise ValueError("empty vector")
    return [checked_range(v, -(1 << (bits - 1)), (1 << (bits - 1)) - 1) for v in values]


@dataclass
class Stats:
    """Operation counts; one element can underflow at several rounding sites."""
    counts: dict[str, int] = field(default_factory=dict)
    by_stage: dict[str, dict[str, int]] = field(default_factory=dict)
    peak_magnitude_bits: dict[str, int] = field(default_factory=dict)

    def hit(self, kind: str, stage: str) -> None:
        self.counts[kind] = self.counts.get(kind, 0) + 1
        bucket = self.by_stage.setdefault(stage, {})
        bucket[kind] = bucket.get(kind, 0) + 1

    def observe(self, value: int, stage: str) -> None:
        self.peak_magnitude_bits[stage] = max(
            self.peak_magnitude_bits.get(stage, 0), abs(value).bit_length())

    def report(self) -> dict:
        return {"counts": dict(self.counts), "by_stage": self.by_stage,
                "peak_magnitude_bits": self.peak_magnitude_bits,
                "underflow_definition": "nonzero value rounds to zero at an operation"}


def rne_div(numerator: int, denominator: int, stats: Stats | None = None,
            stage: str = "divide") -> int:
    """Signed round-to-nearest, ties-to-even, with a positive denominator."""
    integer(numerator)
    if integer(denominator) <= 0:
        raise ValueError("positive denominator required")
    quotient, remainder = divmod(abs(numerator), denominator)
    if 2 * remainder > denominator or (2 * remainder == denominator and quotient & 1):
        quotient += 1
    result = -quotient if numerator < 0 else quotient
    if stats:
        if remainder:
            stats.hit("inexact", stage)
        if numerator and not result:
            stats.hit("underflow", stage)
        stats.observe(numerator, stage + ".numerator")
    return result


def saturate(value: int, bits: int, stats: Stats | None = None,
             stage: str = "saturate", signed: bool = True) -> int:
    integer(value)
    checked_range(bits, 2, 64)
    low, high = (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)
    result = min(high, max(low, value))
    if stats:
        stats.observe(value, stage)
        if result != value:
            stats.hit("saturation", stage)
    return result


def shift_rne(value: int, shift: int, stats: Stats | None = None,
              stage: str = "rescale") -> int:
    integer(value)
    checked_range(shift, -512, 512)
    return value << shift if shift >= 0 else rne_div(value, 1 << -shift, stats, stage)


def rescale(value: int, source_frac: int, output_frac: int,
            stats: Stats | None = None, stage: str = "rescale", bits: int | None = None) -> int:
    checked_range(source_frac, 0, 180)
    checked_range(output_frac, 0, 60)
    result = shift_rne(value, output_frac - source_frac, stats, stage)
    return result if bits is None else saturate(result, bits, stats, stage)


@dataclass(frozen=True)
class Dyadic:
    """Positive exact multiplier mantissa * 2**exponent."""
    mantissa: int
    exponent: int

    def __post_init__(self) -> None:
        if integer(self.mantissa) <= 0:
            raise ValueError("scale must be positive")
        checked_range(self.exponent, -256, 256)


def binary16_scale(bits: int) -> Dyadic:
    bits = checked_range(bits, 0, 65535)
    exponent, fraction = (bits >> 10) & 31, bits & 1023
    if bits & 0x8000 or exponent == 31 or (exponent == 0 and fraction == 0):
        raise ValueError("scale must be positive finite binary16")
    return Dyadic(fraction, -24) if exponent == 0 else Dyadic(1024 + fraction, exponent - 25)


def quantize_a8(values: Sequence[int], input_frac: int, scale: Dyadic,
                stats: Stats | None = None) -> list[int]:
    """Integer-to-A8 using an explicit frozen dyadic scale, RNE then saturation."""
    values = vector(values)
    checked_range(input_frac, 0, 60)
    shift = -input_frac - scale.exponent
    result = []
    for value in values:
        numerator = value << max(0, shift)
        denominator = scale.mantissa << max(0, -shift)
        result.append(saturate(rne_div(numerator, denominator, stats, "a8.quantize"),
                               8, stats, "a8.quantize"))
    return result


def w4_g128_a8_dot(packed_rows: Sequence[bytes], activations: Sequence[int],
                   stats: Stats | None = None) -> list[list[int]]:
    """Low nibble first, signed two's-complement W4/A8, padded G128 rows.

    Padding is never consumed. Each group is an exact signed INT32 dot product;
    the maximum possible magnitude 128*8*128 fits without saturation.
    """
    a = vector(activations, 8)
    if len(a) > 4096 or not packed_rows:
        raise ValueError("W4 dimensions outside 1..4096")
    groups = (len(a) + 127) // 128
    result = []
    for row in packed_rows:
        if len(row) < groups * 64:
            raise ValueError("short padded W4 row")
        sums = []
        for group in range(groups):
            total = 0
            for col in range(group * 128, min((group + 1) * 128, len(a))):
                byte = checked_range(row[col // 2], 0, 255)
                nibble = (byte >> (4 * (col & 1))) & 15
                weight = nibble - 16 if nibble >= 8 else nibble
                total += weight * a[col]
            sums.append(saturate(total, 32, stats, "w4.group_dot"))
        result.append(sums)
    return result


def requantize_groups(partials: Sequence[Sequence[int]],
                      weight_scales: Sequence[Sequence[Dyadic]], activation_scale: Dyadic,
                      output_exponent: int, output_bits: int = 16,
                      stats: Stats | None = None) -> list[int]:
    """RNE each G128 term to a common binary scale, exact sum, final saturation.

    This candidate intentionally differs from the current FP32 qw_reduce contract.
    It is NOT an approved replacement for that API.
    """
    checked_range(output_exponent, -128, 127)
    if not partials or len(partials) != len(weight_scales):
        raise ValueError("group scale shape mismatch")
    result = []
    for row, scales in zip(partials, weight_scales):
        if not row or len(row) != len(scales):
            raise ValueError("group scale shape mismatch")
        total = 0
        for partial, scale in zip(row, scales):
            checked_range(partial, -(1 << 31), (1 << 31) - 1)
            term = partial * scale.mantissa * activation_scale.mantissa
            total += shift_rne(term, scale.exponent + activation_scale.exponent - output_exponent,
                               stats, "w4.group_requant")
            if stats:
                stats.observe(total, "w4.scaled_sum")
        result.append(saturate(total, output_bits, stats, "w4.output"))
    return result


def residual_int16(left: Sequence[int], right: Sequence[int], left_frac: int,
                   right_frac: int, output_frac: int, stats: Stats | None = None) -> list[int]:
    left, right = vector(left, 16), vector(right, 16)
    if len(left) != len(right):
        raise ValueError("residual shape mismatch")
    for frac in (left_frac, right_frac, output_frac):
        checked_range(frac, 0, 60)
    common = max(left_frac, right_frac)
    return [rescale((a << (common - left_frac)) + (b << (common - right_frac)),
                    common, output_frac, stats, "residual.int16", 16)
            for a, b in zip(left, right)]


@dataclass(frozen=True)
class BFP:
    values: tuple[int, ...]
    exponent: int
    bits: int = 16

    def __post_init__(self) -> None:
        checked_range(self.exponent, -128, 127)
        object.__setattr__(self, "values", tuple(vector(self.values, self.bits)))


def pack_bfp(values: Sequence[int], source_exponent: int, bits: int = 16,
             stats: Stats | None = None, stage: str = "bfp.pack") -> BFP:
    checked_range(source_exponent, -256, 256)
    checked_range(bits, 2, 64)
    if not values:
        raise ValueError("empty BFP block")
    values = [integer(v) for v in values]
    low, high = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    exponent = max(-128, min(127, source_exponent))
    while exponent < 127:
        rounded = [shift_rne(v, source_exponent - exponent) for v in values]
        if all(low <= v <= high for v in rounded):
            break
        exponent += 1
    mantissas = [saturate(shift_rne(v, source_exponent - exponent, stats, stage),
                          bits, stats, stage) for v in values]
    return BFP(tuple(mantissas), exponent, bits)


def residual_bfp(left: BFP, right: BFP, bits: int = 16, stats: Stats | None = None) -> BFP:
    if len(left.values) != len(right.values):
        raise ValueError("residual shape mismatch")
    exponent = min(left.exponent, right.exponent)
    values = [(a << (left.exponent - exponent)) + (b << (right.exponent - exponent))
              for a, b in zip(left.values, right.values)]
    return pack_bfp(values, exponent, bits, stats, "residual.bfp")


def inv_sqrt_ratio_q(numerator: int, denominator: int, fraction_bits: int = 30,
                     stats: Stats | None = None) -> int:
    """RNE(2**fraction_bits / sqrt(numerator/denominator)), no float/LUT.

    Compare squared integers at the half-integer boundary to round exactly.
    """
    if integer(numerator) <= 0 or integer(denominator) <= 0:
        raise ValueError("positive root argument required")
    checked_range(fraction_bits, 0, 60)
    scaled = denominator << (2 * fraction_bits)
    root = isqrt(scaled // numerator)
    boundary = (2 * root + 1) ** 2 * numerator
    if 4 * scaled > boundary or (4 * scaled == boundary and root & 1):
        root += 1
    if stats:
        stats.observe(scaled, "norm.rsqrt.radicand")
        if root * root * numerator != scaled:
            stats.hit("inexact", "norm.rsqrt")
        if not root:
            stats.hit("underflow", "norm.rsqrt")
    return root


def _normalizer(values: Sequence[int], fraction_bits: int, mean: bool,
                epsilon: tuple[int, int], stats: Stats | None) -> int:
    checked_range(fraction_bits, 0, 60)
    en, ed = epsilon
    if integer(en) <= 0 or integer(ed) <= 0:
        raise ValueError("epsilon must be a positive exact rational")
    sumsq = sum(v * v for v in values)
    count = len(values) if mean else 1
    # Preserve 1e-6 exactly; do not quantize epsilon to a possibly zero Q-format.
    numerator = sumsq * ed + en * count * (1 << (2 * fraction_bits))
    denominator = count * ed * (1 << (2 * fraction_bits))
    if stats:
        stats.observe(sumsq, "norm.sumsq")
    return saturate(inv_sqrt_ratio_q(numerator, denominator, 30, stats),
                    64, stats, "norm.inverse_q30")


def rms_norm(values: Sequence[int], weights: Sequence[int], input_frac: int,
             weight_frac: int, output_frac: int, centered: bool = True,
             epsilon: tuple[int, int] = (1, 1000000), output_bits: int = 16,
             stats: Stats | None = None) -> list[int]:
    """mean(x*x)+epsilon; gain=(1+weight) if centered, otherwise weight."""
    values, weights = vector(values), vector(weights)
    checked_range(weight_frac, 0, 60)
    if len(values) != len(weights) or type(centered) is not bool:
        raise ValueError("Norm shape/centered mismatch")
    inverse = _normalizer(values, input_frac, True, epsilon, stats)
    offset = (1 << weight_frac) if centered else 0
    return [rescale(x * inverse * (w + offset), input_frac + 30 + weight_frac,
                    output_frac, stats, "norm.output", output_bits)
            for x, w in zip(values, weights)]


@dataclass(frozen=True)
class ExpLUT:
    entries: tuple[int, ...]
    version: str = LUT_VERSION
    input_frac: int = 8
    output_frac: int = 30
    domain_steps: int = 4096

    def __post_init__(self) -> None:
        if (self.version, self.input_frac, self.output_frac, self.domain_steps) != (LUT_VERSION, 8, 30, 4096):
            raise ValueError("unsupported LUT version/domain")
        if len(self.entries) != 4097 or self.entries[0] != 1 << 30:
            raise ValueError("invalid LUT shape/origin")
        previous = 1 << 30
        for value in self.entries:
            checked_range(value, 0, previous)
            previous = value

    def payload(self) -> dict:
        return {"version": self.version, "input_frac": self.input_frac,
                "output_frac": self.output_frac, "domain_steps": self.domain_steps,
                "domain": "[-16,0]", "rounding": "RNE", "entries": list(self.entries)}

    def artifact(self) -> dict:
        data = self.payload()
        data["sha256"] = digest_json(data)
        return data


def digest_json(data: object) -> str:
    return hashlib.sha256(json.dumps(data, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def generate_exp_lut() -> ExpLUT:
    """OFFLINE transcendental generation, not an integer execution kernel."""
    with localcontext() as context:
        context.prec = 80
        entries = tuple(int(((-Decimal(i) / Decimal(256)).exp() * Decimal(1 << 30))
                            .to_integral_value(rounding=ROUND_HALF_EVEN)) for i in range(4097))
    return ExpLUT(entries)


def load_exp_lut(data: dict) -> ExpLUT:
    fields = dict(data)
    digest = fields.pop("sha256", None)
    if digest != digest_json(fields) or set(fields) != {
            "version", "input_frac", "output_frac", "domain_steps", "domain", "rounding", "entries"}:
        raise ValueError("LUT identity mismatch")
    if fields["domain"] != "[-16,0]" or fields["rounding"] != "RNE":
        raise ValueError("LUT format mismatch")
    return ExpLUT(tuple(fields["entries"]), fields["version"], fields["input_frac"],
                  fields["output_frac"], fields["domain_steps"])


def _exp_gap(gap: int, lut: ExpLUT, stats: Stats | None, stage: str) -> int:
    if integer(gap) < 0:
        raise ValueError("nonnegative gap required")
    if gap > lut.domain_steps:
        if stats:
            stats.hit("domain_underflow", stage)
        return 0
    return lut.entries[gap]


def softmax_lut(scores: Sequence[int], lut: ExpLUT, score_frac: int = 8,
                output_frac: int = 15, stats: Stats | None = None) -> list[int]:
    """One global maximum/denominator, even when input spans attention tiles.

    Independent RNE probability outputs need not sum to exactly 2**output_frac.
    No per-tile normalization or final mass correction is hidden here.
    """
    scores = vector(scores)
    checked_range(output_frac, 1, 30)
    maximum = max(scores)
    exponents = [_exp_gap(rescale(maximum - value, score_frac, lut.input_frac,
                                 stats, "softmax.gap"), lut, stats, "softmax.exp") for value in scores]
    total = sum(exponents)
    return [rne_div(value << output_frac, total, stats, "softmax.probability") for value in exponents]


def silu_integer(value: int, input_frac: int, output_frac: int, lut: ExpLUT,
                 stats: Stats | None = None, output_bits: int = 16) -> int:
    gap = rescale(abs(integer(value)), input_frac, lut.input_frac, stats, "silu.gap")
    exponential = _exp_gap(gap, lut, stats, "silu.exp")
    numerator = (1 << lut.output_frac) if value >= 0 else exponential
    sigmoid = rne_div(numerator << 30, (1 << lut.output_frac) + exponential, stats, "silu.sigmoid")
    return rescale(value * sigmoid, input_frac + 30, output_frac, stats, "silu.output", output_bits)


def causal_conv_step(state: Sequence[Sequence[int]], sample: Sequence[int],
                     weights: Sequence[Sequence[int]], data_frac: int, weight_frac: int,
                     output_frac: int, stats: Stats | None = None,
                     activation_lut: ExpLUT | None = None) -> tuple[list[list[int]], list[int]]:
    """Per-channel causal convolution, history oldest first, optional integer SiLU.

    History stores raw input, not convolution/SiLU output, matching qw_step.c.
    """
    sample = vector(sample, 16)
    if len(state) != len(sample) or len(weights) != len(sample):
        raise ValueError("conv channel mismatch")
    checked_range(data_frac, 0, 60)
    checked_range(weight_frac, 0, 60)
    new_state, output = [], []
    for history, current, taps in zip(state, sample, weights):
        history, taps = vector(history, 16), vector(taps, 16)
        if len(taps) != len(history) + 1:
            raise ValueError("conv tap mismatch")
        acc = sum(x * w for x, w in zip(history + [current], taps))
        if stats:
            stats.observe(acc, "conv.accumulator")
        preactivation = rescale(acc, data_frac + weight_frac, output_frac,
                               stats, "conv.output", 16)
        output.append(preactivation if activation_lut is None else silu_integer(
            preactivation, output_frac, output_frac, activation_lut, stats))
        new_state.append(history[1:] + [current])
    return new_state, output


def _normalized_qk(q: Sequence[int], k: Sequence[int], qkv_frac: int,
                   stats: Stats | None) -> tuple[list[int], list[int]]:
    q, k = vector(q), vector(k)
    if len(q) != len(k):
        raise ValueError("q/k shape mismatch")
    iq = _normalizer(q, qkv_frac, False, (1, 1000000), stats)
    ik = _normalizer(k, qkv_frac, False, (1, 1000000), stats)
    dimension_scale = inv_sqrt_ratio_q(len(q), 1, 30, stats)
    qn = [rescale(x * iq * dimension_scale, qkv_frac + 60, 30,
                  stats, "delta.q", 32) for x in q]
    kn = [rescale(x * ik, qkv_frac + 30, 30, stats, "delta.k", 32) for x in k]
    return qn, kn


def _delta_update(state: Sequence[Sequence[int]], qn: Sequence[int], kn: Sequence[int],
                  v: Sequence[int], decay: int, beta: int, bits: int | None,
                  stats: Stats | None) -> tuple[list[list[int]], list[int]]:
    checked_range(decay, 0, 1 << 30)
    checked_range(beta, 0, 1 << 30)
    n = len(qn)
    if len(state) != n or len(v) != n or any(len(row) != n for row in state):
        raise ValueError("DeltaNet square head shape mismatch")

    def cast(value: int, stage: str) -> int:
        if stats:
            stats.observe(value, stage)
        return value if bits is None else saturate(value, bits, stats, stage)

    decayed = [[cast(rne_div(integer(x) * decay, 1 << 30, stats, "delta.decay"),
                     "delta.state.decayed") for x in row] for row in state]
    updated = [list(row) for row in decayed]
    output = []
    for col in range(n):
        memory = cast(rne_div(sum(decayed[row][col] * kn[row] for row in range(n)),
                              1 << 30, stats, "delta.memory"), "delta.memory")
        delta = cast(rne_div((integer(v[col]) - memory) * beta, 1 << 30,
                             stats, "delta.error"), "delta.error")
        for row in range(n):
            update = rne_div(kn[row] * delta, 1 << 30, stats, "delta.outer")
            updated[row][col] = cast(decayed[row][col] + update, "delta.state.updated")
        output.append(cast(rne_div(sum(updated[row][col] * qn[row] for row in range(n)),
                                   1 << 30, stats, "delta.readout"), "delta.readout"))
    return updated, output


def delta_int32_step(state: Sequence[Sequence[int]], q: Sequence[int], k: Sequence[int],
                     v: Sequence[int], decay_q30: int, beta_q30: int, qkv_frac: int = 20,
                     state_frac: int = 20, output_frac: int = 20,
                     stats: Stats | None = None) -> tuple[list[list[int]], list[int]]:
    """Candidate signed INT32 state/memory/error/readout; Q30 normalized q/k.

    decay/beta are captured gates, already quantized to unsigned Q30 in [0,1].
    Their exp/softplus/sigmoid generation is not claimed by this update kernel.
    """
    state = [vector(row) for row in state]
    qn, kn = _normalized_qk(q, k, qkv_frac, stats)
    v = [rescale(x, qkv_frac, state_frac, stats, "delta.v", 32) for x in vector(v)]
    updated, out = _delta_update(state, qn, kn, v, decay_q30, beta_q30, 32, stats)
    out = [rescale(x, state_frac, output_frac, stats, "delta.output", 32) for x in out]
    return updated, out


def delta_bfp_step(state: BFP, q: Sequence[int], k: Sequence[int], v: Sequence[int],
                   decay_q30: int, beta_q30: int, qkv_frac: int = 20,
                   stats: Stats | None = None) -> tuple[BFP, BFP]:
    """Experimental per-head BFP32 state with exact wide internal alignment.

    Exponents may coarsen after update; no INT32 internal width or hardware
    representation is approved by this mathematical reference.
    """
    n = len(q)
    if state.bits != 32 or len(state.values) != n * n:
        raise ValueError("BFP32 square DeltaNet state required")
    qn, kn = _normalized_qk(q, k, qkv_frac, stats)
    exponent = min(state.exponent, -qkv_frac)
    wide = [x << (state.exponent - exponent) for x in state.values]
    rows = [wide[row * n:(row + 1) * n] for row in range(n)]
    v_wide = [shift_rne(x, -qkv_frac - exponent) for x in vector(v)]
    updated, out = _delta_update(rows, qn, kn, v_wide, decay_q30, beta_q30, None, stats)
    return (pack_bfp([x for row in updated for x in row], exponent, 32, stats, "delta.bfp.state"),
            pack_bfp(out, exponent, 32, stats, "delta.bfp.output"))


def split_for_sample(sample_id: str) -> str:
    if type(sample_id) is not str or not sample_id or len(sample_id) > 256:
        raise ValueError("nonempty sequence-level sample_id required")
    digest = hashlib.sha256((SPLIT_VERSION + "\0" + sample_id).encode()).digest()
    return "heldout" if int.from_bytes(digest, "big") % 5 == 0 else "calibration"


def _fraction(value: object) -> Fraction:
    if type(value) is int:
        return Fraction(value)
    if type(value) is Decimal and value.is_finite():
        return Fraction(value)
    raise ValueError("raw trace values must be finite JSON integers/decimals")


def _quantize_fraction(value: Fraction, exponent: int, stats: Stats, stage: str) -> int:
    numerator, denominator = value.numerator, value.denominator
    if exponent < 0:
        numerator <<= -exponent
    else:
        denominator <<= exponent
    return saturate(rne_div(numerator, denominator, stats, stage), 16, stats, stage)


def _scale_for_peak(peak: Fraction) -> int:
    # Fixed one-bit headroom policy, chosen using calibration samples only.
    for exponent in range(-128, 128):
        bound = Fraction(16383 << exponent) if exponent >= 0 else Fraction(16383, 1 << -exponent)
        if peak <= bound:
            return exponent
    raise ValueError("raw range exceeds candidate exponent domain")


def calibrate_trace(document: dict) -> dict:
    if document.get("schema") != "agent-raw-numeric-trace-v1":
        raise ValueError("raw numeric trace schema required")
    source = document.get("source", {})
    if (source.get("dataset_role") != "calibration_candidates" or
            source.get("capture_kind") != "raw_model_inputs_and_state"):
        raise ValueError("evaluation/projected-only captures cannot select scales")
    for key in ("model_sha256", "source_sha256"):
        if not re.fullmatch("[0-9a-f]{64}", str(source.get(key, ""))):
            raise ValueError("model and dirty-source identities required")
    records = document.get("records")
    if type(records) is not list or not records:
        raise ValueError("nonempty raw records required")
    assignments, groups, identities = {}, {}, set()
    ignored_tags = set()
    for record in records:
        sample = record["sample_id"]
        split = split_for_sample(sample)
        assignments[sample] = split
        for key in ("token", "layer", "ordinal"):
            checked_range(record[key], 0, (1 << 31) - 1)
        tag = record["tag"]
        if type(tag) is not str:
            raise ValueError("trace tag required")
        identity = (sample, record["token"], record["layer"], tag, record["ordinal"])
        if identity in identities:
            raise ValueError("duplicate trace record identity")
        identities.add(identity)
        shape = record["shape"]
        if type(shape) is not list or not shape or any(type(d) is not int or d <= 0 for d in shape):
            raise ValueError("positive raw tensor shape required")
        values = record["values"]
        if type(values) is not list or prod(shape) != len(values):
            raise ValueError("raw shape/value mismatch")
        if tag not in RAW_TAGS:
            ignored_tags.add(tag)
            continue
        if len(values) != RAW_COUNTS[tag]:
            raise ValueError("raw tensor size differs from model trace; projection is not a substitute")
        group = f"{record['layer']}:{tag}"
        entry = groups.setdefault(group, {"tag": tag, "layer": record["layer"],
                                         "calibration": [], "heldout": []})
        entry[split].append(record)
    missing = {split: sorted(RAW_TAGS - {g["tag"] for g in groups.values() if g[split]})
               for split in ("calibration", "heldout")}
    output = {}
    for key, entry in sorted(groups.items()):
        if not entry["calibration"]:
            output[key] = {"status": "NO_CALIBRATION_INPUT", "scale": None}
            continue
        calibration = (_fraction(v) for record in entry["calibration"] for v in record["values"])
        peak = max(abs(v) for v in calibration)
        exponent = _scale_for_peak(peak) if peak else 0
        summary = {"status": "EXPERIMENTAL_SCALE", "scale": {"mantissa_bits": 16,
                    "exponent": exponent, "headroom_bits": 1}, "calibration_peak_exact": str(peak)}
        for split in ("calibration", "heldout"):
            stats = Stats()
            count = 0
            for record in entry[split]:
                for value in record["values"]:
                    _quantize_fraction(_fraction(value), exponent, stats, split)
                    count += 1
            summary[split] = {"value_count": count, **stats.report()}
        output[key] = summary
    manifest = dict(sorted(assignments.items()))
    return {"reference_version": REFERENCE_VERSION,
            "status": "INCOMPLETE_RAW_COVERAGE" if any(missing.values()) else "CANDIDATE_CALIBRATION_ONLY",
            "source": source, "split_version": SPLIT_VERSION, "sample_assignments": manifest,
            "split_manifest_sha256": digest_json(manifest), "missing_raw_tags": missing,
            "ignored_projection_tags": sorted(ignored_tags), "groups": output,
            "scale_policy": "power-of-two, INT16, one guard bit, calibration peak only",
            "heldout_policy": "measure frozen scales only; never use heldout to choose exponents",
            "coverage_limit": "observed layer/tag groups; does not assert full model/layer/head coverage"}


def _f32_bits_fraction(bits: int) -> Fraction:
    """Decode finite IEEE754 bits exactly, offline capture ingestion only."""
    checked_range(bits, 0, (1 << 32) - 1)
    exponent, mantissa = (bits >> 23) & 255, bits & 0x7fffff
    if exponent == 255:
        raise ValueError("nonfinite raw float32")
    if exponent:
        mantissa |= 1 << 23
    shift = exponent - 150 if exponent else -149
    mantissa = -mantissa if bits >> 31 else mantissa
    return Fraction(mantissa << shift) if shift >= 0 else Fraction(mantissa, 1 << -shift)


def _f32_quantization_counts(raw: bytes, output_exponent: int) -> dict[str, int]:
    """Vectorized INTEGER bit-field RNE accounting, not floating quantization.

    Float32 mantissas have at most 24 bits. Right shifts >25 always round to
    zero; left shifts >15 always saturate nonzero INT16. This bounds all NumPy
    shifts and avoids overflow while preserving exact scalar RNE semantics.
    """
    import numpy as np
    checked_range(output_exponent, -128, 127)
    words = np.frombuffer(raw, dtype="<u4")
    exponents = ((words >> 23) & 255).astype(np.int64)
    if np.any(exponents == 255):
        raise ValueError("nonfinite raw float32")
    mantissa = ((words & 0x7fffff) | np.where(exponents != 0, 1 << 23, 0).astype(np.uint32)).astype(np.uint64)
    negative = (words >> 31) != 0
    limit = np.where(negative, 32768, 32767).astype(np.uint64)
    shift = np.where(exponents != 0, exponents - 150, -149) - output_exponent
    right = np.clip(-shift, 1, 25).astype(np.uint64)
    denominator = np.left_shift(np.uint64(1), right)
    quotient = np.right_shift(mantissa, right)
    remainder = mantissa & (denominator - np.uint64(1))
    rounded = quotient + ((2 * remainder > denominator) |
                          ((2 * remainder == denominator) & ((quotient & 1) != 0))).astype(np.uint64)
    rounded = np.where(shift < -25, np.uint64(0), rounded)
    left_shift = np.clip(shift, 0, 15).astype(np.uint64)
    left_limit = np.where(shift > 15, np.uint64(0), np.right_shift(limit, left_shift))
    nonzero = mantissa != 0
    return {"saturation": int(np.count_nonzero(nonzero & np.where(shift >= 0, mantissa > left_limit, rounded > limit))),
            "underflow": int(np.count_nonzero(nonzero & (shift < 0) & (rounded == 0))),
            "inexact": int(np.count_nonzero(nonzero & (shift < 0) & (remainder != 0))),
            "negative_zero": int(np.count_nonzero((words == 0x80000000)))}


def _sha256_file(path: Path) -> str:
    hasher = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(4 << 20), b""):
            hasher.update(chunk)
    return hasher.hexdigest()


def _load_split_manifest(path: Path, expected_sha256: str) -> tuple[dict, dict, str]:
    raw = path.read_bytes()
    actual = hashlib.sha256(raw).hexdigest()
    if not re.fullmatch("[0-9a-f]{64}", expected_sha256) or actual != expected_sha256:
        raise ValueError("frozen split manifest raw-byte identity mismatch")
    manifest = json.loads(raw)
    if (manifest.get("format") != "agent-calibration-seeds-v1" or
            manifest.get("freeze_sha256") != digest_json(manifest.get("samples"))):
        raise ValueError("frozen ordered sample-array digest mismatch")
    samples = {}
    for sample in manifest["samples"]:
        sample_id = sample["sample_id"]
        split_for_sample(sample_id)  # Validate ID syntax only, NEVER select binary split by hash.
        if sample_id in samples or sample["split"] not in ("calibration_seed", "heldout_reporting_only"):
            raise ValueError("duplicate/unknown frozen sample or split")
        checked_range(sample["token_count"], 1, (1 << 31) - 1)
        for key in ("input_sha256", "token_ids_sha256"):
            if not re.fullmatch("[0-9a-f]{64}", str(sample.get(key, ""))):
                raise ValueError("frozen sample content identity missing")
        samples[sample_id] = sample
    if {sample["split"] for sample in samples.values()} != {"calibration_seed", "heldout_reporting_only"}:
        raise ValueError("both frozen splits required")
    return manifest, samples, actual


def calibrate_binary(index_path: Path, values_path: Path, split_path: Path,
                     source_path: Path, split_sha256: str) -> dict:
    """Two-pass streaming raw binary calibration, frozen seed/heldout split.

    Every physical byte is covered once in index order. Overlapping/reused ranges,
    duplicate logical records, source aliases, per-chunk hash failures, nonfinite
    values and source/sample identity mismatches fail closed. The complete input
    files are hashed in both passes; no multi-GB values JSON is materialized.
    """
    import numpy as np
    inputs = [Path(path).resolve(strict=True) for path in (index_path, values_path, split_path, source_path)]
    if any(part.casefold() == "evaluation" for path in inputs for part in path.parts):
        raise ValueError("independent evaluation inputs excluded from calibration")
    for i, path in enumerate(inputs):
        for other in inputs[:i]:
            if path.samefile(other):
                raise ValueError("binary/index/manifest/source physical file alias")
    index_path, values_path, split_path, source_path = inputs
    manifest, samples, manifest_sha = _load_split_manifest(split_path, split_sha256)
    source_raw = source_path.read_bytes()
    source = json.loads(source_raw)
    source_sha = hashlib.sha256(source_raw).hexdigest()
    if (source.get("dataset_role") != "calibration_candidates" or
            source.get("capture_kind") != "raw_model_inputs_and_state"):
        raise ValueError("raw calibration source declaration required")
    for key in ("model_sha256", "source_sha256"):
        if not re.fullmatch("[0-9a-f]{64}", str(source.get(key, ""))):
            raise ValueError("model/dirty-source identity missing")
    declared = {}
    for sample in source.get("samples", []):
        sample_id = sample["sample_id"]
        if sample_id in declared or sample_id not in samples:
            raise ValueError("unknown or duplicate captured sample identity")
        if any(sample.get(key) != samples[sample_id][key] for key in ("input_sha256", "token_ids_sha256")):
            raise ValueError("captured sample content differs from frozen sample")
        declared[sample_id] = sample
    if not declared:
        raise ValueError("source must bind sample IDs to frozen input/token identities")
    if source.get("split_manifest_sha256", manifest_sha) != manifest_sha:
        raise ValueError("source split manifest differs")
    binary_stat = values_path.stat()
    groups, observed, sample_tags, sample_tokens = {}, set(), {}, {}
    first_hashes = None
    ignored_tags = set()
    record_total = 0
    for pass_number in (0, 1):
        index_digest, binary_digest = hashlib.sha256(), hashlib.sha256()
        next_offset = 0
        logical_ids = set()
        with index_path.open("rb") as index_file, values_path.open("rb") as binary_file:
            for line_number, line in enumerate(index_file, 1):
                index_digest.update(line)
                try:
                    record = json.loads(line)
                    sample_id, tag = record["sample_id"], record["tag"]
                    if sample_id not in declared or type(tag) is not str:
                        raise ValueError("record has undeclared sample/tag")
                    token = record.get("token", record.get("position"))
                    checked_range(token, 0, samples[sample_id]["token_count"] - 1)
                    if "position" in record and record["position"] != token:
                        raise ValueError("token/position aliases disagree")
                    layer = checked_range(record["layer"], 0, 1024)
                    ordinal = checked_range(record["head_seq"], 0, (1 << 63) - 1)
                    identity = (sample_id, token, layer, ordinal, tag)
                    if identity in logical_ids:
                        raise ValueError("logical record alias")
                    logical_ids.add(identity)
                    shape = record["shape"]
                    if type(shape) is not list or not shape or any(type(x) is not int or x <= 0 for x in shape):
                        raise ValueError("raw tensor shape missing")
                    count = prod(shape)
                    checked_range(count, 1, 1 << 24)
                    if "count" in record and record["count"] != count:
                        raise ValueError("shape/count aliases disagree")
                    offset = checked_range(record["offset_bytes"], 0, binary_stat.st_size)
                    size = checked_range(record["bytes"], 4, 1 << 26)
                    if offset != next_offset or size != count * 4 or offset + size > binary_stat.st_size:
                        raise ValueError("overlap/gap/reused binary alias or invalid range")
                    if tag in RAW_TAGS and count != RAW_COUNTS[tag]:
                        raise ValueError("raw shape differs from model; projections cannot substitute")
                    raw = binary_file.read(size)
                    chunk_sha = hashlib.sha256(raw).hexdigest()
                    if len(raw) != size or record["sha256"] != chunk_sha:
                        raise ValueError("binary record bytes/hash mismatch")
                    binary_digest.update(raw)
                    next_offset += size
                    words = np.frombuffer(raw, dtype="<u4")
                    magnitude_bits = words & 0x7fffffff
                    if np.any((magnitude_bits >> 23) == 255):
                        raise ValueError("nonfinite raw float32")
                    if pass_number == 0:
                        observed.add(sample_id)
                        sample_tokens.setdefault(sample_id, set()).add(token)
                        sample_tags.setdefault(sample_id, set()).add(tag)
                    if tag not in RAW_TAGS:
                        ignored_tags.add(tag)
                        continue
                    key = f"{layer}:{tag}"
                    split = "calibration" if samples[sample_id]["split"] == "calibration_seed" else "heldout"
                    if pass_number == 0:
                        entry = groups.setdefault(key, {"layer": layer, "tag": tag, "calibration_peak_bits": 0,
                            "calibration": {"records": 0, "values": 0, "counts": {}},
                            "heldout": {"records": 0, "values": 0, "counts": {}}})
                        entry[split]["records"] += 1
                        entry[split]["values"] += count
                        if split == "calibration":
                            entry["calibration_peak_bits"] = max(entry["calibration_peak_bits"], int(magnitude_bits.max()))
                    else:
                        entry = groups[key]
                        if entry["scale"] is not None:
                            counts = _f32_quantization_counts(raw, entry["scale"]["exponent"])
                            for kind, amount in counts.items():
                                entry[split]["counts"][kind] = entry[split]["counts"].get(kind, 0) + amount
                except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
                    raise ValueError(f"binary index line {line_number}: {error}") from error
            if binary_file.read(1) or next_offset != binary_stat.st_size:
                raise ValueError("unindexed binary tail or truncated index")
        if not logical_ids:
            raise ValueError("empty binary capture/index")
        hashes = (index_digest.hexdigest(), binary_digest.hexdigest(), len(logical_ids))
        if pass_number == 0:
            first_hashes = hashes
            record_total = hashes[2]
            for entry in groups.values():
                peak_bits = entry.pop("calibration_peak_bits")
                peak = _f32_bits_fraction(peak_bits)
                entry["calibration_peak_ieee754_hex"] = f"{peak_bits:08x}"
                entry["calibration_peak_exact"] = str(peak)
                entry["scale"] = ({"mantissa_bits": 16, "exponent": _scale_for_peak(peak) if peak else 0,
                                   "headroom_bits": 1} if entry["calibration"]["values"] else None)
        elif hashes != first_hashes:
            raise ValueError("binary/index identity changed across calibration/report passes")
    if (_sha256_file(split_path) != manifest_sha or _sha256_file(source_path) != source_sha or
            values_path.stat().st_size != binary_stat.st_size):
        raise ValueError("input identities changed during calibration")
    missing_samples = sorted(set(samples) - observed)
    missing_tags = {sample_id: sorted(RAW_TAGS - sample_tags.get(sample_id, set())) for sample_id in samples}
    missing_tokens = {sample_id: sorted(set(range(info["token_count"])) - sample_tokens.get(sample_id, set()))
                      for sample_id, info in samples.items()}
    incomplete = bool(missing_samples or any(missing_tags.values()) or any(missing_tokens.values()))
    return {"reference_version": REFERENCE_VERSION,
            "status": "INCOMPLETE_RAW_COVERAGE" if incomplete else "CANDIDATE_RAW_RANGE_ONLY",
            "source": source, "source_file_sha256": source_sha,
            "frozen_split": {"manifest_path": str(split_path), "manifest_file_sha256": manifest_sha,
                             "ordered_samples_freeze_sha256": manifest["freeze_sha256"],
                             "assignments": {key: value["split"] for key, value in samples.items()}},
            "binary_identity": {"path": str(values_path), "bytes": binary_stat.st_size,
                                "sha256": first_hashes[1], "device": binary_stat.st_dev,
                                "inode": binary_stat.st_ino, "dtype": "IEEE754-float32-little-endian",
                                "index_path": str(index_path), "index_sha256": first_hashes[0],
                                "records": record_total, "alias_policy": "distinct inputs; contiguous unique physical ranges and logical records; hash every record in two passes"},
            "observed_samples": sorted(observed), "missing_samples": missing_samples,
            "missing_raw_tags_by_sample": missing_tags, "missing_token_positions_by_sample": missing_tokens,
            "ignored_projection_tags": sorted(ignored_tags), "groups": dict(sorted(groups.items())),
            "scale_policy": "INT16 power-of-two one-bit headroom, calibration_seed peaks only",
            "heldout_policy": "heldout_reporting_only observes frozen scale errors; never fits or expands scales",
            "statistics_arithmetic": "IEEE754 integer bit fields; exact scalar RNE-equivalent, no float quantizer",
            "coverage_limit": "raw range evidence only; not full layer/head alignment or graph/model quality validation"}


def run_integer_case(document: dict, lut: ExpLUT | None = None) -> dict:
    """Execute integer JSON operands; no float-to-fixed adapter or hardware input."""
    if document.get("schema") != "agent-integer-case-v1":
        raise ValueError("integer case schema required")
    kernels = {function.__name__: function for function in (
        quantize_a8, w4_g128_a8_dot, requantize_groups, residual_int16, residual_bfp,
        rms_norm, softmax_lut, silu_integer, causal_conv_step, delta_int32_step, delta_bfp_step)}
    name = document.get("kernel")
    if name not in kernels or type(document.get("parameters")) is not dict:
        raise ValueError("known kernel and parameter object required")
    params = dict(document["parameters"])
    if "stats" in params or "lut" in params or "activation_lut" in params:
        raise ValueError("execution context cannot be overridden")
    if name in ("softmax_lut", "silu_integer"):
        if lut is None:
            raise ValueError("pre-generated versioned LUT required")
        params["lut"] = lut
    if name == "causal_conv_step":
        apply_silu = params.pop("apply_silu", False)
        if type(apply_silu) is not bool or (apply_silu and lut is None):
            raise ValueError("integer conv SiLU requires pre-generated LUT")
        params["activation_lut"] = lut if apply_silu else None
    if name == "w4_g128_a8_dot":
        if any(type(row) is not str for row in params["packed_rows"]):
            raise ValueError("packed rows must be hex byte strings")
        params["packed_rows"] = [bytes.fromhex(row) for row in params["packed_rows"]]
    if name in ("quantize_a8", "requantize_groups"):
        scale_key = "scale" if name == "quantize_a8" else "activation_scale"
        params[scale_key] = Dyadic(**params[scale_key])
    if name == "requantize_groups":
        params["weight_scales"] = [[Dyadic(**scale) for scale in row] for row in params["weight_scales"]]
    if name == "residual_bfp":
        params["left"], params["right"] = BFP(**params["left"]), BFP(**params["right"])
    if name == "delta_bfp_step":
        params["state"] = BFP(**params["state"])
    stats = Stats()
    result = kernels[name](**params, stats=stats)

    def encode(value):
        if isinstance(value, BFP):
            return {"values": list(value.values), "exponent": value.exponent, "bits": value.bits}
        if isinstance(value, (list, tuple)):
            return [encode(item) for item in value]
        return integer(value)

    return {"reference_version": REFERENCE_VERSION, "status": "EXPERIMENTAL_INTEGER_KERNEL_RESULT",
            "kernel": name, "output": encode(result), "statistics": stats.report(),
            "lut_sha256": lut.artifact()["sha256"] if lut else None,
            "format_metadata": metadata()}


def metadata() -> dict:
    return {"reference_version": REFERENCE_VERSION, "status": "EXPERIMENTAL_CANDIDATE_NOT_APPROVED",
            "rounding": "signed round-nearest ties-even", "overflow": "explicit saturation only",
            "intermediates": "arbitrary-precision mathematical integers; hardware widths unapproved",
            "w4": {"signed": [-8, 7], "group": 128, "nibble_order": "low first",
                   "a8": [-128, 127], "group_partial_bits": 32,
                   "reduction": "RNE per group to common dyadic exponent; exact sum; output saturation"},
            "residual": "INT16 align/add/one RNE; BFP16 shared signed exponent [-128,127]",
            "norm": {"epsilon_exact": [1, 1000000], "centered_gain": "1+weight",
                     "uncentered_gain": "weight", "inverse_frac": 30,
                     "sqrt": "integer isqrt plus exact midpoint-square comparison"},
            "softmax": {"lut_version": LUT_VERSION, "domain": "max-relative [-16,0]",
                        "normalization": "global; independent RNE probabilities; no mass correction"},
            "conv": "causal oldest-to-newest raw history; optional integer LUT SiLU",
            "delta": "L2 q/k epsilon, q/sqrt(d), decay then prediction/error/outer/readout; INT32 or per-head BFP32 candidate",
            "gate_inputs": "decay and beta from raw trace quantized Q30; generation outside update kernel",
            "calibration": {"split_version": SPLIT_VERSION, "heldout_bucket": "SHA256 modulo5 == 0",
                            "scope": "sequence sample_id; calibration_candidates only",
                            "raw_tags_required": sorted(RAW_TAGS), "raw_value_counts": RAW_COUNTS,
                            "schema": "agent-raw-numeric-trace-v1",
                            "source_fields": ["dataset_role=calibration_candidates",
                                              "capture_kind=raw_model_inputs_and_state", "model_sha256", "source_sha256"],
                            "record_fields": ["sample_id", "token", "layer", "ordinal", "tag", "shape", "values"],
                            "binary_mode": {"split": "external frozen calibration_seed/heldout_reporting_only manifest",
                                            "index": ["sample_id", "token", "layer", "head_seq", "tag", "shape", "offset_bytes", "bytes", "sha256"],
                                            "values": "raw IEEE754 float32 little endian; contiguous no-alias coverage",
                                            "identity": "expected manifest raw SHA256 + ordered samples freeze digest + source/sample/input digests",
                                            "scans": "two streaming passes, record/index/binary hashes stable"}},
            "gaps": ["no full integer graph, token generation or model-quality claim",
                     "no approved NPU accumulator/BFP/storage format or hardware equivalence",
                     "gate softplus/exp generation, RoPE and complete KV/attention graph not integrated",
                     "model weight quantization/export and all model-layer scale coverage not proven",
                     "synthetic numerical tests are not real-model trace calibration or evaluation"]}


def write_json(path: Path, data: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def _reject_output_alias(output: Path, inputs: Sequence[Path | None]) -> None:
    for path in inputs:
        if path is not None and (output.resolve() == path.resolve() or
                (output.exists() and path.exists() and output.samefile(path))):
            raise ValueError("output aliases an input evidence file")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for command in ("metadata", "lut", "calibrate", "run"):
        child = commands.add_parser(command)
        child.add_argument("--out", required=True, type=Path)
        if command == "calibrate":
            capture = child.add_mutually_exclusive_group(required=True)
            capture.add_argument("--trace", type=Path, help="legacy JSON values capture")
            capture.add_argument("--index", type=Path, help="streaming binary trace JSONL index")
            child.add_argument("--values", type=Path)
            child.add_argument("--split-manifest", type=Path)
            child.add_argument("--split-sha256", help="expected frozen manifest raw-byte SHA256")
            child.add_argument("--source", type=Path, help="raw capture/model/source/sample identity")
        if command == "run":
            child.add_argument("--case", required=True, type=Path)
            child.add_argument("--lut", type=Path, help="offline generated LUT artifact")
    args = parser.parse_args(argv)
    if args.command == "calibrate":
        _reject_output_alias(args.out, (args.trace, args.index, args.values, args.split_manifest, args.source))
    if args.command == "run":
        _reject_output_alias(args.out, (args.case, args.lut))
    if args.command == "metadata":
        result = metadata()
    elif args.command == "lut":
        result = generate_exp_lut().artifact()
    elif args.command == "run":
        raw = args.case.read_bytes()
        document = json.loads(raw, parse_float=Decimal)
        lut = load_exp_lut(json.loads(args.lut.read_text(encoding="utf-8"))) if args.lut else None
        result = run_integer_case(document, lut)
        result["integer_case_sha256"] = hashlib.sha256(raw).hexdigest()
    elif args.index is not None:
        if not all((args.values, args.split_manifest, args.split_sha256, args.source)):
            parser.error("binary calibration requires --values, --split-manifest, --split-sha256, --source")
        result = calibrate_binary(args.index, args.values, args.split_manifest, args.source, args.split_sha256)
    else:
        if any((args.values, args.split_manifest, args.split_sha256, args.source)):
            parser.error("binary-only arguments require --index; legacy JSON split policy remains explicit")
        if any(part.casefold() == "evaluation" for part in args.trace.resolve().parts):
            parser.error("the independent evaluation directory is excluded from calibration")
        raw = args.trace.read_bytes()
        document = json.loads(raw, parse_float=Decimal,
                              parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite trace")))
        result = calibrate_trace(document)
        result["capture_file_sha256"] = hashlib.sha256(raw).hexdigest()
    if args.command != "lut":
        result["implementation_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    write_json(args.out, result)
    print(f"{args.command}: {result.get('status', LUT_VERSION)} -> {args.out}")
    return 2 if result.get("status") == "INCOMPLETE_RAW_COVERAGE" else 0


if __name__ == "__main__":
    raise SystemExit(main())
