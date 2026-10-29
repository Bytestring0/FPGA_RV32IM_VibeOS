`timescale 1ns/1ps

// Standalone regression: does not use the shared SoC firmware or build model.
// Reference counts scan one bit at a time, independently of the RTL tree.
module alu_bitcount_tb;
    import rv32_pkg::*;

    logic [4:0] op;
    logic [31:0] a, b;
    wire [31:0] result_full, result_base;
    integer bit_vectors = 0;
    integer bit_checks = 0;
    integer base_checks = 0;
    integer i;
    logic [31:0] one_bit, random_state, sample_a, sample_b;

    rv32_alu #(.EXTENSIONS(1)) full_alu (
        .op(op), .a(a), .b(b), .result(result_full));
    rv32_alu #(.EXTENSIONS(0)) base_alu (
        .op(op), .a(a), .b(b), .result(result_base));

    function automatic [31:0] reference_clz(input logic [31:0] value);
        integer bit_index, count;
        logic found;
        begin
            count = 0;
            found = 0;
            for (bit_index = 31; bit_index >= 0; bit_index = bit_index - 1) begin
                if (value[bit_index]) found = 1;
                else if (!found) count = count + 1;
            end
            reference_clz = count;
        end
    endfunction

    function automatic [31:0] reference_ctz(input logic [31:0] value);
        integer bit_index, count;
        logic found;
        begin
            count = 0;
            found = 0;
            for (bit_index = 0; bit_index < 32; bit_index = bit_index + 1) begin
                if (value[bit_index]) found = 1;
                else if (!found) count = count + 1;
            end
            reference_ctz = count;
        end
    endfunction

    function automatic [31:0] reference_cpop(input logic [31:0] value);
        integer bit_index, count;
        begin
            count = 0;
            for (bit_index = 0; bit_index < 32; bit_index = bit_index + 1)
                if (value[bit_index]) count = count + 1;
            reference_cpop = count;
        end
    endfunction

    // Fixed seed and arithmetic make the corpus reproducible across simulators.
    function automatic [31:0] next_random(input logic [31:0] value);
        logic [31:0] temp;
        begin
            temp = value ^ (value << 13);
            temp = temp ^ (temp >> 17);
            next_random = temp ^ (temp << 5);
        end
    endfunction

    task automatic check_count_op(input logic [4:0] operation,
                                  input logic [31:0] value, expected);
        begin
            op = operation;
            a = value;
            b = 32'hcafe0023; // Counts must ignore the second operand.
            #1;
            if (result_full !== expected)
                $fatal(1, "bitcount op=%0d a=%08h expected=%0d got=%08h",
                       operation, value, expected, result_full);
            if (result_base !== 32'b0)
                $fatal(1, "EXTENSIONS=0 op=%0d expected zero got=%08h",
                       operation, result_base);
            bit_checks = bit_checks + 1;
        end
    endtask

    task automatic check_counts(input logic [31:0] value);
        begin
            check_count_op(ALU_CLZ, value, reference_clz(value));
            check_count_op(ALU_CTZ, value, reference_ctz(value));
            check_count_op(ALU_CPOP, value, reference_cpop(value));
            bit_vectors = bit_vectors + 1;
        end
    endtask

    task automatic check_base_op(input logic [4:0] operation,
                                 input logic [31:0] value_a, value_b, expected);
        begin
            op = operation;
            a = value_a;
            b = value_b;
            #1;
            if (result_base !== expected || result_full !== expected)
                $fatal(1, "base op=%0d a=%08h b=%08h expected=%08h base=%08h full=%08h",
                       operation, value_a, value_b, expected, result_base, result_full);
            base_checks = base_checks + 1;
        end
    endtask

    task automatic check_base_ops(input logic [31:0] value_a, value_b);
        begin
            check_base_op(ALU_ADD, value_a, value_b, value_a + value_b);
            check_base_op(ALU_SUB, value_a, value_b, value_a - value_b);
            check_base_op(ALU_SLL, value_a, value_b, value_a << value_b[4:0]);
            check_base_op(ALU_SRL, value_a, value_b, value_a >> value_b[4:0]);
            check_base_op(ALU_SRA, value_a, value_b, $signed(value_a) >>> value_b[4:0]);
            check_base_op(ALU_SLT, value_a, value_b,
                          $signed(value_a) < $signed(value_b) ? 32'd1 : 32'd0);
            check_base_op(ALU_SLTU, value_a, value_b, value_a < value_b ? 32'd1 : 32'd0);
            check_base_op(ALU_XOR, value_a, value_b, value_a ^ value_b);
            check_base_op(ALU_OR, value_a, value_b, value_a | value_b);
            check_base_op(ALU_AND, value_a, value_b, value_a & value_b);
            check_base_op(ALU_COPY, value_a, value_b, value_b);
        end
    endtask

    initial begin
        op = ALU_ADD;
        a = 0;
        b = 0;
        check_counts(32'b0);
        check_counts(32'hffffffff);
        check_counts(32'haaaaaaaa);
        check_counts(32'h55555555);
        for (i = 0; i < 32; i = i + 1) begin
            one_bit = 32'b1 << i;
            check_counts(one_bit);
            check_counts(~one_bit);
            check_counts(one_bit - 32'd1);
            check_counts(one_bit + 32'd1);
            check_counts(one_bit | (one_bit >> 1));
            check_counts(one_bit | (one_bit << 1));
        end
        // Every 16-bit value in each half independently exercises zero groups,
        // nibble boundaries, and top/bottom priority, including all-zero input.
        for (i = 0; i < 65536; i = i + 1) begin
            check_counts({16'b0, i[15:0]});
            check_counts({i[15:0], 16'b0});
        end
        random_state = 32'h1931cafe;
        for (i = 0; i < 20000; i = i + 1) begin
            random_state = next_random(random_state);
            check_counts(random_state);
        end
        check_base_ops(32'h00000000, 32'h00000000);
        check_base_ops(32'hffffffff, 32'h00000001);
        check_base_ops(32'h80000000, 32'h7fffffff);
        check_base_ops(32'h7fffffff, 32'h80000000);
        for (i = 0; i < 32; i = i + 1)
            check_base_ops(32'h87654321, i);
        for (i = 0; i < 2000; i = i + 1) begin
            random_state = next_random(random_state);
            sample_a = random_state;
            random_state = next_random(random_state);
            sample_b = random_state;
            check_base_ops(sample_a, sample_b);
        end
        $display("PASS alu_bitcount: %0d values, %0d bitcount operations, %0d base operations; both EXTENSIONS settings checked",
                 bit_vectors, bit_checks, base_checks);
        $finish;
    end
endmodule
