`timescale 1ns/1ps
module wait_ram_tb;
    logic clk=0, rst=1;
    logic req_valid=0, req_write=0, rsp_ready=0;
    logic [31:0] req_addr=0, req_wdata=0;
    logic [3:0] req_wstrb=0;
    wire req_ready, rsp_valid, rsp_error;
    wire [31:0] rsp_rdata;
    integer accepted = 0;
    wait_ram dut(.*);
    always #5 clk=~clk;
    always @(posedge clk) if (!rst && req_valid && req_ready) accepted <= accepted + 1;

    task automatic transaction(input logic wr, input logic [31:0] addr,
        input logic [31:0] data, input logic [3:0] strb,
        input logic exp_error, input logic [31:0] exp_data);
        begin
            @(negedge clk);
            req_valid=1; req_write=wr; req_addr=addr; req_wdata=data; req_wstrb=strb;
            while (!req_ready) @(negedge clk);
            @(posedge clk);
            @(negedge clk); req_valid=0;
            while (!rsp_valid) @(negedge clk);
            if (rsp_error !== exp_error || rsp_rdata !== exp_data)
                $fatal(1,"wrong response addr=%h got=%h err=%b",addr,rsp_rdata,rsp_error);
            repeat (3) begin
                @(negedge clk);
                if (!rsp_valid || rsp_error !== exp_error || rsp_rdata !== exp_data || req_ready)
                    $fatal(1,"response changed during backpressure");
            end
            rsp_ready=1;
            @(negedge clk); rsp_ready=0;
        end
    endtask
    initial begin
        repeat (2) @(negedge clk); rst=0;
        transaction(1,0,32'h11223344,4'b1111,0,0);
        transaction(1,0,32'h00aa0000,4'b0100,0,0);
        transaction(0,0,0,0,0,32'h11aa3344);
        transaction(1,64,32'hffffffff,4'b1111,1,0);
        transaction(0,2,0,0,1,0);
        transaction(0,0,0,0,0,32'h11aa3344);
        if (accepted !== 6) $fatal(1,"wrong request count: %0d",accepted);
        $display("PASS wait_ram: %0d accepted requests",accepted);
        $finish;
    end
    initial begin #5000; $fatal(1,"timeout"); end
endmodule
