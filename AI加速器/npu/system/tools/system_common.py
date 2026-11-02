"""Unified current-AG / DDR-cache / 16-lane DMA NPU / SPI-card acceptance.
Run in WSL with an explicit installed RISC-V toolchain and a new output directory.
"""
import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

SYSTEM=Path(__file__).resolve().parents[1]
NPU=SYSTEM.parent
WORK=NPU.parent.parent
CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK
CORE=NPU.parent/'rtl/compute'

def sha(p): return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def save(p,v): p.write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')

def run(argv,log,*,cwd=CPU,expected=0,timeout=600):
    argv=list(map(str,argv))
    save(log.with_suffix('.command.json'),{'argv':argv,'cwd':str(cwd),'expected_exit':expected})
    try:
        with log.open('wb') as out:
            result=subprocess.run(argv,cwd=cwd,stdout=out,stderr=subprocess.STDOUT,timeout=timeout,check=False)
        code=result.returncode
    except subprocess.TimeoutExpired:
        save(log.with_suffix('.result.json'),{'timeout':True,'sha256':sha(log)})
        raise
    save(log.with_suffix('.result.json'),{'actual_exit':code,'expected_exit':expected,'sha256':sha(log)})
    text=log.read_text(encoding='utf-8',errors='replace')
    if code!=expected:
        raise RuntimeError(f'{log}: exit {code}, expected {expected}\n'+'\n'.join(text.splitlines()[-35:]))
    return text

def result(text):
    rows=[json.loads(s.split('SYSTEM_RESULT ',1)[1]) for s in text.splitlines() if s.startswith('SYSTEM_RESULT ')]
    if len(rows)!=1: raise ValueError('missing/duplicate SYSTEM_RESULT')
    return rows[0]
