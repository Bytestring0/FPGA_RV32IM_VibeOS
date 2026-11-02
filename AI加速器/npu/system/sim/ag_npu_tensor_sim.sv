// Independent acceptance wrapper. Production modules are instantiated unchanged.
module ag_npu_tensor_sim #(
    parameter bit DUAL_ISSUE=1,
    parameter bit CACHE_ENABLE=1,
    parameter bit TENSOR_ENABLE=1,
    parameter integer MATRIX_LANES=64,
    parameter integer DDR_OUTSTANDING=1
) (
    input wire clk,rst,card_present,
    output wire console_valid, output wire [7:0] console_data,
    output wire done, output wire [31:0] exit_code,
    output wire trap_valid, output wire [31:0] trap_pc,trap_cause,trap_value,
    output wire retire_valid,retire_second_valid,
    output wire [31:0] retire_pc,retire_second_pc,
    output wire [63:0] cycle_count,instret_count,
    output wire [31:0] dual_retire_count,
    input wire ddr_ready,ddr_fault,
    output wire mem_req_valid,input wire mem_req_ready,
    output wire [29:0] mem_req_addr,output wire mem_req_write,
    output wire [255:0] mem_req_wdata,output wire [31:0] mem_req_wstrb,
    input wire mem_rsp_valid,output wire mem_rsp_ready,
    input wire [255:0] mem_rsp_data,input wire mem_rsp_error,
    output wire sd_clk,sd_mosi,sd_cs_n,
    output wire npu_busy,npu_done,npu_error,
    output wire [31:0] npu_error_code,npu_cycles,npu_results,
    output wire ddr_transaction_active,ddr_transaction_owner,
    output wire [31:0] peripheral_requests,legacy_completions,dma_completions,
    output wire tensor_busy,tensor_done,tensor_error,
    output wire tensor_request,matrix_request,cpu_request,
    output wire matrix_outstanding,tensor_outstanding
);
    ag_npu_system #(.SIM_FAST_UART(1),.DUAL_ISSUE(DUAL_ISSUE),
        .CACHE_ENABLE(CACHE_ENABLE),.LEGACY_ENABLE(0),.TENSOR_ENABLE(TENSOR_ENABLE),.NPU_LANES(MATRIX_LANES),
        .DDR_OUTSTANDING(DDR_OUTSTANDING)) dut (
        .clk(clk),.rst(rst),.irq_external(1'b0),.uart_tx_o(),.gpio(),
        .console_valid(console_valid),.console_data(console_data),
        .done(done),.exit_code(exit_code),.trap_valid(trap_valid),
        .trap_pc(trap_pc),.trap_cause(trap_cause),.trap_value(trap_value),
        .retire_valid(retire_valid),.retire_second_valid(retire_second_valid),
        .retire_pc(retire_pc),.retire_second_pc(retire_second_pc),
        .cycle_count(cycle_count),.instret_count(instret_count),.dual_retire_count(dual_retire_count),
        .ddr_ready(ddr_ready),.ddr_fault(ddr_fault),
        .mem_req_valid(mem_req_valid),.mem_req_ready(mem_req_ready),
        .mem_req_addr(mem_req_addr),.mem_req_write(mem_req_write),
        .mem_req_wdata(mem_req_wdata),.mem_req_wstrb(mem_req_wstrb),
        .mem_rsp_valid(mem_rsp_valid),.mem_rsp_ready(mem_rsp_ready),
        .mem_rsp_data(mem_rsp_data),.mem_rsp_error(mem_rsp_error),
        .sd_clk(sd_clk),.sd_mosi(sd_mosi),.sd_cs_n(sd_cs_n),.sd_miso(1'b1),.sd_card_present(1'b0),
        .npu_busy(npu_busy),.npu_done(npu_done),.npu_error(npu_error),
        .npu_error_code(npu_error_code),.npu_cycles(npu_cycles),.npu_results(npu_results),
        .ddr_transaction_active(ddr_transaction_active),.ddr_transaction_owner(ddr_transaction_owner),
        .peripheral_requests(peripheral_requests),.legacy_completions(legacy_completions),.dma_completions(dma_completions)
    );
    assign tensor_busy=dut.tensor_v2_enabled.tensor_busy;
    assign tensor_done=dut.tensor_v2_enabled.tensor_done;
    assign tensor_error=dut.tensor_v2_enabled.tensor_error;
    assign tensor_request=dut.tensor_qv&&dut.tensor_qr;
    assign matrix_request=dut.dma_qv&&dut.dma_qr;
    assign cpu_request=dut.cpu_qv&&dut.cpu_qr;
    reg [3:0] matrix_inflight,tensor_inflight;
    wire matrix_ack=dut.dma_rv&&dut.dma_rr;
    wire tensor_ack=dut.tensor_rv&&dut.tensor_rr;
    always @(posedge clk)begin
        if(rst)begin matrix_inflight<=0;tensor_inflight<=0;end
        else begin
            case({matrix_request,matrix_ack})
                2'b10:matrix_inflight<=matrix_inflight+1'b1;
                2'b01:matrix_inflight<=matrix_inflight-1'b1;
                default:begin end
            endcase
            case({tensor_request,tensor_ack})
                2'b10:tensor_inflight<=tensor_inflight+1'b1;
                2'b01:tensor_inflight<=tensor_inflight-1'b1;
                default:begin end
            endcase
        end
    end
    assign matrix_outstanding=matrix_inflight!=0;
    assign tensor_outstanding=tensor_inflight!=0;
endmodule
