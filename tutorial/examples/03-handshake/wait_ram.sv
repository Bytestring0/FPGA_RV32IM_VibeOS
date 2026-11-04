// Teaching data RAM: one request at a time, byte strobes, held response.
// Requests must use word-aligned byte addresses. Not an AXI implementation.
module wait_ram #(
    parameter integer WORDS = 16
) (
    input  logic        clk, rst,
    input  logic        req_valid,
    output logic        req_ready,
    input  logic [31:0] req_addr, req_wdata,
    input  logic [3:0]  req_wstrb,
    input  logic        req_write,
    output logic        rsp_valid,
    input  logic        rsp_ready,
    output logic [31:0] rsp_rdata,
    output logic        rsp_error
);
    logic [31:0] ram [0:WORDS-1];
    logic pending;
    logic [1:0] delay_left;
    logic [31:0] saved_data;
    logic saved_error;
    wire valid_addr = req_addr[1:0] == 0 && req_addr < WORDS*4;
    assign req_ready = !rst && !pending && !rsp_valid;

    always_ff @(posedge clk) begin
        if (rst) begin
            pending <= 0; delay_left <= 0;
            saved_data <= 0; saved_error <= 0;
            rsp_valid <= 0; rsp_rdata <= 0; rsp_error <= 0;
        end else begin
            if (rsp_valid && rsp_ready) rsp_valid <= 0;
            if (req_valid && req_ready) begin
                pending <= 1;
                delay_left <= 2;
                saved_error <= !valid_addr;
                saved_data <= 0;
                if (valid_addr) begin
                    if (req_write) begin
                        for (int j = 0; j < 4; j = j + 1)
                            if (req_wstrb[j])
                                ram[req_addr[31:2]][8*j +: 8] <= req_wdata[8*j +: 8];
                    end else saved_data <= ram[req_addr[31:2]];
                end
            end else if (pending) begin
                if (delay_left != 0) delay_left <= delay_left - 1'b1;
                else begin
                    pending <= 0;
                    rsp_valid <= 1;
                    rsp_rdata <= saved_data;
                    rsp_error <= saved_error;
                end
            end
        end
    end
endmodule
