#!/usr/bin/env python3
"""Focused fused-op19 regression and legacy DMA compatibility, no hash pass."""
import argparse
import json
import subprocess
import time
import re
from pathlib import Path
SYSTEM = Path(__file__).resolve().parent.parent

def main():
    p=argparse.ArgumentParser()
    p.add_argument('--lanes',nargs='+',type=int,choices=(16,32,64),default=[16,64])
    p.add_argument('--legacy',action='store_true',help='also run existing DMA short regression')
    p.add_argument('--benchmark',action='store_true',help='run same-input raw DMA plus actual tensor19 comparison')
    p.add_argument('--output',type=Path,default=SYSTEM/'build/w4_fused')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    rtl=SYSTEM/'rtl';core=SYSTEM.parents[1]/'rtl/compute'
    sources=[rtl/x for x in ('npu_dma_top.sv','npu_w4_fused_reduce.sv','gemv_pipeline16.sv','gemv_pipeline_v2.sv')]+[core/x for x in ('gemv_core.sv','group_acc.sv','mac_lane.sv')]
    results=[]
    obj=out/'obj_unit'
    cmd=['verilator','--cc','--exe','--build','-j','4','-Wall','--top-module','npu_w4_fused_reduce','--Mdir',str(obj),str(rtl/'npu_w4_fused_reduce.sv'),str(SYSTEM/'sim/test_w4_fused_reduce.cpp')]
    with (out/'build_unit.log').open('w') as log:
        subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    r=subprocess.run([str(obj/'Vnpu_w4_fused_reduce')],text=True,capture_output=True,timeout=60)
    text=r.stdout+r.stderr;(out/'test_unit.log').write_text(text);print(text,flush=True)
    results.append(dict(name='unit_all_positive_fp16',exit_code=r.returncode,summary=text.splitlines()))
    r.check_returncode()
    for lanes in a.lanes:
        for name in (['fused','legacy'] if a.legacy else ['fused']):
            obj=out/f'obj_{name}_{lanes}'
            test=SYSTEM/'sim'/('test_npu_w4_fused.cpp' if name=='fused' else 'test_npu_dma.cpp')
            cmd=['verilator','--cc','--exe','--build','-j','4','-Wall','--top-module','npu_dma_top','--Mdir',str(obj),f'-GLANES={lanes}','-CFLAGS',f'-DNPU_DMA_LANES={lanes}']+list(map(str,sources))+[str(test)]
            start=time.monotonic()
            with (out/f'build_{name}_{lanes}.log').open('w') as log:
                subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
            r=subprocess.run([str(obj/'Vnpu_dma_top')]+(['--short'] if name=='legacy' else []),text=True,capture_output=True,timeout=60)
            text=r.stdout+r.stderr;(out/f'test_{name}_{lanes}.log').write_text(text)
            print(text,flush=True)
            results.append(dict(name=name,lanes=lanes,exit_code=r.returncode,seconds=round(time.monotonic()-start,3),summary=text.splitlines()))
            (out/'report.json').write_text(json.dumps(dict(format='w4-fused-directed-1',scope='ordered DDR BFM; no board throughput claim',runs=results),indent=2)+'\n')
            r.check_returncode()
            if a.benchmark and name=='fused':
                r=subprocess.run([str(obj/'Vnpu_dma_top'),'--benchmark'],text=True,capture_output=True,timeout=60)
                text=r.stdout+r.stderr;(out/f'benchmark_dma_{lanes}.log').write_text(text);print(text,flush=True);r.check_returncode()
    if a.benchmark:
        obj=out/'obj_tensor_benchmark'
        cmd=['verilator','--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','npu_tensor_v2','--Mdir',str(obj),str(rtl/'npu_tensor_v2.sv'),str(rtl/'npu_delta_vectors.sv'),str(SYSTEM/'sim/benchmark_w4_tensor.cpp')]
        with (out/'build_tensor_benchmark.log').open('w') as log:
            subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
        r=subprocess.run([str(obj/'Vnpu_tensor_v2')],text=True,capture_output=True,timeout=60)
        text=r.stdout+r.stderr;(out/'benchmark_tensor.log').write_text(text);print(text,flush=True);r.check_returncode()
        def records(path):
            return [{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)} for line in path.read_text().splitlines()]
        tensor=records(out/'benchmark_tensor.log');comparisons=[]
        for lanes in a.lanes:
            matrix=records(out/f'benchmark_dma_{lanes}.log')
            for index,t in enumerate(tensor):
                raw,fused=matrix[index*2:index*2+2]
                baseline=raw['cycles']+t['cycles']
                comparisons.append(dict(lanes=lanes,n=t['n'],k=t['k'],raw_dma_cycles=raw['cycles'],tensor19_cycles=t['cycles'],baseline_cycles=baseline,fused_cycles=fused['cycles'],speedup=round(baseline/fused['cycles'],4),baseline_reads=raw['reads']+t['reads'],baseline_writes=raw['writes']+t['writes'],fused_reads=fused['reads'],fused_writes=fused['writes']))
        (out/'benchmark.json').write_text(json.dumps(dict(format='w4-fused-same-input-1',scope='Verilator; identical W4/A8/FP16/max; DDR response at acceptance+2 clocks; request stall tick%5==1; baseline tensor PARALLEL_RAW=1; cycles exclude MMIO configuration',comparisons=comparisons),indent=2)+'\n')
if __name__=='__main__':main()
