"""Bounded tensor RTL regression, independent oracle and cache A/B benchmark (WSL)."""
from pathlib import Path
import argparse,json,subprocess,time,sys,hashlib
from run_tensor_v2_rtl import SYSTEM,pack_fixtures

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--single-cache',action='store_true')
 p.add_argument('--scalar-delta',action='store_true')
 p.add_argument('--scalar-conv',action='store_true')
 p.add_argument('--scalar-raw',action='store_true')
 p.add_argument('--shared-raw-accum',action='store_true')
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 sources=[SYSTEM/'rtl/npu_delta_vectors.sv',SYSTEM/'rtl/npu_tensor_v2.sv',SYSTEM/'sim/test_tensor_v2.cpp',SYSTEM/'sw/npu_tensor_reference.c',Path(__file__).resolve()]
 identity={str(s):hashlib.sha256(s.read_bytes()).hexdigest() for s in sources}
 report={'passed':False,'split_read_cache':not a.single_cache,'parallel_delta':not a.scalar_delta,'parallel_raw':not a.scalar_raw,'shared_raw_accum':a.shared_raw_accum,'source_sha256':identity,'physical_timing_closed':False,'board_verified':False}
 def run(command,log,limit=60):
  start=time.monotonic()
  with (out/log).open('w') as f:
   r=subprocess.run(list(map(str,command)),cwd=SYSTEM,stdout=f,stderr=subprocess.STDOUT,timeout=limit)
  report[log]={'exit':r.returncode,'seconds':round(time.monotonic()-start,3)}
  if r.returncode:raise RuntimeError((out/log).read_text()[-4000:])
 try:
  run([sys.executable,SYSTEM/'sim/test_tensor_reference.py','--raw-only','--output',out/'raw.json'],'oracle.log')
  pack_fixtures(out/'raw.json',out/'raw.bin')
  run([sys.executable,SYSTEM/'sim/test_tensor_reference.py','--norm-only','--output',out/'norm.json'],'norm_oracle.log')
  pack_fixtures(out/'norm.json',out/'norm.bin')
  objects=[]
  for name in ('npu_tensor_reference','npu_fixed_math'):
   obj=out/(name+'.o');objects.append(obj)
   run(['gcc','-O2','-I',SYSTEM/'sw','-c',SYSTEM/'sw'/(name+'.c'),'-o',obj],name+'.log')
  run(['verilator','--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','npu_tensor_v2',
       '-GPARALLEL_CONV='+str(int(not a.scalar_conv)),'-GSPLIT_READ_CACHE='+str(int(not a.single_cache)),'-GPARALLEL_DELTA='+str(int(not a.scalar_delta)),'-GPARALLEL_RAW='+str(int(not a.scalar_raw)),'-GSHARED_RAW_ACCUM='+str(int(a.shared_raw_accum)),
       '--Mdir',out/'obj',SYSTEM/'rtl/npu_delta_vectors.sv',SYSTEM/'rtl/npu_tensor_v2.sv',SYSTEM/'sim/test_tensor_v2.cpp',
       '-CFLAGS','-O2','-LDFLAGS',' '.join(map(str,objects))],'build.log',120)
  run([out/'obj/Vnpu_tensor_v2',out/'raw.bin',out/'norm.bin'],'simulation.log')
  if any(hashlib.sha256(Path(s).read_bytes()).hexdigest()!=h for s,h in identity.items()):raise RuntimeError('sources changed during regression')
  report['passed']=True;print((out/'simulation.log').read_text())
 finally:(out/'report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
