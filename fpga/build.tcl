# Run with scripts/build_fpga.ps1. Own build directory; no modification of RiscV.pds.
set root [file normalize [file join [file dirname [info script]] ..]]
set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676
set combined [file join $root build pds ag_all.sv]
set output [open $combined w]
foreach src {
    rv32_pkg.sv rv32_decode.sv rv32_alu.sv rv32_predictor.sv rv32_divider.sv
    rv32_csr.sv rv32_core.sv uart_tx.sv ag_soc.sv ag_fpga_top.sv
} {
    set input [open [file join $root rtl $src] r]
    puts $output [read $input]
    close $input
}
close $output
add_design $combined
add_constraint [file join $root fpga timing.fdc]
compile -top_module ag_fpga_top -system_verilog -multi_file_compilation_unit TRUE
synthesize -ads -resource_sharing FALSE
if {[info exists ::env(AG_PNR)] && $::env(AG_PNR) == "1"} {
    dev_map
    pnr
    report_timing
}
