import pathlib,shutil
R=pathlib.Path('/mnt/d/FPGA/RiscV')
targets=[
 '提交材料/06_ILA波形/legacy_2022.2_coremark_uart.log',
 '提交材料/01_设计报告/待补_设计报告.md',
 '提交材料/08_位流与镜像/ag_npu_board_top_sbit_hot102.bin',
 '提交材料/08_位流与镜像/firmware_prefill102.bin',
]
dirs=[
 'npu_system_pds/npu_burst16_102_20261104',
 'npu_system_pds/npu_burst4_102_20261104',
 'npu_system_pds/npu_ila_seed6','npu_system_pds/npu_ila_seed7','npu_system_pds/npu_ila_seed8',
 'npu_system_pds/npu_ila_s9','npu_system_pds/npu_ila_s10','npu_system_pds/npu_ila_s11',
 'npu_system_pds/npu_ila102_20261104',
]
freed=0
for t in targets:
    p=R/t
    if p.is_file():
        freed+=p.stat().st_size; p.unlink(); print('DEL file',t)
for d in dirs:
    p=R/d
    if p.is_dir():
        s=sum(f.stat().st_size for f in p.rglob('*') if f.is_file())
        freed+=s; shutil.rmtree(p); print('DEL dir ',d,int(s/1e6),'MB')
print('FREED_MB',round(freed/1e6,1))
