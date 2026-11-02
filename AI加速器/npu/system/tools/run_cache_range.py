#!/usr/bin/env python3
"""One batch: cache range fault boundaries + actual RV32/cache/matrix/tensor DMA.
No model or board execution. Shared sources are hashed before/after.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import run_tensor_system_v2 as integrated

SYSTEM=Path(__file__).resolve().parents[1]
ROOT=SYSTEM.parents[2]

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--toolchain',required=True,type=Path)
    parser.add_argument('--output',required=True,type=Path)
    parser.add_argument('--repair-existing',action='store_true',help='repair failed integration by recompiling only firmware; verify RTL identity')
    args=parser.parse_args();out=args.output.resolve()
    if args.repair_existing:
        repair(args.toolchain,out)
        return
    if out.exists() or not out.is_relative_to(SYSTEM/'build'):raise ValueError('fresh build output required')
    out.mkdir(parents=True)
    selected=[integrated.common.CPU/'rtl/ag_l1cache.sv',integrated.common.CPU/'rtl/ag_soc.sv',
              *(SYSTEM/'sw'/n for n in ('npu_runtime.c','npu_runtime.h','npu_tensor_ag.c','npu_ag_platform.c')),
              integrated.common.CPU/'sim/cache_range_main.cpp',SYSTEM/'sim/cache_range_acceptance.c',Path(__file__)]
    before={str(p.relative_to(ROOT)):sha(p) for p in selected}
    report={'passed':False,'scope':'CACHE_RANGE_BOUNDARIES_AND_REAL_RV32_DMA',
            'board_verified':False,'source_sha256':before,'range_complexity':'full set scan with line-tag filtering'}
    raw_run=integrated.common.run
    def inject(argv,log,*a,**kw):
        argv=[SYSTEM/'sim/cache_range_acceptance.c' if str(x)==str(SYSTEM/'sw/tensor_acceptance.c') else x for x in argv]
        return raw_run(argv,log,*a,**kw)
    try:
        for name in ('npu_runtime.c','npu_runtime.h','npu_ag_platform.c'):
            vendor=SYSTEM/'integration/agent_runtime_20261101/sw/model/vendor/npu_v1'/name
            if sha(vendor)!=sha(SYSTEM/'sw'/name):raise ValueError('vendor differs: '+name)
        raw_run(['verilator','--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','ag_l1cache',
                 '-GSET_BITS=2','--Mdir',out/'cache_obj',integrated.common.CPU/'rtl/ag_l1cache.sv',
                 integrated.common.CPU/'sim/cache_range_main.cpp'],out/'cache_build.log',timeout=240)
        direct=raw_run([out/'cache_obj/Vag_l1cache'],out/'cache_range.log')
        if 'PASS_CACHE_RANGE' not in direct:raise ValueError('cache boundaries missing PASS')
        report['cache_boundaries_passed']=True
        integrated.common.run=inject
        sys.argv=['run_tensor_system_v2.py','--toolchain',str(args.toolchain),
                  '--output',str(out/'rv32'),'--profiles','single','--matrix-lanes','64']
        integrated.main()
        rv32=json.loads((out/'rv32/report.json').read_text())
        text=(out/'rv32/single.log').read_text()
        if not rv32['passed'] or 'PASS_RV32_RANGE_CSR' not in text:raise ValueError('RV32 range CSR checks absent')
        report['rv32_integration_passed']=True
        report['rv32_report']='rv32/report.json'
        report['source_changes_during_run']={str(p.relative_to(ROOT)):sha(p) for p in selected if before[str(p.relative_to(ROOT))]!=sha(p)}
        report['passed']=not report['source_changes_during_run']
        if not report['passed']:raise ValueError('source changed during batch')
    except BaseException as exc:
        report['failure']=repr(exc);raise
    finally:
        (out/'report.json').write_text(json.dumps(report,indent=2)+'\n',encoding='utf-8')
    print('PASS_CACHE_RANGE_SUITE',out/'report.json',flush=True)

def repair(toolchain,out):
    original=json.loads((out/'rv32/report.json').read_text())
    for name,digest in original['source_sha256'].items():
        if name.endswith('.sv') and sha(ROOT/name)!=digest:
            raise ValueError('compiled RTL changed: '+name)
    selected=[*(SYSTEM/'sw'/n for n in ('npu_runtime.c','npu_runtime.h','npu_tensor_driver.c','npu_tensor_driver.h',
              'npu_tensor_ag.c','npu_tensor_v2.h','npu_fixed_math.h','tensor_acceptance.c','freestanding.c')),
              SYSTEM/'sim/cache_range_acceptance.c',integrated.common.CPU/'sw/start.S',integrated.common.CPU/'sw/link.ld']
    before={str(p.relative_to(ROOT)):sha(p) for p in selected}
    result={'passed':False,'scope':'repair integration fixture publish; reuse unchanged RTL executable',
            'source_sha256':before,'rtl_source_sha256':{k:v for k,v in original['source_sha256'].items() if k.endswith('.sv')},
            'original_report_sha256':sha(out/'report.json'),'board_verified':False}
    raw_run=integrated.common.run
    def inject(argv,log,*a,**kw):
        argv=[SYSTEM/'sim/cache_range_acceptance.c' if str(x)==str(SYSTEM/'sw/tensor_acceptance.c') else x for x in argv]
        return raw_run(argv,log,*a,**kw)
    integrated.common.run=inject
    try:
        profiles={}
        for label,defines in [('repair_firmware',()),('repair_wrong_expected',('NEGATIVE_EXPECTATION=1',)),
                              ('repair_skip_cache',('NPU_TEST_SKIP_CACHE=1',))]:
            profiles[label]=integrated.firmware(toolchain,out/label,out/'rv32',defines)
        exe=out/'rv32/single_obj/Vag_npu_tensor_sim'
        result['rtl_executable_sha256']=sha(exe)
        text=raw_run([exe,'10000000','tensor'],out/'repair_single.log',cwd=out/'repair_firmware',timeout=180)
        data=integrated.common.result(text)
        if not data['done'] or data['exit_code'] or data['trap'] or 'PASS_RV32_RANGE_CSR' not in text or 'PASS_TENSOR_CPU_CACHE_DDR_V2' not in text:
            raise ValueError('range real RV32 integration incomplete')
        result['positive']=data
        for label in ('repair_wrong_expected','repair_skip_cache'):
            text=raw_run([exe,'10000000','tensor'],out/(label+'.log'),cwd=out/label,expected=1,timeout=180)
            data=integrated.common.result(text)
            if not data['done'] or data['exit_code']!=10 or data['trap']:raise ValueError('negative did not fail at DDR compare: '+label)
            result[label]=data
        result['firmware']=profiles
        result['source_changes_during_run']={str(p.relative_to(ROOT)):sha(p) for p in selected if before[str(p.relative_to(ROOT))]!=sha(p)}
        result['passed']=not result['source_changes_during_run']
        if not result['passed']:raise ValueError('compiled firmware source changed')
    except BaseException as exc:
        result['failure']=repr(exc);raise
    finally:
        (out/'repair_report.json').write_text(json.dumps(result,indent=2)+'\n',encoding='utf-8')
    print('PASS_CACHE_RANGE_REPAIR',out/'repair_report.json',flush=True)

if __name__=='__main__':main()
