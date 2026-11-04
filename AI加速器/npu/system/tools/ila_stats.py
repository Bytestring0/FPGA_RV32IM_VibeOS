import pathlib
p=pathlib.Path('/mnt/d/FPGA/RiscV/build/contest-validation/ila-2025-samples.csv.vcd')
vals=[]
for line in p.read_text(errors='ignore').splitlines():
    line=line.strip()
    if not line or line[0] in '#$': continue
    if line[0]=='b': vals.append(int(line.partition(' ')[0][1:],2))
c={}
for a,b in zip(vals,vals[1:]): c[b-a]=c.get(b-a,0)+1
tot=sum(c.values())
print('samples',len(vals),'transitions',tot)
for d in sorted(c,key=lambda x:-c[x])[:8]: print('  delta',d,'count',c[d])
single=c.get(4,0);dual=c.get(8,0);back=sum(v for k,v in c.items() if k<0);fwd=[(k,v) for k,v in c.items() if k>8]
print('single_issue',single,round(100*single/tot,1),'%')
print('dual_issue',dual,round(100*dual/tot,1),'%')
print('backward_branch',back,round(100*back/tot,1),'%')
print('wide_forward',sum(v for k,v in fwd))
print('dual_over_issue_pairs',round(100*dual/max(1,(single+dual)),1),'%')
