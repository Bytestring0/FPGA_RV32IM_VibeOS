set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design ag_board.sv
add_constraint board.fdc
compile -top_module ag_board_top -system_verilog -multi_file_compilation_unit TRUE
synthesize -ads -resource_sharing FALSE
