"""Concentrated actual DDR/RTL suite, including independent Fraction fixtures.

Run in Linux/WSL. No board programming, firmware replacement or model claims.
"""
from __future__ import annotations
import argparse, hashlib, json, struct, subprocess, time, sys
from pathlib import Path

SYSTEM=Path(__file__).resolve().parents[1]
NUMERIC=('status','domain_clamps','saturations','underflows','invalid_inputs','zero_vectors')

def pack_fixtures(source:Path,target:Path):
    payload=json.loads(source.read_text())
    with target.open('wb') as f:
        rows=payload['fixtures'];f.write(struct.pack('<I',len(rows)))
        for row in rows:
            initial=bytes.fromhex(row['initial_hex']);expected=bytes.fromhex(row['expected_hex'])
            assert len(initial)==len(expected) and row['ddr_base']==0
            f.write(struct.pack('<17I',*row['descriptor'],len(initial)))
            f.write(initial);f.write(expected)
            f.write(struct.pack('<8I',row['expected_error'],row['expected_result'],
                                *(row['expected_numeric'][k] for k in NUMERIC)))
    return len(rows)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',type=Path,default=SYSTEM/'build/tensor_v2_rtl')
    p.add_argument('--fixtures',type=Path,nargs='+',help='optional external fixtures; default generates the current independent oracle cases')
    p.add_argument('--synthesis',action='store_true')
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    report={'scope':'actual tensor RTL, DDR simulation; no FPGA timing/board/model-quality proof',
            'passed':False,'fixture_counts':{},'sources':{}}
    sources=[SYSTEM/'rtl/npu_tensor_v2.sv',SYSTEM/'sim/test_tensor_v2.cpp',
             SYSTEM/'sw/npu_tensor_reference.c',SYSTEM/'sw/npu_fixed_math.c',SYSTEM/'sw/npu_tensor_v2.h',
             SYSTEM/'sw/npu_fixed_lut_generated.h',SYSTEM/'sw/npu_fixed_lut_q16_generated.h',Path(__file__).resolve()]
    report['sources']={str(s.relative_to(SYSTEM)):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}
    def run(cmd,log):
        started=time.monotonic()
        with (out/log).open('w') as f:
            result=subprocess.run([str(c) for c in cmd],cwd=SYSTEM,stdout=f,stderr=subprocess.STDOUT)
        report[log]={'returncode':result.returncode,'seconds':round(time.monotonic()-started,3)}
        if result.returncode:raise RuntimeError(f'{log} failed: {(out/log).read_text()[-2500:]}')
    try:
        if a.fixtures is None:
            a.fixtures=[]
            for label,flags in [('base',[]),('extended',['--extended-only']),('raw_w4',['--raw-only'])]:
                fixture=out/(label+'.json')
                run([sys.executable,SYSTEM/'sim/test_tensor_reference.py','--output',fixture,*flags],label+'_oracle.log')
                a.fixtures.append(fixture)
        objects=[]
        for name in ('npu_tensor_reference','npu_fixed_math'):
            obj=out/(name+'.o');objects.append(obj)
            run(['gcc','-O2','-I',SYSTEM/'sw','-c',SYSTEM/'sw'/(name+'.c'),'-o',obj],name+'.log')
        binary=[]
        for index,source in enumerate(a.fixtures):
            target=out/f'fixtures_{index}.bin';report['fixture_counts'][source.name]=pack_fixtures(source,target);binary.append(target)
        run(['verilator','--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','npu_tensor_v2',
             '--Mdir',out/'obj',SYSTEM/'rtl/npu_tensor_v2.sv',SYSTEM/'sim/test_tensor_v2.cpp',
             '-CFLAGS','-O2','-LDFLAGS',' '.join(str(o) for o in objects)],'build.log')
        run([out/'obj/Vnpu_tensor_v2',*binary],'simulation.log')
        if a.synthesis:
            run(['yosys','-Q','-T','-p',f'read_verilog -sv rtl/npu_tensor_v2.sv; hierarchy -top npu_tensor_v2; proc; opt; memory_dff; memory_share; memory_collect; wreduce; opt; stat; write_json {out}/resource.json'],'synthesis.log')
            design=json.loads((out/'resource.json').read_text());cells=design['modules']['npu_tensor_v2']['cells']
            memories=[]
            for name,cell in cells.items():
                if cell['type']=='$mem_v2':
                    params={k:int(v,2) for k,v in cell['parameters'].items() if isinstance(v,str) and set(v)<=set('01')}
                    memories.append({'name':name,**{k:params.get(k) for k in ('WIDTH','SIZE','RD_PORTS','WR_PORTS','RD_CLK_ENABLE')}})
            report['generic_memories']=memories
            report['generic_memory_bits']=sum(r['WIDTH']*r['SIZE'] for r in memories)
            report['generic_multipliers']=[{k:int(c['parameters'][k],2) for k in ('A_WIDTH','B_WIDTH','Y_WIDTH')} for c in cells.values() if c['type']=='$mul']
            report['generic_cell_count']=len(cells)
            report['physical_timing_closed']=False
        report['passed']=True
        print((out/'simulation.log').read_text(),end='')
    finally:
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print('TENSOR_V2_REPORT',out/'report.json')

if __name__=='__main__':main()
