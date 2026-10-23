set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
add_design qinglong_board.sv
add_constraint board.fdc
gen_bit_stream -force_to_run -input_db_file {place_route/qinglong_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED