"""Run actual model operands through production NPU RTL, with host graph scheduling.
This does not emulate the RV32 CPU or claim board token throughput.
"""
import argparse,json,os,re,subprocess,time
from pathlib import Path
SYSTEM=Path(__file__).resolve().parents[1]
RUNTIME=SYSTEM/'integration/agent_runtime_20261101'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',required=True,type=Path);p.add_argument('--assets',required=True,type=Path);p.add_argument('--timeout-seconds',type=int,default=600);a=p.parse_args()
 out=a.output.resolve();out.mkdir(parents=True,exist_ok=False)
 src=[*sorted((RUNTIME/'sw/model').glob('qw_*.c')),*[RUNTIME/'sw/model/vendor/npu_v1'/n for n in ('npu_raw.c','npu_raw_reference.c','npu_runtime.c','npu_package.c','npu_storage.c','npu_ag_platform.c')],*[SYSTEM/'sw'/n for n in ('npu_tensor_driver.c','npu_tensor_reference.c','npu_fixed_math.c')],SYSTEM/'sim/tensor_model_host.c']
 report={'passed':False,'scope':'actual full-model weights through matrix/tensor production RTL; host graph scheduling, no RV32/Cache/PHY/board or quality claim','board_verified':False,'quality_approved':False,'precise_norm':True,'fused_matrix':True,'commands':[]}
 def run(cmd,log,limit):
  report['commands'].append(list(map(str,cmd)))
  with (out/log).open('wb') as f:subprocess.run(list(map(str,cmd)),stdout=f,stderr=subprocess.STDOUT,check=True,timeout=limit,env=dict(os.environ,OMP_NUM_THREADS='8',QW_COMPARE_W4_HEAD='1'))
 try:
  objects=[]
  for i,s in enumerate(src):
   obj=out/('c'+str(i)+'.o');objects.append(obj)
   run(['gcc','-c','-O3','-fopenmp','-DQW_HOST_OPENMP','-DQW_ENABLE_TENSOR_V2','-DQW_PRECISE_NORM','-DQW_CONTEXT_MAX=2048u','-DNPU_FAST_CRC=1','-ffp-contract=off','-fno-fast-math','-I'+str(RUNTIME/'sw/model'),'-I'+str(SYSTEM/'sw'),'-Dmain=model_host_main' if s.name=='tensor_model_host.c' else '-D_NPU_RTL_BUILD',s,'-o',obj],'compile_'+str(i)+'.log',120)
  rtl=[SYSTEM/'rtl'/n for n in ('npu_dma_top.sv','npu_w4_fused_reduce.sv','gemv_pipeline16.sv','gemv_pipeline_v2.sv','npu_tensor_v2.sv','npu_delta_vectors.sv')]+[SYSTEM.parents[1]/'rtl/compute'/n for n in ('gemv_core.sv','group_acc.sv','mac_lane.sv')]+[SYSTEM/'sim/model_npu_rtl.sv']
  # A small C++ main calls the unchanged real model host entry.
  (out/'main.cpp').write_text('extern "C" int model_host_main(int,char**); int main(int c,char**v){return model_host_main(c,v);}')
  ld=' '.join(map(str,objects))+' -fopenmp -lm -Wl,--wrap=npu_tensor_init -Wl,--wrap=npu_tensor_execute -Wl,--wrap=npu_raw_reference'
  run(['verilator','--cc','--exe','--build','-j','4','-Wno-fatal','--top-module','model_npu_rtl','--Mdir',out/'obj','-CFLAGS','-O3 -I'+str(SYSTEM/'sw'),'-LDFLAGS',ld,*rtl,SYSTEM/'sim/model_npu_rtl_bridge.cpp',out/'main.cpp'],'rtl_build.log',240)
  catalog=a.assets/'HDTENSOR.JSN'
  if not catalog.is_file():catalog=a.assets/'TENSOR_HEAD.JSON'
  start=time.monotonic();run([out/'obj/Vmodel_npu_rtl',a.assets/'HEADW4.BIN',a.assets/'CPU_RAW.BIN',catalog,out,'1'],'model.log',a.timeout_seconds)
  report['elapsed_wall_seconds']=time.monotonic()-start
  result=json.loads((out/'model_result.json').read_text());report['model_result']=result
  # The host harness labels itself software-reference; arithmetic is intercepted
  # at the explicit link wrappers. Preserve that result and state actual scope.
  log=(out/'model.log').read_text()
  totals=re.search(r'FULL_MODEL_RTL_TOTAL matrix_jobs=(\d+) tensor_jobs=(\d+) matrix_cycles=(\d+) tensor_cycles=(\d+) bus_clocks=(\d+)',log)
  if not totals:raise RuntimeError('missing actual NPU execution totals')
  report['rtl_execution']=dict(zip(('matrix_jobs','tensor_jobs','matrix_cycles','tensor_cycles','bus_clocks'),map(int,totals.groups())))
  report['rtl_execution']['timing_domain']='isolated_NPU_RTL_clock_cycles_not_host_seconds_or_board_speed'
  report['passed']=result.get('execution_passed') is True and report['rtl_execution']['matrix_jobs']==265 and report['rtl_execution']['tensor_jobs']==4298
  if not report['passed']:raise RuntimeError('actual full-model NPU execution incomplete')
  print('FULL_MODEL_NPU_RTL_PASS',out,flush=True)
 finally:(out/'rtl_report.json').write_text(json.dumps(report,indent=2)+'\n')
if __name__=='__main__':main()
