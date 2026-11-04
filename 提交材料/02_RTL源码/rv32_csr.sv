// M-mode only. Interrupt inputs must be synchronous to clk.
module rv32_csr #(
    parameter logic [31:0] HART_ID = 0
) (
    input logic clk, rst, retire, retire_second,
    input logic irq_software, irq_timer, irq_external,
    input logic [11:0] read_addr,
    output logic [31:0] read_data,
    output logic read_legal, read_only,
    input logic write_en,
    input logic [11:0] write_addr,
    input logic [31:0] write_data,
    input logic trap, trap_interrupt, mret,
    input logic [31:0] trap_pc, trap_cause, trap_value,
    output logic irq_pending,
    output logic [31:0] irq_cause, trap_vector, return_pc,
    output logic [63:0] cycle_count, instret_count
);
    logic [31:0] mstatus, mie, mtvec, mscratch, mepc, mcause, mtval;
    logic [31:0] mip, inhibit;
    logic [31:0] enabled;
    always_comb begin
        mip = 0; mip[3] = irq_software; mip[7] = irq_timer; mip[11] = irq_external;
        enabled = mip & mie;
        irq_pending = mstatus[3] && |enabled;
        irq_cause = enabled[11] ? 32'd11 : enabled[3] ? 32'd3 : 32'd7;
    end
    always_comb begin
        trap_vector = {mtvec[31:2], 2'b0};
        if (trap_interrupt && mtvec[1:0] == 1)
            trap_vector = {mtvec[31:2], 2'b0} + {irq_cause[29:0], 2'b0};
        return_pc = mepc;
    end
    always_comb begin
        read_data = 0; read_legal = 1;
        read_only = (read_addr[11:10] == 2'b11);
        case (read_addr)
            12'h300: read_data = mstatus;
            12'h301: read_data = 32'h40001100; // RV32 I + M, fixed WARL.
            12'h304: read_data = mie;
            12'h305: read_data = mtvec;
            12'h306: read_data = 0; // No lower privilege modes.
            12'h310: read_data = 0; // Little-endian, no optional mstatush fields.
            12'h320: read_data = inhibit;
            12'h340: read_data = mscratch;
            12'h341: read_data = mepc;
            12'h342: read_data = mcause;
            12'h343: read_data = mtval;
            12'h344: read_data = mip;
            12'hb00, 12'hc00: read_data = cycle_count[31:0];
            12'hb80, 12'hc80: read_data = cycle_count[63:32];
            12'hb02, 12'hc02: read_data = instret_count[31:0];
            12'hb82, 12'hc82: read_data = instret_count[63:32];
            12'hf11, 12'hf12, 12'hf13: read_data = 0;
            12'hf14: read_data = HART_ID;
            default: read_legal = 0;
        endcase
    end
    always_ff @(posedge clk) begin
        if (rst) begin
            mstatus <= 32'h1800; mie <= 0; mtvec <= 0; mscratch <= 0;
            mepc <= 0; mcause <= 0; mtval <= 0; inhibit <= 0;
            cycle_count <= 0; instret_count <= 0;
        end else begin
            if (!inhibit[0]) cycle_count <= cycle_count + 64'd1;
            if ((retire || retire_second) && !inhibit[2])
                instret_count <= instret_count + {63'b0, retire} + {63'b0, retire_second};
            if (write_en) begin
                case (write_addr)
                    12'h300: mstatus <= (write_data & 32'h88) | 32'h1800;
                    12'h304: mie <= write_data & 32'h888;
                    12'h305: mtvec <= {write_data[31:2], 1'b0, write_data[1:0] == 1};
                    12'h320: inhibit <= write_data & 32'h5;
                    12'h340: mscratch <= write_data;
                    12'h341: mepc <= {write_data[31:2], 2'b0};
                    12'h342: mcause <= write_data;
                    12'h343: mtval <= write_data;
                    12'hb00: cycle_count <= {cycle_count[63:32], write_data};
                    12'hb80: cycle_count <= {write_data, cycle_count[31:0]};
                    12'hb02: instret_count <= {instret_count[63:32], write_data};
                    12'hb82: instret_count <= {write_data, instret_count[31:0]};
                    default: begin end // Fixed WARL fields ignore writes.
                endcase
            end
            if (trap) begin
                mepc <= {trap_pc[31:2], 2'b0};
                mcause <= {trap_interrupt, trap_cause[30:0]};
                mtval <= trap_value;
                mstatus[7] <= mstatus[3]; mstatus[3] <= 0;
            end else if (mret) begin
                mstatus[3] <= mstatus[7]; mstatus[7] <= 1;
            end
        end
    end
endmodule
