import pathlib,shutil
R=pathlib.Path('/mnt/d/FPGA/RiscV'); B=R/'提交材料/03_固件与工具链'
for sub in ('sw','agent'):
    d=B/sub
    if d.exists(): shutil.rmtree(d)
    d.mkdir(parents=True)
n=0
for f in sorted((R/'AI加速器/npu/system/sw').glob('*')):
    if f.suffix in ('.c','.h','.S','.ld'):
        shutil.copy2(f,B/'sw'/f.name); n+=1
for f in sorted((R/'AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent').glob('*.c')):
    shutil.copy2(f,B/'agent'/f.name); n+=1
for f in sorted((R/'AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent').glob('*.h')):
    shutil.copy2(f,B/'agent'/f.name); n+=1
print('synced',n,'files')
