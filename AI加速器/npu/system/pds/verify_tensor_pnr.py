"""Verify a finished system PNR from actual artifacts; never rerun implementation.

Keeps the original build report (including failed tool/audit records) intact and
writes verified_report.json. Reused placement must retain identical source,
firmware and constraint bytes. No programming or board claims.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

SYSTEM=Path(__file__).resolve().parents[1]
WORK=SYSTEM.parents[2]
CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK

def sha(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def read(path):return json.loads(path.read_text(encoding='utf-8-sig'))

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--run',type=Path,required=True)
    p.add_argument('--simulation-report',type=Path,required=True)
    a=p.parse_args();run=a.run.resolve()
    original=read(run/'report.json');report=dict(original)
    report['passed']=False
    report['original_flow_receipt']={'sha256':sha(run/'report.json'),'passed':original.get('passed'),'failure':original.get('failure')}
    report.pop('failure',None)
    report['verification_scope']='actual completed PNR artifacts, exact source/firmware/constraints and CPU report; no board proof'
    try:
        if original.get('mode')!='pnr' or original.get('actual_exit')!=0:raise ValueError('PDS process did not complete successfully')
        for name,digest in original['source_sha256'].items():
            source=WORK/name
            if not source.is_file() or sha(source)!=digest:raise ValueError('source changed: '+name)
        for name,key in (('ag_npu_board.sv','generated_rtl_sha256'),('firmware.hex','firmware_sha256'),('board.fdc','constraint_sha256')):
            if sha(run/name)!=original[key]:raise ValueError('input changed: '+name)
        route=run/'place_route/ag_npu_board_top_pnr.adf';timing=run/'report_timing/ag_npu_board_top.rtr'
        if not route.is_file() or not timing.is_file():raise ValueError('actual route/timing result missing')
        if timing.stat().st_mtime+2<route.stat().st_mtime:raise ValueError('timing predates route snapshot')
        if route.stat().st_mtime<(run/'ag_npu_board.sv').stat().st_mtime:raise ValueError('route predates current frozen design')
        sim=read(a.simulation_report)
        if not sim.get('passed') or sim.get('tensor_v2_enabled')!=original.get('tensor_v2_enabled') or sim.get('matrix_lanes')!=original.get('matrix_lanes') or sim.get('ddr_outstanding',1)!=original.get('ddr_outstanding',1):raise ValueError('system simulation configuration mismatch')
        identities={k.replace('\\','/'):v for k,v in sim['source_sha256'].items()}
        for name,digest in original['source_sha256'].items():
            key=name.replace('\\','/')
            if key in identities and identities[key]!=digest:raise ValueError('CPU/PDS source mismatch: '+key)
        report['simulation_binding']={'report':str(a.simulation_report.resolve()),'sha256':sha(a.simulation_report),'passed':True}
        report['all_constrained_paths_met']='Design Summary : All Constraints Met.' in timing.read_text(encoding='utf-8-sig')
        report['routed_artifacts']={'route_sha256':sha(route),'timing_sha256':sha(timing)}
        if original.get('ddr_outstanding')==4:
            netlist=run/'synthesize/ag_npu_board_top_syn.vm';fdc=run/'board.fdc'
            if not netlist.is_file():
                previous=Path(original['reuse_placement_db']).resolve().parents[1]
                netlist=previous/'synthesize/ag_npu_board_top_syn.vm';fdc=previous/'board.fdc'
                if sha(fdc)!=sha(run/'board.fdc'):raise ValueError('reused CDC constraint bytes mismatch')
            cdc=run/'queue4_cdc_verified.json'
            cmd=[sys.executable,str(CPU/'fpga/board/audit_ddr_queue4_cdc.py'),'--netlist',str(netlist),'--fdc',str(fdc),'--output',str(cdc),'--require-bounded-gray']
            with (run/'queue4_cdc_verified.log').open('wb') as log:r=subprocess.run(cmd,stdout=log,stderr=subprocess.STDOUT,check=False)
            if r.returncode:raise ValueError('actual CDC binding verification failed')
            report['queue4_cdc_audit']={'exit_code':0,'report':str(cdc),'report_sha256':sha(cdc)}
        report.update(passed=True,board_verified=False,physical_timing_closed=False,quality_approved=False)
    except BaseException as error:report['failure']=repr(error);raise
    finally:(run/'verified_report.json').write_text(json.dumps(report,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')
    print('SYSTEM_PNR_ARTIFACTS_VERIFIED',run/'verified_report.json','internal_timing',report['all_constrained_paths_met'])

if __name__=='__main__':main()
