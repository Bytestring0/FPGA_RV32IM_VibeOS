import pathlib,shutil
B=pathlib.Path('/mnt/d/FPGA/RiscV/提交材料/10_全部源码')
tp=B/'coremark/third_party'
keep={'core_main.c','core_list_join.c','core_matrix.c','core_state.c','core_util.c','coremark.h','core_portme.h','README.md','LICENSE.md','LICENSE'}
for f in list(tp.iterdir()):
    if f.is_file() and f.name in keep: continue
    if f.is_dir(): shutil.rmtree(f)
    else: f.unlink()
print('third_party now',sorted(p.name for p in tp.iterdir()))
tot=sum(1 for f in B.rglob('*') if f.is_file())
size=sum(f.stat().st_size for f in B.rglob('*') if f.is_file())
print('TOTAL',tot,'files',round(size/1024,1),'KiB')
