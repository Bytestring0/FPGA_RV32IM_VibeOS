import pathlib, shutil, json
R=pathlib.Path('/mnt/d/FPGA/RiscV')
B=R/'提交材料'
dirs=['01_设计报告','02_RTL源码','03_固件与工具链','04_CoreMark','05_时序与资源','06_ILA波形','07_AI应用','08_位流与镜像','09_演示视频']
for d in dirs: (B/d).mkdir(parents=True,exist_ok=True)

def cp(src,dst):
    s=R/src
    if not s.exists(): return False
    d=B/dst; d.parent.mkdir(parents=True,exist_ok=True)
    if s.is_dir(): shutil.copytree(s,d,dirs_exist_ok=True)
    else: shutil.copy2(s,d)
    return True

n_rtl=0
for f in sorted((R/'rtl').glob('*.sv')):
    cp(f'rtl/{f.name}', f'02_RTL源码/{f.name}'); n_rtl+=1
for f in sorted((R/'fpga/board').glob('*.sv')):
    cp(f'fpga/board/{f.name}', f'02_RTL源码/board_{f.name}'); n_rtl+=1
cp('npu_system_pds/npu_hot102_20261104/board.fdc','02_RTL源码/board.fdc')

n_fw=0
for pat in ('AI加速器/npu/system/sw/*.c','AI加速器/npu/system/sw/*.h','AI加速器/npu/system/sw/*.S'):
    for f in sorted(R.glob(pat)):
        cp(f'AI加速器/npu/system/sw/{f.name}', f'03_固件与工具链/sw/{f.name}'); n_fw+=1
for pat in ('AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent/*.c','AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent/*.h'):
    for f in sorted(R.glob(pat)):
        cp(f'AI加速器/npu/system/integration/agent_runtime_20261101/sw/agent/{f.name}', f'03_固件与工具链/agent/{f.name}'); n_fw+=1
for f in sorted((R/'third_party/coremark').glob('*.c')):
    cp(f'third_party/coremark/{f.name}', f'04_CoreMark/src/{f.name}')
for f in sorted((R/'sw/coremark').glob('*')):
    if f.is_file(): cp(f'sw/coremark/{f.name}', f'04_CoreMark/src/{f.name}')
cp('sw/start.S','04_CoreMark/src/start.S'); cp('sw/link.ld','04_CoreMark/src/link.ld')

cp('npu_system_pds/npu_hot102_20261104/prj_tasks/pnr_1/generate_bitstream/ag_npu_board_top.sbit','08_位流与镜像/ag_npu_board_top_sbit_hot102.bin')
cp('AI加速器/npu/system/build/agent_prefill102_20261104/firmware.bin','08_位流与镜像/firmware_prefill102.bin')
cp('npu_system_pds/npu_hot102_20261104/prj_tasks/pnr_1/report_timing/ag_npu_board_top.rtr','05_时序与资源/timing_hot102.rtr')
cp('build/contest-validation/ag-coremark102-ila.uart.log','06_ILA波形/legacy_2022.2_coremark_uart.log')
print('created dirs',len(dirs),'rtl',n_rtl,'fw',n_fw)
