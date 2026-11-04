import pathlib,shutil
R=pathlib.Path('/mnt/d/FPGA/RiscV'); B=R/'提交材料/10_全部源码'
a=B/'产物'; a.mkdir(exist_ok=True)
pairs=[(R/'提交材料/08_位流与镜像/ag_npu_board_top.sbit',a/'ag_npu_board_top.sbit'),
       (R/'提交材料/08_位流与镜像/firmware_direct_chat.bin',a/'firmware.bin'),
       (R/'提交材料/08_位流与镜像/烧录资产/stage1/APPRES.BIN',a/'APPRES_stage1.BIN'),
       (R/'提交材料/08_位流与镜像/烧录资产/stage1/FONT.BIN',a/'FONT_stage1.BIN'),
       (R/'提交材料/08_位流与镜像/烧录资产/stage2/APPRES.BIN',a/'APPRES_stage2.BIN'),
       (R/'提交材料/08_位流与镜像/烧录资产/stage2/FONT.BIN',a/'FONT_stage2.BIN'),
       (R/'提交材料/06_ILA波形/ila-2025-xpc.vcd',a/'ila-2025-xpc.vcd')]
for s,d in pairs:
    if s.exists(): shutil.copy2(s,d); print('added',d.name,s.stat().st_size)
print('TOTAL files',sum(1 for f in B.rglob('*') if f.is_file()))
