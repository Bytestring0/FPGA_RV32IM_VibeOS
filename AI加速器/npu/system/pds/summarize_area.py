"""Summarize an actual PDS synthesis report without rerunning any tool.
Area diagnostics only: no timing closure or board-performance claim.
"""
import argparse,json,re
from pathlib import Path

def summarize(run):
    path=run/"synthesize/ag_npu_board_top.snr"
    report=path.read_text(encoding="utf-8-sig",errors="replace")
    summary={"scope":"actual PDS synthesis snapshot area diagnostic","run":str(run.resolve()),"synthesis_report":str(path.resolve()),"board_verified":False,"physical_timing_closed":False}
    patterns={"luts":r"Total LUTs:\s*(\d+) of (\d+)","registers":r"Total Registers:\s*(\d+) of (\d+)","drm36k":r"Total DRM\s*=\s*([\d.]+) of (\d+)","apms":r"Total APMs\s*=\s*([\d.]+) of (\d+)"}
    for key,pattern in patterns.items():
        match=re.search(pattern,report)
        if not match:raise ValueError("resource field missing: "+key)
        used,capacity=map(float,match.groups())
        summary[key]={"used":used,"capacity":capacity,"percent":round(100*used/capacity,2)}
    modules=[]
    for line in report.splitlines():
        cols=[value.strip() for value in line.split("|")]
        if len(cols)>6 and cols[1] and re.fullmatch(r"[\d.]+",cols[2]) and re.fullmatch(r"[\d.]+",cols[3]):
            if any(token in cols[1] for token in ("tensor_v2_enabled.tensor","cpu_soc","+ dma","partial_sram_enabled.partial_store","+ bridge","cached.l1")):
                modules.append(dict(name=cols[1],luts=float(cols[2]),registers=float(cols[3]),lutram=float(cols[4]),apms=float(cols[5]),drm36k=float(cols[6])))
    summary["modules"]=modules
    match=re.search(r"^\s*cpu_clk\s+([\d.]+) MHz\s+([\d.]+) MHz\s+([\d.]+)\s+([\d.]+)\s+(-?[\d.]+)",report,re.M)
    if match:summary["synthesis_cpu_timing"]={"requested_mhz":float(match[1]),"estimated_mhz":float(match[2]),"setup_slack_ns":float(match[5])}
    return summary

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument("run",type=Path);parser.add_argument("--output",type=Path);args=parser.parse_args()
    result=summarize(args.run)
    if args.output:args.output.write_text(json.dumps(result,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print(json.dumps(result,indent=2,ensure_ascii=False))
