"""Concentrated handoff verification from a clean checkout; no assets or board IO.

Build/test outputs stay under system/build. Physical STA, FPGA token/s and model
quality are deliberately not claimed by this batch.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time

SYSTEM=Path(__file__).resolve().parents[1]
REPO=SYSTEM.parents[2]

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--toolchain',required=True,type=Path)
    p.add_argument('--output',required=True,type=Path)
    a=p.parse_args();out=a.output.resolve();tc=a.toolchain.resolve()
    if sys.platform!='linux':p.error('run in Linux/WSL')
    if out.exists() or not out.is_relative_to(SYSTEM/'build'):p.error('fresh output under system/build required')
    if not (tc/'bin/riscv-none-elf-gcc').is_file():p.error('installed xPack Linux RISC-V toolchain required')
    out.mkdir(parents=True)
    protected=[*SYSTEM.rglob('*.c'),*SYSTEM.rglob('*.h'),*SYSTEM.rglob('*.sv'),
               *SYSTEM.rglob('*.py'),*SYSTEM.rglob('*.ld'),*REPO.joinpath('rtl').glob('*.sv')]
    protected=[x for x in protected if not x.is_relative_to(SYSTEM/'build') and '__pycache__' not in x.parts]
    def identities():return {x.relative_to(REPO).as_posix():hashlib.sha256(x.read_bytes()).hexdigest() for x in protected}
    before=identities()
    report={'passed':False,'scope':'clean-checkout packaging and CPU/NPU integration acceptance',
            'board_verified':False,'physical_timing_closed':False,'quality_approved':False,
            'assets_required':False,'source_sha256':before,'checks':{}}
    def save():
        (out/'report.json').write_text(json.dumps(report,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    def check(name,script,*args,timeout=600):
        cmd=[sys.executable,str(SYSTEM/script),*map(str,args)]
        start=time.monotonic()
        with (out/(name+'.log')).open('w') as log:
            result=subprocess.run(cmd,cwd=REPO,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
        report['checks'][name]={'exit_code':result.returncode,'seconds':round(time.monotonic()-start,3),
                               'argv':cmd,'log_sha256':hashlib.sha256((out/(name+'.log')).read_bytes()).hexdigest()}
        save()
        if result.returncode:raise RuntimeError(f'{name} failed; see {out/(name+".log")}')
        print('PASS_HANDOFF_STAGE',name,flush=True)
    save()
    try:
        check('tensor_rtl','tools/run_tensor_v2_rtl.py','--output',out/'tensor_rtl')
        check('cpu_cache_ddr','tools/run_tensor_system_v2.py','--toolchain',tc,'--output',out/'cpu',
              '--ddr-outstanding','4','--matrix-lanes','64')
        check('cache_range','tools/run_cache_range.py','--toolchain',tc,'--output',out/'cache_range')
        check('queue4','tools/run_ddr_queue4.py','--output',out/'queue4')
        check('async_driver','tools/run_tensor_async_driver.py','--toolchain',tc,'--output',out/'async_driver')
        check('stream_loader','tools/run_stream_sha_loader.py','--toolchain',tc,'--output',out/'stream_loader')
        check('agent_build','tools/build_tensor_agent_cooperative.py','--toolchain',tc,'--output',out/'agent',
              '--ddr-resources','--sd-multiblock')
        report['sources_unchanged']=identities()==before
        report['passed']=report['sources_unchanged']
        if not report['passed']:raise RuntimeError('source changed during batch')
    except BaseException as error:
        report['failure']=repr(error)
        raise
    finally:save()
    print('HANDOFF_ACCEPTANCE_PASS',out/'report.json',flush=True)

if __name__=='__main__':main()
