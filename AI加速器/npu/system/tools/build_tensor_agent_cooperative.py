"""Build-only real tensor-v2 cooperative Agent firmware; never program or run it.
Keeps all model/Agent checks and the complete canonical R4 prompt. Optional
--ddr-resources relocates only immutable profile/LUT data, with a real SD loader.
"""
from __future__ import annotations
import argparse, hashlib, json, re, sys
from pathlib import Path
from deployment import PINS, deployment_header, command, verify_arch, sha, save
from model_assets import TOKENIZER
SYSTEM=Path(__file__).resolve().parents[1];WORK=SYSTEM.parents[2];CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK
RUNTIME=SYSTEM/'integration/agent_runtime_20261101';MODEL=RUNTIME/'sw/model';AGENT=RUNTIME/'sw/agent';VENDOR=MODEL/'vendor/npu_v1'
PROFILE=SYSTEM/'integration/agent_protocol_20261102/analysis/protocol-r4-original-system.txt'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--toolchain',required=True);p.add_argument('--output',required=True)
 p.add_argument('--ddr-resources',action='store_true');p.add_argument('--sd-multiblock',action='store_true');p.add_argument('--ui-source',type=Path)
 p.add_argument('--task',default='计算7*8保存');a=p.parse_args()
 if sys.platform!='linux':p.error('run in WSL/Linux')
 out=Path(a.output).resolve();prefix=str(Path(a.toolchain).resolve()/'bin/riscv-none-elf-')
 if out.exists() or not out.is_relative_to(SYSTEM/'build'):p.error('fresh output under system/build required')
 if not Path(prefix+'gcc').is_file():p.error('missing toolchain')
 if not a.task or '\0' in a.task or '<|' in a.task or len(a.task.encode())>512:p.error('invalid task')
 # Build consumes pinned metadata only. The real loader verifies TOKEN.BIN on SD.
 tokenbytes,tokensha=TOKENIZER['bytes'],TOKENIZER['sha256']
 profile=PROFILE.read_bytes()
 if b'\r' in profile or b'\0' in profile or not profile:p.error('canonical profile must retain exact LF UTF8 bytes')
 text=profile.decode('utf-8')
 out.mkdir(parents=True)
 (out/'npu_opt_deployment.h').write_text(deployment_header(),encoding='ascii')
 (out/'agent_tool_profile.h').write_text('/* Complete byte-exact canonical R4; no generated answer. */\nstatic const char qw_agent_profile[] __attribute__((section(".agent_profile"),used)) = '+json.dumps(text,ensure_ascii=False)+';\n',encoding='utf-8')
 (out/'tensor_agent_deployment.h').write_text('#define AGENT_TOKEN_BYTES '+str(tokenbytes)+'u\n#define AGENT_TIMEOUT_MS 259200000u\n#define AGENT_TASK_TEXT '+json.dumps(a.task,ensure_ascii=False)+'\nstatic const char agent_token_name[11] = {'+','.join(str(ord(c)) for c in 'TOKEN   BIN')+'};\nstatic const char agent_resource_name[11] = {'+','.join(str(ord(c)) for c in 'APPRES  BIN')+'};\nstatic const unsigned char agent_token_sha[32] = {'+','.join('0x'+tokensha[i:i+2] for i in range(0,64,2))+'};\n',encoding='utf-8')
 # Keep immutable numeric tables intact; section placement is the only change.
 ld=(SYSTEM/'sw/link_agent_128k.ld').read_text()
 ld=ld.replace('LENGTH = 128K }','LENGTH = 128K\n APPDDR (r) : ORIGIN = 0xbf000000, LENGTH = 1M }')
 target='APPDDR' if a.ddr_resources else 'RAM'
 resources='    .ddr_resources '+('0xbf000000 ' if a.ddr_resources else '')+': { __agent_resource_start = .; KEEP(*(.agent_profile)) *(.rodata.npu_fixed_lut_*) __agent_resource_end = .; } > '+target+'\n'
 ld=ld.replace('    .rodata :',resources+'    .rodata :').replace('*(.rodata .rodata.*)','*(.resource_pin) *(.rodata .rodata.*)')
 ld=ld.replace('    __stack_top =','    ASSERT(SIZEOF(.ddr_resources) <= 1048576, "App resource reserve overflow")\n    __stack_top =')
 linker=out/'link_tensor_agent.ld';linker.write_text(ld)
 board=SYSTEM/'sw/tensor_agent_cooperative_board.c'
 sources=[board,SYSTEM/'sw/agent_target_support.c',*sorted(MODEL.glob('qw_*.c')),
 *[AGENT/n for n in ('ag_agent.c','ag_loop.c','qw_tool_normalize.c','qw_agent_protocol_backend.c')],
 *[VENDOR/n for n in ('npu_raw.c','npu_raw_reference.c','npu_runtime.c','npu_package.c','npu_storage.c','npu_ag_platform.c','sd_card.c','npu_fat32.c','freestanding.c')],
 *[SYSTEM/'sw'/n for n in ('npu_fixed_math.c','npu_tensor_reference.c','npu_tensor_driver.c','npu_tensor_ag.c')]]
 if a.ui_source:sources.append(a.ui_source.resolve())
 headers=[*MODEL.glob('*.h'),*AGENT.glob('*.h'),*VENDOR.glob('*.h'),*SYSTEM.joinpath('sw').glob('npu_*.h'),SYSTEM/'sw/agent_target_support.h',SYSTEM/'sw/tensor_agent_cooperative_board.h']
 inputs=list(dict.fromkeys([CPU/'sw/start.S',SYSTEM/'sw/link_agent_128k.ld',PROFILE,Path(__file__),SYSTEM/'tools/deployment.py',SYSTEM/'tools/model_assets.py',*sources,*headers]))
 identity={str(x):sha(x) for x in inputs}
 report={'scope':'real_tensor_v2_cooperative_agent_firmware_build_only','passed':False,'build_only':True,'board_verified':False,'board_programmed':False,'sd_written':False,'quality_approved':False,'latency_bound_verified':False,'ui_implemented':False,'ui_source_supplied':bool(a.ui_source),'cold_storage_stats_instrumented':True,'warm_no_sd_read_assertion':True,'warm_no_sd_read_verified':False,'observed_max_step_ticks_measured':False,'external_ui_hook_default':0,'tool_protocol':'npu_agent_tool_protocol_v1','persistence':'volatile','task':a.task,'context_tokens':2048,'prompt_bytes':len(profile),'prompt_sha256':hashlib.sha256(profile).hexdigest(),'prompt_truncated':False,'ddr_resources':a.ddr_resources,'app_ddr_base':'0xbe000000','app_ddr_limit':'0xbf000000','resource_ddr_base':'0xbf000000','resource_ddr_capacity':1048576,'arena_base':'0x81000000','arena_capacity':0x3d000000,'model_deployment':PINS,'tokenizer_verified_at_build':False,'tokenizer':{'bytes':tokenbytes,'sha256':tokensha,'pin_source':str(SYSTEM/'tools/model_assets.py')},'source_sha256':identity,'tcm_bytes':131072,'stack_reserved_bytes':8192,'host_inference_runs':0,'board_runs':0}
 save(out/'report.json',report)
 elf=out/'firmware.elf';mapping=out/'firmware.map';binary=out/'firmware.bin';resource=out/'APPRES.BIN'
 flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-flto','-std=c11','-ffreestanding','-fno-builtin','-fno-pic','-msmall-data-limit=0','-mno-relax','-ffunction-sections','-fdata-sections','-ffp-contract=off','-fno-fast-math','-fstack-usage','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DQW_FREESTANDING','-DQW_ENABLE_TENSOR_V2','-DQW_CONTEXT_MAX=2048u','-DQW_AGENT_PROMPT_MAX=8192u','-DNPU_FAST_CRC=1','-DSD_SPI_CLOCK_HZ=81000000u','-DSD_SPI_DATA_DIVIDER=16u','-include',SYSTEM/'sw/agent_target_support.h','-I',MODEL,'-I',AGENT,'-I',VENDOR,'-I',MODEL/'freestanding_include','-I',SYSTEM/'sw','-I',out,'-nostdlib','-nostartfiles','-Wl,--gc-sections','-Wl,--no-relax','-Wl,--undefined=memcpy','-Wl,--undefined=memset','-Wl,-Map,'+str(mapping),'-T',linker]
 if a.sd_multiblock:flags+=['-DSD_SPI_MULTIBLOCK=1','-DNPU_FAT32_MULTI_READ=1']
 def pin(digest):
  path=out/'resource_pin.c';path.write_text('const unsigned char agent_resource_sha[32] __attribute__((section(".resource_pin"))) = {'+','.join('0x'+digest[i:i+2] for i in range(0,64,2))+'};\n')
  # Deliberately non-LTO: placeholder bytes cannot constant-fold the loader.
  command([prefix+'gcc','-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-ffreestanding','-c',path,'-o',out/'resource_pin.o'],out,'resource_pin_build')
 try:
  pin('00'*32)
  command([prefix+'gcc',*flags,CPU/'sw/start.S',*sources,out/'resource_pin.o','-lgcc','-o',elf],out,'layout_compile')
  command([prefix+'objcopy','-O','binary','--only-section=.ddr_resources',elf,resource],out,'resource_extract')
  digest=sha(resource);size=resource.stat().st_size
  if not 0<size<=1048576:raise RuntimeError('invalid immutable resource size')
  report['resource']={'short_name':'APPRES  BIN','bytes':size,'sha256':digest,'required_on_sd':True,'contains':'complete canonical R4 and unchanged referenced fixed-point LUTs'}
  pin(digest)
  command([prefix+'gcc',*flags,CPU/'sw/start.S',*sources,out/'resource_pin.o','-lgcc','-o',elf],out,'final_compile')
  check=out/'resource_final.bin';command([prefix+'objcopy','-O','binary','--only-section=.ddr_resources',elf,check],out,'resource_recheck')
  if sha(check)!=digest:raise RuntimeError('resource bytes changed after SHA pin final link')
  maptext=mapping.read_text();match=re.search(r'^\s*(0x[0-9a-fA-F]+)\s+__bss_end\s*=',maptext,re.M)
  if not match:raise RuntimeError('TCM bound missing')
  used=int(match[1],16);report.update(static_tcm_bytes=used,headroom_before_stack_bytes=131072-8192-used)
  if used>=131072-8192:raise RuntimeError('TCM stack reserve overflow')
  command([prefix+'nm','-u',elf],out,'undefined')
  if(out/'undefined.log').read_text().strip():raise RuntimeError('unresolved firmware symbol')
  command([prefix+'nm','-S','--size-sort',elf],out,'symbols_sizes');command([prefix+'size','-A',elf],out,'size')
  command([prefix+'readelf','-A',elf],out,'attributes');command([prefix+'readelf','-h',elf],out,'elf_header');report['actual_isa']=verify_arch(out)
  drop=['--remove-section=.ddr_resources'] if a.ddr_resources else []
  command([prefix+'objcopy','-O','binary',*drop,elf,binary],out,'objcopy')
  command([sys.executable,CPU/'scripts/bin2hex.py',binary,out/'firmware.hex','--words','32768'],out,'hex')
  if len((out/'firmware.hex').read_text().split())!=32768:raise RuntimeError('TCM HEX length invalid')
  # Combined real-model/real-Agent host link only. Never execute the result.
  host=out/'host_combined.c';host.write_text('#include "qw_agent_protocol_backend.h"\n#include "qw_tensor_async.h"\nint main(void){ ag_backend_poll_fn volatile poll=qw_agent_protocol_poll; int (*volatile begin)(qw_tensor_task*,qw_model*,uint32_t,int)=qw_tensor_task_begin; return !poll || !begin; }\n')
  hostflags=['gcc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DQW_ENABLE_TENSOR_V2','-DQW_CONTEXT_MAX=2048u','-DQW_AGENT_PROMPT_MAX=8192u','-ffp-contract=off','-fno-fast-math','-I'+str(MODEL),'-I'+str(AGENT),'-I'+str(VENDOR),'-I'+str(SYSTEM/'sw'),'-I'+str(out)]
  command([*hostflags,'-c',board,'-o',out/'host_board_compile.o'],out,'host_board_compile')
  hostsrc=[x for x in sources if x!=board and x.name not in ('freestanding.c','agent_target_support.c')]
  command([*hostflags,*hostsrc,host,'-lm','-o',out/'host_combined_build_only'],out,'host_combined_build_only')
  report['host_combined_build_only']=True
  if any(sha(Path(x))!=value for x,value in identity.items()):raise RuntimeError('source changed during build')
  report.update(passed=True,artifacts={x.name:{'bytes':x.stat().st_size,'sha256':sha(x)} for x in (elf,binary,out/'firmware.hex',mapping,resource,out/'host_combined_build_only')})
  print('TENSOR_AGENT_COOPERATIVE_FIRMWARE_BUILD_PASS',out,'static_tcm_bytes',used,'quality_approved=false board_runs=0 host_inference_runs=0',flush=True)
 except BaseException as error:
  report['failure']=repr(error)
  if mapping.exists():report['failed_map']=str(mapping)
  raise
 finally:save(out/'report.json',report)
if __name__=='__main__':main()
