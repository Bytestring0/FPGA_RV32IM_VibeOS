"""Small build-only helpers. No inference, device access, or asset writes."""
from pathlib import Path
import hashlib
import subprocess


def runtime_audit(embedded, merged):
    """Record why the hardware entry cannot use the software-only merged backend."""
    result = {"selected_runtime": str(embedded), "merged_runtime": str(merged),
              "selection_reason": "tensor_v2_async_and_w4_head_required",
              "merged_backend_hardware_supported": False, "differences": []}
    for folder in ("model", "agent"):
        left, right = embedded / folder, merged / folder
        names = sorted({p.relative_to(left) for p in left.rglob("*") if p.is_file()} |
                       {p.relative_to(right) for p in right.rglob("*") if p.is_file()})
        for name in names:
            a, b = left / name, right / name
            if not a.is_file() or not b.is_file():
                state = "embedded_only" if a.is_file() else "merged_only"
            elif a.read_bytes() != b.read_bytes():
                state = "different"
            else:
                continue
            result["differences"].append({"path": str(Path(folder) / name), "state": state})
    return result


def asset_manifest(directory, pins, tokenizer):
    """Verify explicit external inputs with streaming SHA; never copy them."""
    if directory is None:
        return {"verified": False, "reason": "external_assets_not_supplied"}
    directory = Path(directory).resolve()
    entries = {}
    for key, pin in [*pins.items(), ("tokenizer", tokenizer)]:
        candidates = [directory / pin["name"]]
        if key == "catalog":
            candidates.append(directory / "HDTENSOR.JSN")
        source = next((p for p in candidates if p.is_file()), candidates[0])
        if not source.is_file():
            raise ValueError(f"missing external {key}: {source}")
        if source.stat().st_size != pin["bytes"]:
            raise ValueError(f"external {key} size mismatch: {source}")
        digest = hashlib.sha256()
        with source.open("rb") as stream:
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(chunk)
        actual = digest.hexdigest()
        if actual != pin["sha256"]:
            raise ValueError(f"external {key} SHA256 mismatch: {source}")
        entries[key] = {"path": str(source), "bytes": pin["bytes"], "sha256": actual}
    return {"verified": True, "files": entries, "inference_runs": 0}


def bounded_command(argv, out, name, timeout_seconds=120):
    """Match deployment logs while bounding every compiler/tool subprocess."""
    from deployment import save, sha
    argv = list(map(str, argv))
    save(out / (name + ".command.json"), {"argv": argv, "cwd": str(out),
                                            "timeout_seconds": timeout_seconds})
    timed_out = False
    with (out / (name + ".log")).open("wb") as log:
        try:
            result = subprocess.run(argv, cwd=out, stdout=log, stderr=subprocess.STDOUT,
                                    check=False, timeout=timeout_seconds)
            code = result.returncode
        except subprocess.TimeoutExpired:
            timed_out, code = True, None
    save(out / (name + ".result.json"), {"actual_exit": code, "timed_out": timed_out,
                                           "log_sha256": sha(out / (name + ".log"))})
    if timed_out or code:
        raise RuntimeError(f"{name} {'timed out' if timed_out else f'failed with {code}'}; see {out / (name + '.log')}")
