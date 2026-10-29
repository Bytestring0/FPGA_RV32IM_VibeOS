dbg_connect -ip 127.0.0.1 -port 65420
dbg_scan_chain
dbg_program -device 0 -file D:/FPGA/RiscV/build/ag-coremark102-ila/generate_bitstream/ag_board_top.sbit
dbg_import_fic -file D:/FPGA/RiscV/build/ag-coremark102-ila/ag-coremark-ila.fic
dbg_get_cur_core
dbg_list
dbg_add_all_to_waveform
dbg_add_all_to_listing
dbg_trig_immd
dbg_delay -time 2
dbg_save_project -file D:/FPGA/RiscV/build/contest-validation/ag-coremark-ila.dprj
