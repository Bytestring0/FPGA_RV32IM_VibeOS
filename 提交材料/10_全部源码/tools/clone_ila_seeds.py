import pathlib,shutil
r=pathlib.Path('/mnt/d/FPGA/RiscV')
src=r/'npu_system_pds/npu_ila102_20261104'
for seed in (6,7,8):
    dst=r/f'npu_system_pds/npu_ila_seed{seed}'
    if dst.exists(): shutil.rmtree(dst)
    dst.mkdir(parents=True)
    for n in ('ag_npu_board.sv','board.fdc','firmware.hex','ila-board.fic'):
        shutil.copy2(src/n,dst/n)
    t=(src/'build_ila.tcl').read_text(encoding='utf-8')
    t=t.replace('-gplace_seed 5',f'-gplace_seed {seed}')
    (dst/'build_ila.tcl').write_text(t,encoding='utf-8')
    print('SEED',seed,'ready')
