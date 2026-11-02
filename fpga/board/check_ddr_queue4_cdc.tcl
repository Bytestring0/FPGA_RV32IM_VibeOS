# Source after PDS synthesis, or run audit_ddr_queue4_cdc.py standalone.
# get_cells returns @tcicmdresult symbolic handles in PDS2022; Tcl llength is
# NOT an object count. Audit actual mapped cells and emitted SDC bindings.
# Builder may set these four variables before sourcing this file.
if {![info exists queue4_python]} {set queue4_python python}
if {![info exists queue4_audit]} {
 error "set queue4_audit to the absolute CPU/fpga/board/audit_ddr_queue4_cdc.py path"
}
if {![info exists queue4_netlist]} {set queue4_netlist synthesize/ag_npu_board_top_syn.vm}
if {![info exists queue4_fdc]} {set queue4_fdc board.fdc}
if {![info exists queue4_output]} {set queue4_output queue4_cdc_real_audit.json}
# Use an argv list; never interpolate model/board data into shell source.
set queue4_command [list $queue4_python $queue4_audit --netlist $queue4_netlist --fdc $queue4_fdc --output $queue4_output --require-bounded-gray]
if {[catch {exec {*}$queue4_command 2>@1} queue4_log]} {
 puts $queue4_log
 error "QUEUE4_CDC_REAL_BINDINGS_FAILED (see audit JSON/log)"
}
puts $queue4_log
puts "QUEUE4_CDC_REAL_AUDIT_COMPLETED $queue4_output"
