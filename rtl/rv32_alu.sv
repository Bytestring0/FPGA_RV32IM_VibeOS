// Separate left/right shifters keep late forwarded operands off reversal muxes.
// Zba/Zbb are only instantiated in the full execution lane.
module rv32_alu #(parameter bit EXTENSIONS = 1) (
    input logic [4:0] op,
    input logic [31:0] a, b,
    output logic [31:0] result
);
    import rv32_pkg::*;
    wire rotating = EXTENSIONS && (op == ALU_ROL || op == ALU_ROR);
    wire [31:0] reverse_a;
    wire [31:0] shift_upper = rotating ? a : (op == ALU_SRA ? {32{a[31]}} : 32'b0);
    wire [63:0] shifted_right = {shift_upper, a} >> b[4:0];
    wire [63:0] shifted_left = {a, (rotating ? a : 32'b0)} << b[4:0];
    for (genvar k = 0; k < 32; k = k + 1) begin
        assign reverse_a[k] = a[31-k];
    end
    wire [31:0] add_a = !EXTENSIONS ? a : op == ALU_SH1ADD ? {a[30:0],1'b0} :
        op == ALU_SH2ADD ? {a[29:0],2'b0} : op == ALU_SH3ADD ? {a[28:0],3'b0} : a;
    function automatic [5:0] leading_zeros(input logic [31:0] v);
        logic [31:0] t;
        logic [5:0] n;
        begin
            t = v; n = 0;
            if (t[31:16] == 0) begin n = n + 6'd16; t = t << 16; end
            if (t[31:24] == 0) begin n = n + 6'd8; t = t << 8; end
            if (t[31:28] == 0) begin n = n + 6'd4; t = t << 4; end
            if (t[31:30] == 0) begin n = n + 6'd2; t = t << 2; end
            if (!t[31]) n = n + 6'd1;
            leading_zeros = v == 0 ? 6'd32 : n;
        end
    endfunction
    wire [2:0] pop4 [0:7];
    wire [3:0] pop8 [0:3];
    wire [4:0] pop16 [0:1];
    wire [5:0] pop32 = {1'b0,pop16[0]} + {1'b0,pop16[1]};
    for (genvar p = 0; p < 8; p = p + 1)
        assign pop4[p] = {2'b0,a[p*4]} + {2'b0,a[p*4+1]} + {2'b0,a[p*4+2]} + {2'b0,a[p*4+3]};
    for (genvar p = 0; p < 4; p = p + 1)
        assign pop8[p] = {1'b0,pop4[p*2]} + {1'b0,pop4[p*2+1]};
    for (genvar p = 0; p < 2; p = p + 1)
        assign pop16[p] = {1'b0,pop8[p*2]} + {1'b0,pop8[p*2+1]};
    always_comb begin
        result = 0;
        case (op)
            ALU_ADD: result = add_a + b;
            ALU_SUB: result = a - b;
            ALU_SLL: result = shifted_left[63:32];
            ALU_SRL, ALU_SRA: result = shifted_right[31:0];
            ALU_SLT: result = {31'b0,$signed(a) < $signed(b)};
            ALU_SLTU: result = {31'b0,a < b};
            ALU_XOR: result = a ^ b;
            ALU_OR: result = a | b;
            ALU_AND: result = a & b;
            ALU_COPY: result = b;
            // Keep a single parallel selection layer for base and Zb results.
            // EXTENSIONS is constant-folded for the auxiliary execution lane.
            ALU_ANDN: result = EXTENSIONS ? a & ~b : 0;
            ALU_ORN: result = EXTENSIONS ? a | ~b : 0;
            ALU_XNOR: result = EXTENSIONS ? ~(a ^ b) : 0;
            ALU_MIN: result = EXTENSIONS ? ($signed(a) < $signed(b) ? a : b) : 0;
            ALU_MAX: result = EXTENSIONS ? ($signed(a) > $signed(b) ? a : b) : 0;
            ALU_MINU: result = EXTENSIONS ? (a < b ? a : b) : 0;
            ALU_MAXU: result = EXTENSIONS ? (a > b ? a : b) : 0;
            ALU_ROL: result = EXTENSIONS ? shifted_left[63:32] : 0;
            ALU_ROR: result = EXTENSIONS ? shifted_right[31:0] : 0;
            ALU_CLZ, ALU_CTZ: result = EXTENSIONS ? {26'b0,leading_zeros(op == ALU_CTZ ? reverse_a : a)} : 0;
            ALU_CPOP: result = EXTENSIONS ? {26'b0,pop32} : 0;
            ALU_SEXTB: result = EXTENSIONS ? {{24{a[7]}},a[7:0]} : 0;
            ALU_SEXTH: result = EXTENSIONS ? {{16{a[15]}},a[15:0]} : 0;
            ALU_ZEXTH: result = EXTENSIONS ? {16'b0,a[15:0]} : 0;
            ALU_ORCB: result = EXTENSIONS ? {{8{|a[31:24]}},{8{|a[23:16]}},{8{|a[15:8]}},{8{|a[7:0]}}} : 0;
            ALU_REV8: result = EXTENSIONS ? {a[7:0],a[15:8],a[23:16],a[31:24]} : 0;
            ALU_SH1ADD, ALU_SH2ADD, ALU_SH3ADD: result = EXTENSIONS ? add_a + b : 0;
            default: result = 0;
        endcase
    end
endmodule
