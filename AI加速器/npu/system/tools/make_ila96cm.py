import pathlib,shutil,subprocess
r=pathlib.Path('/mnt/d/FPGA/RiscV')
src=r/'npu_system_pds/npu_ila96'
dst=r/'npu_system_pds/npu_ila96cm'
if dst.exists(): shutil.rmtree(dst)
dst.mkdir(parents=True)
for n in ('ag_npu_board.sv','board.fdc','ila-board.fic','synthesize.tcl'):
    shutil.copy2(src/n,dst/n)
cm=r/'AI加速器/npu/system/build/coremark_chain102_20261104/coremark.bin'
subprocess.run(['python3',str(r/'scripts/bin2hex.py'),str(cm),str(dst/'firmware.hex'),'--words','16384'],check=True)
m=(dst/'synthesize.tcl').read_text(encoding='utf-8').replace('npu_ila96','npu_ila96cm')
(dst/'synthesize.tcl').write_text(m,encoding='utf-8')
adf='D:/FPGA/RiscV/npu_system_pds/npu_ila96cm/prj_tasks/syn_1/synthesize/ag_npu_board_top_syn.adf'
(dst/'implement_ila.tcl').write_text('''set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design ag_npu_board.sv
add_constraint board.fdc
add_fic ila-board.fic
dev_map -force_to_run -input_db_file {'''+adf+'''}
pnr -route_mode normal -gplace_seed 2 -share_router_control_signal FALSE -max_hold_violated_paths_num 5000 -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree 50 -action_timeout 90
report_timing
''',encoding='utf-8')
(dst/'bitstream_ila.tcl').write_text('''set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
gen_bit_stream -force_to_run -input_db_file {D:/FPGA/RiscV/npu_system_pds/npu_ila96cm/prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED
''',encoding='utf-8')
print('ILA96CM ready hex_bytes',(dst/'firmware.hex').stat().st_size)
