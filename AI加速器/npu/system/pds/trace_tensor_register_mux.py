"""Attribute mapped LUT root families to direct DFF consumers, one tensor pass."""
import collections,json,re,sys
from pathlib import Path
run=Path(sys.argv[1]);text=(run/"synthesize/ag_npu_board_top_syn.vm").read_text(encoding="utf-8",errors="replace");text=text.split("module npu_tensor_v2\n",1)[1].split("endmodule",1)[0]
roots=collections.Counter();consumers=collections.defaultdict(set);samples={};direct=collections.Counter()
for part in re.split(r"\n(?=    GTP_)",text):
 m=re.match(r"    (GTP_[A-Z0-9_]+) /\* (.*?) \*/",part)
 if not m:continue
 kind,name=m.groups(); root=re.match(r"\\?(N\d+)",name)
 if kind.startswith("GTP_LUT") and root:roots[root[1]]+=1
 if kind.startswith("GTP_DFF"):
  d=re.search(r"\.D \((N\d+)\[",part)
  if d:
   reg=name.lstrip("\\").split("[")[0];consumers[d[1]].add(reg);samples.setdefault(d[1],name)
groups=collections.Counter();rows=[]
for root,count in roots.items():
 if root in consumers:
  target=",".join(sorted(consumers[root]));groups[target]+=count
  rows.append({"root":root,"luts":count,"consumers":sorted(consumers[root]),"example_register":samples[root]})
rows.sort(key=lambda x:-x["luts"])
result={"scope":"LUT root-family counts only for families whose named bus directly drives a DFF; not a full cone census","consumer_group_luts":groups.most_common(),"top_consumed_roots":rows[:80],"qkv_roots":[r for r in rows if any(k in r["consumers"] for k in ("delta_q","delta_k","delta_v"))]}
(run/"tensor_register_mux_attribution.json").write_text(json.dumps(result,indent=2)+"\n")
print(json.dumps({"consumer_group_luts":groups.most_common(20),"top_consumed_roots":rows[:8],"qkv_root_count":len(result["qkv_roots"])},indent=2))
