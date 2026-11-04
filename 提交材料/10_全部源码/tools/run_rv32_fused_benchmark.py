#!/usr/bin/env python3
"""Real production RV32 cycle-CSR scalar C vs fused W4A8, independent harness."""
import argparse
import json
import re
import subprocess
import sys
import time
from pathlib import Path
import system_common as common
import run_tensor_system_v2 as acceptance
SYSTEM=common.SYSTEM

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--compiler-prefix',default='/usr/bin/riscv64-unknown-elf-')
    p.add_argument('--output',type=Path,default=SYSTEM/'build/rv32_fused_benchmark')
    p.add_argument('--profiles',nargs='+',choices=('single','dual'),default=['single','dual'])
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    def run(cmd,log,cwd=None,timeout=120):
        start=time.monotonic()
        with (out/log).open('w') as f:
            r=subprocess.run(list(map(str,cmd)),cwd=cwd or common.CPU,stdout=f,stderr=subprocess.STDOUT,timeout=timeout)
        if r.returncode:raise RuntimeError((out/log).read_text()[-6000:])
        return (out/log).read_text(),round(time.monotonic()-start,3)
    fw=out/'firmware';fw.mkdir(exist_ok=True);tc=a.compiler_prefix
    flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-O2','-std=c11','-ffreestanding','-fno-builtin','-fno-pic','-msmall-data-limit=0','-mno-relax','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-nostdlib','-nostartfiles','-Wl,--gc-sections','-Wl,-Map,'+str(fw/'firmware.map'),'-T',common.CPU/'sw/link.ld','-I',SYSTEM/'sw','-I',SYSTEM/'integration/agent_runtime_20261101/sw/model/freestanding_include']
    sources=[common.CPU/'sw/start.S',SYSTEM/'sim/benchmark_rv32_fused.c',*[SYSTEM/'sw'/n for n in ('npu_fused_matrix.c','npu_raw.c','npu_runtime.c','freestanding.c')]]
    run([tc+'gcc',*flags,*sources,'-lgcc','-o',fw/'firmware.elf'],'firmware_build.log')
    run([tc+'objcopy','-O','binary',fw/'firmware.elf',fw/'firmware.bin'],'objcopy.log')
    run([sys.executable,common.CPU/'scripts/bin2hex.py',fw/'firmware.bin',fw/'firmware.hex'],'hex.log')
    run([tc+'objdump','-d',fw/'firmware.elf'],'firmware_disassembly.log')
    # Import builder only: all generated files are local to this independent run.
    cpp=acceptance.harness(out,0,4);code=cpp.read_text()
    begin=code.index('    bool ok=d.done');end=code.index('    std::puts(ok?',begin)
    code=code[:begin]+'''    bool ok=d.done&&!d.exit_code&&!trapped&&d.instret_count>100&&
        d.dma_completions==2&&tensor_completions==0&&matrix_requests>0&&cpu_requests>0&&
        pending.empty()&&!d.matrix_outstanding&&!d.tensor_outstanding&&
        console.find("PASS_RV32_FUSED_BENCH")!=std::string::npos;
'''+code[end:];cpp.write_text(code)
    cpu_sources=[common.CPU/line.strip() for line in (common.CPU/'rtl/files.f').read_text().splitlines() if line.strip() and not line.startswith('#')]
    rtl=[*cpu_sources,common.NPU/'rtl/npu_bram.sv',common.NPU/'rtl/npu_bram_top.sv',*[common.CORE/n for n in ('mac_lane.sv','group_acc.sv','gemv_core.sv')],*sorted((SYSTEM/'rtl').glob('*.sv')),SYSTEM/'sim/ag_npu_tensor_sim.sv']
    report=dict(format='rv32-fused-cycle-csr-1',passed=False,scope='production RV32 RTL scalar C baseline versus full production fused driver; Verilator ordered DDR BFM, not board measurements',shape=dict(n=16,k=2048),maximum=65536,cache='enabled; full clean+invalidate before each timed path, equal cold input state; setup excluded; CPU output publication and hardware driver cache publication/acquisition included',baseline='GCC O2 scalar signed-W4 unpack by pair, volatile DDR INT8/FP16 loads, signed32 dot, exact uint64 coefficient divide/RNE, per-group RNE14, ordered64 saturating sum/narrow32; no vector ISA; no compiler precomputation; hardware driver C compiled O2 as well',timers='real RV32 cycle CSR; hardware job includes cache maintenance, MMIO setup, polling, numeric readback, output acquisition; output comparison excluded from both timers',profiles={})
    (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    for profile in a.profiles:
        dual=int(profile=='dual');obj=out/(profile+'_obj')
        _,build_seconds=run(['verilator','--cc','--exe','--build','--timing','-j','4','-O3','-CFLAGS','-O3','-Wno-fatal','--top-module','ag_npu_tensor_sim',"-GDUAL_ISSUE=1'b"+str(dual),'-GMATRIX_LANES=64','-GDDR_OUTSTANDING=4','-GTENSOR_PARALLEL_RAW=0','-GTENSOR_PARALLEL_DELTA=0','-GTENSOR_SHARED_RAW_ACCUM=1',*rtl,'--Mdir',obj,cpp],profile+'_build.log',timeout=180)
        text,simulation_seconds=run([obj/'Vag_npu_tensor_sim','10000000','tensor'],profile+'.log',cwd=fw,timeout=120)
        rows=[json.loads(line.split(' ',1)[1]) for line in text.splitlines() if line.startswith('RV32_FUSED_BENCH ')]
        result=common.result(text)
        if len(rows)!=2 or rows[0]['q16']!=rows[1]['q16'] or any(r['rows_compared']!=16 for r in rows) or not result['done'] or result['exit_code'] or result['trap'] or (dual and not result['dual_retired']):raise RuntimeError('benchmark completion, repeat equality, or CPU retirement failed')
        for row in rows:row['end_to_end_speedup']=round(row['cpu_cycles']/row['hardware_job_cpu_cycles'],4)
        report['profiles'][profile]=dict(runs=rows,result=result,build_seconds=build_seconds,simulation_seconds=simulation_seconds)
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
        print(profile,json.dumps(rows),flush=True)
    report['passed']=True;(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    md=['# Production RV32 scalar C versus fused W4A8','',report['scope'],'',report['baseline'],'',report['cache'],'',report['timers'],'','| Core | Repeat | CPU cycles | Fused job CPU cycles | Engine cycles | Speedup |','|---|---:|---:|---:|---:|---:|']
    for profile,value in report['profiles'].items():
        for r in value['runs']:md.append(f"| {profile} | {r['pass']} | {r['cpu_cycles']} | {r['hardware_job_cpu_cycles']} | {r['hardware_engine_cycles']} | {r['end_to_end_speedup']}x |")
    md+=['','Both passes use unchanged DDR inputs. The real CPU compares all 16 Q16 output rows directly; complete outputs are retained in the JSON and console logs. No hash or host execution time substitutes for cycle CSR measurements.','', 'These are one 16x2048 operator measurements with scalar C and 64-bit software division, not full-model tokens/s or a board clock claim.']
    (out/'BENCHMARK.md').write_text('\n'.join(md)+'\n')
    print('PASS_RV32_FUSED_REPORT',out/'report.json',flush=True)
if __name__=='__main__':main()
