#!/usr/bin/env python3
"""Independent integer graph numerical pieces; NOT an integer model graph or NPU ABI.

This module does not import model weights, hardware code or frozen evaluation.
Only the OFFLINE RoPE Q30 table generator uses Decimal transcendental arithmetic.
Execution operations accept exact integers/BF16 bit patterns and use integer RNE.
The candidate BFP export format is analytical evidence, not approved storage.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from decimal import Decimal, ROUND_HALF_EVEN, localcontext
import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Sequence

from agent_integer_reference import Stats, integer, checked_range, shift_rne, saturate, pack_bfp, digest_json

GRAPH_VERSION = "agent-integer-graph-ops-v1"
ROPE_VERSION = "rope-q30-theta10000000-rotary64-pos0-v1"
BFP_VERSION = "candidate-per-head-bfp-v1-NOT-APPROVED"
ROPE_CONTEXTS = (128, 1024, 2048)
Q30 = 1 << 30
BFP_HEADER = struct.Struct("<4sBBBBIII")
BFP_TAGS = frozenset({"linear.delta.state.before", "linear.delta.state.after",
                      "linear.delta.q", "linear.delta.k", "linear.delta.v",
                      "attention.key.state", "attention.value.state",
                      "linear.conv.state.before", "linear.conv.state.after"})


def _sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sincos_step(angle: Decimal) -> tuple[Decimal, Decimal]:
    """OFFLINE Taylor generator at 95-digit precision, angle <= 1 radian."""
    sine, cosine = angle, Decimal(1)
    sine_term, cosine_term = angle, Decimal(1)
    square = angle * angle
    for n in range(1, 100):
        sine_term *= -square / Decimal((2 * n) * (2 * n + 1))
        cosine_term *= -square / Decimal((2 * n - 1) * 2 * n)
        sine += sine_term
        cosine += cosine_term
        if abs(sine_term) < Decimal("1e-90") and abs(cosine_term) < Decimal("1e-90"):
            break
    else:
        raise ArithmeticError("offline trig series did not converge")
    return cosine, sine


def _q30(value: Decimal) -> int:
    return int((value * Q30).to_integral_value(rounding=ROUND_HALF_EVEN))


@dataclass(frozen=True)
class RoPETable:
    context_tokens: int
    rows: tuple[tuple[tuple[int, int], ...], ...]
    version: str = ROPE_VERSION

    def __post_init__(self):
        if self.version != ROPE_VERSION or self.context_tokens not in ROPE_CONTEXTS:
            raise ValueError("unsupported RoPE version/context")
        if len(self.rows) != self.context_tokens:
            raise ValueError("truncated RoPE position domain")
        for row in self.rows:
            if len(row) != 32:
                raise ValueError("rotary64 needs 32 sincos pairs")
            for pair in row:
                if len(pair) != 2 or any(type(x) is not int or abs(x) > Q30 for x in pair):
                    raise ValueError("invalid signed Q30 sincos pair")
        if any(pair != (Q30, 0) for pair in self.rows[0]):
            raise ValueError("position zero is not identity")

    def payload(self) -> dict:
        return {"version": self.version, "context_tokens": self.context_tokens,
                "max_position": self.context_tokens - 1, "position_base": 0,
                "theta": 10000000, "rotary_dim": 64, "fraction_bits": 30,
                "pairing": "(i,i+32) for i in [0,31]; remaining dims unchanged",
                "rounding": "RNE each output pair once",
                "rows": [[list(pair) for pair in row] for row in self.rows]}

    def artifact(self) -> dict:
        data = self.payload()
        data["sha256"] = digest_json(data)
        return data


def generate_rope_table(context_tokens: int) -> RoPETable:
    """Decimal-only OFFLINE generation; context length never gets truncated."""
    if type(context_tokens) is not int or context_tokens not in ROPE_CONTEXTS:
        raise ValueError("supported contexts are exactly 128, 1024, 2048")
    rows = [[] for _ in range(context_tokens)]
    with localcontext() as context:
        context.prec = 95
        theta = Decimal(10000000)
        for i in range(32):
            frequency = theta ** (Decimal(-2 * i) / Decimal(64))
            cstep, sstep = _sincos_step(frequency)
            cosine, sine = Decimal(1), Decimal(0)
            for position in range(context_tokens):
                rows[position].append((_q30(cosine), _q30(sine)))
                cosine, sine = cosine * cstep - sine * sstep, sine * cstep + cosine * sstep
    return RoPETable(context_tokens, tuple(tuple(row) for row in rows))


def load_rope_table(artifact: dict) -> RoPETable:
    if type(artifact) is not dict:
        raise ValueError("RoPE artifact object required")
    data = dict(artifact)
    digest = data.pop("sha256", None)
    if digest != digest_json(data) or set(data) != {"version", "context_tokens", "max_position",
             "position_base", "theta", "rotary_dim", "fraction_bits", "pairing", "rounding", "rows"}:
        raise ValueError("RoPE artifact identity/shape mismatch")
    if (data["theta"] != 10000000 or data["rotary_dim"] != 64 or data["fraction_bits"] != 30 or
        data["position_base"] != 0 or data["max_position"] != data["context_tokens"] - 1 or
        data["pairing"] != "(i,i+32) for i in [0,31]; remaining dims unchanged" or
        data["rounding"] != "RNE each output pair once"):
        raise ValueError("RoPE domain/semantics mismatch")
    return RoPETable(data["context_tokens"], tuple(tuple(tuple(pair) for pair in row)
                                                   for row in data["rows"]), data["version"])


def rope_apply_q30(values: Sequence[int], position: int, table: RoPETable,
                   bits: int = 32, stats: Stats | None = None) -> list[int]:
    """Pure integer pairing from qw_rope: angle=position*theta**(-2*i/64).

    Require caller's original absolute position. No modulo position, context
    shrink, alias or silent retry of an out-of-domain position is permitted.
    """
    if type(position) is not int or not 0 <= position < table.context_tokens:
        raise ValueError("RoPE absolute position outside generated context domain")
    checked_range(bits, 2, 64)
    if len(values) < 64:
        raise ValueError("RoPE input must include 64 rotary lanes")
    lower, upper = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    result = [checked_range(integer(value), lower, upper) for value in values]
    for i, (cosine, sine) in enumerate(table.rows[position]):
        a, b = result[i], result[i + 32]
        result[i] = saturate(shift_rne(a * cosine - b * sine, -30, stats, "rope.rne"),
                             bits, stats, "rope.output")
        result[i + 32] = saturate(shift_rne(b * cosine + a * sine, -30, stats, "rope.rne"),
                                  bits, stats, "rope.output")
    return result


@dataclass(frozen=True)
class BF16Value:
    mantissa: int
    exponent: int


def bf16_decode(bits: int, stats: Stats | None = None) -> BF16Value:
    """Return exact signed mantissa * 2**exponent (negative zero maps to zero)."""
    bits = checked_range(bits, 0, 65535)
    exponent, fraction = (bits >> 7) & 255, bits & 127
    if exponent == 255:
        raise ValueError("BF16 nonfinite values prohibited")
    if not exponent:
        mantissa, scale = fraction, -133
    else:
        mantissa, scale = 128 + fraction, exponent - 134
    if bits & 0x8000:
        mantissa = -mantissa
        if mantissa == 0 and stats:
            stats.hit("negative_zero", "bf16.decode")
    return BF16Value(mantissa, scale) if mantissa else BF16Value(0, 0)


def _bf16_row(packed: bytes, width: int, stats: Stats | None = None) -> list[BF16Value]:
    if type(packed) is not bytes or type(width) is not int or width <= 0 or len(packed) != 2 * width:
        raise ValueError("BF16 little-endian row length mismatch")
    return [bf16_decode(packed[i] | (packed[i + 1] << 8), stats)
            for i in range(0, len(packed), 2)]


def bf16_embedding_slice(packed_row: bytes, output_exponent: int, output_bits: int = 32,
                         stats: Stats | None = None) -> list[int]:
    """Decode one raw BF16 embedding row without materializing the vocabulary."""
    checked_range(output_exponent, -256, 256)
    if type(packed_row) is not bytes or not packed_row or len(packed_row) % 2:
        raise ValueError("one nonempty BF16 embedding row required")
    row = _bf16_row(packed_row, len(packed_row) // 2, stats)
    return [saturate(shift_rne(value.mantissa, value.exponent - output_exponent,
                               stats, "bf16.embedding.rne"),
                     output_bits, stats, "bf16.embedding.output") for value in row]


def bf16_embedding_row(matrix: bytes, token_id: int, vocab: int, width: int,
                       output_exponent: int, output_bits: int = 32,
                       stats: Stats | None = None) -> list[int]:
    """Tied-model embedding row from raw BF16 bits, no float conversion."""
    checked_range(vocab, 1, 248320)
    checked_range(width, 1, 4096)
    checked_range(token_id, 0, vocab - 1)
    checked_range(output_exponent, -256, 256)
    if type(matrix) is not bytes or len(matrix) != vocab * width * 2:
        raise ValueError("contiguous BF16 embedding matrix length mismatch")
    offset = token_id * width * 2
    return bf16_embedding_slice(matrix[offset:offset + width * 2], output_exponent, output_bits, stats)


def bf16_head_dot_row(packed_row: bytes, activation: Sequence[int], input_frac: int,
                      output_exponent: int, output_bits: int = 32,
                      stats: Stats | None = None) -> int:
    """Exact integer accumulation before one final RNE/saturation.

    This is NOT the model's sequential FP32 accumulation contract and does not
    silently quantize the BF16 weights to W4; it is an independent oracle.
    """
    checked_range(input_frac, 0, 60)
    checked_range(output_exponent, -256, 256)
    weights = _bf16_row(packed_row, len(activation), stats)
    operands = [integer(value) for value in activation]
    active = [(weight, value) for weight, value in zip(weights, operands) if weight.mantissa and value]
    if not active:
        return 0
    minimum = min(weight.exponent for weight, _ in active)
    accumulator = sum((weight.mantissa * value) << (weight.exponent - minimum) for weight, value in active)
    if stats:
        stats.observe(accumulator, "bf16.head.accumulator")
    result = shift_rne(accumulator, minimum - input_frac - output_exponent, stats, "bf16.head.rne")
    return saturate(result, output_bits, stats, "bf16.head.output")


def bf16_head_selected(matrix: bytes, row_ids: Sequence[int], activations: Sequence[int],
                       vocab: int, input_frac: int, output_exponent: int,
                       output_bits: int = 32, stats: Stats | None = None) -> list[dict]:
    """Explicit selected logits only; no claim of full 248320-row head."""
    width = len(activations)
    checked_range(vocab, 1, 248320)
    checked_range(width, 1, 4096)
    if type(matrix) is not bytes or len(matrix) != vocab * width * 2:
        raise ValueError("BF16 tied head matrix shape/stride mismatch")
    if not row_ids or len(row_ids) > vocab or len(set(row_ids)) != len(row_ids):
        raise ValueError("selected row IDs must be unique")
    result = []
    for row_id in row_ids:
        checked_range(row_id, 0, vocab - 1)
        offset = row_id * width * 2
        result.append({"row_id": row_id, "value": bf16_head_dot_row(
            matrix[offset:offset + width * 2], activations, input_frac,
            output_exponent, output_bits, stats)})
    return result


def _identity(identity: dict) -> dict:
    if type(identity) is not dict or set(identity) != {"model_sha256", "source_sha256", "trace_sha256"}:
        raise ValueError("exact model/source/raw trace identity required")
    if any(not re.fullmatch("[0-9a-f]{64}", str(value)) for value in identity.values()):
        raise ValueError("model/source/raw trace SHA256 must be lowercase full digests")
    return dict(identity)


def export_bfp_candidate(values: Sequence[int], shape: Sequence[int], input_exponent: int,
                         bits: int, identity: dict, tag: str,
                         stats: Stats | None = None) -> tuple[bytes, dict]:
    """Candidate per-head BFP16/BFP32 dense row-major export.

    Shape=(head_count,rows,cols), INT input * 2**input_exponent, one signed
    exponent per head. No claim of hardware descriptor compatibility.
    """
    identity = _identity(identity)
    if type(shape) not in (list, tuple) or len(shape) != 3:
        raise ValueError("three-dimensional per-head shape required")
    heads, rows, cols = [checked_range(d, 1, 1 << 24) for d in shape]
    if bits not in (16, 32) or type(bits) is not int or tag not in BFP_TAGS:
        raise ValueError("unapproved bits/tag for candidate state export")
    checked_range(input_exponent, -128, 127)
    if type(values) not in (list, tuple) or len(values) != heads * rows * cols:
        raise ValueError("flat per-head tensor shape/value mismatch")
    source = [integer(value) for value in values]
    stats = stats if stats is not None else Stats()
    packed = [pack_bfp(source[i * rows * cols:(i + 1) * rows * cols], input_exponent, bits,
                       stats, "bfp.export.head") for i in range(heads)]
    header = BFP_HEADER.pack(b"IBFP", 1, bits, 0, 3, heads, rows, cols)
    binding = {"identity": identity, "tag": tag, "shape": [heads, rows, cols],
               "input_exponent": input_exponent, "version": BFP_VERSION}
    binding_hash = digest_json(binding)
    binding_bytes = bytes.fromhex(binding_hash)
    exponent_bytes = b"".join(item.exponent.to_bytes(1, "little", signed=True) for item in packed)
    width = bits // 8
    payload = b"".join(value.to_bytes(width, "little", signed=True)
                       for item in packed for value in item.values)
    blob = header + binding_bytes + exponent_bytes + payload
    data_offset = len(header) + len(binding_bytes) + len(exponent_bytes)
    entries = [{"head": i, "start": data_offset + i * rows * cols * width,
                "end": data_offset + (i + 1) * rows * cols * width,
                "exponent": item.exponent, "scale": f"2**{item.exponent}",
                "elements": rows * cols} for i, item in enumerate(packed)]
    manifest = {"version": BFP_VERSION, "status": "CANDIDATE_NOT_QUALITY_APPROVED",
                "format": "IBFP/v1 experimental dense little-endian; not a hardware ABI",
                "identity": identity, "binding_sha256": binding_hash, "binding_bytes": 32,
                "tensor_tag": tag, "shape": [heads, rows, cols],
                "layout": "[head][row][column] contiguous; no overlap/alias",
                "input": {"dtype": "signed arbitrary-width integers", "exponent": input_exponent},
                "rounding": "signed RNE ties-even independently per head",
                "overflow": "explicit signed saturation at head packing; counters in statistics",
                "bits": bits, "header_bytes": len(header), "exponent_bytes": heads,
                "data_offset": data_offset, "payload_bytes": len(payload),
                "total_bytes": len(blob), "heads": entries,
                "statistics": stats.report(), "sha256": _sha256(blob),
                "gaps": ["candidate format only, no NPU ABI/DDR placement approval",
                         "no full integer graph or model-quality evidence"]}
    return blob, manifest


def verify_bfp_candidate(blob: bytes, manifest: dict) -> list[list[int]]:
    """Independent readback with range/identity checks; fail on aliases/tails."""
    if type(blob) is not bytes or type(manifest) is not dict or len(blob) < BFP_HEADER.size:
        raise ValueError("BFP blob/manifest missing")
    magic, version, bits, axis, ndim, heads, rows, cols = BFP_HEADER.unpack_from(blob)
    if (magic, version, axis, ndim) != (b"IBFP", 1, 0, 3) or bits not in (16, 32):
        raise ValueError("unsupported candidate BFP header")
    identity = _identity(manifest["identity"])
    input_spec = manifest.get("input")
    if (type(input_spec) is not dict or set(input_spec) != {"dtype", "exponent"} or
        input_spec["dtype"] != "signed arbitrary-width integers"):
        raise ValueError("BFP input format mismatch")
    checked_range(input_spec["exponent"], -128, 127)
    binding = {"identity": identity, "tag": manifest.get("tensor_tag"),
               "shape": [heads, rows, cols], "input_exponent": input_spec["exponent"],
               "version": BFP_VERSION}
    binding_digest = digest_json(binding)
    if (manifest.get("binding_sha256") != binding_digest or manifest.get("binding_bytes") != 32 or
            blob[BFP_HEADER.size:BFP_HEADER.size + 32] != bytes.fromhex(binding_digest)):
        raise ValueError("BFP binary/source/tag/shape/input binding mismatch")
    if (manifest.get("version") != BFP_VERSION or manifest.get("tensor_tag") not in BFP_TAGS or
        manifest.get("shape") != [heads, rows, cols] or manifest.get("bits") != bits or
        manifest.get("header_bytes") != BFP_HEADER.size or manifest.get("exponent_bytes") != heads or
        manifest.get("data_offset") != BFP_HEADER.size + 32 + heads or
        manifest.get("total_bytes") != len(blob) or manifest.get("sha256") != _sha256(blob)):
        raise ValueError("BFP manifest/blob identity or layout mismatch")
    width = bits // 8
    expected = BFP_HEADER.size + 32 + heads + heads * rows * cols * width
    if (len(blob) != expected or len(manifest.get("heads", [])) != heads or
        manifest.get("payload_bytes") != heads * rows * cols * width):
        raise ValueError("BFP bytes truncated/aliased/extended")
    decoded = []
    previous = BFP_HEADER.size + 32 + heads
    for head, entry in enumerate(manifest["heads"]):
        exponent_offset = BFP_HEADER.size + 32 + head
        exponent = int.from_bytes(blob[exponent_offset:exponent_offset + 1],
                                  "little", signed=True)
        if (entry != {"head": head, "start": previous, "end": previous + rows * cols * width,
                      "exponent": exponent, "scale": f"2**{exponent}", "elements": rows * cols} or
            entry["end"] > len(blob)):
            raise ValueError("BFP head alias/layout/exponent mismatch")
        decoded.append([int.from_bytes(blob[offset:offset + width], "little", signed=True)
                        for offset in range(previous, entry["end"], width)])
        previous = entry["end"]
    if previous != len(blob):
        raise ValueError("BFP trailing data")
    return decoded


def metadata() -> dict:
    return {"graph_version": GRAPH_VERSION, "status": "EXPERIMENTAL_NUMERICAL_PIECES_ONLY",
            "rope": {"version": ROPE_VERSION, "contexts": list(ROPE_CONTEXTS),
                     "theta": 10000000, "rotary_dim": 64, "fraction_bits": 30,
                     "position": "absolute, zero based, no reset/wrap; out of domain fails",
                     "generator": "offline 95-digit Decimal Taylor/complex recurrence",
                     "execution": "signed integer products, per pair RNE, explicit saturation"},
            "bf16": {"bit_format": "little-endian BF16: 1 sign, 8 exponent, 7 fraction",
                     "embedding": "flat row-major token*hidden+j or single row slice, exact mantissa/exponent",
                     "head": "selected tied BF16 rows, exact wide integer dot then RNE/saturate",
                     "not_w4_head": True, "float_kernel": False},
            "bfp": {"version": BFP_VERSION, "bits": [16, 32],
                    "shape": "head,row,column", "header": "20 bytes: 4s magic, 4 u8, 3 u32le",
                    "exponents": "signed byte per head", "payload": "signed little-endian mantissa, dense",
                    "identity": "32-byte blob binding: model/source/raw trace, tag, shape, input exponent, version",
                    "approval": "none; mathematical candidate only"},
            "gaps": ["not a full integer Qwen graph, no full-vocabulary logits/quality claim",
                     "RoPE table is offline approximation; not exact FP32 libm state match",
                     "no frozen heldout tuning; no hardware timing/ABI endorsement",
                     "BFP export candidate not an approved weight, state or NPU descriptor format"]}


def write_json(path: Path, payload: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, sort_keys=True, indent=2) + "\n", encoding="utf-8")


def _reject_output_alias(output: Path, inputs: Sequence[Path | None]) -> None:
    for path in inputs:
        if path is not None and (output.resolve() == path.resolve() or
                (output.exists() and path.exists() and output.samefile(path))):
            raise ValueError("output aliases source evidence")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("metadata"); p.add_argument("--out", required=True, type=Path)
    p = commands.add_parser("rope-lut"); p.add_argument("--context", type=int, choices=ROPE_CONTEXTS, required=True)
    p.add_argument("--out", required=True, type=Path)
    p = commands.add_parser("run"); p.add_argument("--case", required=True, type=Path)
    p.add_argument("--rope-lut", type=Path); p.add_argument("--out", required=True, type=Path)
    p = commands.add_parser("export-bfp"); p.add_argument("--case", required=True, type=Path)
    p.add_argument("--out", required=True, type=Path); p.add_argument("--manifest", required=True, type=Path)
    args = parser.parse_args(argv)
    if args.command == "metadata":
        write_json(args.out, metadata()); return 0
    if args.command == "rope-lut":
        write_json(args.out, generate_rope_table(args.context).artifact()); return 0
    if args.command == "export-bfp":
        _reject_output_alias(args.out, (args.case, args.manifest))
        _reject_output_alias(args.manifest, (args.case, args.out))
        case = json.loads(args.case.read_text(encoding="utf-8"))
        if case.get("schema") != "agent-graph-bfp-export-v1":
            raise ValueError("BFP case schema missing")
        blob, manifest = export_bfp_candidate(**case["parameters"])
        verify_bfp_candidate(blob, manifest)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_bytes(blob)
        write_json(args.manifest, manifest)
        return 0
    _reject_output_alias(args.out, (args.case, args.rope_lut))
    case = json.loads(args.case.read_text(encoding="utf-8"))
    if case.get("schema") != "agent-graph-integer-case-v1":
        raise ValueError("integer graph case schema missing")
    name = case.get("kernel"); params = case.get("parameters")
    if type(params) is not dict or "stats" in params:
        raise ValueError("integer kernel parameters required; cannot override statistics")
    stats = Stats()
    if name == "rope":
        if args.rope_lut is None:
            raise ValueError("explicit versioned RoPE LUT required")
        raw = json.loads(args.rope_lut.read_text(encoding="utf-8"))
        table = load_rope_table(raw)
        if case.get("expected_lut_sha256") != raw["sha256"]:
            raise ValueError("integer case did not pin RoPE LUT identity")
        output = rope_apply_q30(table=table, stats=stats, **params)
    elif name == "bf16_embedding_slice":
        params = dict(params); params["packed_row"] = bytes.fromhex(params.pop("packed_row_hex"))
        output = bf16_embedding_slice(stats=stats, **params)
    elif name == "bf16_head_row":
        params = dict(params); params["packed_row"] = bytes.fromhex(params.pop("packed_row_hex"))
        output = bf16_head_dot_row(stats=stats, **params)
    elif name == "bf16_embedding":
        params = dict(params); params["matrix"] = bytes.fromhex(params.pop("matrix_hex"))
        output = bf16_embedding_row(stats=stats, **params)
    elif name == "bf16_head_selected":
        params = dict(params); params["matrix"] = bytes.fromhex(params.pop("matrix_hex"))
        output = bf16_head_selected(stats=stats, **params)
    else:
        raise ValueError("unknown integer graph numerical kernel")
    write_json(args.out, {"graph_version": GRAPH_VERSION, "status": "EXPERIMENTAL_NUMERICAL_RESULT",
                          "kernel": name, "output": output, "statistics": stats.report(),
                          "case_sha256": _sha256(args.case.read_bytes()),
                          "rope_lut_sha256": case.get("expected_lut_sha256") if name == "rope" else None,
                          "limitations": metadata()["gaps"]})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
