dbg_connect -ip 127.0.0.1 -port 65420
dbg_scan_chain
dbg_program -device 0 -file D:/FPGA/RiscV/npu_system_pds/npu_ila96cm/prj_tasks/pnr_1/generate_bitstream/ag_npu_board_top.sbit
dbg_import_fic -file D:/FPGA/RiscV/npu_system_pds/npu_ila96cm/ila-board.fic
dbg_fla_trig_immd -fla 0
dbg_delay -time 2
dbg_fla_stop -fla 0
dbg_fla_export_wf_data -fla 0 -format vcd -file D:/FPGA/RiscV/build/contest-validation/ila-2025-samples.csv
dbg_save_project -file D:/FPGA/RiscV/build/contest-validation/ila-2025.dprj
