"""Finish a completed 2025.2 production run: routed timing, CDC, bitstream.
Does not rerun synthesis/place/route or program hardware.
"""
import argparse,json,re,sys
from pathlib import Path
from build_system import WORK,CPU,installed_pds,pds_environment,run_pds,save,text,sha

def unique(run,pattern):
 paths=list(run.glob(pattern))
 if len(paths)!=1:raise RuntimeError('expected one '+pattern+', got '+str(paths))
 return paths[0]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',required=True,type=Path);a=p.parse_args();out=a.run.resolve()
 timing=unique(out,'prj_tasks/pnr_1/report_timing/*.rtr')
 route=unique(out,'prj_tasks/pnr_1/place_route/*_pnr.adf')
 report=json.loads(text(WORK/'npu_system_pds/npu_fast81_margin_20261103/report.json'))
 report.update(passed=False,tool_version='2025.2',mode='pnr',board_verified=False,bitstream_generated=False,scope='production 2025.2 final routed artifacts; no old database reuse')
 report.pop('failure',None);report.pop('timed_out',None)
 report['all_constrained_paths_met']='Design Summary : All Constraints Met.' in text(timing)
 report['timing_report']=str(timing);report['routed_database']=str(route)
 report['internal_timing_closed']=report['all_constrained_paths_met']
 save(out/'report.json',report)
 if not report['all_constrained_paths_met']:raise RuntimeError('routed setup/hold constraints failed; inspect '+str(timing))
 if timing.stat().st_mtime+2<route.stat().st_mtime:raise RuntimeError('timing predates final routed database')
 netlist=unique(out,'prj_tasks/syn_1/synthesize/*_syn.vm')
 import subprocess
 audit=[sys.executable,str(CPU/'fpga/board/audit_ddr_queue4_cdc.py'),'--netlist',str(netlist),'--fdc',str(out/'board.fdc'),'--output',str(out/'queue4_cdc_real_audit.json'),'--require-bounded-gray']
 with (out/'queue4_cdc_real_audit.log').open('wb') as f:r=subprocess.run(audit,stdout=f,stderr=subprocess.STDOUT)
 report['queue4_cdc_audit']={'exit_code':r.returncode,'report':str(out/'queue4_cdc_real_audit.json')}
 if r.returncode:raise RuntimeError('real 2025 netlist CDC binding failed')
 snr=text(unique(out,'prj_tasks/syn_1/synthesize/*.snr'))
 patterns={'luts':r'Total LUTs:\s*(\d+)','registers':r'Total Registers:\s*(\d+)','drm36k':r'Total DRMs?\s*=\s*([\d.]+)','apms':r'Total APMs\s*=\s*([\d.]+)'}
 report['synthesis_resources']={k:float(re.search(v,snr)[1]) for k,v in patterns.items()}
 setup=[s for s in text(out/'accelerated.tcl').splitlines() if s.startswith(('set_arch ','add_design ','add_constraint '))]
 setup+=['gen_bit_stream -force_to_run -input_db_file {'+route.as_posix()+'} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED']
 script=out/'bitstream2025.tcl';script.write_text('\n'.join(setup)+'\n',encoding='utf-8')
 pds=Path(installed_pds());env=pds_environment(pds)
 report['pds_root']=str(pds)
 try:
  run_pds([str(pds/'bin/pds_shell.exe'),'-file',str(script),'-work_dir',str(out),'-project_name','ag_npu_accelerated'],out,env,report,600,'bitstream2025.log')
  bits=list(out.glob('prj_tasks/pnr_1/generate_bitstream/*.sbit'))
  if not bits:bits=list(out.rglob('*.sbit'))
  if len(bits)!=1:raise RuntimeError('expected one generated bitstream: '+str(bits))
  bit=bits[0];report.update(passed=True,bitstream_generated=True,bitstream={'path':str(bit),'bytes':bit.stat().st_size,'sha256':sha(bit)})
  report['timing_report_sha256']=sha(timing)
  print('PDS2025_BITSTREAM_READY',bit,flush=True)
 except BaseException as e:report['failure']=str(e);raise
 finally:save(out/'report.json',report)
if __name__=='__main__':main()
