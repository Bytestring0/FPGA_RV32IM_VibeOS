module rv32_decode (
    input  logic [31:0] insn,
    // Flat port avoids a PDS 2022.2 ADS bug with package-qualified struct ports.
    // Must match the 87-bit packed rv32_pkg::decode_t.
    output wire [86:0] decoded
);
    import rv32_pkg::*;
    decode_t d;
    assign decoded = d;
    logic [6:0] op, f7;
    logic [2:0] f3;
    always_comb begin
        op = insn[6:0]; f3 = insn[14:12]; f7 = insn[31:25];
        d = '0;
        d.rs1 = insn[19:15]; d.rs2 = insn[24:20]; d.rd = insn[11:7];
        d.funct3 = f3;
        d.alu = ALU_ADD;
        case (op)
            7'h37, 7'h17: begin
                d.reg_write = 1; d.src_imm = 1;
                d.imm = {insn[31:12], 12'b0};
                d.src_pc = (op == 7'h17);
                d.alu = (op == 7'h37) ? ALU_COPY : ALU_ADD;
            end
            7'h6f: begin
                d.jal = 1; d.reg_write = 1;
                d.imm = {{11{insn[31]}}, insn[31], insn[19:12],
                         insn[20], insn[30:21], 1'b0};
            end
            7'h67: begin
                d.jalr = 1; d.reg_write = 1; d.use_rs1 = 1;
                d.imm = {{20{insn[31]}}, insn[31:20]};
                d.illegal = (f3 != 0);
            end
            7'h63: begin
                d.branch = 1; d.use_rs1 = 1; d.use_rs2 = 1;
                d.imm = {{19{insn[31]}}, insn[31], insn[7],
                         insn[30:25], insn[11:8], 1'b0};
                d.illegal = (f3 == 2 || f3 == 3);
            end
            7'h03: begin
                d.load = 1; d.reg_write = 1; d.use_rs1 = 1; d.src_imm = 1;
                d.imm = {{20{insn[31]}}, insn[31:20]};
                d.illegal = (f3 == 3 || f3 == 6 || f3 == 7);
            end
            7'h23: begin
                d.store = 1; d.use_rs1 = 1; d.use_rs2 = 1; d.src_imm = 1;
                d.imm = {{20{insn[31]}}, insn[31:25], insn[11:7]};
                d.illegal = (f3 > 2);
            end
            7'h13, 7'h33: begin
                d.reg_write = 1; d.use_rs1 = 1;
                d.use_rs2 = (op == 7'h33); d.src_imm = (op == 7'h13);
                d.imm = {{20{insn[31]}}, insn[31:20]};
                if (op == 7'h33 && f7 == 7'h01) begin
                    d.mul = !f3[2]; d.div = f3[2];
                end else begin
                    case (f3)
                        0: begin
                            d.alu = (op == 7'h33 && f7 == 7'h20) ? ALU_SUB : ALU_ADD;
                            if (op == 7'h33 && f7 != 0 && f7 != 7'h20) d.illegal = 1;
                        end
                        1: begin d.alu = ALU_SLL; if (f7 != 0) d.illegal = 1; end
                        2: d.alu = ALU_SLT;
                        3: d.alu = ALU_SLTU;
                        4: d.alu = ALU_XOR;
                        5: begin
                            d.alu = f7[5] ? ALU_SRA : ALU_SRL;
                            if (f7 != 0 && f7 != 7'h20) d.illegal = 1;
                        end
                        6: d.alu = ALU_OR;
                        7: d.alu = ALU_AND;
                    endcase
                    if (op == 7'h33 && f3 != 0 && f3 != 5 && f7 != 0) d.illegal = 1;
                end
                // RV32 Zba/Zbb encodings. Unknown/reserved encodings remain
                // illegal; these overrides do not enable other B subsets.
                if (op == 7'h33) begin
                    if (f7 == 7'h20 && (f3 == 4 || f3 == 6 || f3 == 7)) begin
                        d.illegal = 0;
                        case (f3)
                            4: d.alu = ALU_XNOR;
                            6: d.alu = ALU_ORN;
                            7: d.alu = ALU_ANDN;
                            default: begin end
                        endcase
                    end
                    if (f7 == 7'h05 && f3 >= 4) begin
                        d.illegal = 0;
                        case (f3)
                            4: d.alu = ALU_MIN;
                            5: d.alu = ALU_MINU;
                            6: d.alu = ALU_MAX;
                            7: d.alu = ALU_MAXU;
                            default: begin end
                        endcase
                    end
                    if (f7 == 7'h30 && (f3 == 1 || f3 == 5)) begin
                        d.illegal = 0; d.alu = f3 == 1 ? ALU_ROL : ALU_ROR;
                    end
                    if (f7 == 7'h04 && f3 == 4 && insn[24:20] == 0) begin
                        d.illegal = 0; d.alu = ALU_ZEXTH; d.use_rs2 = 0;
                    end
                    if (f7 == 7'h10 && (f3 == 2 || f3 == 4 || f3 == 6)) begin
                        d.illegal = 0;
                        case (f3)
                            2: d.alu = ALU_SH1ADD;
                            4: d.alu = ALU_SH2ADD;
                            6: d.alu = ALU_SH3ADD;
                            default: begin end
                        endcase
                    end
                end else begin
                    if (f3 == 1 && insn[31:25] == 7'h30) begin
                        case (insn[24:20])
                            0: begin d.alu = ALU_CLZ; d.illegal = 0; end
                            1: begin d.alu = ALU_CTZ; d.illegal = 0; end
                            2: begin d.alu = ALU_CPOP; d.illegal = 0; end
                            4: begin d.alu = ALU_SEXTB; d.illegal = 0; end
                            5: begin d.alu = ALU_SEXTH; d.illegal = 0; end
                            default: begin end
                        endcase
                    end
                    if (f3 == 5 && f7 == 7'h30) begin d.alu = ALU_ROR; d.illegal = 0; end
                    if (f3 == 5 && insn[31:20] == 12'h287) begin d.alu = ALU_ORCB; d.illegal = 0; end
                    if (f3 == 5 && insn[31:20] == 12'h698) begin d.alu = ALU_REV8; d.illegal = 0; end
                end
            end
            7'h0f: begin
                // FENCE is a full pipeline barrier; FENCE.I also flushes fetch/BTB.
                d.serial = 1; d.fence_i = (f3 == 1);
                d.illegal = (f3 != 0 && f3 != 1);
            end
            7'h73: begin
                d.serial = 1;
                if (f3 == 0) begin
                    case (insn)
                        32'h00000073: d.ecall = 1;
                        32'h00100073: d.ebreak = 1;
                        32'h30200073: d.mret = 1;
                        32'h10500073: begin end // WFI: legal implementation as NOP.
                        default: d.illegal = 1;
                    endcase
                end else if (f3 == 4) d.illegal = 1;
                else begin
                    d.csr = 1; d.reg_write = 1;
                    d.csr_imm = f3[2]; d.use_rs1 = !f3[2];
                    d.csr_addr = insn[31:20];
                end
            end
            default: d.illegal = 1;
        endcase
    end
endmodule
