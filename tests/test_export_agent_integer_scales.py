"""Synthetic scale-export tests; float half decoding is TEST ORACLE ONLY."""
from copy import deepcopy
from fractions import Fraction
import hashlib
import json
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import export_agent_integer_scales as scales

OUT = Path(os.environ.get("AGENT_SCALE_TEST_OUT",
           r"E:\FPGA_RV32IM_Work\deepseek_agent_accel_20261102\integer_reference\scale_export_v1\test_tmp"))


def write_json(path, data):
    Path(path).write_text(json.dumps(data, sort_keys=True, indent=2), encoding="utf-8")


def crc_rebind(data):
    data = bytearray(data)
    data[48:52] = bytes(4)
    struct.pack_into("<I", data, 48, zlib.crc32(data))
    return bytes(data)


def fixture(folder, package_name="QWENW4.BIN"):
    root = Path(folder)
    paths = {"package": root / package_name, "catalog": root / "TENSOR.JSON", "source": root / "source.json",
             "out": root / "candidate"}
    specs = [("blk.0.attn_qkv.weight", 2, 129, [1, 0x3c00, 0x7bff, 0x0400]),
             ("blk.0.ffn_up.weight", 1, 128, [0x3555])]
    position, records, chunks = 1024, [], []
    for name, n, k, half in specs:
        groups = (k + 127) // 128
        stride = groups * 64
        weight = bytes([0x87]) * (n * stride)
        scale = struct.pack("<" + "H" * len(half), *half)
        weight_offset = position
        scale_offset = (weight_offset + len(weight) + 511) & ~511
        position = (scale_offset + len(scale) + 511) & ~511
        record = {"name_id": scales.name_id(name), "flags": 0, "n": n, "k": k,
                  "padded_k": groups * 128, "row_stride": stride,
                  "weight_offset": weight_offset, "weight_length": len(weight),
                  "scale_offset": scale_offset, "scale_length": len(scale),
                  "shared_const_id": 0, "reserved": 0}
        records.append((name, record))
        chunks.extend([(weight_offset, weight), (scale_offset, scale)])
    raw = bytearray(position)
    raw[:8] = b"AGNPUPK\0"
    struct.pack_into("<HHIII", raw, 8, 1, 512, 0, len(records), 64)
    struct.pack_into("<QQQ", raw, 24, 512, 1024, position)
    struct.pack_into("<IIII", raw, 48, 0, 1, 0x01020304, 0)
    for i, (_, record) in enumerate(records):
        scales.TENSOR_STRUCT.pack_into(raw, 512 + i * 64, *(record[key] for key in scales.RECORD_FIELDS))
    for offset, chunk in chunks:
        raw[offset:offset + len(chunk)] = chunk
    raw = crc_rebind(raw)
    paths["package"].write_bytes(raw)
    catalog, declared = [], []
    for name, record in records:
        groups = (record["k"] + 127) // 128
        weight_hash = scales.digest(raw[record["weight_offset"]:record["weight_offset"] + record["weight_length"]])
        scale_hash = scales.digest(raw[record["scale_offset"]:record["scale_offset"] + record["scale_length"]])
        catalog.append({"name": name, "aliases": [], "target_dtype": "signed_W4_zero_point_0",
            "quant_group": "K_axis_128", "scale_dtype": "IEEE-754 binary16 little-endian",
            "shape_export_NK": [record["n"], record["k"]], "groups_per_row": groups,
            "row_stride_bytes": record["row_stride"], "padded_K": record["padded_k"], "logical_K": record["k"],
            "weight_bytes": record["weight_length"], "scale_bytes": record["scale_length"],
            "weight_sha256": weight_hash, "scale_sha256": scale_hash,
            "parameter_version": "synthetic-only"})
        declared.append({"name": name, **{key: value for key, value in record.items()
                                         if key not in ("flags", "reserved")}})
    write_json(paths["catalog"], {"schema": "qwen35_w4a8_operator_tensor_catalog_v1", "tensors": catalog})
    write_json(paths["source"], {"parameter_package": {"format": "ag-npu-parameters-v1",
        "image": "logical-source-name.BIN", "bytes": len(raw), "sha256": scales.digest(raw),
        "crc32": f"{struct.unpack_from('<I', raw, 48)[0]:08x}", "tensors": declared}})
    paths["sha"], paths["bytes"] = scales.digest(raw), len(raw)
    return paths


def export(paths):
    return scales.export_scales(paths["package"], paths["catalog"], paths["source"], paths["out"],
                                paths["sha"], paths["bytes"])


def rewrite_candidate(binary, metadata, mutate):
    blob = bytearray(binary.read_bytes())
    meta = json.loads(metadata.read_text())
    mutate(blob, meta)
    for tensor in meta["binding"]["tensors"]:
        region = tensor["candidate_scale"]
        region["sha256"] = scales.digest(blob[region["offset"]:region["offset"] + region["bytes"]])
    binding = scales.digest(scales.canonical(meta["binding"]))
    blob[32:64] = bytes.fromhex(binding)
    meta["binding_sha256"] = binding
    meta["binary"]["sha256"] = scales.digest(blob)
    binary.write_bytes(blob); write_json(metadata, meta)


class HalfTests(unittest.TestCase):
    def test_all_positive_finite_half_exact_fraction(self):
        for bits in range(1, 0x7c00):
            mantissa, exponent = scales.fp16_dyadic(bits)
            exact = Fraction(mantissa << exponent) if exponent >= 0 else Fraction(mantissa, 1 << -exponent)
            oracle = Fraction.from_float(struct.unpack("<e", struct.pack("<H", bits))[0])
            self.assertEqual(exact, oracle, bits)
        for invalid in (0, 0x8000, 0xbc00, 0x7c00, 0x7e00, 0xfc00, -1, 65536, 1.0, True):
            with self.assertRaises(ValueError):
                scales.fp16_dyadic(invalid)


class PackageExportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)

    def test_exact_export_storage_padding_alignment_and_identity(self):
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            paths = fixture(folder)
            delivery = export(paths)
            result = scales.verify_export(paths["out"] / scales.NAMES[0], paths["out"] / scales.NAMES[1],
                                          paths["out"] / scales.NAMES[2])
            self.assertEqual(result["tensors"], 2)
            self.assertEqual(result["scale_elements"], 5)
            self.assertEqual(result["bytes"], 160)
            self.assertEqual(set(delivery["files"]), {"SCALEV1.BIN", "SCLMETA.JSN"})
            metadata = json.loads((paths["out"] / scales.NAMES[1]).read_text())
            binary = (paths["out"] / scales.NAMES[0]).read_bytes()
            original = paths["package"].read_bytes()
            for tensor in metadata["binding"]["tensors"]:
                n, groups = tensor["scale_shape"]
                region = tensor["candidate_scale"]
                self.assertEqual(region["offset"] % 32, 0)
                self.assertEqual(region["row_stride_bytes"] % 32, 0)
                old = tensor["original_scale"]
                for i, (word,) in enumerate(struct.iter_unpack("<H", original[old["offset"]:old["offset"] + old["bytes"]])):
                    row, group = divmod(i, groups)
                    mantissa, exponent, reserved = struct.unpack_from("<HbB", binary,
                        region["offset"] + row * region["row_stride_bytes"] + group * 4)
                    self.assertEqual((mantissa, exponent), scales.fp16_dyadic(word))
                    self.assertEqual(reserved, 0)
                self.assertIn("no weight copy", tensor["original_weight"]["file"])
            self.assertIn("NOT_APPROVED", metadata["status"])
            with self.assertRaisesRegex(ValueError, "exists"):
                export(paths)

    def test_crc_bounds_reserved_and_region_alias_rejected(self):
        for mode in ("crc", "outside", "overlap", "duplicate_id", "reserved", "padding_k"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=OUT) as folder:
                paths = fixture(folder)
                raw = bytearray(paths["package"].read_bytes())
                if mode == "crc":
                    raw[-1] ^= 1
                elif mode == "outside":
                    struct.pack_into("<Q", raw, 512 + 40, len(raw) + 512)
                elif mode == "overlap":
                    struct.pack_into("<Q", raw, 512 + 40, 1024)
                elif mode == "duplicate_id":
                    raw[576:580] = raw[512:516]
                elif mode == "reserved":
                    raw[100] = 1
                else:
                    struct.pack_into("<I", raw, 512 + 16, 128)
                if mode != "crc":
                    raw = crc_rebind(raw)
                paths["package"].write_bytes(raw)
                with self.assertRaises(ValueError):
                    scales.read_package(paths["package"], scales.digest(raw), len(raw))

    def test_catalog_manifest_invalid_scales_and_input_alias_rejected(self):
        for mode in ("shape", "scale_sha", "axis", "manifest_sha", "zero_scale", "negative_scale", "input_alias"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=OUT) as folder:
                paths = fixture(folder)
                catalog = json.loads(paths["catalog"].read_text())
                source = json.loads(paths["source"].read_text())
                if mode == "shape":
                    catalog["tensors"][0]["shape_export_NK"][0] += 1
                elif mode == "scale_sha":
                    catalog["tensors"][0]["scale_sha256"] = "0" * 64
                elif mode == "axis":
                    catalog["tensors"][0]["quant_group"] = "N_axis_128"
                elif mode == "manifest_sha":
                    source["parameter_package"]["sha256"] = "0" * 64
                elif mode in ("zero_scale", "negative_scale"):
                    raw = bytearray(paths["package"].read_bytes())
                    record = source["parameter_package"]["tensors"][0]
                    struct.pack_into("<H", raw, record["scale_offset"], 0 if mode == "zero_scale" else 0xbc00)
                    raw = crc_rebind(raw); paths["package"].write_bytes(raw)
                    paths["sha"] = scales.digest(raw)
                    source["parameter_package"]["sha256"] = paths["sha"]
                    source["parameter_package"]["crc32"] = f"{struct.unpack_from('<I', raw, 48)[0]:08x}"
                    scale = raw[record["scale_offset"]:record["scale_offset"] + record["scale_length"]]
                    catalog["tensors"][0]["scale_sha256"] = scales.digest(scale)
                else:
                    paths["catalog"] = paths["package"]
                if mode != "input_alias":
                    write_json(paths["catalog"], catalog); write_json(paths["source"], source)
                with self.assertRaises((ValueError, UnicodeDecodeError)):
                    export(paths)
        with tempfile.TemporaryDirectory(dir=OUT) as folder:
            paths = fixture(folder, "SCALEV1.BIN")
            paths["out"] = Path(folder)
            with self.assertRaisesRegex(ValueError, "alias"):
                export(paths)

    def test_candidate_alias_invalid_record_and_padding_rejected_even_rebound(self):
        for mode in ("alias", "zero_mantissa", "padding", "record_reserved"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory(dir=OUT) as folder:
                paths = fixture(folder); export(paths)
                binary, meta = paths["out"] / scales.NAMES[0], paths["out"] / scales.NAMES[1]
                def mutate(blob, metadata):
                    tensors = metadata["binding"]["tensors"]
                    offset = tensors[0]["candidate_scale"]["offset"]
                    if mode == "alias":
                        tensors[1]["candidate_scale"]["offset"] = offset
                    elif mode == "zero_mantissa":
                        struct.pack_into("<H", blob, offset, 0)
                    elif mode == "padding":
                        blob[offset + 8] = 1
                    else:
                        blob[offset + 3] = 1
                rewrite_candidate(binary, meta, mutate)
                with self.assertRaises(ValueError):
                    scales.verify_export(binary, meta)


if __name__ == "__main__":
    unittest.main()
