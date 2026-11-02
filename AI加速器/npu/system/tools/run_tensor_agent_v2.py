"""One real complete R4 Agent task on the tensor-v2 integer MODEL reference.
Preserves original tools/receipts and task scorer. No FPGA speed claim.
"""
import argparse,json,os,subprocess,time
from pathlib import Path
from run_tensor_model_v2 import SYSTEM,RUNTIME,sha,save
def replace_once(text,old,new):
 if text.count(old)!=1:raise ValueError('ambiguous integration anchor: '+old[:70])
 return text.replace(old,new)
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True);p.add_argument('--timeout-seconds',type=int,default=3600)
 p.add_argument('--assets',required=True,type=Path,help='verified model/tokenizer directory')
 a=p.parse_args();out=Path(a.output).resolve()
 if out.exists() or not out.is_relative_to(SYSTEM/'build'):p.error('fresh output below system/build')
 if not 1<=a.timeout_seconds<=7200:p.error('bounded timeout required')
 original=RUNTIME/'tests/agent_protocol_optimized_host.c';text=original.read_text()
 text=replace_once(text,'#include "qw_agent_protocol_backend.h"','#include "qw_agent_protocol_backend.h"\n#include "qw_tensor_v2.h"\n#include "npu_tensor_driver.h"')
 text=replace_once(text,'unsigned char *arena = aligned_alloc(32u, arena_bytes);',
 'size_t tensor_bytes=qw_tensor_workspace_bytes();\n    if(tensor_bytes>capacity-arena_bytes)return 1;\n    unsigned char *arena = aligned_alloc(32u, arena_bytes+tensor_bytes);')
 insertion='''npu_tensor_region tensor_grants[5]={
        {options.arena_ddr_offset+(uint32_t)plan.state_offset,((uint32_t)plan.state_bytes+31u)&~31u,3u},
        {options.arena_ddr_offset+(uint32_t)arena_bytes,(uint32_t)tensor_bytes,3u},
        {options.arena_ddr_offset+(uint32_t)plan.activation_offset,(uint32_t)plan.activation_bytes,3u},
        {options.arena_ddr_offset+(uint32_t)plan.partial_offset,(uint32_t)plan.partial_bytes,1u},
        {options.arena_ddr_offset+(uint32_t)plan.weights_offset,(uint32_t)plan.weights_bytes,1u}};
    npu_tensor_config tensor_config={0};tensor_config.mode=NPU_TENSOR_SOFTWARE_REFERENCE;
    tensor_config.mapping=(npu_tensor_mapping){arena,arena_bytes+tensor_bytes,options.arena_ddr_offset};
    tensor_config.regions=tensor_grants;tensor_config.region_count=5;
    npu_tensor_context tensor_device;
    if(npu_tensor_init(&tensor_device,&tensor_config))return 1;
    rc=qw_tensor_enable(model,npu_tensor_model_execute,&tensor_device,arena+arena_bytes,tensor_bytes,tensor_grants[1].offset);
    printf("TENSOR_GRAPH_ENABLE rc=%d integer_model=1 quality_approved=0 hardware=0\\n",rc);fflush(stdout);if(rc)return 1;
    uint64_t open_end = monotonic_ns(NULL);'''
 text=replace_once(text,'uint64_t open_end = monotonic_ns(NULL);',insertion)
 text=text.replace('graph=MIXED remaining_float=Norm,Delta,composite_decay,RoPE,body_quantize_reduce','graph=INTEGER_Q16_Q24 candidate_version=0x20000 legacy_flags_below_are_metadata_only')
 text=replace_once(text,'dump_profile(model, profile_enabled);','''dump_profile(model, profile_enabled);
    qw_tensor_status ts={0};qw_tensor_get_status(model,&ts);
    printf("TENSOR_GRAPH_STATUS jobs=%" PRIu64 " raw_jobs=%" PRIu64 " tokens=%" PRIu64 " saturations=%u underflows=%u invalid=%u unknown=%u\\n",ts.jobs,ts.raw_jobs,ts.tokens,ts.numeric.saturations,ts.numeric.underflows,ts.numeric.invalid_inputs,ts.unknown);''')
 out.mkdir(parents=True);generated=out/'agent_tensor_host.c';generated.write_text(text)
 core=sorted((RUNTIME/'sw/model').glob('qw_*.c'))
 agents=[RUNTIME/'sw/agent'/x for x in ('ag_agent.c','ag_loop.c','qw_tool_normalize.c','qw_agent_protocol_backend.c')]
 vendor=[RUNTIME/'sw/model/vendor/npu_v1'/x for x in ('npu_raw.c','npu_raw_reference.c','npu_runtime.c','npu_package.c','npu_storage.c','npu_ag_platform.c')]
 sources=[*core,*agents,*vendor,SYSTEM/'sw/npu_fixed_math.c',SYSTEM/'sw/npu_tensor_driver.c',SYSTEM/'sw/npu_tensor_reference.c',generated]
 resources=[a.assets.resolve()/name for name in ('HEADW4.BIN','CPU_RAW.BIN','TENSOR_HEAD.JSON','TOKEN.BIN')]+[SYSTEM/'integration/agent_protocol_20261102/analysis/protocol-r4-original-system.txt']
 identity=[*sources,original,*sorted((RUNTIME/'sw/model').glob('*.h')),*sorted((RUNTIME/'sw/agent').glob('*.h')),
 SYSTEM/'sw/npu_tensor_v2.h',SYSTEM/'sw/npu_tensor_driver.h',SYSTEM/'sw/npu_fixed_math.h',SYSTEM/'sw/npu_fixed_lut_generated.h',SYSTEM/'sw/npu_fixed_lut_q16_generated.h',Path(__file__)]
 report={'scope':'one_complete_R4_real_integer_model_tool_task','passed':False,'quality_approved':False,'hardware_execution':False,
 'source_sha256':{str(x):sha(x) for x in identity},'resources':{str(x):sha(x) for x in resources}}
 save(out/'report.json',report)
 try:
  cmd=['gcc','-std=c11','-O3','-flto','-fopenmp','-DQW_HOST_OPENMP','-DQW_ENABLE_TENSOR_V2','-DQW_CONTEXT_MAX=2048u','-DQW_AGENT_PROMPT_MAX=8192u',
  '-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffp-contract=off','-fno-fast-math',
  '-I'+str(RUNTIME/'sw/model'),'-I'+str(RUNTIME/'sw/agent'),'-I'+str(SYSTEM/'sw'),*map(str,sources),'-lm','-o',str(out/'agent_tensor_host')]
  report['build_argv']=cmd
  with(out/'build.log').open('wb')as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT)
  report['build_exit']=r.returncode
  if r.returncode:raise RuntimeError('Agent candidate compile failed')
  report['binary_sha256']=sha(out/'agent_tensor_host');start=time.monotonic()
  report['run_argv']=[str(out/'agent_tensor_host'),*map(str,resources),str(a.timeout_seconds*1000)]
  print('REAL_INTEGER_AGENT_TASK_RUNNING',out,flush=True)
  with(out/'model.log').open('wb')as log:r=subprocess.run(report['run_argv'],stdout=log,stderr=subprocess.STDOUT,env=dict(os.environ,OMP_NUM_THREADS='8'),timeout=a.timeout_seconds+30)
  report.update(exit_code=r.returncode,elapsed_host_seconds=time.monotonic()-start,log_sha256=sha(out/'model.log'))
  report['passed']=r.returncode==0 and 'AGENT_PROTOCOL_REAL_MODEL_PASS' in (out/'model.log').read_text()
  if not report['passed']:raise RuntimeError('real tool task failed; preserve original generation/receipts')
  return 0
 except BaseException as error:report['failure']=repr(error);raise
 finally:save(out/'report.json',report)
if __name__=='__main__':raise SystemExit(main())
