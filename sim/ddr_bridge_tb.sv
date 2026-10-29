`timescale 1ns/1ps
// Protocol model only: does not simulate the encrypted PHY or DRAM calibration.
module ddr_bridge_tb;
    real cpu_half = 4.901961, ui_half = 4.0;
    logic clk=0, ui_clk=0, reset_n=0, rst=1, init_done=0, ui_running=1;
    initial begin
        if ($value$plusargs("CPU_HALF=%f", cpu_half)) begin end
        forever #(cpu_half) clk = !clk;
    end
    initial begin #1.37; forever #(ui_half) if (ui_running) ui_clk = !ui_clk; end
    logic iv=0, ir, irv, irr=0, dv=0, dr, dw=0, drv, drr=0, dre;
    logic [31:0] ia=0, da=0, dd=0, ddata;
    logic [3:0] ds=0;
    logic [1:0] ie;
    logic [63:0] idata;
    logic qv, qr, qw, rv, rr, re, ready_cpu, fault_cpu;
    logic [29:0] qa;
    logic [255:0] qd, rd;
    logic [31:0] qs;
    logic [27:0] awaddr, araddr;
    logic awvalid, arvalid, awready, arready, wready, wlast, rvalid, rlast;
    logic [255:0] wdata, rdata;
    logic [31:0] wstrb;
    logic [3:0] wid, rid;
    ag_extmem front (
        .clk(clk), .rst(rst), .i_req_valid(iv), .i_req_ready(ir), .i_req_addr(ia),
        .i_rsp_valid(irv), .i_rsp_ready(irr), .i_rsp_data(idata), .i_rsp_error(ie),
        .d_req_valid(dv), .d_req_ready(dr), .d_req_addr(da), .d_req_wdata(dd),
        .d_req_wstrb(ds), .d_req_write(dw), .d_rsp_valid(drv), .d_rsp_ready(drr),
        .d_rsp_data(ddata), .d_rsp_error(dre), .req_valid(qv), .req_ready(qr),
        .req_addr(qa), .req_write(qw), .req_wdata(qd), .req_wstrb(qs),
        .rsp_valid(rv), .rsp_ready(rr), .rsp_data(rd), .rsp_error(re));
    ag_hmic_bridge #(.TIMEOUT_BITS(8)) bridge (
        .cpu_clk(clk), .ui_clk(ui_clk), .reset_n(reset_n), .init_done(init_done),
        .ready_cpu(ready_cpu), .fault_cpu(fault_cpu),
        .req_valid(qv), .req_ready(qr), .req_addr(qa), .req_write(qw), .req_wdata(qd), .req_wstrb(qs),
        .rsp_valid(rv), .rsp_ready(rr), .rsp_data(rd), .rsp_error(re),
        .axi_awaddr(awaddr), .axi_araddr(araddr), .axi_awvalid(awvalid), .axi_arvalid(arvalid),
        .axi_awready(awready), .axi_arready(arready), .axi_wdata(wdata), .axi_wstrb(wstrb),
        .axi_wready(wready), .axi_wusero_last(wlast), .axi_wusero_id(wid),
        .axi_rdata(rdata), .axi_rvalid(rvalid), .axi_rlast(rlast), .axi_rid(rid));
    logic [31:0] memory_words [int unsigned];
    logic [31:0] expected [int unsigned];
    logic [31:0] lfsr=32'h712a9c31;
    logic wp=0, rp=0, fast_mode=0, silence=0, bad_last=0, bad_id=0;
    logic [27:0] wa, ra;
    integer delay_left=0, transactions=0, checks=0;
    function automatic logic [31:0] word_at(input int unsigned a);
        return memory_words.exists(a) ? memory_words[a] : 32'hca11ab1e;
    endfunction
    function automatic logic [255:0] line_at(input logic [27:0] a);
        for (int k=0;k<8;k++) line_at[32*k+:32] = word_at(a+k);
    endfunction
    assign awready = reset_n && !wp && !rp && !silence && (fast_mode || lfsr[0]);
    assign arready = reset_n && !wp && !rp && !silence && (fast_mode || lfsr[2]);
    assign wready = !silence && ((wp && lfsr[4]) || (fast_mode && awvalid && awready));
    assign rvalid = !silence && ((rp && delay_left==0) || (fast_mode && arvalid && arready));
    assign rdata = line_at(rp ? ra : araddr);
    assign rlast = !bad_last; assign wlast = !bad_last;
    assign rid = bad_id ? 4'd7 : 4'd0; assign wid = bad_id ? 4'd3 : 4'd0;
    always @(posedge ui_clk) begin
        lfsr <= {lfsr[30:0], lfsr[31]^lfsr[21]^lfsr[1]^lfsr[0]};
        if (!reset_n) begin wp<=0; rp<=0; delay_left<=0; end
        else begin
            if (awvalid && awready) begin
                if (awaddr[2:0] != 0) $fatal(1,"Unaligned HMIC AW");
                wa <= awaddr; wp <= !wready; transactions++;
            end
            if (arvalid && arready) begin
                if (araddr[2:0] != 0) $fatal(1,"Unaligned HMIC AR");
                ra <= araddr; rp <= !fast_mode; delay_left <= 1+int'(lfsr[8:5]); transactions++;
            end
            if (wp && wready || awvalid && awready && wready) begin
                for (int k=0;k<8;k++) begin
                    logic [31:0] v;
                    int unsigned a;
                    a = (wp ? wa : awaddr)+k; v = word_at(a);
                    for(int b=0;b<4;b++) if(wstrb[4*k+b]) v[8*b+:8]=wdata[32*k+8*b+:8];
                    memory_words[a] = v;
                end
                wp <= 0;
            end
            if (rp && delay_left>0) delay_left <= delay_left-1;
            if (rp && rvalid) rp <= 0;
        end
    end
    task automatic reset_bridge;
        @(negedge clk); reset_n=0; rst=1; init_done=0; iv=0; dv=0; irr=0; drr=0;
        ui_running=1; silence=0; bad_last=0; bad_id=0;
        repeat(8) @(negedge clk);
        reset_n=1;
        repeat(8) @(negedge clk);
        rst=0;
    endtask
    task automatic calibrated;
        @(negedge ui_clk); init_done=1;
        wait(ready_cpu); repeat(2) @(negedge clk);
    endtask
    task automatic data_op(input logic write_op, input logic [31:0] addr, data,
        input logic [3:0] mask, input logic error_expected=0);
        logic [31:0] got, exp;
        logic err;
        int unsigned index_word;
        @(negedge clk); dv=1; dw=write_op; da=addr; dd=data; ds=mask;
        do @(posedge clk); while(!dr);
        @(negedge clk); dv=0;
        while(!drv) @(negedge clk);
        got=ddata; err=dre;
        repeat(5) begin
            @(negedge clk);
            if(!drv || got!==ddata || err!==dre) $fatal(1,"Unstable CPU data response");
        end
        if(err!==error_expected) $fatal(1,"data error mismatch addr=%h got=%b",addr,err);
        if(!err) begin
            index_word=(addr & 32'h3fffffff)>>2;
            exp=expected.exists(index_word) ? expected[index_word] : 32'hca11ab1e;
            if(write_op) begin
                for(int b=0;b<4;b++) if(mask[b]) exp[8*b+:8]=data[8*b+:8];
                expected[index_word]=exp;
            end else if(got!==exp) $fatal(1,"read addr=%h got=%h expected=%h",addr,got,exp);
        end
        checks++; drr=1; @(negedge clk); drr=0;
    endtask
    task automatic fetch_op(input logic [31:0] addr, input logic [1:0] error_expected=0);
        logic [63:0] got; logic [1:0] err; int unsigned a;
        @(negedge clk); iv=1; ia=addr;
        do @(posedge clk); while(!ir);
        @(negedge clk); iv=0;
        while(!irv) @(negedge clk);
        got=idata; err=ie;
        repeat(7) begin
            @(negedge clk);
            if(!irv || got!==idata || err!==ie) $fatal(1,"Unstable CPU fetch response");
        end
        if(err!==error_expected) $fatal(1,"fetch error addr=%h got=%b expected=%b",addr,err,error_expected);
        a=(addr & 32'h3fffffff)>>2;
        if(!err[0] && got[31:0]!==word_at(a)) $fatal(1,"fetch low mismatch");
        if(!err[1] && got[63:32]!==word_at(a+1)) $fatal(1,"fetch high mismatch");
        checks++; irr=1; @(negedge clk); irr=0;
    endtask
    initial begin
        reset_bridge();
        data_op(0,32'h80000000,0,0,1);
        if(transactions!=0) $fatal(1,"DDR command before calibration");
        calibrated();
        // A blocked instruction response must not prevent an outstanding data
        // access from finishing; retain the instruction fault while D proceeds.
        @(negedge clk); iv=1;ia=32'h40000000;
        do @(posedge clk); while(!ir);
        @(negedge clk);iv=0;
        while(!irv) @(negedge clk);
        data_op(0,32'h80000000,0,0);
        if(!irv || ie!=3) $fatal(1,"Blocked I response corrupted by D transaction");
        checks++;irr=1;@(negedge clk);irr=0;
        for(int k=0;k<256;k++) begin
            data_op(1,32'h80000000+4*(k%64),32'(k*'h10203041),4'(k));
            data_op(0,32'h80000000+4*(k%64),0,0);
        end
        for(int k=0;k<64;k++) fetch_op(32'h80000000+4*k);
        fast_mode=1;
        for(int k=0;k<32;k++) begin
            data_op(1,32'h80001000+4*k,32'h55aa00ff ^ 32'(k),15);
            fetch_op(32'h80001000+4*k);
        end
        data_op(1,32'hbffffffc,32'h1234abcd,15); fetch_op(32'hbffffffc,2);
        data_op(0,32'hc0000000,0,0,1); fetch_op(32'h7ffffffc,3); fetch_op(32'h80000002,3);
        fork fetch_op(32'h8000001c); data_op(0,32'h80001008,0,0); join
        fast_mode=0;
        // Error on only the second line preserves the valid first instruction.
        fork
            fetch_op(32'h8000001c,2);
            begin wait(arvalid && araddr==28'd8); @(negedge ui_clk); bad_id=1; end
        join
        reset_bridge(); calibrated();
        // A common reset aborts an accepted read; it must not create a ghost response.
        @(negedge clk); dv=1; dw=0; da=32'h80001000; ds=0;
        do @(posedge clk); while(!dr);
        @(negedge clk); dv=0;
        wait(arvalid && arready);
        reset_bridge(); calibrated();
        repeat(20) begin @(negedge clk); if(drv || irv) $fatal(1,"Response survived reset"); end
        fork
            data_op(0,32'h80000000,0,0,1);
            begin wait(arvalid && arready); @(negedge ui_clk); init_done=0; end
        join
        repeat(8) @(negedge clk);
        if(!fault_cpu) $fatal(1,"calibration loss must latch fault");
        init_done=1; data_op(0,32'h80000000,0,0,1);
        reset_bridge(); calibrated(); bad_id=1; data_op(0,32'h80000000,0,0,1);
        reset_bridge(); calibrated(); bad_last=1; data_op(1,32'h80000000,0,15,1);
        reset_bridge(); calibrated(); silence=1; data_op(0,32'h80000000,0,0,1);
        reset_bridge(); calibrated(); ui_running=0; data_op(0,32'h80000000,0,0,1);
        reset_bridge(); calibrated(); data_op(0,32'h80001000,0,0);
        $display("PASS DDR bridge checks=%0d native_transactions=%0d cpu_half_ns=%f",checks,transactions,cpu_half);
        $finish;
    end
    initial begin #5000000; $fatal(1,"DDR test timeout"); end
endmodule
