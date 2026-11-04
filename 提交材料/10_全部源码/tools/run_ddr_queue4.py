"""Concentrated depth4 bridge/owner-FIFO contract and unchanged depth1 regression."""
import argparse,hashlib,json,subprocess,time
from pathlib import Path
SYSTEM=Path(__file__).resolve().parents[1];WORK=SYSTEM.parents[2];CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--bridge',type=Path,default=CPU/'rtl/ag_hmic_bridge.sv');a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);bridge=a.bridge.resolve()
 sources=[bridge,SYSTEM/'rtl/ddr_arbiter2.sv',SYSTEM/'sim/ddr_queue4_tb.sv',SYSTEM/'sim/test_ddr_queue4.cpp',CPU/'rtl/ag_extmem.sv',CPU/'sim/ddr_bridge_tb.sv',Path(__file__).resolve()]
 report={'passed':False,'scope':'ordered line transport + CDC protocol simulation; no DRAM PHY/board timing/prefetch claim','sources':{str(s):sha(s) for s in sources},'checks':{}}
 def run(name,cmd,timeout=180):
  start=time.monotonic();cmd=list(map(str,cmd));(out/(name+'.command.json')).write_text(json.dumps(cmd,indent=2))
  with(out/(name+'.log')).open('wb')as log:r=subprocess.run(cmd,cwd=WORK,stdout=log,stderr=subprocess.STDOUT,timeout=timeout)
  report['checks'][name]={'exit':r.returncode,'seconds':round(time.monotonic()-start,3),'log_sha256':sha(out/(name+'.log'))}
  if r.returncode:raise RuntimeError(name+': '+(out/(name+'.log')).read_text(errors='replace')[-6000:])
 try:
  run('queue_build',['verilator','--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','ddr_queue4_tb','--Mdir',out/'queue_obj',bridge,SYSTEM/'rtl/ddr_arbiter2.sv',SYSTEM/'sim/ddr_queue4_tb.sv',SYSTEM/'sim/test_ddr_queue4.cpp','-CFLAGS','-O2'])
  for cpu,ui in [(5,3),(3,7),(4,4),(11,2)]:
   name=f'queue_{cpu}_{ui}';run(name,[out/'queue_obj/Vddr_queue4_tb',cpu,ui]);print((out/(name+'.log')).read_text(),end='',flush=True)
  run('legacy_build',['verilator','--binary','--timing','-j','4','-Wno-fatal','--top-module','ddr_bridge_tb','--Mdir',out/'legacy_obj',bridge,CPU/'rtl/ag_extmem.sv',CPU/'sim/ddr_bridge_tb.sv'])
  run('legacy',[out/'legacy_obj/Vddr_bridge_tb'])
  report['source_changes']={str(s):sha(s) for s in sources if sha(s)!=report['sources'][str(s)]}
  if report['source_changes']:raise ValueError('source changed')
  report['passed']=True
 except BaseException as e:report['failure']=repr(e);raise
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 print('PASS_DDR_QUEUE4_REPORT',out/'report.json')
if __name__=='__main__':main()
