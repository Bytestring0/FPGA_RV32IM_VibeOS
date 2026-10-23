# PDS exploration target: 110 MHz. Check routed timing before claiming closure.
create_clock -name cpu_clk -period 9.091 [get_ports clk]
set_false_path -from [get_ports reset_n]
# LEDs and asynchronous UART have no external receiver clocked by cpu_clk.
# Keep all internal register/RAM/DSP paths timed; exclude only these pad paths.
set_false_path -to [get_ports {uart_txd led[*]}]
