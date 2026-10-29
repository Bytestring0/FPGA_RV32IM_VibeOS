set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design ag_board.sv
add_constraint board.fdc
compile -top_module ag_board_top -system_verilog -multi_file_compilation_unit TRUE
synthesize -ads -resource_sharing FALSE
dev_map
pnr -gplace_seed 0 -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree 200 -enable_route_constraint_iterate -route_constraint_iterate_times 3 -route_constraint_slack 0.1
report_timing
