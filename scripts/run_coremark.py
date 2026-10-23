import os
import re
import subprocess
from pathlib import Path
os.chdir(Path(__file__).resolve().parents[1])
run = subprocess.run(["build/soc/Vqinglong_soc"], text=True, capture_output=True)
print(run.stdout, end="")
print(run.stderr, end="")
Path("build/coremark-run.log").write_text(run.stdout + run.stderr)
duration_warning = "ERROR! Must execute for at least 10 secs for a valid result!"
other_errors = [line for line in run.stdout.splitlines() if "ERROR" in line and line != duration_warning]
if run.returncode or other_errors or "Cannot validate operation" in run.stdout:
    raise SystemExit("CoreMark execution/CRC failed")
if not all(x in run.stdout for x in ("crclist", "crcmatrix", "crcstate", "MEASURED_CYCLES_HEX=")):
    raise SystemExit("Incomplete CoreMark output")
ticks = int(re.search(r"MEASURED_CYCLES_HEX=([0-9a-f]+)", run.stdout)[1], 16)
iterations = int(re.search(r"Iterations\s+:\s+(\d+)", run.stdout)[1])
# Explicit upstream reference CRCs for the 2 KiB performance/validation seeds.
seed = int(re.search(r"seedcrc\s+:\s+0x([0-9a-f]+)", run.stdout)[1], 16)
expected = {0xe9f5: (0xe714, 0x1fd7, 0x8e3a), 0x18f2: (0xe3c1, 0x0747, 0x8d84)}
actual = tuple(int(re.search(name+r"\s+:\s+0x([0-9a-f]+)", run.stdout)[1], 16)
               for name in ("crclist", "crcmatrix", "crcstate"))
if seed not in expected or actual != expected[seed]:
    raise SystemExit(f"CRC mismatch: seed={seed:04x}, CRCs={actual}")
print("All three algorithm CRCs match upstream reference values.")
print(f"Simulation throughput: {iterations * 1e6 / ticks:.6f} iterations/Mcycle (CoreMark/MHz equivalent)")
if duration_warning in run.stdout:
    print("Duration check: short run (<10 simulated seconds); functional/per-cycle comparison only.")
elif "Correct operation validated" in run.stdout:
    print("Duration check: upstream >=10-second check passed at the firmware's configured CPU_HZ.")
else:
    raise SystemExit("Missing upstream duration/validation result")
print("RTL simulation only; this is not a measured board clock or certified CoreMark score.")
