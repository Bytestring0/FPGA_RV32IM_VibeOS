"""Focused async driver contract / sanitizer / RV32 compilation; no RTL rebuild."""
import argparse,hashlib,json,subprocess,time
from pathlib import Path
SYSTEM=Path(__file__).resolve().parents[1]
WORK=SYSTEM.parents[2]
CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--toolchain',required=True,type=Path);a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=True);tc=a.toolchain.resolve()
 sources=[SYSTEM/'sw/npu_tensor_driver.c',SYSTEM/'sw/npu_tensor_driver.h',SYSTEM/'sw/npu_tensor_reference.c',SYSTEM/'sw/npu_fixed_math.c',SYSTEM/'sw/npu_tensor_v2.h',SYSTEM/'sw/npu_fixed_math.h',SYSTEM/'sw/npu_fixed_lut_generated.h',SYSTEM/'sw/npu_fixed_lut_q16_generated.h',SYSTEM/'sim/test_tensor_driver.c',SYSTEM/'sim/test_tensor_async_mmio.c',Path(__file__).resolve()]
 report={'passed':False,'scope':'async driver contract, explicit synthetic CSR MMIO; no RTL/hardware/UI timing claim','source_sha256':{str(s.relative_to(WORK)):sha(s) for s in sources},'checks':{}}
 def run(name,cmd,cwd=WORK):
  start=time.monotonic();cmd=list(map(str,cmd));(out/(name+'.command.json')).write_text(json.dumps({'argv':cmd,'cwd':str(cwd)},indent=2))
  with(out/(name+'.log')).open('wb')as log:r=subprocess.run(cmd,cwd=cwd,stdout=log,stderr=subprocess.STDOUT,timeout=120)
  report['checks'][name]={'returncode':r.returncode,'seconds':round(time.monotonic()-start,3),'log_sha256':sha(out/(name+'.log'))}
  if r.returncode:raise RuntimeError(name+' failed: '+(out/(name+'.log')).read_text(errors='replace')[-4000:])
 try:
  base=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-pedantic','-I',SYSTEM/'sw']
  implementation=[SYSTEM/'sw/npu_tensor_driver.c',SYSTEM/'sw/npu_tensor_reference.c',SYSTEM/'sw/npu_fixed_math.c']
  for label,test,extra in [('reference','test_tensor_driver.c',[]),('async','test_tensor_async_mmio.c',['-DNPU_TENSOR_TEST_MMIO'])]:
   run(label+'_build',[*base,*extra,*implementation,SYSTEM/'sim'/test,'-o',out/label]);run(label,[out/label])
   run(label+'_sanitized_build',[*base,'-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer',*extra,*implementation,SYSTEM/'sim'/test,'-o',out/(label+'_sanitized')])
   run(label+'_sanitized',[out/(label+'_sanitized')])
  prefix=str(tc/'bin/riscv-none-elf-')
  flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-std=c11','-ffreestanding','-fno-builtin','-fno-pic','-msmall-data-limit=0','-mno-relax','-ffunction-sections','-fdata-sections','-Wall','-Wextra','-Werror','-fstack-usage','-nostdlib','-nostartfiles','-Wl,--gc-sections','-T',CPU/'sw/link.ld','-I',SYSTEM/'sw']
  run('rv32_contract_build',[prefix+'gcc',*flags,'-DNPU_TENSOR_TEST_MMIO',CPU/'sw/start.S',SYSTEM/'sim/test_tensor_async_mmio.c',SYSTEM/'sw/npu_tensor_driver.c',SYSTEM/'sw/npu_tensor_reference.c',SYSTEM/'sw/freestanding.c','-lgcc','-o',out/'rv32_contract.elf'])
  run('rv32_contract_undefined',[prefix+'nm','-u',out/'rv32_contract.elf'])
  run('rv32_contract_size',[prefix+'size',out/'rv32_contract.elf'])
  run('rv32_production_driver_compile',[prefix+'gcc',*flags,'-c',SYSTEM/'sw/npu_tensor_driver.c','-o',out/'rv32_production_driver.o'])
  report['rv32_execution']=False;report['rv32_elf_sha256']=sha(out/'rv32_contract.elf')
  report['source_changes_during_run']={str(s.relative_to(WORK)):sha(s) for s in sources if sha(s)!=report['source_sha256'][str(s.relative_to(WORK))]}
  if report['source_changes_during_run']:raise ValueError('sources changed during contract run')
  report['passed']=True
 except BaseException as error:report['failure']=repr(error);raise
 finally:(out/'report.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n')
 print('PASS_TENSOR_ASYNC_DRIVER',out/'report.json')
if __name__=='__main__':main()
