# Internal first phase used by program_board.ps1 in Fabric Configuration shell.
# The caller sets bitstream, captures UART, then requests verification/disconnect.
# Boundary-scan programming updates FPGA configuration RAM only.
if {![info exists bitstream] || ![file isfile $bitstream]} {
    error "Set bitstream to an existing .sbit file before sourcing this script."
}
cfg_set_tcl_break -flag true
cfg_connect -ip 127.0.0.1 -port 65420
cfg_add_device -file $bitstream
cfg_read_device_property -mode 0 -device_index 0
cfg_program -device_index 0
# Keep this session open while software runs. The PowerShell caller performs
# cfg_verify only after UART capture: configuration readback interrupts execution.
