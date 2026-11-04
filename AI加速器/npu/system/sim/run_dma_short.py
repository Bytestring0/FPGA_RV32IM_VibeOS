#!/usr/bin/env python3
"""Bounded DMA-only lane regression: build <=120s, simulation <=60s per lane."""
import argparse
import hashlib
import time
import json
from pathlib import Path
import re
import subprocess

SYSTEM = Path(__file__).resolve().parent.parent

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--lanes', nargs='+', type=int, choices=(16,32,64), default=[16,32,64])
    p.add_argument('--output', type=Path, default=SYSTEM/'build/dma_short')
    p.add_argument('--legacy-core', action='store_true')
    p.add_argument('--reuse-act-line', action='store_true')
    args = p.parse_args()
    out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    rtl = SYSTEM/'rtl'; core = SYSTEM.parents[1]/'rtl/compute'
    sources = [rtl/'npu_dma_top.sv', rtl/'npu_w4_fused_reduce.sv', rtl/'gemv_pipeline16.sv', rtl/'gemv_pipeline_v2.sv', core/'gemv_core.sv', core/'group_acc.sv', core/'mac_lane.sv']
    runs = []
    report = {'format':'npu-dma-short-1', 'scope':'Verilator ordered DDR BFM only; no clock/board tokens per second claim',
              'source_sha256':{str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources+[SYSTEM/'sim/test_npu_dma.cpp',Path(__file__).resolve()]}, 'runs':runs}
    for lanes in args.lanes:
        obj = out/f'obj{lanes}'
        cmd = ['verilator','--cc','--exe','--build','-j','4','-Wall','--top-module','npu_dma_top','--Mdir',str(obj),f'-GLANES={lanes}']
        if args.legacy_core: cmd += ['-GPIPELINED_CORE=0']
        if args.reuse_act_line: cmd += ["-GREUSE_ACT_LINE=1'b1"]
        cmd += ['-CFLAGS',f'-DNPU_DMA_LANES={lanes}'] + list(map(str,sources)) + [str(SYSTEM/'sim/test_npu_dma.cpp')]
        build_start = time.monotonic()
        with (out/f'build{lanes}.log').open('w') as log:
            subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, check=True, timeout=120)
        build_seconds = time.monotonic()-build_start
        test_start = time.monotonic()
        result = subprocess.run([str(obj/'Vnpu_dma_top'),'--short'], text=True, capture_output=True, timeout=60)
        test_seconds = time.monotonic()-test_start
        text = result.stdout + result.stderr
        (out/f'test{lanes}.log').write_text(text)
        runs.append({'lanes':lanes, 'exit_code':result.returncode, 'build_seconds':round(build_seconds,3), 'test_seconds':round(test_seconds,3), 'build_command':cmd, 'summary':[line for line in text.splitlines() if line.startswith(('COVER ','PASS_'))], 'perf':[{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)} for line in text.splitlines() if line.startswith('PERF ')]})
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(text, flush=True)
        result.check_returncode()

if __name__ == '__main__': main()
