// Two logical clients sharing one synchronous byte-write RAM port.
// The top-level ownership lock excludes ALL simultaneous CPU/NPU accesses.
// No reset/initialization of the array: input loading belongs to software.
module npu_bram #(
    parameter integer WORDS = 8192,
    parameter integer AW = $clog2(WORDS)
) (
    input wire clk,
    input wire a_en,
    input wire [AW-1:0] a_addr,
    input wire [31:0] a_wdata,
    input wire [3:0] a_wstrb,
    output wire [31:0] a_rdata,
    input wire b_en,
    input wire b_we,
    input wire [AW-1:0] b_addr,
    input wire [31:0] b_wdata,
    output wire [31:0] b_rdata
);
    reg [31:0] mem [0:WORDS-1];
    reg [31:0] read_data;
    wire ram_en = a_en || b_en;
    wire [AW-1:0] ram_addr = b_en ? b_addr : a_addr;
    wire [31:0] ram_wdata = b_en ? b_wdata : a_wdata;
    wire [3:0] ram_strobe = b_en ? {4{b_we}} : a_wstrb;
    assign a_rdata=read_data;
    assign b_rdata=read_data;
    integer byte_index;
    always @(posedge clk) begin
        if (ram_en) begin
            read_data <= mem[ram_addr];
            for (byte_index=0;byte_index<4;byte_index=byte_index+1)
                if (ram_strobe[byte_index]) mem[ram_addr][8*byte_index +: 8] <= ram_wdata[8*byte_index +: 8];
        end
    end
endmodule
