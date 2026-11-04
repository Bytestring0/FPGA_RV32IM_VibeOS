package rv32_pkg;
    // Named constants retain compatibility with the PDS 2022.2 ADS frontend,
    // which accepts packed structs but rejects SystemVerilog enum declarations.
    typedef logic [4:0] alu_op_t;
    localparam alu_op_t ALU_ADD=0, ALU_SUB=1, ALU_SLL=2, ALU_SLT=3,
        ALU_SLTU=4, ALU_XOR=5, ALU_SRL=6, ALU_SRA=7,
        ALU_OR=8, ALU_AND=9, ALU_COPY=10;
    localparam alu_op_t ALU_ANDN=11, ALU_ORN=12, ALU_XNOR=13,
        ALU_MIN=14, ALU_MAX=15, ALU_MINU=16, ALU_MAXU=17,
        ALU_ROL=18, ALU_ROR=19, ALU_CLZ=20, ALU_CTZ=21, ALU_CPOP=22,
        ALU_SEXTB=23, ALU_SEXTH=24, ALU_ZEXTH=25, ALU_ORCB=26, ALU_REV8=27,
        ALU_SH1ADD=28, ALU_SH2ADD=29, ALU_SH3ADD=30;
    typedef struct packed {
        logic illegal;
        logic [4:0] rs1, rs2, rd;
        logic use_rs1, use_rs2, reg_write;
        logic [31:0] imm;
        alu_op_t alu;
        logic src_pc, src_imm;
        logic branch, jal, jalr;
        logic load, store;
        logic [2:0] funct3;
        logic mul, div;
        logic csr, csr_imm;
        logic [11:0] csr_addr;
        logic ecall, ebreak, mret, fence_i, serial;
    } decode_t;
endpackage
