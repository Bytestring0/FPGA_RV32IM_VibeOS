"""Compile unmodified upstream RV32UI/UM tests with our bare-metal test env."""
import os
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
os.chdir(root)
out = root / "build/isa"
out.mkdir(parents=True, exist_ok=True)
prefix = os.environ.get("RISCV_PREFIX", "riscv64-unknown-elf-")
sources = sorted((root / "third_party/riscv-tests/isa/rv32ui").glob("*.S"))
sources += sorted((root / "third_party/riscv-tests/isa/rv32um").glob("*.S"))
sources += sorted((root / "third_party/riscv-tests/isa/rv32uzba").glob("*.S"))
sources += sorted((root / "third_party/riscv-tests/isa/rv32uzbb").glob("*.S"))
if not sources:
    raise SystemExit("Run scripts/fetch_deps.sh first")
passed = []
for src in sources:
    # ma_data deliberately requires hardware misaligned accesses; this core
    # implements permitted precise traps instead, checked by trap_tests.S.
    if src.stem == "ma_data":
        continue
    name = src.parent.name + "-" + src.stem
    elf, binary = out / (name + ".elf"), out / (name + ".bin")
    subprocess.run([prefix+"gcc", "-march=rv32im_zicsr_zifencei_zba_zbb", "-mabi=ilp32",
                    "-nostdlib", "-nostartfiles", "-Itests/env",
                    "-Ithird_party/riscv-tests/isa/macros/scalar",
                    "-Tsw/link.ld", str(src), "-o", str(elf)], check=True,
                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    subprocess.run([prefix+"objcopy", "-O", "binary", str(elf), str(binary)], check=True)
    subprocess.run(["python3", "scripts/bin2hex.py", str(binary), "build/firmware.hex"], check=True)
    run = subprocess.run(["build/soc/Vqinglong_soc"], text=True, capture_output=True,
                         env={**os.environ, "MAX_CYCLES": "2000000"})
    (out / (name+".log")).write_text(run.stdout + run.stderr)
    if run.returncode:
        print(name, run.stdout, run.stderr)
        raise SystemExit("FAIL " + name)
    passed.append(name)
    print("PASS", name)
(out / "summary.txt").write_text("\n".join(passed) + f"\n{len(passed)} tests passed\n")
print(f"{len(passed)} upstream ISA tests passed")
