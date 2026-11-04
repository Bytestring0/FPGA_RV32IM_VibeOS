"""One-pass actual mapped tensor LUT attribution; no synthesis or RTL edits."""
import collections,json,re,sys
from pathlib import Path
run=Path(sys.argv[1]); vm=run/"synthesize/ag_npu_board_top_syn.vm"
roots=collections.Counter(); src=collections.Counter(); types=collections.Counter(); samples={}; root_src=collections.defaultdict(collections.Counter)
active=False; cell=None; total=0
def finish():
 global total
 if not cell:return
 kind,name,line,source=cell
 if not kind.startswith("GTP_LUT"):return
 total+=1;types[kind]+=1
 root=re.match(r"(?:\\)?(N[0-9]+)",name)
 key=root.group(1) if root else re.split(r"[\[./]",name.lstrip("\\"))[0]
 roots[key]+=1
 if source:src[source]+=1;root_src[key][source]+=1
 samples.setdefault(key,{"netlist_line":line,"instance":name})
with vm.open(encoding="utf-8",errors="replace") as f:
 for ln,text in enumerate(f,1):
  if text.startswith("module "):
   finish();cell=None;active=text.strip()=="module npu_tensor_v2"
  if not active:continue
  m=re.match(r"\s+(GTP_[A-Z0-9_]+) /\* (.*?) \*/",text)
  if m:finish();cell=[m[1],m[2],ln,None]
  elif cell:
   m=re.search(r"// (?:\.\./)?ag_npu_board\.sv:(\d+)",text)
   if m:cell[3]=int(m[1])
  if text.startswith("endmodule"):finish();cell=None;active=False
finish()
board=(run/"ag_npu_board.sv").read_text(encoding="utf-8-sig").splitlines()
source_headers=[]
for i,line in enumerate(board,1):
 if line.startswith("// Source: "):source_headers.append((i,line[11:]))
def source_info(n):
 h=max((h for h in source_headers if h[0]<n),default=(0,"generated board"))
 return {"frozen_line":n,"original_file":h[1],"original_line":n-h[0],"text":board[n-1].strip()}
result={"scope":"actual netlist tensor own primitive LUT counts; rounder child modules excluded","total_own_lut_primitives":total,"types":dict(types),"top_instance_roots":[dict(root=k,count=v,**samples[k],source_lines=[dict(count=c,**source_info(n)) for n,c in root_src[k].most_common(4)]) for k,v in roots.most_common(60)],"top_source_lines":[dict(count=c,**source_info(n)) for n,c in src.most_common(40)],"annotated_lut_count":sum(src.values())}
(run/"tensor_netlist_attribution.json").write_text(json.dumps(result,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
print(json.dumps({"total_own_lut_primitives":total,"top_instance_roots":result["top_instance_roots"][:8],"annotated_lut_count":sum(src.values())},indent=2,ensure_ascii=False))
