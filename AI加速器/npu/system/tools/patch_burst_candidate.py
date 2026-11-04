import pathlib,re,sys
root=pathlib.Path('/mnt/d/FPGA/RiscV')
src=root/'npu_system_pds/npu_hot102_20261104'
dst=root/'npu_system_pds/npu_burst16_102_20261104'
dst.mkdir(parents=True,exist_ok=True)
for name in ('ag_npu_board.sv','board.fdc','synthesize.tcl','implement.tcl','bitstream_hot.tcl','firmware.hex'):
    (dst/name).write_bytes((src/name).read_bytes())
m=(dst/'ag_npu_board.sv').read_text(encoding='utf-8')
rtl=(root/'rtl/ag_hmic_bridge.sv').read_text(encoding='utf-8')
start=rtl.index('// Default DEPTH1')
end=rtl.index('endmodule',start)+len('endmodule')
wrapper=rtl[start:end]
burst=(root/'rtl/ag_hmic_read_burst.sv').read_text(encoding='utf-8')
i=m.index('module ag_hmic_bridge #(')
j=m.index('endmodule',i)+len('endmodule')
m=m[:i]+'// --- true-burst bridge (candidate) ---\n'+burst.strip()+'\n\n'+wrapper+m[j:]
assert m.count('ag_hmic_bridge #(.DEPTH(32))')==1
m=m.replace('ag_hmic_bridge #(.DEPTH(32))','ag_hmic_bridge #(.DEPTH(32),.MAX_READ_BURST(16))')
assert m.count('wire [27:0] axi_awaddr, axi_araddr;')==1
m=m.replace('wire [27:0] axi_awaddr, axi_araddr;','wire [27:0] axi_awaddr, axi_araddr;wire [3:0] axi_arlen;')
assert m.count('.axi_araddr(axi_araddr), .axi_arvalid(axi_arvalid)')==1
m=m.replace('.axi_araddr(axi_araddr), .axi_arvalid(axi_arvalid)','.axi_araddr(axi_araddr), .axi_arlen(axi_arlen), .axi_arvalid(axi_arvalid)')
assert m.count(".axi_arlen(4'b0)")==1
m=m.replace(".axi_arlen(4'b0)",'.axi_arlen(axi_arlen)')
(dst/'ag_npu_board.sv').write_text(m,encoding='utf-8')
t=(dst/'implement.tcl').read_text(encoding='utf-8').replace('npu_hot102_20261104','npu_burst16_102_20261104')
(dst/'implement.tcl').write_text(t,encoding='utf-8')
print('PATCH_OK bytes',len(m),'burst_module',m.count('module ag_hmic_read_burst'))
