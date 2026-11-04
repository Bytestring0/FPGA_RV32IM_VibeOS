import pathlib,shutil,re
r=pathlib.Path('/mnt/d/FPGA/RiscV')
src=r/'npu_system_pds/npu_ila_s9'
hot=r/'npu_system_pds/npu_hot102_20261104'
dst=r/'npu_system_pds/npu_ila96'
if dst.exists(): shutil.rmtree(dst)
dst.mkdir(parents=True)
for n in ('firmware.hex','ila-board.fic'):
    shutil.copy2(src/n,dst/n)
m=(src/'ag_npu_board.sv').read_text(encoding='utf-8')
assert 'parameter integer CLOCK_MHZ = 102' in m
m=m.replace('parameter integer CLOCK_MHZ = 102','parameter integer CLOCK_MHZ = 96')
(dst/'ag_npu_board.sv').write_text(m,encoding='utf-8')
f=(src/'board.fdc').read_text(encoding='utf-8')
assert '-multiply_by 34 -divide_by 9' in f
f=f.replace('-multiply_by 34 -divide_by 9','-multiply_by 32 -divide_by 9')
(dst/'board.fdc').write_text(f,encoding='utf-8')
(dst/'synthesize.tcl').write_text((hot/'synthesize.tcl').read_text(encoding='utf-8').replace('npu_hot102_20261104','npu_ila96'),encoding='utf-8')
adf='D:/FPGA/RiscV/npu_system_pds/npu_ila96/prj_tasks/syn_1/synthesize/ag_npu_board_top_syn.adf'
(dst/'implement_ila.tcl').write_text('''set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design ag_npu_board.sv
add_constraint board.fdc
add_fic ila-board.fic
dev_map -force_to_run -input_db_file {'''+adf+'''}
pnr -route_mode normal -gplace_seed 2 -share_router_control_signal FALSE -max_hold_violated_paths_num 5000 -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree 50 -fix_hold_violation_threshold_for_cross_srb 50 -action_timeout 90
report_timing
''',encoding='utf-8')
(dst/'bitstream_ila.tcl').write_text('''set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
gen_bit_stream -force_to_run -input_db_file {D:/FPGA/RiscV/npu_system_pds/npu_ila96/prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED
''',encoding='utf-8')
print('ILA96 project ready')
