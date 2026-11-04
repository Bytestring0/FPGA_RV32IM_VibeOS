import pathlib,shutil
R=pathlib.Path('/mnt/d/FPGA/RiscV')
B=R/'提交材料/10_全部源码'
if B.exists(): shutil.rmtree(B)
def cp(src,dst,patterns=None):
    s=R/src
    if not s.exists(): print('MISS',src); return 0
    d=B/dst; d.mkdir(parents=True,exist_ok=True); n=0
    if s.is_dir():
        for f in sorted(s.rglob('*')):
            if f.is_file():
                if patterns and f.suffix not in patterns: continue
                rel=f.relative_to(s); t=d/rel; t.parent.mkdir(parents=True,exist_ok=True)
                shutil.copy2(f,t); n+=1
    else:
        shutil.copy2(s,d/s.name); n=1
    return n
tot=0
tot+=cp('rtl','rtl',{'.sv'})
tot+=cp('fpga/board','fpga/board',{'.sv','.fdc'})
tot+=cp('AI加速器/npu/system/sw','firmware/system_sw')
tot+=cp('AI加速器/npu/system/integration/agent_runtime_20261101/sw/model','firmware/model')
tot+=cp('AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent','firmware/agent')
tot+=cp('third_party/coremark','coremark/third_party')
tot+=cp('sw/coremark','coremark/port')
cp('sw/start.S','coremark/port/start.S')
cp('sw/link.ld','coremark/port/link.ld')
tot+=cp('AI加速器/npu/system/tools','tools',{'.py'})
tot+=cp('scripts/bin2hex.py','tools/bin2hex.py')
tot+=cp('npu_system_pds/npu_hot102_20261104/synthesize.tcl','pds/synthesize.tcl')
tot+=cp('npu_system_pds/npu_hot102_20261104/implement.tcl','pds/implement.tcl')
tot+=cp('npu_system_pds/npu_hot102_20261104/bitstream_hot.tcl','pds/bitstream_hot.tcl')
cp('npu_system_pds/npu_hot102_20261104/board.fdc','pds/board.fdc')
print('COPIED',tot,'files')
for d in sorted(B.rglob('*')):
    if d.is_dir():
        k=sum(1 for f in d.rglob('*') if f.is_file())
        if k: print(' ',d.relative_to(B),k)
