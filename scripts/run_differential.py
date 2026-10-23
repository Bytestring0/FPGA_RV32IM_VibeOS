import os
import re
import subprocess
from pathlib import Path
os.chdir(Path(__file__).resolve().parents[1])
out=Path("build/differential")
out.mkdir(parents=True, exist_ok=True)
cc=os.environ.get("RISCV_PREFIX","riscv64-unknown-elf-")
logs=[]
retired_checked=0
second_retired=0
tests=sorted(Path("build/isa").glob("*.bin"))
if len(tests)<49:
    raise SystemExit("Run make isa first")
for program_seed in (20261020, 314159, 271828):
    source=out/f"random-{program_seed}.S"
    elf=source.with_suffix(".elf")
    binary=source.with_suffix(".bin")
    subprocess.run(["python3","scripts/gen_random.py","--seed",str(program_seed),"--out",str(source)],check=True)
    subprocess.run([cc+"gcc","-march=rv32im_zicsr_zifencei_zba_zbb","-mabi=ilp32","-nostdlib",
                    "-Tsw/link.ld",str(source),"-o",str(elf)],check=True,capture_output=True)
    subprocess.run([cc+"objcopy","-O","binary",str(elf),str(binary)],check=True)
    tests.append(binary)
for test in tests:
    for seed in (1, 42, 2026):
        run=subprocess.run(["build/core/Vrv32_core",str(test),str(seed)],
                           capture_output=True,text=True)
        log=f"{test.stem} seed={seed}: {run.stdout}{run.stderr}"
        logs.append(log)
        if run.returncode:
            (out/"results.log").write_text("".join(logs))
            raise SystemExit(log)
        checked=re.search(r"\bretired_checked=(\d+)\b",run.stdout)
        second=re.search(r"\bsecond_retired=(\d+)\b",run.stdout)
        if not checked or not second:
            raise SystemExit(f"Missing retirement counters: {log}")
        retired_checked+=int(checked[1])
        second_retired+=int(second[1])
    print("PASS",test.stem,"(3 randomized bus seeds + retirement oracle)",flush=True)
(out/"results.log").write_text("".join(logs))
if os.environ.get("EXPECT_DUAL","1") != "0" and second_retired==0:
    raise SystemExit("No second-lane instruction retired; dual-issue coverage is empty")
summary=(f"{len(tests)*3} differential runs passed; "
         f"{retired_checked} instructions checked; {second_retired} second-lane retirements\n")
(out/"summary.txt").write_text(summary)
print(summary,end="")
