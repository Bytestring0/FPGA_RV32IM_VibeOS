"""Prepare/build the actual MES2L676-100HP CPU+DDR+NPU+SD board project.
Windows only; freezes sources into an ASCII path, never overwrites an old run.
Bitstream generation is optional and requires passing routed internal timing; board/SD/ILA verification remains separate.
"""
import argparse,hashlib,json,os,re,shutil,subprocess,sys,time
from pathlib import Path
import xml.etree.ElementTree as ET

SYSTEM=Path(__file__).resolve().parents[1]
NPU=SYSTEM.parent;WORK=NPU.parent.parent;CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK
def sha(p):return hashlib.sha256(Path(p).read_bytes()).hexdigest()
def text(p):return Path(p).read_text(encoding='utf-8-sig')
def save(p,v):p.write_text(json.dumps(v,indent=2,ensure_ascii=False)+'\n',encoding='utf-8')

def installed_pds():
    for candidate in (os.environ.get('PDS_ROOT'),r'D:\App\pango\PDS_2025.2-ads'):
        if candidate and (Path(candidate)/'bin/pds_shell.exe').is_file():return candidate
    raise FileNotFoundError('PDS2025.2 is required; set PDS_ROOT to its installation')

def pds_environment(pds):
    env=dict(os.environ)
    env.setdefault('SystemRoot',os.environ.get('WINDIR',r'C:\Windows'))
    env.setdefault('WINDIR',env['SystemRoot'])
    env['PATH']=str(pds/'bin')+os.pathsep+env.get('PATH',str(Path(env['SystemRoot'])/'System32'))
    if not env.get('PANGO_LICENSE_FILE'):
        import winreg
        for hive,keyname in ((winreg.HKEY_CURRENT_USER,'Environment'),(winreg.HKEY_LOCAL_MACHINE,r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment')):
            try:
                with winreg.OpenKey(hive,keyname) as key:env['PANGO_LICENSE_FILE']=winreg.QueryValueEx(key,'PANGO_LICENSE_FILE')[0]
            except OSError:continue
            if env.get('PANGO_LICENSE_FILE'):break
        if not env.get('PANGO_LICENSE_FILE'):
            licenses=sorted((pds/'license').glob('*.lic'))
            if len(licenses)==1:env['PANGO_LICENSE_FILE']=str(licenses[0])
    return env

def run_pds(command,out,env,report,timeout,logname):
    started=time.monotonic()
    with (out/logname).open('wb') as log:
        proc=subprocess.Popen(command,cwd=out,env=env,stdout=log,stderr=subprocess.STDOUT)
        report['pds_pid']=proc.pid
        save(out/'report.json',report)
        try:proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            report['timed_out']=True
            subprocess.run([str(Path(env['SystemRoot'])/'System32/taskkill.exe'),'/PID',str(proc.pid),'/T','/F'],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,timeout=15,check=False)
            proc.wait(timeout=15)
            raise RuntimeError('PDS exceeded command timeout; process tree terminated')
        finally:report[logname+'_elapsed_seconds']=round(time.monotonic()-started,3)
    report['actual_exit']=proc.returncode
    if re.search(r'^E:|Program Error Out\.',text(out/logname),re.M):raise RuntimeError('PDS reported an error; inspect '+logname)
    if proc.returncode:raise RuntimeError('PDS process failed; inspect '+logname)

def main(a):
    if os.name!='nt':raise ValueError('use the bundled Windows Python for PDS')
    if not re.fullmatch('[a-z0-9_]+',a.run):raise ValueError('ASCII run name required')
    if a.command_timeout_seconds is None:
        a.command_timeout_seconds=300 if a.mode=='compile' else 1800 if a.mode=='synthesis' else 2700 if a.mode=='map' else a.pnr_timeout_minutes*60+1800
    if a.bitstream and a.mode!='pnr':raise ValueError('--bitstream requires --mode pnr and passing routed timing')
    if a.read_burst>a.ddr_outstanding:raise ValueError('read burst exceeds reserved DDR return credits')
    out=WORK/'npu_system_pds'/a.run
    if out.exists():raise FileExistsError(out)
    firmware=Path(a.firmware).resolve();ip=Path(a.ip).resolve();pds=Path(a.pds).resolve()
    if not firmware.is_file():raise FileNotFoundError(firmware)
    firmware_build=firmware.parent/'report.json'
    firmware_binding=None
    if firmware_build.is_file():
        fw=json.loads(text(firmware_build))
        if fw.get('scope')=='real_tensor_v2_cooperative_agent_firmware_build_only':
            if not fw.get('passed') or fw.get('clock_hz')!=a.clock*1000000:
                raise ValueError('Agent firmware report clock does not match requested hardware clock')
            if fw.get('partial_sram_enabled',False)!=a.partial_sram:raise ValueError('Agent firmware/hardware partial SRAM configuration differs')
            pin=fw.get('artifacts',{}).get(firmware.name,{})
            if pin.get('sha256')!=sha(firmware):raise ValueError('Agent firmware differs from its build report')
            firmware_binding={'report':str(firmware_build),'sha256':sha(firmware_build),'clock_hz':fw['clock_hz']}
    firmware_words=text(firmware).split()
    if len(firmware_words)!=a.tcm_kib*256 or any(not re.fullmatch('[0-9a-fA-F]{8}',word) for word in firmware_words):
        raise ValueError('firmware.hex must contain exactly the selected TCM word count (8 hex digits per word)')
    idf=ET.fromstring(text(ip/'ag_ddr3.idf'))
    if [idf.findtext('header/'+k) for k in ('version','device','package','speedgrade')]!=['1.10','PG2L100H','FBG676','-6']:
        raise ValueError('reviewed HMIC_S 1.10 device configuration required')
    reference=ET.fromstring(text(CPU/'fpga/ip/ag_ddr3/ag_ddr3.idf'))
    refparams={p.findtext('name'):p.findtext('value') for p in reference.findall('param_list/param')}
    params={p.findtext('name'):p.findtext('value') for p in idf.findall('param_list/param')}
    if any(params.get(k)!=v for k,v in refparams.items()):raise ValueError('DDR IP parameters differ from CPU configuration')
    vendor=[ip/x.attrib['pathname'].replace('\\','/') for x in idf.findall('file_list/source/file')]
    if not vendor or any(not p.is_file() for p in vendor):raise ValueError('incomplete generated vendor sources')
    paths=[CPU/line.strip() for line in text(CPU/'rtl/files.f').splitlines() if line.strip() and not line.startswith('#')]
    paths += [CPU/'rtl/ag_hmic_bridge.sv',CPU/'fpga/board/ag_board_clock.sv',
              NPU/'rtl/npu_bram.sv',NPU/'rtl/npu_bram_top.sv']
    if a.read_burst>1:paths += [CPU/'rtl/ag_hmic_read_burst.sv']
    paths += [NPU.parent/'rtl/compute'/name for name in ('mac_lane.sv','group_acc.sv','gemv_core.sv')]
    paths += sorted((SYSTEM/'rtl').glob('*.sv'))
    simulation_binding=None
    if a.simulation_report:
        simpath=Path(a.simulation_report).resolve();sim=json.loads(text(simpath))
        if not sim.get('passed'):raise ValueError('passed system simulation report required')
        if a.tensor_v2:
            if sim.get('cpu_mode')!='original':raise ValueError('PDS requires simulation of original production CPU RTL, not a compatibility patch')
            if sim.get('tensor_v2_enabled') is not True or sim.get('matrix_lanes')!=a.npu_lanes or sim.get('cache_enabled') is not True:
                raise ValueError('tensor PDS requires matching tensor/matrix/cache system simulation configuration')
            if sim.get('partial_sram_enabled',False)!=a.partial_sram:raise ValueError('PDS/simulation partial SRAM configuration differs')
            if sim.get('tensor_shared_raw_accum',False)!=a.shared_raw_accum:
                raise ValueError('PDS/simulation shared raw accumulator configuration differs')
            if sim.get('tensor_parallel_raw') is not (not a.scalar_raw) or sim.get('tensor_parallel_delta') is not (not a.scalar_delta):
                raise ValueError('PDS/simulation tensor parallelism configuration differs')
            required=[p for p in paths if p.parent==SYSTEM/'rtl' or (p.parent==CPU/'rtl' and p.name!='ag_hmic_bridge.sv')]
            missing=[p.relative_to(WORK).as_posix() for p in required if p.relative_to(WORK).as_posix() not in {k.replace('\\','/') for k in sim.get('source_sha256',{})}]
            if missing:raise ValueError('system simulation missing hardware identities: '+', '.join(missing))
        if sim.get('ddr_outstanding',1)!=a.ddr_outstanding:raise ValueError('PDS/simulation outstanding depth differs')
        recorded={k.replace('\\','/'):v for k,v in sim['source_sha256'].items()}
        for source in paths:
            key=source.relative_to(WORK).as_posix()
            if key in recorded and sha(source)!=recorded[key]:raise ValueError('PDS/simulation source mismatch: '+key)
        simulation_binding={'report':str(simpath),'sha256':sha(simpath),'passed':True}
    board=text(CPU/'fpga/board/ag_ddr_board_top.sv').replace('module ag_ddr_board_top','module ag_npu_board_top')
    board=board.replace('parameter integer CLOCK_MHZ = 102',f'parameter integer CLOCK_MHZ = {a.clock}')
    board=board.replace('parameter bit BOARD_MULH_PIPELINE = 0','parameter bit BOARD_MULH_PIPELINE = 1')
    board=board.replace('    output wire uart_txd, output wire [7:0] led,',
        '    output wire uart_txd, output wire [7:0] led,\n    output wire sd_clk,sd_mosi,sd_cs_n,\n    input wire sd_miso,sd_detect_n,')
    start=board.index('    ag_soc #(');end=board.index('    ag_hmic_bridge bridge',start)
    board=board[:start]+f'''    ag_npu_system #(.CLOCK_HZ(CLOCK_MHZ*1000000),.INIT_FILE("firmware.hex"),
        .MULH_PIPELINE(BOARD_MULH_PIPELINE),.CACHE_ENABLE(BOARD_CACHE_ENABLE),
        .CACHE_SET_BITS(BOARD_CACHE_SET_BITS),.TCM_WORDS({a.tcm_kib*256}),.LEGACY_ENABLE(0),
         .NPU_REUSE_ACT_LINE({int(a.npu_reuse_act_line)}),.TENSOR_ENABLE({int(a.tensor_v2)}),
         .LOCAL_CPU_RESET({int(a.local_cpu_reset)}),.NPU_LANES({a.npu_lanes}),.DDR_OUTSTANDING({a.ddr_outstanding}),.PARTIAL_SRAM_ENABLE({int(a.partial_sram)}),
          .TENSOR_PARALLEL_RAW({int(not a.scalar_raw)}),.TENSOR_PARALLEL_DELTA({int(not a.scalar_delta)}),
          .TENSOR_SHARED_RAW_ACCUM({int(a.shared_raw_accum)})) soc (
        .clk(cpu_clk),.rst(reset_sync[2]),.irq_external(1'b0),
        .uart_tx_o(uart_txd),.gpio(gpio),.done(done),.exit_code(exit_code),
        .ddr_ready(ddr_ready),.ddr_fault(ddr_fault),
        .mem_req_valid(req_valid),.mem_req_ready(req_ready),.mem_req_addr(req_addr),
        .mem_req_write(req_write),.mem_req_wdata(req_wdata),.mem_req_wstrb(req_wstrb),
        .mem_rsp_valid(rsp_valid),.mem_rsp_ready(rsp_ready),.mem_rsp_data(rsp_data),.mem_rsp_error(rsp_error),
        .sd_clk(sd_clk),.sd_mosi(sd_mosi),.sd_cs_n(sd_cs_n),
        .sd_miso(sd_miso),.sd_card_present(!sd_detect_n)
    );
'''+board[end:]
    board=board.replace('    ag_hmic_bridge bridge',f'    ag_hmic_bridge #(.DEPTH({a.ddr_outstanding}),.MAX_READ_BURST({a.read_burst})) bridge')
    if a.read_burst>1:
        board=board.replace('wire [27:0] axi_awaddr, axi_araddr;', 'wire [27:0] axi_awaddr, axi_araddr;wire [3:0] axi_arlen;')
        board=board.replace('.axi_araddr(axi_araddr), .axi_arvalid(axi_arvalid)', '.axi_araddr(axi_araddr), .axi_arlen(axi_arlen), .axi_arvalid(axi_arvalid)')
        board=board.replace(".axi_arlen(4'b0)", '.axi_arlen(axi_arlen)')
    out.mkdir(parents=True)
    merged='\n'.join('// Source: '+str(p.relative_to(WORK))+'\n'+text(p) for p in paths)+'\n'+board
    if a.cpu_regfile_write_buffers:
        anchor='    logic [31:0] w_pc, w_insn, w_result;'
        buffers='''
    // Opt-in physical-only identity buffers on distributed regfile write data.
    // INIT=2'b10 maps input0->0,input1->1; no extra cycle or ISA change.
    (* syn_keep = 1 *) wire [31:0] regfile_hold_wdata0,regfile_hold_wdata1;
    genvar regfile_hold_bit;
    generate for(regfile_hold_bit=0;regfile_hold_bit<32;regfile_hold_bit=regfile_hold_bit+1) begin: regfile_hold_buffers
        (* syn_keep = 1, syn_noprune = 1 *) GTP_LUT1 #(.INIT(2'h2)) lane0_buf(.I0(w_result[regfile_hold_bit]),.Z(regfile_hold_wdata0[regfile_hold_bit]));
        (* syn_keep = 1, syn_noprune = 1 *) GTP_LUT1 #(.INIT(2'h2)) lane1_buf(.I0(w_second_result[regfile_hold_bit]),.Z(regfile_hold_wdata1[regfile_hold_bit]));
    end endgenerate
'''
        changes=[(anchor,anchor+buffers),('regs_lane0[w_rd] <= w_result;','regs_lane0[w_rd] <= regfile_hold_wdata0;'),('regs_lane1[w_second_rd] <= w_second_result;','regs_lane1[w_second_rd] <= regfile_hold_wdata1;')]
        for old,new in changes:
            if merged.count(old)!=1:raise ValueError('CPU regfile buffer anchor changed: '+old)
            merged=merged.replace(old,new)
    (out/'ag_npu_board.sv').write_text(merged,encoding='utf-8')
    shutil.copyfile(firmware,out/'firmware.hex')
    if a.clock==27:
        clocks='create_clock -name cpu_clk -period 37.037037 [get_ports clk_27m]\n'
    else:
        ratio={54:(2,1),81:(3,1),96:(32,9),102:(34,9),108:(4,1),162:(6,1)}[a.clock]
        clocks=('create_clock -name board_27m -period 37.037037 [get_ports clk_27m]\n'
                f'create_generated_clock -name cpu_clk -source [get_ports clk_27m] -multiply_by {ratio[0]} -divide_by {ratio[1]} '
                '[get_pins {clocking_unit/pll_mode.pll/CLKOUT0}]\n')
    fdc=clocks+'set_false_path -from [get_ports reset_n]\nset_false_path -to [get_ports {uart_txd led[*]}]\n'
    cdc_file='fpga/board/ddr_queue4_cdc.fdc' if a.ddr_outstanding>=4 else 'fpga/board/ddr_cdc.fdc'
    cdc=text(CPU/cdc_file)
    if a.ddr_outstanding>=4:
        # Gray source bits must reach the first sync stage within one SOURCE
        # period. The stock 7ns limit is unsafe for the 162MHz CPU (6.173ns).
        cpu_bound=min(7.0,int((1000.0/a.clock-0.15)*10)/10.0)
        if cpu_bound<=0:raise ValueError('CPU clock too fast for Gray CDC margin')
        for source in ('bridge/*requests*wgray*','bridge/*responses*rgray*'):
            old='set_max_delay 7.0 -datapath_only -from [get_cells -hierarchical {'+source+'}]'
            if cdc.count(old)!=1:raise ValueError('CPU-sourced Gray FDC anchor missing: '+source)
            cdc=cdc.replace(old,old.replace('7.0',str(cpu_bound),1))
        for src,dst in (('cpu_clk','ddrphy_sysclk'),('ddrphy_sysclk','cpu_clk')):
            bound='set_max_delay 8.0 -datapath_only -from [get_clocks '+src+'] -to [get_clocks '+dst+']'
            if cdc.count(bound)!=1:raise ValueError('FIFO CDC clock-to-clock max-delay missing: '+src+' -> '+dst)
    fdc+='\n'.join((text(CPU/'fpga/ip/ag_ddr3/board_ddr.fdc'),text(CPU/'fpga/board/board_pins.fdc'),cdc))
    if a.clock==27:
        fdc=fdc.replace('set_false_path -from [get_pins {clocking_unit/pll_mode.pll/LOCK}]','')
    fdc+='\n# MicroSD SPI from the supplied board manual, physical page 53.\n'
    for port,pin in [('sd_clk','F19'),('sd_mosi','H14'),('sd_miso','F18'),('sd_cs_n','H15'),('sd_detect_n','G19')]:
        fdc+=f'define_attribute {{p:{port}}} {{PAP_IO_LOC}} {{{pin}}}\n'
        fdc+=f'define_attribute {{p:{port}}} {{PAP_IO_VCCIO}} {{3.3}}\n'
        fdc+=f'define_attribute {{p:{port}}} {{PAP_IO_STANDARD}} {{LVCMOS33}}\n'
    fdc+='\n# SD external timing and detect polarity require actual card/board validation.\n'
    (out/'board.fdc').write_text(fdc,encoding='utf-8')
    setup=['set_arch -family Logos2 -device PG2L100H -speedgrade -6 -package FBG676','add_design ag_npu_board.sv']
    setup += ['add_design {'+str(p).replace('\\','/')+'}' for p in vendor]
    setup += ['add_constraint board.fdc','compile -top_module ag_npu_board_top -system_verilog -multi_file_compilation_unit TRUE']
    if a.mode in ('synthesis','map','pnr'):setup+=['synthesize -ads -automatic_partition_block TRUE -max_parallel_jobs 6 -resource_sharing '+('TRUE' if a.resource_sharing else 'FALSE')]
    if a.ddr_outstanding>=4 and a.mode in ('synthesis','map','pnr'):
        shutil.copyfile(CPU/'fpga/board/check_ddr_queue4_cdc.tcl',out/'check_ddr_queue4_cdc.tcl')
        # PDS pre-parses Tcl source/exec while queuing flow actions, so run the
        # real-netlist audit from Python after PDS has actually completed.
    if a.mode in ('map','pnr'):setup+=['dev_map']
    if a.mode=='pnr':
        hold_fix=' -fix_hold_violation_threshold_for_clock_path_cross_srb 50 -max_hold_violated_paths_num 5000'
        reuse=''
        if a.reuse_placement_db:
            placed=Path(a.reuse_placement_db).resolve();prior=placed.parents[1]
            if not placed.is_file() or any(sha(prior/name)!=sha(out/name) for name in ('ag_npu_board.sv','firmware.hex','board.fdc')):
                raise ValueError('reused placement requires exact matching RTL, firmware and constraints')
            reuse=' -input_place_db_file {'+str(placed).replace('\\','/')+'}'
        route=(f' -groute_seed {a.route_seed}' if a.route_seed is not None else '')
        router=(' -route_mode fast' if a.fast_router else ' -route_mode normal')+(' -share_router_control_signal FALSE' if a.disable_control_chain else '')
        if a.reuse_placement_db:
            # Placement already contains the mapped netlist; avoid repeating
            # compilation, synthesis and device mapping for route-only work.
            setup=[line for line in setup if not line.startswith(('compile ','synthesize ','dev_map'))]
        setup+=[f'pnr -gplace_seed {a.seed} -fix_hold_violation_in_route TRUE -fix_hold_violation_threshold_for_clk_tree 50 -action_timeout {a.pnr_timeout_minutes}'+hold_fix+reuse+route+router,'report_timing']
    (out/'build.tcl').write_text('\n'.join(setup)+'\n',encoding='utf-8')
    report={'passed':False,'mode':a.mode,'board':'MES2L676-100HP','part':'PG2L100H-6FBG676',
        'command_timeout_seconds':a.command_timeout_seconds,'clock_mhz':a.clock,'tcm_bytes':a.tcm_kib*1024,'placement_seed':a.seed,'ddr_bytes':1073741824,'lanes':a.npu_lanes,'legacy_enabled':False,
        'npu_reuse_act_line':a.npu_reuse_act_line,'resource_sharing':a.resource_sharing,'fast_router':a.fast_router,'shared_router_control_signal':not a.disable_control_chain,
        'tensor_parallel_raw':not a.scalar_raw,'tensor_parallel_delta':not a.scalar_delta,'tensor_shared_raw_accum':a.shared_raw_accum,'cpu_regfile_write_buffers':a.cpu_regfile_write_buffers,'hold_fix_iterations':a.hold_fix_iterations,
        'bitstream_requested':a.bitstream,'bitstream_generated':False,
        'reuse_placement_db':a.reuse_placement_db,'route_seed':a.route_seed,'pnr_timeout_minutes':a.pnr_timeout_minutes,
        'partial_sram_enabled':a.partial_sram,'tensor_v2_enabled':a.tensor_v2,'matrix_lanes':a.npu_lanes,'ddr_outstanding':a.ddr_outstanding,
        'board_verified':False,'ila_captured':False,'sd_external_timing_verified':False,'sd_detect_polarity_verified':False,
        'physical_timing_closed':False,'source_sha256':{str(p.relative_to(WORK)):sha(p) for p in paths},
        'vendor_source_sha256':{str(p):sha(p) for p in vendor},'generated_rtl_sha256':sha(out/'ag_npu_board.sv'),
        'firmware_sha256':sha(out/'firmware.hex'),'constraint_sha256':sha(out/'board.fdc'),
        'firmware_binding':firmware_binding,'simulation_binding':simulation_binding}
    save(out/'report.json',report)
    if not a.build:print('PREPARED',out);return
    env=pds_environment(pds)
    if not (pds/'bin/pds_shell.exe').is_file():raise FileNotFoundError(pds/'bin/pds_shell.exe')
    command=[str(pds/'bin/pds_shell.exe'),'-file',str(out/'build.tcl'),'-work_dir',str(out),'-project_name','ag_npu_board']
    try:
        run_pds(command,out,env,report,a.command_timeout_seconds,'console.log')
        report['console_sha256']=sha(out/'console.log')
        db_root=Path(a.reuse_placement_db).resolve().parents[1] if a.reuse_placement_db else out
        if a.reuse_placement_db:
            report['reused_stage_root']=str(db_root)
            report['reused_placement_sha256']=sha(Path(a.reuse_placement_db).resolve())
        syn_root=db_root/'prj_tasks/syn_1'
        pnr_root=db_root/'prj_tasks/pnr_1'
        compiled=syn_root/'compile/ag_npu_board_top_comp.adf'
        if not compiled.is_file():raise RuntimeError('PDS returned without a compile database')
        if a.mode!='compile' and not (syn_root/'synthesize/ag_npu_board_top_syn.vm').is_file():raise RuntimeError('missing actual synthesized netlist')
        if a.mode in ('map','pnr') and not list((pnr_root/'device_map').glob('*.dmr')):raise RuntimeError('missing device-map result')
        if a.mode!='compile':
            snr=text(syn_root/'synthesize/ag_npu_board_top.snr')
            patterns={'luts':r'Total LUTs:\s*(\d+)', 'registers':r'Total Registers:\s*(\d+)',
                'latches':r'Total Latches:\s*(\d+)', 'drm36k':r'Total DRMs?\s*=\s*([\d.]+)',
                'apms':r'Total APMs\s*=\s*([\d.]+)'}
            report['synthesis_resources']={key:float(re.search(pattern,snr)[1]) for key,pattern in patterns.items()}
            if report['synthesis_resources']['latches']:raise RuntimeError('unexpected inferred latches')
            cpu_est=re.search(r'^\s*cpu_clk\s+([\d.]+) MHz\s+([\d.]+) MHz\s+([\d.]+)\s+([\d.]+)\s+(-?[\d.]+)',snr,re.M)
            if cpu_est:report['synthesis_cpu_timing']={'requested_mhz':float(cpu_est[1]),'estimated_mhz':float(cpu_est[2]),'setup_slack_ns':float(cpu_est[5])}
        if a.ddr_outstanding>=4 and a.mode!='compile':
            audit_output=out/'queue4_cdc_real_audit.json'
            audit_netlist=out/'prj_tasks/syn_1/synthesize/ag_npu_board_top_syn.vm'
            audit_fdc=out/'board.fdc'
            if a.reuse_placement_db:
                audit_fdc=db_root/'board.fdc'
                if sha(audit_fdc)!=sha(out/'board.fdc'):raise RuntimeError('reused netlist CDC constraint identity differs')
            if not audit_netlist.is_file():
                audit_netlist=syn_root/'synthesize/ag_npu_board_top_syn.vm'
            audit_command=[sys.executable,str(CPU/'fpga/board/audit_ddr_queue4_cdc.py'),'--netlist',str(audit_netlist),'--fdc',str(audit_fdc),'--output',str(audit_output),'--require-bounded-gray','--depth',str(a.ddr_outstanding)]
            with (out/'queue4_cdc_real_audit.log').open('wb') as log:
                audited=subprocess.run(audit_command,cwd=out,stdout=log,stderr=subprocess.STDOUT,check=False)
            report['queue4_cdc_audit']={'exit_code':audited.returncode,'report':str(audit_output),'log_sha256':sha(out/'queue4_cdc_real_audit.log')}
            if audit_output.is_file():report['queue4_cdc_audit']['report_sha256']=sha(audit_output)
            if audited.returncode:raise RuntimeError('real-netlist DDR queue CDC audit failed')
        if a.mode=='pnr':
            route=out/'prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf'
            timing=out/'prj_tasks/pnr_1/report_timing/ag_npu_board_top.rtr'
            if not route.is_file() or not timing.is_file():raise RuntimeError('missing routed database or timing report')
            if timing.stat().st_mtime<route.stat().st_mtime:raise RuntimeError('timing report predates routed database')
            report['all_constrained_paths_met']='Design Summary : All Constraints Met.' in text(timing)
            report['internal_timing_closed']=report['all_constrained_paths_met']
            if not report['all_constrained_paths_met']:raise RuntimeError('routed setup/hold timing did not meet all constraints')
            # SD external timing remains explicitly unverified, even when internal timing passes.
            if a.bitstream:
                bit_setup=[line for line in setup if line.startswith(('set_arch ','add_design ','add_constraint '))]
                bit_setup+=['gen_bit_stream -force_to_run -input_db_file {prj_tasks/pnr_1/place_route/ag_npu_board_top_pnr.adf} -write_drm_data TRUE -scbv {High Voltage} -unused_io_status UNUSED']
                bit_tcl=out/'bitstream.tcl'
                bit_tcl.write_text('\n'.join(bit_setup)+'\n',encoding='utf-8')
                bit_command=[str(pds/'bin/pds_shell.exe'),'-file',str(bit_tcl),'-work_dir',str(out),'-project_name','ag_npu_board']
                run_pds(bit_command,out,env,report,300,'bitstream.log')
                bit=out/'prj_tasks/pnr_1/generate_bitstream/ag_npu_board_top.sbit'
                if not bit.is_file() or not bit.stat().st_size:raise RuntimeError('missing generated bitstream')
                report['bitstream_generated']=True
                report['bitstream']={'path':str(bit),'sha256':sha(bit),'bytes':bit.stat().st_size}
                report['timing_report_sha256']=sha(timing)
        if report['source_sha256']!={str(p.relative_to(WORK)):sha(p) for p in paths}:raise RuntimeError('sources changed during PDS build')
        report['passed']=True
    except BaseException as exc:
        report['failure']=repr(exc);raise
    finally:save(out/'report.json',report)
    print('PDS_'+a.mode.upper()+'_COMPLETE',out,'board_verified=false sd_external_timing_verified=false')

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--run',required=True);p.add_argument('--firmware',required=True)
    p.add_argument('--ip',default=str(WORK/'npu_system_pds/ddr_ip'))
    p.add_argument('--clock',type=int,choices=(27,54,81,96,102,108,162),default=102)
    p.add_argument('--pds',default=installed_pds(),help='PDS2025.2 installation root')
    p.add_argument('--tcm-kib',type=int,choices=(64,128),default=64)
    p.add_argument('--mode',choices=('compile','synthesis','map','pnr'),default='map');p.add_argument('--build',action='store_true')
    p.add_argument('--resource-sharing',action='store_true',help='trial PDS operator sharing; leave default off for established physical results')
    p.add_argument('--npu-reuse-act-line',action='store_true',
                   help='reuse each cached 256-bit activation line across its two 128-bit beats (default: off)')
    p.add_argument('--partial-sram',action='store_true')
    p.add_argument('--local-cpu-reset',action='store_true',help='register CPU reset release locally; requires matching system simulation')
    p.add_argument('--shared-raw-accum',action='store_true',help='share raw W4 post-scale accumulator; requires matching RTL simulation configuration')
    p.add_argument('--bitstream',action='store_true',help='generate sbit after PNR passes internal setup/hold timing')
    p.add_argument('--scalar-raw',action='store_true',help='area-only variant requires matching system simulation')
    p.add_argument('--scalar-delta',action='store_true',help='area-only variant requires matching system simulation')
    p.add_argument('--tensor-v2',action='store_true',help='enable experimental tensor/state integer device; default baseline disabled')
    p.add_argument('--ddr-outstanding',type=int,choices=(1,4,8,16,32),default=1)
    p.add_argument('--read-burst',type=int,choices=(1,2,4,8,16),default=1,help='coalesce ordered adjacent reads into true HMIC bursts')
    p.add_argument('--npu-lanes',type=int,choices=(16,32,64),default=16)
    p.add_argument('--command-timeout-seconds',type=int,choices=range(1,10801),metavar='SECONDS',default=None,help='hard PDS process-tree timeout; defaults compile 300s, synthesis 1800s, map 2700s, PNR action budget plus 1800s')
    p.add_argument('--pnr-timeout-minutes',type=int,choices=range(1,91),default=15)
    p.add_argument('--fast-router',action='store_true',help='use the vendor fast router; PG2L does not support router thread-count override')
    p.add_argument('--disable-control-chain',action='store_true',help='disable shared router CE/reset chains to reduce congestion')
    p.add_argument('--simulation-report');p.add_argument('--seed',type=int,default=2);p.add_argument('--hold-fix-iterations',type=int,choices=range(0,11),default=0);p.add_argument('--reuse-placement-db');p.add_argument('--route-seed',type=int);p.add_argument('--cpu-regfile-write-buffers',action='store_true')
    main(p.parse_args())
