"""Short host checks for build provenance; never perform model inference."""
import hashlib
from pathlib import Path
import sys
import tempfile
import unittest
from cooperative_build_support import asset_manifest, runtime_audit, bounded_command


class BuildSupportTests(unittest.TestCase):
    def test_explicit_assets_and_catalog_alias(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            pins = {}
            for key, name in (("weights", "W.BIN"), ("cpu", "CPU.BIN"),
                              ("catalog", "TENSOR_HEAD.JSON"), ("tokenizer", "TOKEN.BIN")):
                data = key.encode()
                (root / ("HDTENSOR.JSN" if key == "catalog" else name)).write_bytes(data)
                pins[key] = {"name": name, "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
            tokenizer = pins.pop("tokenizer")
            self.assertTrue(asset_manifest(root, pins, tokenizer)["verified"])
            (root / "W.BIN").write_bytes(b"corrupt")
            with self.assertRaisesRegex(ValueError, "mismatch"):
                asset_manifest(root, pins, tokenizer)
            (root / "W.BIN").unlink()
            with self.assertRaisesRegex(ValueError, "missing external weights"):
                asset_manifest(root, pins, tokenizer)
            self.assertFalse(asset_manifest(None, pins, tokenizer)["verified"])

    def test_runtime_selection_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for side in ("embedded", "merged"):
                for folder in ("model", "agent"):
                    (root / side / folder).mkdir(parents=True)
            (root / "embedded/model/tensor.c").write_text("hardware")
            (root / "merged/agent/backend.c").write_text("reference")
            report = runtime_audit(root / "embedded", root / "merged")
            self.assertEqual(len(report["differences"]), 2)
            self.assertFalse(report["merged_backend_hardware_supported"])

    def test_command_success_failure_and_timeout(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bounded_command([sys.executable, "-c", "print('ok')"], root, "success", 2)
            with self.assertRaisesRegex(RuntimeError, "failed with 3"):
                bounded_command([sys.executable, "-c", "raise SystemExit(3)"], root, "failure", 2)
            with self.assertRaisesRegex(RuntimeError, "timed out"):
                bounded_command([sys.executable, "-c", "import time; time.sleep(2)"], root, "timeout", 0.1)


if __name__ == "__main__":
    unittest.main()
