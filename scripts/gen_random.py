"""Reproducible dependency-heavy integer streams, checked by C++ RV32 oracle."""
import argparse
import random
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("--seed", type=int, default=20261020)
p.add_argument("--count", type=int, default=4000)
p.add_argument("--out", default="build/random.S")
a = p.parse_args()
rng = random.Random(a.seed)
lines = [".section .text.start", ".globl _start", "_start:", "li x31, 0xe000"]
for n in range(1, 31):
    lines.append(f"li x{n}, {rng.getrandbits(32)}")
for offset in range(0, 512, 4):
    lines.append(f"sw x{rng.randrange(1,31)}, {offset}(x31)")
rr = ["add","sub","sll","srl","sra","slt","sltu","xor","or","and",
      "mul","mulh","mulhsu","mulhu","div","divu","rem","remu",
      "sh1add","sh2add","sh3add","andn","orn","xnor","min","minu","max","maxu","rol","ror"]
last = 1
for n in range(a.count):
    rd, rs, rt = rng.randrange(31), last if rng.random()<.65 else rng.randrange(31), rng.randrange(31)
    kind = rng.randrange(7)
    if kind < 2:
        lines.append(f"{rng.choice(rr)} x{rd}, x{rs}, x{rt}")
    elif kind == 2:
        lines.append(f"{rng.choice(['addi','slti','sltiu','xori','ori','andi'])} x{rd}, x{rs}, {rng.randrange(-2048,2048)}")
    elif kind == 3:
        op=rng.choice(["lb","lbu","lh","lhu","lw","sb","sh","sw"])
        align = 4 if op in ("lw","sw") else 2 if op in ("lh","lhu","sh") else 1
        off=rng.randrange(512//align)*align
        lines.append(f"{op} x{rd}, {off}(x31)")
    elif kind == 4:
        lines += [f"{rng.choice(['beq','bne','blt','bge','bltu','bgeu'])} x{rs}, x{rt}, L{n}",
                  f"addi x{rd}, x{rs}, {rng.randrange(-128,128)}", f"L{n}:"]
    elif kind == 5:
        lines += [f"jal x{rd}, J{n}", "sw x1, 0(x31)", f"J{n}:"]
    else:
        lines.append(f"{rng.choice(['clz','ctz','cpop','sext.b','sext.h','zext.h','orc.b','rev8'])} x{rd}, x{rs}")
    last = rd
lines += ["lui x31, 0x10000", "sw zero, 4(x31)", "1: j 1b"]
Path(a.out).write_text("\n".join(lines)+"\n")
