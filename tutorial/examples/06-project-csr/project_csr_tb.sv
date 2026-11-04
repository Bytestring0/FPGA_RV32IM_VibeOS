`timescale 1ns/1ps
module project_csr_tb;
    logic clk=0,rst=1,retire=0,retire_second=0;
    logic irq_software=0,irq_timer=0,irq_external=0;
    logic [11:0] read_addr=0,write_addr=0;
    wire [31:0] read_data,irq_cause,trap_vector,return_pc;
    wire read_legal,read_only,irq_pending;
    logic write_en=0,trap=0,trap_interrupt=0,mret=0;
    logic [31:0] write_data=0,trap_pc=0,trap_cause=0,trap_value=0;
    wire [63:0] cycle_count,instret_count;
    rv32_csr dut(.*);
    always #5 clk=~clk;
    task automatic csr_write(input logic [11:0] addr,input logic [31:0] data);
        begin
            @(negedge clk); write_addr=addr; write_data=data; write_en=1;
            @(negedge clk); write_en=0;
        end
    endtask
    task automatic csr_expect(input logic [11:0] addr,input logic [31:0] val);
        begin
            read_addr=addr; #1;
            if (!read_legal || read_data !== val)
                $fatal(1,"CSR %h got=%h expected=%h",addr,read_data,val);
        end
    endtask
    initial begin
        repeat(2) @(negedge clk); rst=0;
        csr_write(12'h305,32'h101); // vectored base 0x100
        csr_write(12'h304,32'h880); // timer+external enabled
        csr_write(12'h300,32'h8);   // global MIE
        @(negedge clk); irq_external=1; irq_timer=1; #1;
        if (!irq_pending || irq_cause !== 11) $fatal(1,"interrupt priority");
        trap_interrupt=1; #1;
        if (trap_vector !== 32'h12c) $fatal(1,"vectored interrupt target");
        trap_pc=32'h40; trap_cause=11; trap_value=0; trap=1;
        @(negedge clk); trap=0; irq_external=0; irq_timer=0; #1;
        csr_expect(12'h341,32'h40);
        csr_expect(12'h342,32'h8000000b);
        csr_expect(12'h300,32'h1880); // MPIE=old MIE, MIE=0
        if (return_pc !== 32'h40) $fatal(1,"return_pc");
        @(negedge clk); mret=1;
        @(negedge clk); mret=0; #1;
        csr_expect(12'h300,32'h1888); // restore MIE
        read_addr=12'hc00; #1; if (!read_legal || !read_only) $fatal(1,"read-only counter");
        read_addr=12'hfff; #1; if (read_legal) $fatal(1,"unknown CSR");
        $display("PASS project_csr: trap, vector and return");
        $finish;
    end
    initial begin #1000; $fatal(1,"timeout"); end
endmodule
