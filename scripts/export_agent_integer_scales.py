#!/usr/bin/env python3
"""Exact FP16-bit static W4-scale export, experimental and NOT an NPU ABI.

Only scale words are converted to positive integer significand * 2**exponent.
No floating arithmetic, re-quantization, weight copy, W4 head or calibration.
Original AGNPUPK structure, CRC32, SHA256, tensor/catalog/manifest identities
must match. Output uses new typed fields; old FP16 fields are never relabeled.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib

FORMAT = "agent-static-dyadic-scales-v1-CANDIDATE"
SOURCE_BYTES = 256604672
SOURCE_SHA256 = "f2299b56becb55c0f4bf06e38d982055b893c0d8e70d369f97f84fab68ef4141"
HEADER = struct.Struct("<8sHHIIIQ")  # 32B; followed by 32B layout/source binding digest
MAGIC = b"AGSCLV1\0"
NAMES = ("SCALEV1.BIN", "SCLMETA.JSN", "INTMAN.JSN")
RECORD_FIELDS = ("name_id", "flags", "n", "k", "padded_k", "row_stride",
                 "weight_offset", "weight_length", "scale_offset", "scale_length", "shared_const_id", "reserved")
TENSOR_STRUCT = struct.Struct("<6I4Q2I")


def number(value, low=0, high=(1 << 64) - 1):
    if type(value) is not int or not low <= value <= high:
        raise ValueError("invalid integer field")
    return value


def align32(value):
    return (number(value) + 31) & ~31


def digest(data):
    return hashlib.sha256(data).hexdigest()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":")).encode()


def name_id(name):
    if type(name) is not str or not name or "\0" in name:
        raise ValueError("valid tensor name required")
    result = 2166136261
    for byte in name.encode("utf-8"):
        result = ((result ^ byte) * 16777619) & 0xffffffff
    return result or 1


def fp16_dyadic(bits):
    """Exact binary16 positive finite decode; no float conversion or rounding."""
    bits = number(bits, 0, 65535)
    exponent, fraction = (bits >> 10) & 31, bits & 1023
    if bits & 0x8000 or exponent == 31 or (exponent == 0 and fraction == 0):
        raise ValueError("scale must be positive finite FP16")
    return (fraction, -24) if exponent == 0 else (1024 + fraction, exponent - 25)


def _json(path):
    raw = Path(path).read_bytes()
    return json.loads(raw), {"path": str(Path(path).resolve()), "bytes": len(raw), "sha256": digest(raw)}


def _valid_sha(value):
    if type(value) is not str or re.fullmatch("[0-9a-f]{64}", value) is None:
        raise ValueError("full lowercase SHA256 required")
    return value


def read_package(path, expected_sha256, expected_bytes):
    """One full-package scan for SHA/CRC and per-tensor region digests."""
    path = Path(path).resolve(strict=True)
    _valid_sha(expected_sha256)
    expected_bytes = number(expected_bytes, 512, 0x40000000)
    before = path.stat()
    if before.st_size != expected_bytes:
        raise ValueError("source package fixed length mismatch")
    with path.open("rb") as stream:
        header = stream.read(512)
        if len(header) != 512 or header[:8] != b"AGNPUPK\0":
            raise ValueError("package magic/header mismatch")
        version, header_bytes = struct.unpack_from("<HH", header, 8)
        flags, count, record_bytes = struct.unpack_from("<III", header, 12)
        table, payload, total = struct.unpack_from("<QQQ", header, 24)
        crc, quant, endian, reserved = struct.unpack_from("<IIII", header, 48)
        if ((version, header_bytes, flags, record_bytes, quant, endian, reserved) !=
                (1, 512, 0, 64, 1, 0x01020304, 0) or any(header[64:]) or not 1 <= count <= 4096):
            raise ValueError("unsupported/reserved package fields")
        table_end = table + count * 64
        if (table != 512 or total != before.st_size or total % 512 or
                payload != (table_end + 511) & ~511 or payload > total):
            raise ValueError("package table/payload bounds")
        records = []
        intervals = []
        ids = set()
        for index in range(count):
            raw = stream.read(64)
            if len(raw) != 64:
                raise ValueError("truncated tensor table")
            record = dict(zip(RECORD_FIELDS, TENSOR_STRUCT.unpack(raw)))
            record["index"] = index
            groups = (record["k"] + 127) // 128
            if (not record["name_id"] or record["name_id"] in ids or record["flags"] or record["reserved"] or
                    not record["n"] or not 1 <= record["k"] <= 4096 or
                    record["padded_k"] != groups * 128 or record["row_stride"] < groups * 64 or
                    record["row_stride"] % 32 or record["weight_length"] != record["n"] * record["row_stride"] or
                    record["scale_length"] != record["n"] * groups * 2):
                raise ValueError("tensor dimensions/flags/name alias mismatch")
            ids.add(record["name_id"])
            record["groups"] = groups
            for region in ("weight", "scale"):
                offset, length = record[region + "_offset"], record[region + "_length"]
                if offset % 512 or offset < payload or offset > total or length > total - offset:
                    raise ValueError("tensor region offset/length outside package")
                intervals.append((offset, offset + length, index, region))
            records.append(record)
        intervals.sort()
        if any(previous[1] > current[0] for previous, current in zip(intervals, intervals[1:])):
            raise ValueError("weight/scale physical alias or overlap")
        region_hashes = {(idx, region): hashlib.sha256() for _, _, idx, region in intervals}
        package_hash, package_crc, offset = hashlib.sha256(), 0, 0
        stream.seek(0)
        interval_index = 0
        while True:
            chunk = stream.read(4 << 20)
            if not chunk:
                break
            package_hash.update(chunk)
            if offset == 0:
                crc_chunk = chunk[:48] + bytes(4) + chunk[52:]
            else:
                crc_chunk = chunk
            package_crc = zlib.crc32(crc_chunk, package_crc)
            end = offset + len(chunk)
            while interval_index < len(intervals) and intervals[interval_index][1] <= offset:
                interval_index += 1
            scan = interval_index
            while scan < len(intervals) and intervals[scan][0] < end:
                start, stop, idx, region = intervals[scan]
                region_hashes[(idx, region)].update(chunk[max(start, offset) - offset:min(stop, end) - offset])
                scan += 1
            offset = end
    after = path.stat()
    if ((before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino) or
            offset != total or package_hash.hexdigest() != expected_sha256 or package_crc != crc):
        raise ValueError("source package changed or SHA256/CRC32 mismatch")
    for record in records:
        for region in ("weight", "scale"):
            record[region + "_sha256"] = region_hashes[(record["index"], region)].hexdigest()
    return records, {"path": str(path), "bytes": total, "sha256": expected_sha256,
                     "crc32": f"{crc:08x}", "tensor_count": count,
                     "format": "AGNPUPK/v1 signed W4 G128; original untouched"}, before


def _match_sources(records, package, catalog, manifest):
    if catalog.get("schema") != "qwen35_w4a8_operator_tensor_catalog_v1":
        raise ValueError("catalog schema mismatch")
    entries = catalog.get("tensors")
    declared = manifest.get("parameter_package", {})
    if (declared.get("format") != "ag-npu-parameters-v1" or declared.get("bytes") != package["bytes"] or
            declared.get("sha256") != package["sha256"] or declared.get("crc32") != package["crc32"]):
        raise ValueError("source manifest package identity mismatch")
    by_name = {}
    for entry in entries:
        name = entry["name"]
        if name in by_name:
            raise ValueError("catalog name alias/duplicate")
        by_name[name] = entry
    declared_records = declared.get("tensors")
    if type(declared_records) is not list or len(declared_records) != len(records):
        raise ValueError("manifest tensor count mismatch")
    seen = set()
    for record, listed in zip(records, declared_records):
        name = listed["name"]
        if name in seen or name not in by_name or name_id(name) != record["name_id"]:
            raise ValueError("tensor name identity/order/alias mismatch")
        seen.add(name)
        entry = by_name[name]
        if (name in ("output.weight", "token_embd.weight") or "lm_head" in name or
                entry.get("use") == "tied_token_embedding_and_output_head_resource"):
            raise ValueError("W4 head/embedding conversion excluded")
        for key in ("name_id", "n", "k", "padded_k", "row_stride", "shared_const_id",
                    "weight_offset", "weight_length", "scale_offset", "scale_length"):
            if listed.get(key) != record[key]:
                raise ValueError("manifest tensor storage differs from package")
        if (entry.get("target_dtype") != "signed_W4_zero_point_0" or entry.get("quant_group") != "K_axis_128" or
                entry.get("scale_dtype") != "IEEE-754 binary16 little-endian" or
                entry.get("shape_export_NK") != [record["n"], record["k"]] or
                entry.get("groups_per_row") != record["groups"] or
                entry.get("row_stride_bytes") != record["row_stride"] or entry.get("padded_K") != record["padded_k"] or
                entry.get("logical_K") != record["k"] or entry.get("weight_bytes") != record["weight_length"] or
                entry.get("scale_bytes") != record["scale_length"] or
                entry.get("weight_sha256") != record["weight_sha256"] or entry.get("scale_sha256") != record["scale_sha256"]):
            raise ValueError("catalog dimensions/scale/weight identity differs from package")
        record["name"] = name
        record["catalog_aliases"] = entry.get("aliases", [])
        record["parameter_version"] = entry.get("parameter_version")
    actual_w4 = {entry["name"] for entry in entries if entry.get("quant_group") is not None}
    if actual_w4 != seen:
        raise ValueError("catalog W4 tensor set differs from package")
    return declared.get("image")


def _no_alias(paths):
    for i, path in enumerate(paths):
        for other in paths[:i]:
            if path.resolve() == other.resolve() or (path.exists() and other.exists() and path.samefile(other)):
                raise ValueError("input/output file alias rejected")


def export_scales(package_path, catalog_path, source_manifest_path, out_dir,
                  expected_sha256=SOURCE_SHA256, expected_bytes=SOURCE_BYTES):
    inputs = [Path(path).resolve(strict=True) for path in (package_path, catalog_path, source_manifest_path)]
    out_dir = Path(out_dir).resolve()
    outputs = [out_dir / name for name in NAMES]
    _no_alias(inputs + outputs)
    if any(path.exists() for path in outputs):
        raise ValueError("candidate output exists; use fresh output paths")
    catalog, catalog_identity = _json(inputs[1])
    source_manifest, manifest_identity = _json(inputs[2])
    records, package_identity, source_stat = read_package(inputs[0], expected_sha256, expected_bytes)
    declared_image = _match_sources(records, package_identity, catalog, source_manifest)
    sources = {"package": package_identity, "tensor_catalog": catalog_identity,
               "deployment_manifest": manifest_identity,
               "package_basename_alias": {"actual": inputs[0].name, "manifest_declares": declared_image,
                                            "identity_rule": "same pinned SHA256/bytes, basename is not identity"}}
    tensors, regions, offset = [], [], 64
    with inputs[0].open("rb") as original:
        for record in records:
            original.seek(record["scale_offset"])
            raw = original.read(record["scale_length"])
            if digest(raw) != record["scale_sha256"]:
                raise ValueError("source scales changed after validated package scan")
            row_bytes = align32(record["groups"] * 4)
            region = bytearray(record["n"] * row_bytes)
            for index, (bits,) in enumerate(struct.iter_unpack("<H", raw)):
                mantissa, exponent = fp16_dyadic(bits)
                row, group = divmod(index, record["groups"])
                struct.pack_into("<HbB", region, row * row_bytes + group * 4, mantissa, exponent, 0)
            data = bytes(region)
            tensors.append({"name": record["name"], "name_id": record["name_id"],
                "shape_NK": [record["n"], record["k"]], "padded_K": record["padded_k"],
                "scale_shape": [record["n"], record["groups"]], "scale_axis": ["N_output_row", "K_group128"],
                "group_size": 128, "zero_point": 0, "alignment_bytes": 32,
                "original_weight": {"file": "source package; no weight copy", "offset": record["weight_offset"],
                    "bytes": record["weight_length"], "sha256": record["weight_sha256"],
                    "storage": "signed W4, low nibble first, row-major", "row_stride_bytes": record["row_stride"]},
                "original_scale": {"offset": record["scale_offset"], "bytes": record["scale_length"],
                    "sha256": record["scale_sha256"], "storage": "FP16 LE row-major [N,G]", "row_stride_bytes": record["groups"] * 2},
                "candidate_scale": {"file": NAMES[0], "offset": offset, "bytes": len(data), "sha256": digest(data),
                    "storage": "u16 mantissa + i8 binary_exponent + u8 reserved_zero, little-endian",
                    "record_bytes": 4, "row_stride_bytes": row_bytes, "row_major": True,
                    "elements": record["n"] * record["groups"], "padding_bytes": len(data) - record["n"] * record["groups"] * 4},
                "compute": "exact positive integer mantissa * 2**signed_exponent static multiplier",
                "accumulation": "W4*A8 G128 signed INT32 dot bound; scale products/sums need explicit future graph widths",
                "rounding": "none for exact static conversion; future requantization RNE not performed here",
                "alias": {"physical": "none, all input/output intervals validated nonoverlap", "catalog": record["catalog_aliases"]},
                "parameter_version": record["parameter_version"]})
            regions.append(data)
            offset += len(data)
    after = inputs[0].stat()
    if (source_stat.st_size, source_stat.st_mtime_ns, source_stat.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino):
        raise ValueError("source package changed during export")
    if digest(inputs[1].read_bytes()) != catalog_identity["sha256"] or digest(inputs[2].read_bytes()) != manifest_identity["sha256"]:
        raise ValueError("catalog/manifest changed during export")
    binding = {"format": FORMAT, "sources": sources, "tensors": tensors}
    binding_sha = digest(canonical(binding))
    blob_header = HEADER.pack(MAGIC, 1, 64, 4, len(tensors), 0, offset) + bytes.fromhex(binding_sha)
    out_dir.mkdir(parents=True, exist_ok=True)
    blob_hash = hashlib.sha256()
    with outputs[0].open("xb") as stream:
        for part in [blob_header] + regions:
            stream.write(part); blob_hash.update(part)
    metadata = {"format": FORMAT, "status": "CANDIDATE_STATIC_SCALE_ONLY_NOT_APPROVED",
                "binding": binding, "binding_sha256": binding_sha,
                "binary": {"file": NAMES[0], "bytes": offset, "sha256": blob_hash.hexdigest(),
                           "header_bytes": 64, "alignment_bytes": 32, "tensor_count": len(tensors)},
                "static_conversion": "exact bit decode; zero saturation, zero underflow, zero rounding",
                "limitations": ["no joint activation/state calibration or integer graph quality approval",
                                "no NPU ABI, descriptor or accumulator format approval",
                                "original W4 weights referenced only, no re-quantization or W4 head export"]}
    meta_raw = json.dumps(metadata, sort_keys=True, indent=2).encode() + b"\n"
    outputs[1].write_bytes(meta_raw)
    delivery = {"format": "agent-static-scale-delivery-v1", "status": metadata["status"], "sources": sources,
                "files": {NAMES[0]: {"bytes": offset, "sha256": blob_hash.hexdigest()},
                          NAMES[1]: {"bytes": len(meta_raw), "sha256": digest(meta_raw)}},
                "binding_sha256": binding_sha, "limitations": metadata["limitations"]}
    outputs[2].write_bytes(json.dumps(delivery, sort_keys=True, indent=2).encode() + b"\n")
    verify_export(outputs[0], outputs[1], outputs[2])
    return delivery


def verify_export(binary_path, metadata_path, manifest_path=None):
    paths = [Path(binary_path).resolve(strict=True), Path(metadata_path).resolve(strict=True)]
    if manifest_path is not None:
        paths.append(Path(manifest_path).resolve(strict=True))
    _no_alias(paths)
    metadata, _ = _json(paths[1])
    blob = paths[0].read_bytes()
    if len(blob) < 64 or len(blob) % 32:
        raise ValueError("candidate binary alignment/header invalid")
    magic, version, header_bytes, record_bytes, count, flags, total = HEADER.unpack_from(blob)
    binding = metadata["binding"]
    binding_sha = digest(canonical(binding))
    if ((magic, version, header_bytes, record_bytes, flags, total) != (MAGIC, 1, 64, 4, 0, len(blob)) or
            binding.get("format") != FORMAT or metadata.get("format") != FORMAT or
            metadata.get("binding_sha256") != binding_sha or blob[32:64] != bytes.fromhex(binding_sha)):
        raise ValueError("candidate format/source/layout binding mismatch")
    if metadata["binary"] != {"file": NAMES[0], "bytes": len(blob), "sha256": digest(blob),
            "header_bytes": 64, "alignment_bytes": 32, "tensor_count": count}:
        raise ValueError("candidate binary metadata/hash mismatch")
    tensors = binding["tensors"]
    if len(tensors) != count:
        raise ValueError("candidate tensor count mismatch")
    next_offset, seen = 64, set()
    elements = 0
    for tensor in tensors:
        n, k = tensor["shape_NK"]
        number(n, 1); number(k, 1, 4096)
        groups = (k + 127) // 128
        storage = tensor["candidate_scale"]
        row_bytes = align32(groups * 4)
        size = n * row_bytes
        if (tensor["name_id"] in seen or name_id(tensor["name"]) != tensor["name_id"] or
                tensor["scale_shape"] != [n, groups] or tensor["group_size"] != 128 or tensor["zero_point"] != 0 or
                tensor["alignment_bytes"] != 32 or storage["offset"] != next_offset or storage["bytes"] != size or
                storage["row_stride_bytes"] != row_bytes or storage["elements"] != n * groups or
                storage["padding_bytes"] != size - n * groups * 4 or next_offset + size > len(blob)):
            raise ValueError("candidate tensor shape/offset/alias invalid")
        region = blob[next_offset:next_offset + size]
        if digest(region) != storage["sha256"]:
            raise ValueError("candidate tensor region hash mismatch")
        for row in range(n):
            row_start = row * row_bytes
            for group in range(groups):
                mantissa, exponent, reserved = struct.unpack_from("<HbB", region, row_start + group * 4)
                if (reserved or not 1 <= mantissa <= 2047 or not -24 <= exponent <= 5 or
                        (mantissa < 1024 and exponent != -24)):
                    raise ValueError("invalid dyadic scale record")
            if any(region[row_start + groups * 4:row_start + row_bytes]):
                raise ValueError("nonzero row padding")
        seen.add(tensor["name_id"]); next_offset += size; elements += n * groups
    if next_offset != len(blob):
        raise ValueError("unindexed/aliased candidate tail")
    if manifest_path is not None:
        delivery, _ = _json(paths[2])
        if (delivery.get("binding_sha256") != binding_sha or delivery.get("sources") != binding["sources"] or
                delivery.get("files") != {NAMES[0]: {"bytes": len(blob), "sha256": digest(blob)},
                    NAMES[1]: {"bytes": paths[1].stat().st_size, "sha256": digest(paths[1].read_bytes())}}):
            raise ValueError("delivery file identity mismatch")
    return {"status": "VERIFIED_CANDIDATE_NOT_APPROVED", "tensors": count, "scale_elements": elements,
            "bytes": len(blob), "sha256": digest(blob), "binding_sha256": binding_sha}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    p = commands.add_parser("export")
    for name in ("package", "catalog", "source-manifest", "out-dir"):
        p.add_argument("--" + name, type=Path, required=True)
    p = commands.add_parser("verify")
    p.add_argument("--binary", type=Path, required=True); p.add_argument("--metadata", type=Path, required=True)
    p.add_argument("--manifest", type=Path)
    args = parser.parse_args(argv)
    if args.command == "export":
        result = export_scales(args.package, args.catalog, args.source_manifest, args.out_dir)
    else:
        result = verify_export(args.binary, args.metadata, args.manifest)
    print(json.dumps(result, sort_keys=True, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
