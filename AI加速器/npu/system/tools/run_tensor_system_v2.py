"""Focused RV32 CPU + production tensor/matrix RTL + enabled Cache + DDR acceptance.

WSL/Linux only. Adds no RTL/driver/model edits. Expected values stay in TCM;
external DDR BFM starts with poison and receives writes only from hardware.
"""
from __future__ import annotations
import argparse
import ctypes as C
import importlib.util
import json
import struct
import sys
import time
from pathlib import Path
import system_common as common

SYSTEM=common.SYSTEM
BASE=0x10000
CAPACITY=2048
NUMERIC=('status','domain_clamps','saturations','underflows','invalid_inputs','zero_vectors')


def load_oracle():
    path=SYSTEM/'sim/test_tensor_reference.py'
    spec=importlib.util.spec_from_file_location('tensor_fraction_oracle',path)
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
    return module


def fixtures(out):
    """Independent Python Fraction golden, cross-checked against host C reference."""
    oracle=load_oracle()
    so=out/'libtensor_reference.so'
    common.run(['gcc','-shared','-fPIC','-O2','-I',SYSTEM/'sw',SYSTEM/'sw/npu_tensor_reference.c',
                SYSTEM/'sw/npu_fixed_math.c','-o',so],out/'oracle_build.log')
    lib=C.CDLL(str(so));lib.npu_tensor_reference.argtypes=[C.POINTER(oracle.Job),C.POINTER(oracle.Mapping),C.POINTER(oracle.Stats)]
    rows=[];memory=bytearray([0xa5]*CAPACITY)
    def add(op,name,*,kind=0,zero=False,edges=False,from_previous=False):
        nonlocal memory
        if kind!=1:memory=bytearray([0xa5]*CAPACITY)
        d={f:0 for f in oracle.FIELDS}
        d.update(opcode=op,n=10 if op==10 else 9,m=3,k=4,a=BASE,b=BASE+256,c=BASE+512,out=BASE+768,state=BASE+1024)
        if op==11:d['param0']=1
        if op in (8,22):d['param1']=1
        if op in (13,20):d.update(param0=1030792151,param1=536870912)
        if from_previous:d.update(a=BASE+768,out=BASE+1536,param0=281477120)
        if op==14:d.update(n=8,k=8,param0=536870912)
        if op==19:d.update(n=2,m=2,c=0,stride_a=32,param0=65536)
        initial=bytearray(memory);writes={}
        def put(relative,value):
            struct.pack_into('<I',memory,relative,value&0xffffffff);writes[relative//4]=value&0xffffffff
        if op!=19:
            na,nb,nc,ns=oracle.lengths(d)
            vectors=[]
            for field,count,mult,offset in [('a',na,7919,100000),('b',nb,6151,90000),('c',nc,3563,65000),('state',ns,100003,16000000)]:
                vals=[]
                for i in range(count):
                    if (field=='state' and kind==1) or (field=='a' and from_previous):v=struct.unpack_from('<i',memory,d[field]-BASE+4*i)[0]
                    else:
                        v=0 if zero else (i*mult+(12347 if kind==1 else 0))%(2*offset+1)-offset
                        if op in (4,5,6,7) and i in (0,8):v=(-1200000 if i==0 else 1200000)
                        if op==10 and field in ('b','c'):v=900000000 if field=='b' else 500000000
                        if edges and field in ('a','b'):
                            samples=(-1,-3,2147483647,-2147483648) if field=='a' else (32768,32768,2147483647,-2147483648)
                            v=samples[i%4]
                        put(d[field]-BASE+4*i,v)
                    vals.append(v)
                vectors.append(vals)
            o=oracle.Oracle();output,state,result=o.execute(d,*vectors)
            expected=bytearray(memory)
            for i,v in enumerate(output):
                if op==15:expected[d['out']-BASE+i]=v&255
                else:struct.pack_into('<i',expected,d['out']-BASE+4*i,v)
            for i,v in enumerate(state):struct.pack_into('<i',expected,d['state']-BASE+4*i,v)
        else:
            o=oracle.Oracle();result=0
            partials=[]
            for row in range(2):
                for group in range(2):
                    partial=sum((((row*11+c*7+31)&15)-8)*((c*13+31)%255-127) for c in range(group*128,min((group+1)*128,129)))
                    put(row*32+group*8,row|(group<<16));put(row*32+group*8+4,partial);partials.append(partial)
            # FP16 scales [1,.5] per row. These are inputs, not expected outputs.
            put(256,0x38003c00);put(260,0x38003c00)
            expected=bytearray(memory)
            for row in range(2):
                terms=[]
                for group in range(2):
                    coef=o.sat(o.rne(65536*16384,127*(1 if group==0 else 2)))
                    terms.append(o.rne(partials[row*2+group]*coef,1<<14))
                struct.pack_into('<i',expected,768+row*4,o.sat(o.sum64(terms)))
        data=(C.c_ubyte*CAPACITY).from_buffer_copy(memory)
        mapping=oracle.Mapping(C.cast(data,C.c_void_p),CAPACITY,BASE);stats=oracle.Stats();job=oracle.Job(**d)
        rc=lib.npu_tensor_reference(C.byref(job),C.byref(mapping),C.byref(stats))
        want=[getattr(o,k) for k in NUMERIC]
        if rc or bytes(data)!=expected or stats.result!=result or [getattr(stats.numeric,k) for k in NUMERIC]!=want:
            raise ValueError(f'independent oracle disagreement {name}: rc={rc}')
        changes=[(i//4,struct.unpack_from('<I',expected,i)[0]) for i in range(0,CAPACITY,4) if expected[i:i+4]!=memory[i:i+4]]
        # Always include all output/state words, even when equal to initial poison.
        rows.append(dict(name=name,descriptor=[d[f] for f in oracle.FIELDS],kind=kind,
                         initial=sorted(writes.items()),expected=changes,stats=[0,result,*want]))
        memory=expected
    for op in range(1,19):
        add(op,f'op{op}',kind=3 if op==14 else 0)
        if op in (12,13):add(op,f'op{op}_second_state_step',kind=1)
    add(20,'delta_q24_first')
    add(20,'delta_q24_second_state_step',kind=1)
    add(8,'delta_q24_to_norm_q48_epsilon',kind=1,from_previous=True)
    add(22,'reciprocal_norm')
    add(3,'mul_rne_saturation',edges=True)
    add(15,'quant_zero',zero=True)
    add(19,'matrix_v1_to_reduce_raw_w4',kind=2)
    header=['/* Generated from independent Fraction oracle; expected data is TCM-only. */',
            '#define TENSOR_SYSTEM_OPCODE_COUNT %du'%len({r['descriptor'][0] for r in rows})]
    for index,row in enumerate(rows):
        for key in ('initial','expected'):
            pairs=row[key]
            values=','.join('{%du,0x%08xu}'%(off,value) for off,value in pairs) or '{0,0}'
            header.append(f'static const struct patch f{index}_{key}[]={{'+values+'};')
    header.append('static const struct fixture fixtures[]={')
    for i,r in enumerate(rows):
        job=','.join(str(x)+'u' for x in r['descriptor']);s=r['stats']
        stats='{%du,%du,{'%(s[0],s[1])+','.join(str(x)+'u' for x in s[2:])+'}}'
        header.append('{"%s",{%s},f%d_initial,f%d_expected,%du,%du,%du,%s},'%(r['name'],job,i,i,len(r['initial']),len(r['expected']),r['kind'],stats))
    header.append('};')
    # A separate mode1 CPU/cache/MMIO check: 3 rows, 129 cols, FP16 scales
    # start at byte2 within a line. Goldens stay in TCM; no target reference.
    fused=[]
    for maximum in (65536,32768):
        values=[];o=oracle.Oracle()
        for row in range(3):
            terms=[]
            for group in range(2):
                partial=sum((((row*11+c*7+31)&15)-8)*((c*13+31)%255-127) for c in range(group*128,min((group+1)*128,129)))
                coefficient=o.sat(o.rne(maximum*16384,127*(1 if group==0 else 2)))
                terms.append(o.rne(partial*coefficient,1<<14))
            values.append(o.sat(o.sum64(terms)))
        fused.append(values)
    header.append('static const uint32_t fused_expected[2][3] __attribute__((unused)) = {'+','.join('{'+','.join(str(x&0xffffffff)+'u' for x in row)+'}' for row in fused)+'};')
    (out/'tensor_system_fixtures.h').write_text('\n'.join(header)+'\n')
    common.save(out/'fixtures.json',{'oracle':'independent Fraction, cross-checked host C', 'rows':rows})
    return rows


def firmware(tc,out,include,defines=()):
    out.mkdir();prefix=str(tc);elf=out/'firmware.elf'
    flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-std=c11','-ffreestanding','-fno-builtin',
           '-fno-pic','-msmall-data-limit=0','-mno-relax','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror',
           '-fstack-usage','-nostdlib','-nostartfiles','-Wl,--gc-sections','-Wl,-Map,'+str(out/'firmware.map'),
           '-T',common.CPU/'sw/link.ld','-I',SYSTEM/'sw','-I',include,
           '-I',SYSTEM/'integration/agent_runtime_20261101/sw/model/freestanding_include']
    sources=[common.CPU/'sw/start.S',*(SYSTEM/'sw'/name for name in ('tensor_acceptance.c','npu_tensor_driver.c',
              'npu_tensor_ag.c','npu_tensor_reference.c','npu_runtime.c','freestanding.c','npu_fused_matrix.c','npu_raw.c'))]
    common.run([prefix+'gcc',*flags,*('-D'+v for v in defines),*sources,'-lgcc','-o',elf],out/'compile.log')
    common.run([prefix+'objcopy','-O','binary',elf,out/'firmware.bin'],out/'objcopy.log')
    if common.run([prefix+'nm','-u',elf],out/'undefined.log').strip():raise ValueError('undefined firmware symbols')
    symbols=common.run([prefix+'nm','--size-sort',elf],out/'symbols.log')
    if 'npu_tensor_reference' in symbols:raise ValueError('host oracle linked into RV32 firmware')
    size=(out/'firmware.bin').stat().st_size
    if size>57344:raise ValueError(f'firmware exceeds TCM reserve: {size}')
    common.run([sys.executable,common.CPU/'scripts/bin2hex.py',out/'firmware.bin',out/'firmware.hex'],out/'hex.log')
    return {'binary_bytes':size,'elf_sha256':common.sha(elf),'host_reference_linked':False}


def harness(out,case_count,ddr_outstanding=1):
    """Reuse the original protocol checks; generate an ordered credit BFM."""
    original=SYSTEM/'sim/test_system.cpp';code=original.read_text()
    def change(old,new):
        nonlocal code
        if code.count(old)!=1:raise ValueError('ambiguous harness anchor: '+old[:80])
        code=code.replace(old,new)
    code=code.replace('Vag_npu_sim','Vag_npu_tensor_sim')
    change('#include <vector>','#include <vector>\n#include <deque>\n#include <algorithm>')
    change('    bool pending=false,response_error=false,held=false,trapped=false;',
        '    struct Return { uint64_t due; std::array<uint32_t,8> data{}; bool error=false; };\n'
        '    std::deque<Return> pending; unsigned peak_pending=0;\n'
        '    unsigned tensor_completions=0,tensor_requests=0,matrix_requests=0,cpu_requests=0; uint64_t overlap=0; bool last_tensor_done=false;\n'
        '    bool held=false,trapped=false;')
    change('    std::array<uint32_t,8> response{};','    const unsigned ddr_outstanding='+str(ddr_outstanding)+';')
    change('    uint32_t latency=0,random=0x32718ab9,reads=0,writes=0,stalls=0,spi_edges=0;',
           '    uint32_t random=0x32718ab9,reads=0,writes=0,stalls=0,spi_edges=0;')
    change('        d.mem_req_ready=!d.rst&&!pending&&((random&3u)!=0);',
           '        d.mem_req_ready=!d.rst&&pending.size()<ddr_outstanding&&((random&3u)!=0);')
    change('        d.mem_rsp_valid=!d.rst&&pending&&latency==0;\n        d.mem_rsp_error=response_error;\n        for(int i=0;i<8;i++) d.mem_rsp_data[i]=response[i];',
           '        d.mem_rsp_valid=!d.rst&&!pending.empty()&&pending.front().due<=cycles;\n'
           '        d.mem_rsp_error=!pending.empty()&&pending.front().error;\n'
           '        for(int i=0;i<8;i++) d.mem_rsp_data[i]=pending.empty()?0:pending.front().data[i];')
    change('        if(d.rst){pending=false;held=false;latency=0;}',
           '        if(d.rst){pending.clear();held=false;peak_pending=0;}')
    change('            if(reply)pending=false;\n            else if(pending&&latency)--latency;',
           '            if(reply){if(pending.empty())throw std::runtime_error("DDR ACK without credit");pending.pop_front();}')
    change('                if(pending)throw std::runtime_error("more than one downstream transaction");\n                pending=true;latency=2+(random&7u);\n                response_error=(q.address&31u)||uint64_t(q.address)+32>ram.size();\n                response.fill(0);',
           '                if(pending.size()>=ddr_outstanding)throw std::runtime_error("DDR return credit overflow");\n'
           '                Return response;response.due=cycles+3+(random&7u);\n'
           '                response.error=(q.address&31u)||uint64_t(q.address)+32>ram.size();')
    change('                if(!response_error){','                if(!response.error){')
    change('response[b/4]|=uint32_t(ram[q.address+b])','response.data[b/4]|=uint32_t(ram[q.address+b])')
    change('            }\n            if(d.npu_done&&pending&&d.ddr_transaction_owner)',
           '                pending.push_back(response);peak_pending=std::max(peak_pending,unsigned(pending.size()));\n'
           '            }\n            if((d.npu_done&&d.matrix_outstanding)||(d.tensor_done&&d.tensor_outstanding))')
    anchor='    bool ok=d.done&&!d.exit_code&&!trapped&&d.instret_count>100;'
    start=code.index(anchor);end=code.index('    std::puts(ok?',start)
    code=code[:start]+'''    std::printf("TENSOR_SYSTEM_BUS {\\"tensor_completions\\":%u,\\"overlap_cycles\\":%llu,\\"tensor_requests\\":%u,\\"matrix_requests\\":%u,\\"cpu_requests\\":%u,\\"ddr_outstanding\\":%u,\\"peak_pending\\":%u}\\n",tensor_completions,(unsigned long long)overlap,tensor_requests,matrix_requests,cpu_requests,ddr_outstanding,peak_pending);
    bool ok=d.done&&!d.exit_code&&!trapped&&d.instret_count>100&&
        d.dma_completions==3&&reads>100&&writes>100&&stalls>0&&overlap>0&&
        tensor_completions==CASE_COUNT&&tensor_requests>0&&matrix_requests>0&&cpu_requests>0&&
        pending.empty()&&!d.matrix_outstanding&&!d.tensor_outstanding&&
        (ddr_outstanding==1||peak_pending>1)&&
        console.find("PASS_TENSOR_CPU_CACHE_DDR_V2")!=std::string::npos;
'''.replace('CASE_COUNT',str(case_count))+code[end:]
    anchor='        bool old_sd_clk=d.sd_clk;'
    change(anchor,'''        if(!d.rst){
            tensor_requests+=d.tensor_request;matrix_requests+=d.matrix_request;cpu_requests+=d.cpu_request;
            overlap+=d.tensor_busy&&d.npu_busy;
            if(d.tensor_done&&!last_tensor_done)++tensor_completions;
            last_tensor_done=d.tensor_done;
        }
'''+anchor)
    path=out/'test_tensor_system.cpp';path.write_text(code)
    return path


def tagged(text,tag):
    rows=[json.loads(line[len(tag)+1:]) for line in text.splitlines() if line.startswith(tag+' ')]
    if len(rows)!=1:raise ValueError(f'missing or duplicate {tag}')
    return rows[0]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    tc_group=p.add_mutually_exclusive_group(required=True);tc_group.add_argument('--toolchain',type=Path);tc_group.add_argument('--compiler-prefix')
    p.add_argument('--timeout-seconds',type=int,default=60);p.add_argument('--output',required=True,type=Path)
    p.add_argument('--cpu-mode',choices=('original','portable_metadata'),default='original',help='production CPU RTL by default; compatibility patch is explicit and never valid for PDS')
    p.add_argument('--partial-sram',action='store_true')
    p.add_argument('--fused-matrix',action='store_true',help='include real mode1 driver + CPU cached Q16 readback, twice')
    p.add_argument('--scalar-raw',action='store_true',help='area-only full system variant; numeric ABI unchanged')
    p.add_argument('--scalar-delta',action='store_true',help='area-only full system variant; numeric ABI unchanged')
    p.add_argument('--shared-raw-accum',action='store_true',help='reuse exact raw RNE/saturation accumulator; numeric order unchanged')
    p.add_argument('--matrix-lanes',type=int,choices=(16,32,64),default=64)
    p.add_argument('--ddr-outstanding',type=int,choices=(1,4),default=1)
    p.add_argument('--profiles',choices=('single','dual'),nargs='+',default=['single','dual'])
    a=p.parse_args();out=a.output.resolve();tc=str(a.toolchain.resolve()/'bin/riscv-none-elf-') if a.toolchain else a.compiler_prefix
    if not 1<=a.timeout_seconds<=120:p.error('timeout-seconds must be 1..120')
    original_run=common.run
    def bounded_run(*args,**kwargs):
        kwargs['timeout']=min(kwargs.get('timeout',a.timeout_seconds),a.timeout_seconds)
        return original_run(*args,**kwargs)
    common.run=bounded_run
    if out.exists() or not out.is_relative_to(SYSTEM/'build'):raise ValueError('new output under system/build required')
    if not Path(tc+'gcc').is_file():raise ValueError('installed WSL Linux toolchain required')
    out.mkdir(parents=True)
    # Guard actual RTL, firmware inputs, and Python goldens/harness. Editing
    # reporting-only tools must not invalidate a passed hardware simulation.
    selected=[SYSTEM/'rtl',common.CPU/'rtl',SYSTEM/'sw']
    protected=[p for folder in selected for p in folder.glob('*') if p.is_file()]
    protected += [SYSTEM/'sim/ag_npu_tensor_sim.sv',SYSTEM/'sim/test_system.cpp',
                  SYSTEM/'sim/test_tensor_reference.py',SYSTEM/'tools/system_common.py',
                  Path(__file__),common.CPU/'rtl/files.f',
                  *SYSTEM.joinpath('integration/agent_runtime_20261101/sw/model/freestanding_include').glob('*.h'),
                  common.NPU/'rtl/npu_bram.sv',common.NPU/'rtl/npu_bram_top.sv',
                  *(common.CORE/n for n in ('mac_lane.sv','group_acc.sv','gemv_core.sv')),
                  common.CPU/'sw/start.S',common.CPU/'sw/link.ld',common.CPU/'scripts/bin2hex.py']
    protected=sorted(set(protected))
    before={str(p.relative_to(common.WORK)):common.sha(p) for p in protected}
    report={'passed':False,'scope':'real RV32 + production ag_npu_system tensor/matrix + Cache/DDR simulation',
            'board_verified':False,'physical_timing_closed':False,'full_model_inference':False,
            'cpu_mode':a.cpu_mode,'tensor_v2_enabled':True,'matrix_lanes':a.matrix_lanes,
            'tensor_parallel_raw':not a.scalar_raw,'tensor_parallel_delta':not a.scalar_delta,
            'tensor_shared_raw_accum':a.shared_raw_accum,
            'cache_enabled':True,'ddr_outstanding':a.ddr_outstanding,
            'ddr_bfm':'ordered credit-reserved request/response deque','source_sha256':before,'profiles':{}}
    common.save(out/'report.json',report)
    try:
        report['tools']={'verilator':common.run(['verilator','--version'],out/'verilator.log').strip(),
                         'compiler':common.run([tc+'gcc','--version'],out/'compiler.log').splitlines()[0]}
        report['partial_sram_enabled']=a.partial_sram
        rows=fixtures(out);report['fixture_count']=len(rows);report['opcodes']=sorted({r['descriptor'][0] for r in rows})
        if not rows:raise ValueError('empty fixture set')
        defines=(['TEST_PARTIAL_SRAM=1'] if a.partial_sram else [])+(['NPU_TEST_FUSED=1'] if a.fused_matrix else [])
        report['fused_matrix_enabled']=a.fused_matrix
        report['firmware']=firmware(tc,out/'firmware',out,defines)
        report['wrong_expected_firmware']=firmware(tc,out/'wrong_expected',out,['NEGATIVE_EXPECTATION=1']+defines)
        report['skip_cache_firmware']=firmware(tc,out/'skip_cache',out,['NPU_TEST_SKIP_CACHE=1']+defines)
        if a.fused_matrix:report['wrong_fused_firmware']=firmware(tc,out/'wrong_fused',out,['NEGATIVE_FUSED_EXPECTATION=1']+defines)
        cpu_sources=[common.CPU/line.strip() for line in (common.CPU/'rtl/files.f').read_text().splitlines() if line.strip() and not line.startswith('#')]
        if a.cpu_mode=='portable_metadata':
            original=common.CPU/'rtl/rv32_core.sv';code=original.read_text()
            changes=[('    wire [6:0] first_meta = fetch_meta[0], second_meta = fetch_meta[1];',
                      '    wire [6:0] response_meta = issue_metadata(i_rsp_data[31:0]);\n    wire [6:0] response_meta_second = issue_metadata(i_rsp_data[63:32]);\n    wire [6:0] first_meta = fetch_meta[0], second_meta = fetch_meta[1];'),
                     ('fetch_meta[fi] <= issue_metadata(i_rsp_data[31:0]);','fetch_meta[fi] <= response_meta;'),
                     ('fetch_meta[fi] <= issue_metadata(i_rsp_data[63:32]);','fetch_meta[fi] <= response_meta_second;')]
            for old,new in changes:
                if code.count(old)!=1:raise ValueError('ambiguous compatibility anchor')
                code=code.replace(old,new)
            copy=out/'rv32_core_sim.sv';copy.write_text(code);cpu_sources=[copy if p==original else p for p in cpu_sources]
            report['cpu_compatibility']={'production_modified':False,'formal_equivalence_verified':False,'original_sha256':common.sha(original),'copy_sha256':common.sha(copy)}
        sources=[*cpu_sources,common.NPU/'rtl/npu_bram.sv',common.NPU/'rtl/npu_bram_top.sv',
                 *(common.CORE/n for n in ('mac_lane.sv','group_acc.sv','gemv_core.sv')),
                 *sorted((SYSTEM/'rtl').glob('*.sv')),SYSTEM/'sim/ag_npu_tensor_sim.sv']
        executed_rows=rows+([rows[-1],rows[-1]] if a.partial_sram else [])
        cpp=harness(out,len(executed_rows),a.ddr_outstanding)
        if a.partial_sram:
            code=cpp.read_text().replace('d.dma_completions==3','d.dma_completions==5');cpp.write_text(code)
        if a.fused_matrix:
            count=3+(2 if a.partial_sram else 0)
            code=cpp.read_text().replace('d.dma_completions=='+str(count),'d.dma_completions=='+str(count+2));cpp.write_text(code)
        for label in a.profiles:
            dual=int(label=='dual');obj=out/(label+'_obj')
            common.run(['verilator','--cc','--exe','--build','--timing','-j','4','-O3','-CFLAGS','-O3',
                '-Wno-fatal','--top-module','ag_npu_tensor_sim',"-GDUAL_ISSUE=1'b"+str(dual),
                "-GPARTIAL_SRAM_ENABLE=1'b"+str(int(a.partial_sram)),'-GMATRIX_LANES='+str(a.matrix_lanes),'-GDDR_OUTSTANDING='+str(a.ddr_outstanding),
                '-GTENSOR_PARALLEL_RAW='+str(int(not a.scalar_raw)),
                '-GTENSOR_PARALLEL_DELTA='+str(int(not a.scalar_delta)),
                '-GTENSOR_SHARED_RAW_ACCUM='+str(int(a.shared_raw_accum)),*sources,'--Mdir',obj,cpp],out/(label+'_build.log'),timeout=900)
            exe=obj/'Vag_npu_tensor_sim'
            text=common.run([exe,'10000000','tensor'],out/(label+'.log'),cwd=out/'firmware',timeout=180)
            result=common.result(text);bus=tagged(text,'TENSOR_SYSTEM_BUS');summary=tagged(text,'TENSOR_SYSTEM_SUMMARY')
            cases=[json.loads(line.split(' ',1)[1]) for line in text.splitlines() if line.startswith('TENSOR_SYSTEM_CASE ')]
            if len(cases)!=len(executed_rows) or summary['jobs']!=len(executed_rows) or summary['opcodes']!=len(report['opcodes']) or result['trap'] or result['exit_code'] or not result['done'] or (dual and not result['dual_retired']):raise ValueError('CPU tensor acceptance incomplete')
            wrong=common.result(common.run([exe,'10000000','tensor'],out/(label+'_wrong_expected.log'),cwd=out/'wrong_expected',expected=1,timeout=180))
            stale=common.result(common.run([exe,'10000000','tensor'],out/(label+'_skip_cache.log'),cwd=out/'skip_cache',expected=1,timeout=180))
            if not wrong['done'] or wrong['exit_code']!=10 or wrong['trap']:raise ValueError('wrong golden did not fail at DDR compare')
            if not stale['done'] or stale['trap'] or stale['exit_code'] not in ((10,43) if a.fused_matrix else (10,)):raise ValueError('skip-cache negative did not reject stale data')
            if a.fused_matrix and stale['exit_code']==43 and 'FUSED_EXECUTE_FAILURE' not in (out/(label+'_skip_cache.log')).read_text():raise ValueError('skip-cache failure not in fused driver')
            if a.fused_matrix:
                fused=[json.loads(line.split(' ',1)[1]) for line in text.splitlines() if line.startswith('FUSED_MATRIX_SYSTEM ')]
                if len(fused)!=2 or any(r['rows']!=3 or r['result_count']!=3 for r in fused):raise ValueError('missing fused driver CPU/cache completion')
                negative=common.result(common.run([exe,'10000000','tensor'],out/(label+'_wrong_fused.log'),cwd=out/'wrong_fused',expected=1,timeout=180))
                if negative['exit_code']!=10 or negative['trap'] or not negative['done']:raise ValueError('wrong fused golden did not fail at CPU Q16 compare')
                report.setdefault('fused_matrix_profiles',{})[label]={'runs':fused,'wrong_expected':negative,'skip_cache':stale}
            report['profiles'][label]={'result':result,'bus':bus,'summary':summary,'cases':cases,'wrong_expected':wrong,'skip_cache':stale}
            common.save(out/'report.json',report)
            print('PASS_TENSOR_SYSTEM_V2',label,json.dumps(result),flush=True)
        after={str(p.relative_to(common.WORK)):common.sha(p) for p in protected}
        report['source_changes_during_run']={k:after[k] for k in before if before[k]!=after[k]}
        if report['source_changes_during_run']:raise ValueError('shared source changed during acceptance; report lists affected files')
        report['passed']=True
    except BaseException as exc:
        report['failure']=repr(exc);raise
    finally:common.save(out/'report.json',report)
    print('TENSOR_SYSTEM_V2_REPORT',out/'report.json',flush=True)

if __name__=='__main__':main()
