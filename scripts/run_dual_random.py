"""Precise-trap dual-issue test under independent randomized I/D wait states."""
import os
import subprocess
from pathlib import Path

os.chdir(Path(__file__).resolve().parents[1])
binary = Path("build/dual_stress.bin")
if not binary.exists():
    subprocess.run(
        [os.environ.get("RISCV_PREFIX", "riscv64-unknown-elf-") + "objcopy",
         "-O", "binary", "build/dual_stress.elf", str(binary)],
        check=True,
    )
logs = []
for seed in (1, 7, 42, 73, 101, 123, 2026, 31415, 65535, 20261020):
    result = subprocess.run(
        ["build/core/Vrv32_core", str(binary), str(seed), "10000000", "no-oracle"],
        capture_output=True, text=True,
    )
    text = result.stdout + result.stderr
    logs.append(text)
    print(text, end="", flush=True)
    Path("build/dual-stress-random.log").write_text("".join(logs))
    if result.returncode:
        raise SystemExit(f"Failed with randomized bus seed {seed}")
print("10 directed dual-issue/trap runs passed under randomized bus latency.")
