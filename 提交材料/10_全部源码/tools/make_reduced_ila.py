import pathlib,shutil,re
r=pathlib.Path('/mnt/d/FPGA/RiscV')
src=r/'npu_system_pds/npu_ila102_20261104'
fic=(src/'ila-board.fic').read_text(encoding='utf-8').splitlines()
out=[]
for l in fic:
    if l.startswith('Project.unit<0>.dataDepth='):
        l='Project.unit<0>.dataDepth=512'
    m=re.match(r'Project\.unit<0>\.triggerChannel<0><(\d+)>=',l)
    if m and int(m.group(1))>=32:
        continue
    if l.startswith('Project.unit<0>.triggerPortWidth<0>='):
        l='Project.unit<0>.triggerPortWidth<0>=32'
    if l.startswith('Project.unit<0>.triggerBus<0><32>') or l.startswith('Project.unit<0>.busInserter<1>'):
        continue
    out.append(l)
text='\n'.join(out)+'\n'
for seed in (9,10,11):
    dst=r/f'npu_system_pds/npu_ila_s{seed}'
    if dst.exists(): shutil.rmtree(dst)
    dst.mkdir(parents=True)
    for n in ('ag_npu_board.sv','board.fdc','firmware.hex'):
        shutil.copy2(src/n,dst/n)
    (dst/'ila-board.fic').write_text(text,encoding='utf-8')
    t=(src/'build_ila.tcl').read_text(encoding='utf-8')
    t=re.sub(r'-gplace_seed \d+',f'-gplace_seed {seed}',t)
    (dst/'build_ila.tcl').write_text(t,encoding='utf-8')
print('REDUCED_FIC channels',sum(1 for l in out if 'triggerChannel' in l),'depth512')
