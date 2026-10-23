set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design qinglong_board.sv
add_constraint board.fdc
compile -top_module qinglong_board_top -system_verilog -multi_file_compilation_unit TRUE
synthesize -ads -resource_sharing FALSE
dev_map
pnr -gplace_seed 0 -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree 150
report_timing