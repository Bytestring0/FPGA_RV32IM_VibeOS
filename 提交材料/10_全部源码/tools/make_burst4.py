import pathlib,shutil
r=pathlib.Path('/mnt/d/FPGA/RiscV')
src=r/'npu_system_pds/npu_burst16_102_20261104'
dst=r/'npu_system_pds/npu_burst4_102_20261104'
dst.mkdir(parents=True,exist_ok=True)
for n in ('ag_npu_board.sv','board.fdc','synthesize.tcl','implement.tcl','firmware.hex'):
    (dst/n).write_bytes((src/n).read_bytes())
m=(dst/'ag_npu_board.sv').read_text(encoding='utf-8')
assert m.count('.MAX_READ_BURST(16)')==1
m=m.replace('.MAX_READ_BURST(16)','.MAX_READ_BURST(4)')
(dst/'ag_npu_board.sv').write_text(m,encoding='utf-8')
for n in ('synthesize.tcl','implement.tcl'):
    t=(dst/n).read_text(encoding='utf-8').replace('npu_burst16_102_20261104','npu_burst4_102_20261104').replace('-gplace_seed 3','-gplace_seed 2')
    (dst/n).write_text(t,encoding='utf-8')
print('BURST4_PROJECT_READY')
