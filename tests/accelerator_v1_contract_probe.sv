// Review probe for Tigrisz/LLM-Accelerator cd06ae6e9d820bca3bb5d22b8eaf91e32dec43ec.
// It reproduces integration limitations of that exact version. This is not
// a specification that future, corrected implementations should preserve.
`timescale 1ns/1ps
module accelerator_v1_contract_probe;
    reg clk = 0;
    always #5 clk = ~clk;
    reg rst_n = 0, bus_valid = 0;
    reg [7:0] bus_addr = 0;
    reg [31:0] bus_wdata = 0;
    reg [3:0] bus_wstrb = 15;
    wire start, clear_status, busy, done, error;
    wire [7:0] error_code;
    wire [15:0] n_rows;
    wire [31:0] k_elems, cycle_count, result_count;
    wire [63:0] weight_base, activation_base;
    reg result_valid = 0;
    wire result_pending;
    control_regs regs_dut (
        .clk(clk), .rst_n(rst_n), .bus_valid(bus_valid), .bus_write(1'b1),
        .bus_addr(bus_addr), .bus_wdata(bus_wdata), .bus_wstrb(bus_wstrb),
        .bus_ready(), .bus_rdata(), .start_pulse(start), .clear_status_pulse(clear_status),
        .weight_base(weight_base), .activation_base(activation_base), .n_rows(n_rows), .k_elems(k_elems),
        .status_busy(busy), .status_done(done), .status_error(error),
        .status_act_loaded(1'b1), .status_reader_done(1'b0), .status_result_pending(result_pending),
        .error_code(error_code), .cycle_count(cycle_count), .result_count(result_count)
    );
    task_controller controller_dut (
        .clk(clk), .rst_n(rst_n), .start_request(start), .clear_status(clear_status),
        .n_rows(n_rows), .k_elems(k_elems), .cache_loaded(1'b1), .cache_loaded_k(32'd128),
        .reader_error(1'b0), .activation_valid(1'b0), .activation_ready(1'b0), .activation_last(1'b0),
        .result_valid(result_valid), .result_ready(1'b1), .reader_start(), .cache_read_start(),
        .busy(busy), .done(done), .error(error), .error_code(error_code),
        .cycle_count(cycle_count), .result_count(result_count), .result_pending(result_pending)
    );
    reg reader_start = 0, rd_valid = 0, rd_error = 0;
    wire reader_busy, reader_error, reader_req;
    weight_reader reader_dut (
        .clk(clk), .rst_n(rst_n), .start(reader_start), .weight_base(64'd0),
        .n_rows(16'd1), .k_elems(32'd128), .busy(reader_busy), .done(), .error(reader_error),
        .rd_req_valid(reader_req), .rd_req_ready(1'b1), .rd_addr(), .rd_nbytes(),
        .rd_data_valid(rd_valid), .rd_data_ready(), .rd_data(128'd0), .rd_last(1'b1), .rd_error(rd_error),
        .weight_valid(), .weight_ready(1'b1), .weight_data(), .group_first(), .group_last(), .row_id(), .group_id()
    );
    task tick; begin @(posedge clk); #1; end endtask
    task write_reg(input [7:0] address, input [31:0] data, input [3:0] strobe);
        begin
            @(negedge clk); bus_valid = 1; bus_addr = address; bus_wdata = data; bus_wstrb = strobe;
            tick(); bus_valid = 0;
        end
    endtask
    initial begin
        repeat (2) tick(); @(negedge clk); rst_n = 1; tick();
        write_reg(8'h18, 32'h1234, 4'hf);
        write_reg(8'h18, 32'h5678, 4'h1);
        if (n_rows !== 16'h5678) $fatal(1, "upstream strobe behavior changed; re-review");
        $display("OBSERVED: byte strobe writes the full N register (0x5678, not byte-merged 0x1278)");
        write_reg(8'h18, 2, 15); write_reg(8'h1c, 128, 15); write_reg(0, 1, 15);
        tick(); if (!busy) $fatal(1, "task did not start");
        write_reg(8'h18, 1, 15);
        @(negedge clk); result_valid = 1; tick(); result_valid = 0;
        if (!done || result_count != 1) $fatal(1, "upstream config behavior changed; re-review");
        $display("OBSERVED: changing N from 2 to 1 while busy makes DONE assert after just one result");
        @(negedge clk); reader_start = 1; tick(); reader_start = 0; tick();
        @(negedge clk); rd_valid = 1; rd_error = 1; tick(); rd_valid = 0;
        if (!reader_error) $fatal(1, "reader error not injected");
        write_reg(0, 2, 15); tick();
        @(negedge clk); reader_start = 1; tick(); reader_start = 0; repeat (3) tick();
        if (!reader_error || !reader_busy || reader_req) $fatal(1, "upstream error behavior changed; re-review");
        $display("OBSERVED: CLEAR_STATUS and another START do not recover the reader from S_ERROR; reset is required");
        $display("REVIEW PROBE PASS: three integration limitations reproduced on the pinned upstream RTL");
        $finish;
    end
    initial begin #10000; $fatal(1,"probe timed out"); end
endmodule
