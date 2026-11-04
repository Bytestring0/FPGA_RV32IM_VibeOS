"""Synthesize actual tensor RTL alone for quick area exploration with PDS.
No CPU, DDR PHY, dummy logic, dev_map, PNR or board-performance claim.
"""
import argparse,json,re,time
from pathlib import Path
from build_system import SYSTEM,WORK,installed_pds,pds_environment,run_pds,save,text

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--run",required=True)
    p.add_argument("--pds",type=Path,default=Path(installed_pds()))
    p.add_argument("--clock",type=float,default=162)
    p.add_argument("--timeout-seconds",type=int,default=1200)
    p.add_argument("--parallel-raw",action="store_true")
    p.add_argument("--scalar-delta",action="store_true")
    p.add_argument("--scalar-conv",action="store_true")
    p.add_argument("--prepare-only",action="store_true")
    a=p.parse_args()
    if not re.fullmatch("[a-z0-9_]+",a.run):p.error("ASCII run name required")
    if a.clock<=0 or a.timeout_seconds<=0:p.error("positive clock and timeout required")
    out=WORK/"npu_system_pds"/a.run
    if out.exists():p.error("fresh run required: "+str(out))
    paths=sorted((SYSTEM/"rtl").glob("*.sv"))
    selected=SYSTEM/"rtl/npu_tensor_v2.sv"
    code=text(selected)
    parameters={"PARALLEL_RAW":int(a.parallel_raw),"PARALLEL_DELTA":int(not a.scalar_delta),"PARALLEL_CONV":int(not a.scalar_conv),"SHARED_RAW_ACCUM":1}
    for key,value in parameters.items():
        pattern=r"(parameter bit "+key+r"\s*=\s*)[01]"
        code,count=re.subn(pattern,lambda m:m[1]+str(value),code,count=1)
        if count!=1:raise ValueError("parameter declaration missing: "+key)
    merged="\n".join("// Source: "+str(src.relative_to(WORK))+"\n"+(code if src==selected else text(src)) for src in paths)+"\n"
    out.mkdir(parents=True)
    (out/"tensor.sv").write_text(merged,encoding="utf-8")
    (out/"tensor.fdc").write_text("create_clock -name tensor_clk -period "+str(1000/a.clock)+" [get_ports clk]\nset_false_path -from [get_ports rst]\n",encoding="ascii")
    setup=["set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676","add_design tensor.sv","add_constraint tensor.fdc","compile -top_module npu_tensor_v2 -system_verilog -multi_file_compilation_unit TRUE","synthesize -ads -resource_sharing FALSE"]
    (out/"build.tcl").write_text("\n".join(setup)+"\n",encoding="ascii")
    report={"passed":False,"scope":"standalone real tensor PDS synthesis area exploration; unconstrained external ports; no device map or board timing","board_verified":False,"physical_timing_closed":False,"clock_mhz":a.clock,"parameters":parameters,"sources":[str(src.relative_to(WORK)) for src in paths],"command_timeout_seconds":a.timeout_seconds,"prepared_only":a.prepare_only}
    save(out/"report.json",report)
    if a.prepare_only:print("TENSOR_AREA_PREPARED",out);return
    command=[str(a.pds/"bin/pds_shell.exe"),"-file",str(out/"build.tcl"),"-work_dir",str(out),"-project_name","tensor_area"]
    try:
        run_pds(command,out,pds_environment(a.pds),report,a.timeout_seconds,"console.log")
        snr=out/"synthesize/npu_tensor_v2.snr"
        if not snr.is_file() or not (out/"synthesize/npu_tensor_v2_syn.vm").is_file():raise RuntimeError("actual synthesis outputs missing")
        body=text(snr)
        patterns={"luts":r"Total LUTs:\s*(\d+)","registers":r"Total Registers:\s*(\d+)","latches":r"Total Latches:\s*(\d+)","drm36k":r"Total DRM\s*=\s*([\d.]+)","apms":r"Total APMs\s*=\s*([\d.]+)"}
        report["resources"]={key:float(re.search(pattern,body)[1]) for key,pattern in patterns.items()}
        report["resource_rows"]=[line[:250] for line in body.splitlines() if line.startswith("|") and any(k in line for k in ("tensor","rounder","qkv","bank"))]
        match=re.search(r"^\s*tensor_clk\s+([\d.]+) MHz\s+([\d.]+) MHz\s+([\d.]+)\s+([\d.]+)\s+(-?[\d.]+)",body,re.M)
        if match:report["synthesis_timing"]={"requested_mhz":float(match[1]),"estimated_mhz":float(match[2]),"setup_slack_ns":float(match[5])}
        report["critical_endpoints"]=[line for line in body.splitlines() if line.startswith(("Startpoint","Endpoint"))][:6]
        if report["resources"]["latches"]:raise RuntimeError("unexpected inferred latches")
        report["passed"]=True
    except BaseException as exc:report["failure"]=repr(exc);raise
    finally:save(out/"report.json",report)
    print(json.dumps({key:report[key] for key in ("passed","resources","critical_endpoints")},indent=2))
    print("TENSOR_AREA_COMPLETE",out,"standalone synthesis only")

if __name__=="__main__":main()
