"""Generate a candidate bitstream ONLY from an intact, routed, timing-passed run.

No board programming, SD writes or Flash writes. Internal timing is separate
from SD external timing and real-model quality approval.
"""
from __future__ import annotations
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import time

SYSTEM=Path(__file__).resolve().parents[1]
WORK=SYSTEM.parents[2]

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--report',type=Path)
    p.add_argument('--pds',type=Path,default=Path(r'D:\pango\PDS_2022.2-SP6.4'))
    a=p.parse_args()
    if os.name!='nt':p.error('use Windows Python and PDS')
    run=a.run.resolve()
    if not run.is_relative_to(WORK/'npu_system_pds'):p.error('existing project run required')
    report_path=a.report.resolve() if a.report else run/'report.json'
    if report_path.parent!=run:p.error('verification report must belong to this run')
    original=json.loads(report_path.read_text(encoding='utf-8-sig'))
    if original.get('mode')!='pnr' or original.get('passed') is not True or original.get('all_constrained_paths_met') is not True:
        p.error('completed PNR and passing constrained internal timing required')
    if original.get('ddr_outstanding')==4 and original.get('queue4_cdc_audit',{}).get('exit_code')!=0:
        p.error('real bounded-Gray CDC audit required')
    for name,digest in original['source_sha256'].items():
        source=WORK/name
        if not source.is_file() or sha(source)!=digest:p.error('source changed: '+name)
    for name,key in (('ag_npu_board.sv','generated_rtl_sha256'),('firmware.hex','firmware_sha256'),('board.fdc','constraint_sha256')):
        if sha(run/name)!=original[key]:p.error('frozen input changed: '+name)
    route=run/'place_route/ag_npu_board_top_pnr.adf'
    timing=run/'report_timing/ag_npu_board_top.rtr'
    if not route.is_file() or not timing.is_file() or 'Design Summary : All Constraints Met.' not in timing.read_text(encoding='utf-8-sig'):
        p.error('actual routed database and passing timing report required')
    output=run/'bitstream_candidate.json'
    if output.exists():p.error('candidate already recorded; refuse overwrite')
    # Recreate project setup but never repeat compile/synthesis/map/route.
    setup=[]
    for line in (run/'build.tcl').read_text(encoding='utf-8-sig').splitlines():
        if line.startswith(('set_arch ','add_design ','add_constraint ')):setup.append(line)
    if not setup or not any(s.startswith('set_arch ') for s in setup):p.error('frozen project setup absent')
    setup.append('gen_bit_stream -force_to_run -input_db_file {place_route/ag_npu_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED')
    script=run/'generate_candidate.tcl';script.write_text('\n'.join(setup)+'\n',encoding='utf-8')
    env=dict(os.environ)
    if not env.get('PANGO_LICENSE_FILE'):
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,'Environment') as key:env['PANGO_LICENSE_FILE']=winreg.QueryValueEx(key,'PANGO_LICENSE_FILE')[0]
    project=run/'ag_npu_board.pds';project_bytes=project.read_bytes()
    report={'passed':False,'board_programmed':False,'board_verified':False,'quality_approved':False,
            'internal_timing_passed':True,'sd_external_timing_verified':False,
            'routed_report_sha256':sha(report_path),'timing_sha256':sha(timing),'route_sha256':sha(route),
            'firmware_sha256':original['firmware_sha256'],'generated_rtl_sha256':original['generated_rtl_sha256'],
            'constraint_sha256':original['constraint_sha256'],'scope':'candidate artifact only; no board/model-quality approval'}
    started=time.time()
    try:
        command=[str(a.pds/'bin/pds_shell.exe'),'-file',str(script),'-work_dir',str(run),'-project_name','ag_npu_board']
        with (run/'generate_candidate.log').open('wb') as log:
            result=subprocess.run(command,cwd=run,env=env,stdout=log,stderr=subprocess.STDOUT,timeout=600,check=False)
        report['exit_code']=result.returncode
        if result.returncode:raise RuntimeError('PDS candidate bitstream generation failed')
        bit=run/'generate_bitstream/ag_npu_board_top.sbit'
        if not bit.is_file() or bit.stat().st_mtime<started-2:raise RuntimeError('no fresh candidate bitstream')
        report.update(passed=True,bitstream={'path':str(bit),'bytes':bit.stat().st_size,'sha256':sha(bit)})
    except BaseException as error:report['failure']=repr(error);raise
    finally:
        project.write_bytes(project_bytes)
        output.write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print('CANDIDATE_BITSTREAM_CREATED',output)

if __name__=='__main__':main()
