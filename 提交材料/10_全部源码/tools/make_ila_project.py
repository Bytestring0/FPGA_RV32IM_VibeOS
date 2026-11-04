import pathlib
r=pathlib.Path('/mnt/d/FPGA/RiscV')
src=r/'npu_system_pds/npu_hot102_20261104'
dst=r/'npu_system_pds/npu_ila102_20261104'
dst.mkdir(parents=True,exist_ok=True)
for n in ('ag_npu_board.sv','board.fdc','firmware.hex'):
    (dst/n).write_bytes((src/n).read_bytes())
adf='D:/FPGA/RiscV/npu_system_pds/npu_hot102_20261104/prj_tasks/syn_1/synthesize/ag_npu_board_top_syn.adf'
fic=(r/'build/contest-validation/ag-coremark-ila.fic').read_text(encoding='utf-8')
out=[]
for line in fic.splitlines():
    if line.startswith('Project.device.designInputFile='):
        line='Project.device.designInputFile='+adf
    line=line.replace('board/system/soc/cpu/','soc/cpu_soc/cpu/').replace('board/cpu_clk','cpu_clk')
    out.append(line)
(dst/'ila-board.fic').write_text('\n'.join(out)+'\n',encoding='utf-8')
(dst/'build_ila.tcl').write_text('''set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design ag_npu_board.sv
add_constraint board.fdc
add_fic ila-board.fic
dev_map -force_to_run -input_db_file {'''+adf+'''}
pnr -route_mode normal -gplace_seed 2 -share_router_control_signal FALSE -max_hold_violated_paths_num 5000 -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree 50 -fix_hold_violation_threshold_for_clock_path_cross_srb 50 -action_timeout 90
report_timing
''',encoding='utf-8')
(dst/'bitstream_ila.tcl').write_text('''set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
gen_bit_stream -force_to_run -input_db_file {D:/FPGA/RiscV/npu_system_pds/npu_ila102_20261104/prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED
''',encoding='utf-8')
print('ILA_PROJECT_READY fic_lines',len(out),'clock',[l for l in out if 'clockChannel' in l])
