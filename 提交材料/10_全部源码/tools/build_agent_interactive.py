#!/usr/bin/env python3
"""Build the real full-model Agent/UI image; emit firmware assets only."""
import argparse
import pathlib
import subprocess

SYSTEM = pathlib.Path(__file__).resolve().parents[1]
ROOT = SYSTEM.parents[2]

def run(argv):
    result = subprocess.run([str(x) for x in argv], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.stdout: print(result.stdout[:5000], flush=True)
    if result.returncode: raise RuntimeError('build command failed with exit '+str(result.returncode))

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--output', required=True, type=pathlib.Path)
    p.add_argument('--config', required=True, type=pathlib.Path)
    p.add_argument('--clock-hz', type=int, default=102000000)
    p.add_argument('--smoke-tokens', type=int, default=2)
    p.add_argument('--uart-assets', action='store_true')
    p.add_argument('--profile-device', action='store_true')
    p.add_argument('--simple-profile', action='store_true')
    p.add_argument('--compiler-prefix', default='riscv64-unknown-elf-')
    a = p.parse_args()
    out = a.output.resolve(); out.mkdir(parents=True, exist_ok=True)
    config = a.config.resolve()
    for name in ('npu_opt_deployment.h','agent_tool_profile.h','tensor_agent_deployment.h'):
        if (out/name).resolve() != (config/name).resolve():
            (out/name).write_bytes((config/name).read_bytes())
    if a.simple_profile:
        import json
        _tools=[("calculate","math expression, returns result_id",{"expression":"string"}),
                ("save_result","only if user asks to save",{"result_id":"string"}),
                ("query_state","notes and timer state",{}),
                ("start_timer","needs user duration",{"value":"number","unit":"秒|分钟|小时","label":"string"}),
                ("finish","final answer or chat reply",{"text":"string"}),
                ("clarify","ask for missing information",{"question":"string"})]
        profile="<tools>\n"+"\n".join(json.dumps({"name":n,"use":u,"args":a},ensure_ascii=False,separators=(',',':')) for n,u,a in _tools)+"\n</tools>\n"+"One tool per turn. Names and arg keys exactly as listed. Math uses calculate; read the real receipt, then finish. Save only when asked. Timer without a duration asks clarify. Reply in English with finish. No thinking.\n<tool_call>\n<function=NAME>\n<parameter=KEY>\nvalue\n</parameter>\n</function>\n</tool_call>\n"
        (out/'agent_tool_profile.h').write_text('static const char qw_agent_profile[] __attribute__((section(".agent_profile"),used)) = '+json.dumps(profile,ensure_ascii=False)+';\n',encoding='utf-8')
    deployment=out/'tensor_agent_deployment.h'
    lines=deployment.read_text().splitlines()
    deployment.write_text('\n'.join('#define AGENT_CLOCK_HZ '+str(a.clock_hz)+'u' if line.startswith('#define AGENT_CLOCK_HZ ') else line for line in lines)+'\n')
    runtime = SYSTEM/'integration/agent_runtime_20261101'
    model = runtime/'sw/model'; agent = runtime/'sw/agent'; vendor = model/'vendor/npu_v1'
    flags = ['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-std=c11',
             '-ffreestanding','-fno-builtin','-fno-pic','-msmall-data-limit=0','-mno-relax',
             '-ffunction-sections','-fdata-sections','-ffp-contract=off','-fno-fast-math',
             '-Wall','-Wextra','-Werror','-Wno-misleading-indentation']
    defines = ['QW_FREESTANDING','QW_ENABLE_TENSOR_V2','QW_CONTEXT_MAX=2048u',
               'QW_AGENT_PROMPT_MAX=8192u','NPU_FAST_CRC=1',
               'SD_SPI_CLOCK_HZ='+str(a.clock_hz)+'u','SD_SPI_DATA_DIVIDER=3u',
               'SD_SPI_MULTIBLOCK=1','NPU_FAT32_MULTI_READ=1','AGENT_MODEL_SMOKE_TOKENS='+str(a.smoke_tokens),
               'AGENT_INTERACTIVE_UI','QW_TENSOR_ONLY_TARGET','QW_FUSED_MATRIX','QW_PRECISE_NORM']
    if a.uart_assets: defines += ['AGENT_UART_ASSETS']
    if a.profile_device: defines += ['AGENT_PROFILE_DEVICE']
    flags += ['-D'+x for x in defines]
    flags += ['-include',SYSTEM/'sw/agent_target_support.h']
    for inc in (model,agent,vendor,model/'freestanding_include',SYSTEM/'sw',ROOT/'sw',ROOT/'sw/coremark',ROOT/'third_party/coremark',out):
        flags += ['-I',inc]
    cold = [agent/name for name in ('ag_agent.c','ag_loop.c','qw_tool_normalize.c','qw_agent_protocol_backend.c')]
    cold += [SYSTEM/'sw'/name for name in ('ag_text_display.c','ag_agent_ui.c','ag_coremark_ui.c')]
    cold += [model/name for name in ('qw_tokenizer.c','qw_load.c','qw_json.c','qw_math.c','qw_libm.c','qw_int_sample.c','qw_int_head.c')]
    cold += [SYSTEM/'sw'/name for name in ('npu_tensor_reference.c','npu_fixed_math.c')]
    cold += [vendor/name for name in ('npu_raw_reference.c','npu_raw.c','npu_package.c','npu_storage.c')]
    objects = []
    for source in cold:
        obj = out/('agent_ddr_'+source.stem+'.o')
        run([a.compiler_prefix+'gcc',*flags,'-DCPU_HZ='+str(a.clock_hz)+'u','-c',source,'-o',obj]); objects.append(obj)
    sources = [SYSTEM/'sw/tensor_agent_cooperative_board.c',SYSTEM/'sw/agent_target_support.c']
    sources += [f for f in sorted(model.glob('qw_*.c')) if f not in cold]
    sources += [vendor/name for name in ('npu_raw.c','npu_raw_reference.c','npu_runtime.c',
                'npu_package.c','npu_storage.c','npu_ag_platform.c','sd_card.c','npu_fat32.c','freestanding.c')]
    sources += [SYSTEM/'sw'/name for name in ('npu_fixed_math.c','npu_tensor_reference.c',
                'npu_tensor_driver.c','npu_tensor_ag.c','npu_fused_matrix.c')]
    if a.uart_assets: sources += [SYSTEM/'sw/ag_asset_loader.c']
    sources = [source for source in sources if source not in cold]
    bench_flags=['-march=rv32im_zicsr_zifencei_zba_zbb','-mabi=ilp32','-O3','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-msmall-data-limit=0','-mno-relax','-funroll-all-loops','-finline-limit=1000','-frename-registers','-mtune=sifive-7-series','-I'+str(ROOT/'third_party/coremark'),'-I'+str(ROOT/'sw/coremark'),'-I'+str(ROOT/'sw'),'-DCPU_HZ='+str(a.clock_hz)+'u','-DITERATIONS=10000','-DPERFORMANCE_RUN=1','-DCOMPILER_FLAGS="-march=rv32im_zicsr_zifencei_zba_zbb -mabi=ilp32 -O3 -ffreestanding -fno-builtin -msmall-data-limit=0 -funroll-all-loops -finline-limit=1000 -frename-registers -mtune=sifive-7-series"']
    bench_src=[ROOT/'third_party/coremark'/n for n in ('core_main.c','core_list_join.c','core_matrix.c','core_state.c','core_util.c')]
    run([a.compiler_prefix+'gcc',*bench_flags,'-nostdlib','-nostartfiles','-Wl,--gc-sections','-Wl,--no-relax','-T',ROOT/'sw/link.ld',SYSTEM/'sw/coremark_return_start.S',*bench_src,SYSTEM/'sw/coremark_return_port.c',SYSTEM/'sw/coremark_return_printf.c','-lgcc','-o',out/'benchmark.elf'])
    run([a.compiler_prefix+'objcopy','-O','binary',out/'benchmark.elf',out/'benchmark.bin'])
    image=(out/'benchmark.bin').read_bytes()
    if len(image)>65536: raise RuntimeError('TCM benchmark exceeds 64 KiB')
    (out/'benchmark.bin').write_bytes(image+b'\0'*(-len(image)%4))
    (out/'benchmark_image.S').write_text('.section .rodata.tcm_benchmark,"a",@progbits\n.balign 4\n.globl tcm_benchmark_image_start,tcm_benchmark_image_end\ntcm_benchmark_image_start:\n.incbin "'+str(out/'benchmark.bin')+'"\ntcm_benchmark_image_end:\n',encoding='utf-8')
    for src,name in ((SYSTEM/'sw/tcm_benchmark_switch.S','agent_ddr_benchmark_switch.o'),(out/'benchmark_image.S','agent_ddr_benchmark_image.o')):
        run([a.compiler_prefix+'gcc',*flags,'-c',src,'-o',out/name]);objects.append(out/name)
    link = out/'link_tensor_agent.ld'
    link.write_bytes((SYSTEM/'sw/link_agent_interactive.ld').read_bytes())
    elf = out/'firmware.elf'
    run([a.compiler_prefix+'gcc',*flags,'-flto','-nostdlib','-nostartfiles',
         '-Wl,--gc-sections','-Wl,--no-relax','-Wl,--undefined=memcpy','-Wl,--undefined=memset',
         '-T',link,SYSTEM/'sw/agent_start.S',*sources,*objects,'-lgcc','-o',elf])
    symbols = subprocess.check_output([a.compiler_prefix+'nm','-u',str(elf)], text=True)
    if symbols.strip(): raise RuntimeError('unresolved firmware symbols: '+symbols)
    for option,name in (('--only-section=.ddr_resources','APPRES.BIN'),('--remove-section=.ddr_resources','firmware.bin')):
        run([a.compiler_prefix+'objcopy','-O','binary',option,elf,out/name])
    run(['python3',ROOT/'scripts/bin2hex.py',out/'firmware.bin',out/'firmware.hex','--words','16384'])
    run([a.compiler_prefix+'size','-A',elf])
    syms = subprocess.check_output([a.compiler_prefix+'nm','-n',str(elf)], text=True)
    for line in syms.splitlines():
        if any(line.endswith(' '+name) for name in ('_start','main','ag_loop_step','ag_execute','qt_open','ag_agent_ui_poll','__agent_resource_start','__agent_resource_end','__bss_end')):
            print(line)
    print('AGENT_INTERACTIVE_BUILD_PASS tcm=65536 stack=8192 resource_bytes='+str((out/'APPRES.BIN').stat().st_size)+' binary_bytes='+str((out/'firmware.bin').stat().st_size))

if __name__ == '__main__':
    main()
