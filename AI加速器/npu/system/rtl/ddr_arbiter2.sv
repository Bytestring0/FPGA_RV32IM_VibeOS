// Two ordered masters sharing the AG32-byte DDR line interface. DEPTH=1
// preserves the original single-outstanding circuit. DEPTH=4 queues owners
// through their corresponding response ACK; the downstream is ordered/no-tag.
module ddr_arbiter2 #(
    parameter integer DEPTH=1
) (
    input wire clk, rst,
    input wire a_req_valid, output wire a_req_ready,
    input wire [29:0] a_req_addr, input wire a_req_write,
    input wire [255:0] a_req_wdata, input wire [31:0] a_req_wstrb,
    output wire a_rsp_valid, input wire a_rsp_ready,
    output wire [255:0] a_rsp_data, output wire a_rsp_error,
    input wire b_req_valid, output wire b_req_ready,
    input wire [29:0] b_req_addr, input wire b_req_write,
    input wire [255:0] b_req_wdata, input wire [31:0] b_req_wstrb,
    output wire b_rsp_valid, input wire b_rsp_ready,
    output wire [255:0] b_rsp_data, output wire b_rsp_error,
    output wire mem_req_valid, input wire mem_req_ready,
    output wire [29:0] mem_req_addr, output wire mem_req_write,
    output wire [255:0] mem_req_wdata, output wire [31:0] mem_req_wstrb,
    input wire mem_rsp_valid, output wire mem_rsp_ready,
    input wire [255:0] mem_rsp_data, input wire mem_rsp_error,
    output wire active, output wire owner
);
 generate if(DEPTH==1)begin: single
    reg active_reg,owner_reg;
    reg prefer_b, locked, locked_owner;
    wire selected = locked ? locked_owner :
                    (a_req_valid && b_req_valid ? prefer_b : b_req_valid);
    assign active=active_reg;assign owner=owner_reg;
    assign mem_req_valid = !rst && !active_reg && (selected ? b_req_valid : a_req_valid);
    assign mem_req_addr = selected ? b_req_addr : a_req_addr;
    assign mem_req_write = selected ? b_req_write : a_req_write;
    assign mem_req_wdata = selected ? b_req_wdata : a_req_wdata;
    assign mem_req_wstrb = selected ? b_req_wstrb : a_req_wstrb;
    assign a_req_ready = !rst && !active_reg && !selected && mem_req_ready;
    assign b_req_ready = !rst && !active_reg && selected && mem_req_ready;
    assign a_rsp_valid = !rst && active_reg && !owner_reg && mem_rsp_valid;
    assign b_rsp_valid = !rst && active_reg && owner_reg && mem_rsp_valid;
    assign a_rsp_data = mem_rsp_data;
    assign b_rsp_data = mem_rsp_data;
    assign a_rsp_error = mem_rsp_error;
    assign b_rsp_error = mem_rsp_error;
    assign mem_rsp_ready = !rst && active_reg && (owner_reg ? b_rsp_ready : a_rsp_ready);
    always @(posedge clk) begin
        if (rst) begin
            active_reg <= 0; owner_reg <= 0; prefer_b <= 0;
            locked <= 0; locked_owner <= 0;
        end else begin
            if (mem_req_valid && !mem_req_ready) begin
                locked <= 1; locked_owner <= selected;
            end
            if (mem_req_valid && mem_req_ready) begin
                active_reg <= 1; owner_reg <= selected;
                locked <= 0; prefer_b <= !selected;
            end
            if (mem_rsp_valid && mem_rsp_ready) active_reg <= 0;
        end
    end
 end else begin: queued
    localparam integer PTR=$clog2(DEPTH);
    reg owners[0:DEPTH-1];
    reg [PTR-1:0] head,tail;
    reg [PTR:0] count;
    reg prefer_b,locked,locked_owner;
    wire selected=locked?locked_owner:(a_req_valid&&b_req_valid?prefer_b:b_req_valid);
    wire pop=!rst&&count!=0&&mem_rsp_valid&&mem_rsp_ready;
    // A response ACK can return credit in the same cycle; ownership is stored
    // before any later response. No zero-latency combinational DDR responder.
    wire space=count<DEPTH||pop;
    wire push=mem_req_valid&&mem_req_ready;
    assign active=count!=0;
    assign owner=count!=0?owners[head]:1'b0;
    assign mem_req_valid=!rst&&space&&(selected?b_req_valid:a_req_valid);
    assign mem_req_addr=selected?b_req_addr:a_req_addr;
    assign mem_req_write=selected?b_req_write:a_req_write;
    assign mem_req_wdata=selected?b_req_wdata:a_req_wdata;
    assign mem_req_wstrb=selected?b_req_wstrb:a_req_wstrb;
    assign a_req_ready=!rst&&space&&!selected&&mem_req_ready;
    assign b_req_ready=!rst&&space&&selected&&mem_req_ready;
    assign a_rsp_valid=!rst&&count!=0&&!owner&&mem_rsp_valid;
    assign b_rsp_valid=!rst&&count!=0&&owner&&mem_rsp_valid;
    assign a_rsp_data=mem_rsp_data;assign b_rsp_data=mem_rsp_data;
    assign a_rsp_error=mem_rsp_error;assign b_rsp_error=mem_rsp_error;
    assign mem_rsp_ready=!rst&&count!=0&&(owner?b_rsp_ready:a_rsp_ready);
    always @(posedge clk)begin
      if(rst)begin head<=0;tail<=0;count<=0;prefer_b<=0;locked<=0;locked_owner<=0;end
      else begin
        if(mem_req_valid&&!mem_req_ready)begin locked<=1;locked_owner<=selected;end
        if(push)begin owners[tail]<=selected;tail<=tail+1'b1;locked<=0;prefer_b<=!selected;end
        if(pop)head<=head+1'b1;
        case({push,pop})2'b10:count<=count+1'b1;2'b01:count<=count-1'b1;default:begin end endcase
      end
    end
    initial if(DEPTH!=4)$error("ddr_arbiter2 supports DEPTH1 or4");
 end endgenerate
endmodule
