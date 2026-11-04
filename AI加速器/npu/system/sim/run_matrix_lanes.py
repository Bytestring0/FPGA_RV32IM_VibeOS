#!/usr/bin/env python3
"""One concentrated16/32/64-lane DMA+pipeline regression, without board actions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

SYSTEM=Path(__file__).resolve().parent.parent
RTL=SYSTEM/'rtl'
CORE=SYSTEM.parents[1]/'rtl/compute'

def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()

def synthesis_check(lanes,out,sources):
    tag=f'synth{lanes}';script=out/(tag+'.ys');netlist=out/(tag+'.json')
    script.write_text('read_verilog -sv '+' '.join('"'+str(s)+'"' for s in sources)+'\n'+
        f'hierarchy -top npu_dma_top -chparam LANES {lanes}\nproc\nopt\nmemory_dff\nmemory_share\nmemory_collect\nopt\nstat\nwrite_json "{netlist}"\n')
    cmd=['yosys','-Q','-T','-s',str(script)]
    with (out/(tag+'.log')).open('w') as log:subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=120)
    modules=json.loads(netlist.read_text())['modules'];counts={};memories=[]
    def visit(name):
        for cell in modules[name]['cells'].values():
            kind=cell['type']
            if kind in modules:visit(kind)
            else:
                counts[kind]=counts.get(kind,0)+1
                if kind=='$mem_v2':memories.append({key:int(cell['parameters'][key],2) for key in ('WIDTH','SIZE','RD_PORTS','WR_PORTS','RD_CLK_ENABLE','WR_CLK_ENABLE')})
    visit('npu_dma_top')
    result={'lanes':lanes,'command':cmd,'script_sha256':digest(script),'netlist_sha256':digest(netlist),'hierarchical_cell_counts':counts,'memories':memories,'scope':'Yosys coarse cells before technology mapping, no LUT/DSP/BRAM placement or clock claim'}
    activation_memories=[m for m in memories if m['SIZE'] in (64,128)]
    expected=[{'WIDTH':256,'SIZE':64 if lanes==64 else 128,'RD_PORTS':1,'WR_PORTS':1,'RD_CLK_ENABLE':1,'WR_CLK_ENABLE':1}]*(2 if lanes==64 else 1)
    assert activation_memories==expected,('activation cache failed synchronous1R1W inference',lanes,memories)
    assert all(m['SIZE'] in (64,128) or (m['SIZE']==4 and m['WIDTH']==256) for m in memories),('unexpected memory beyond activation cache and weight FIFO',lanes,memories)
    assert counts.get('$mul')==lanes,('physical multiplier count',lanes,counts.get('$mul'))
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('--lanes',nargs='+',type=int,choices=(16,32,64),default=[16,32,64]);p.add_argument('--output',type=Path,default=SYSTEM/'build/matrix_lanes');p.add_argument('--legacy-core',action='store_true',help='Also test original unpipelined16-lane core');p.add_argument('--reuse-act-line',action='store_true',help='Also test opt-in original16-lane cache-line reuse');p.add_argument('--resume',action='store_true',help='Reuse successful records only when every RTL/test source hash is unchanged');p.add_argument('--synthesis',action='store_true',help='Also require Yosys16/32/64 multiplier and synchronous1R1W cache inference');args=p.parse_args()
    out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
    sources=[RTL/'npu_dma_top.sv',RTL/'npu_w4_fused_reduce.sv',RTL/'gemv_pipeline16.sv',RTL/'gemv_pipeline_v2.sv',CORE/'gemv_core.sv',CORE/'group_acc.sv',CORE/'mac_lane.sv']
    cpp=SYSTEM/'sim/test_npu_dma.cpp';runs=[]
    report={'format':'npu-matrix-lanes-report-1','source_sha256':{str(s.relative_to(SYSTEM.parents[2])):digest(s) for s in sources+[cpp,Path(__file__).resolve()]},'runs':runs,'default_lanes':16,'v1_descriptor_and_records_unchanged':True,'scope':'Verilator DDR BFM cycles only; no PDS, clock, board tokens/s or quality claim'}
    runner_key=str(Path(__file__).resolve().relative_to(SYSTEM.parents[2]))
    if args.resume and (out/'report.json').exists():
        previous=json.loads((out/'report.json').read_text())
        for key,value in report['source_sha256'].items():
            if key!=runner_key and previous['source_sha256'].get(key)!=value:raise SystemExit('RESUME_REJECT source changed: '+key)
        for run in previous['runs']:
            if run.get('build_exit_code')==0 and run.get('test_exit_code')==0:
                run.setdefault('runner_sha256',previous['source_sha256'][runner_key]);runs.append(run)
    version=subprocess.run(['verilator','--version'],text=True,capture_output=True,check=True,timeout=120).stdout.strip();report['verilator']=version
    if args.synthesis:
        report['synthesis']=[]
        for lanes in args.lanes:
            try:report['synthesis'].append(synthesis_check(lanes,out,sources))
            except (AssertionError,subprocess.CalledProcessError) as error:
                report['synthesis_error']=str(error);(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');raise
    variants=[(n,'pipeline',[]) for n in args.lanes]
    if args.legacy_core:variants.append((16,'legacy',['-GPIPELINED_CORE=0']))
    if args.reuse_act_line:variants.append((16,'reuse',["-GREUSE_ACT_LINE=1'b1"]))
    for lanes,label,params in variants:
        for direct in ([False,True] if label=='pipeline' else [False]):
            tag=f'{lanes}_{label}_'+('core' if direct else 'dma')
            if any(r['tag']==tag for r in runs):continue
            top=('gemv_pipeline16' if lanes==16 else 'gemv_pipeline_v2') if direct else 'npu_dma_top'
            selected=([RTL/(top+'.sv'),CORE/'group_acc.sv',CORE/'mac_lane.sv'] if direct else sources)
            cmd=['verilator','--cc','--exe','--build','-j','4','-Wall','--top-module',top,'--Mdir',str(out/(tag+'_obj'))]
            if not direct or lanes!=16:cmd.append(f'-GLANES={lanes}')
            cmd+=params+['-CFLAGS',f'-DNPU_DMA_LANES={lanes}'+(' -DNPU_PIPELINE_TEST' if direct else '')]+[str(s) for s in selected]+[str(cpp)]
            run={'tag':tag,'lanes':lanes,'build_command':cmd,'build_command_sha256':hashlib.sha256(json.dumps(cmd,separators=(',',':')).encode()).hexdigest(),'runner_sha256':report['source_sha256'][runner_key]};runs.append(run)
            with (out/(tag+'_build.log')).open('w') as log:
                proc=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,timeout=120)
            run['build_exit_code']=proc.returncode
            if proc.returncode:
                (out/'report.json').write_text(json.dumps(report,indent=2)+'\n');raise SystemExit(f'BUILD_FAIL {tag}; see {out/(tag+"_build.log")}')
            exe=out/(tag+'_obj')/('V'+top);run['executable_sha256']=digest(exe)
            result=subprocess.run([str(exe)],text=True,capture_output=True,timeout=60);text=result.stdout+result.stderr;(out/(tag+'_test.log')).write_text(text);run['test_exit_code']=result.returncode;run['test_log_sha256']=digest(out/(tag+'_test.log'))
            run['summary']=[s for s in text.splitlines() if s.startswith(('PASS_','COVER '))]
            run['perf']=[{k:int(v) for k,v in re.findall(r'(\w+)=(\d+)',line)} for line in text.splitlines() if line.startswith('PERF ')]
            (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
            if result.returncode:raise SystemExit(f'TEST_FAIL {tag}: {text[-1500:]}')
            print(tag+': '+run['summary'][-1],flush=True)
    report['passed']=True;(out/'report.json').write_text(json.dumps(report,indent=2)+'\n');print('MATRIX_LANES_SUITE_PASS runs='+str(len(runs)),flush=True)

if __name__=='__main__':main()
