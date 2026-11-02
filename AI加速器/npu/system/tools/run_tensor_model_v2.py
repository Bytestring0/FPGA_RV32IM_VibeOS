"""Build/run one actual full-model integer candidate comparison in WSL.
Reports execution, numerical drift and HOST throughput separately from quality
approval and FPGA performance. Never programs a board or writes model files.
"""
from __future__ import annotations
import argparse,hashlib,json,os,subprocess,sys,time
from pathlib import Path
SYSTEM=Path(__file__).resolve().parents[1]
RUNTIME=SYSTEM/'integration/agent_runtime_20261101'
def sha(p):
 h=hashlib.sha256()
 with Path(p).open('rb') as f:
  for block in iter(lambda:f.read(1<<20),b''):h.update(block)
 return h.hexdigest()
def save(p,data):p.write_text(json.dumps(data,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--output',required=True)
 parser.add_argument('--decode-count',type=int,default=8)
 parser.add_argument('--build-only',action='store_true')
 parser.add_argument('--assets',type=Path,help='directory with HEADW4.BIN, CPU_RAW.BIN, TENSOR_HEAD.JSON')
 a=parser.parse_args();out=Path(a.output).resolve()
 if sys.platform!='linux':parser.error('run in WSL/Linux')
 if out.exists() or not out.is_relative_to(SYSTEM/'build'):parser.error('fresh output under system/build required')
 if not 1<=a.decode_count<=64:parser.error('decode count 1..64')
 inputs=[a.assets.resolve()/name for name in ('HEADW4.BIN','CPU_RAW.BIN','TENSOR_HEAD.JSON')] if a.assets else []
 if not a.build_only and (not inputs or any(not p.is_file() for p in inputs)):parser.error('--assets must contain actual immutable model resources')
 core=sorted((RUNTIME/'sw/model').glob('qw_*.c'))
 vendor=[RUNTIME/'sw/model/vendor/npu_v1'/p for p in
 ('npu_raw.c','npu_raw_reference.c','npu_runtime.c','npu_package.c','npu_storage.c','npu_ag_platform.c')]
 sources=[*core,*vendor,SYSTEM/'sw/npu_tensor_driver.c',SYSTEM/'sw/npu_tensor_reference.c',
 SYSTEM/'sw/npu_fixed_math.c',SYSTEM/'sim/tensor_model_host.c']
 identity=[*sources,*sorted((RUNTIME/'sw/model').glob('qw_*.h')),
 SYSTEM/'sw/npu_tensor_v2.h',SYSTEM/'sw/npu_tensor_driver.h',
 SYSTEM/'sw/npu_fixed_math.h',SYSTEM/'sw/npu_fixed_lut_generated.h',SYSTEM/'sw/npu_fixed_lut_q16_generated.h',Path(__file__)]
 out.mkdir(parents=True)
 report={'scope':'actual_full_model_tensor_v2_host_reference','passed':False,
 'quality_approved':False,'board_verified':False,'official_bf16_comparison':False,
 'source_sha256':{str(p.relative_to(SYSTEM)):sha(p) for p in identity},
 'input_sha256':{str(p):sha(p) for p in inputs}}
 save(out/'report.json',report)
 try:
  command=['gcc','-std=c11','-O3','-flto','-fopenmp','-DQW_HOST_OPENMP',
  '-DQW_ENABLE_TENSOR_V2','-DQW_CONTEXT_MAX=2048u','-Wall','-Wextra','-Werror',
  '-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math',
  '-I'+str(RUNTIME/'sw/model'),'-I'+str(SYSTEM/'sw'),*map(str,sources),'-lm','-o',str(out/'tensor_model_host')]
  report['compile_argv']=command
  with (out/'build.log').open('wb') as log:r=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=False)
  report['compile_exit']=r.returncode
  if r.returncode:raise RuntimeError('candidate build failed; see build.log')
  report['binary_sha256']=sha(out/'tensor_model_host')
  if a.build_only:report.update(build_only=True,passed=True,execution_passed=False);return 0
  command=[str(out/'tensor_model_host'),*map(str,inputs),str(out),str(a.decode_count)]
  report['run_argv']=command;begin=time.monotonic()
  print('ACTUAL_FULL_MODEL_RUNNING',out,flush=True)
  with (out/'model.log').open('wb') as log:r=subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,OMP_NUM_THREADS='8'),check=False,timeout=3600)
  report['elapsed_host_seconds']=time.monotonic()-begin;report['exit_code']=r.returncode
  report['log_sha256']=sha(out/'model.log')
  if (out/'model_result.json').is_file():report['model_result']=json.loads((out/'model_result.json').read_text())
  report['passed']=r.returncode==0 and 'PASS_ACTUAL_MODEL_TENSOR_EXECUTION' in (out/'model.log').read_text()
  if not report['passed']:raise RuntimeError('actual model execution failed; preserve logs')
  return 0
 except BaseException as error:report['failure']=repr(error);raise
 finally:
  report['sources_unchanged']=all(sha(SYSTEM/p)==digest for p,digest in report['source_sha256'].items())
  save(out/'report.json',report)
if __name__=='__main__':raise SystemExit(main())
