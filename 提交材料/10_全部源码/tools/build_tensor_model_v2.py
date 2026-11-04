"""Build the real RV32 tensor-v2 full-model firmware, no programming or SD writes.
The expected hashes come from an actual same-policy model run, not canned
answers, and only validate results after real hardware computation.
"""
import argparse,hashlib,json,re,sys
from pathlib import Path
from deployment import PINS,deployment_header,command,verify_arch,sha,save
SYSTEM=Path(__file__).resolve().parents[1]
WORK=SYSTEM.parents[2];CPU=WORK/'CPU' if (WORK/'CPU/rtl/rv32_core.sv').is_file() else WORK
MODEL=SYSTEM/'integration/agent_runtime_20261101/sw/model';VENDOR=MODEL/'vendor/npu_v1'
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--toolchain',required=True)
 p.add_argument('--output',required=True);p.add_argument('--expected',required=True)
 p.add_argument('--sd-multiblock',action='store_true');a=p.parse_args()
 out=Path(a.output).resolve();expected=Path(a.expected).resolve()
 if sys.platform!='linux':p.error('run in WSL/Linux')
 if out.exists() or not out.is_relative_to(SYSTEM/'build'):p.error('fresh output under system/build required')
 result=json.loads(expected.read_text())
 if not result.get('execution_passed') or result.get('numeric_version')!=0x20000 or not 1<=len(result.get('actual_tokens',[]))<=64:p.error('actual tensor-v2 model result with token/logits hashes required')
 prefix=str(Path(a.toolchain).resolve()/'bin/riscv-none-elf-')
 if not Path(prefix+'gcc').is_file():p.error('missing toolchain')
 out.mkdir(parents=True)
 generated=out/'npu_opt_deployment.h';generated.write_text(deployment_header(),encoding='ascii')
 golden=out/'tensor_expected.h';records=result['actual_tokens']
 golden.write_text('/* Actual model observations; execution golden, not quality approval. */\n#define TENSOR_EXPECTED_COUNT '+str(len(records))+'u\nstatic const uint32_t tensor_expected_token[] = {'+','.join(str(r['sampled_token'])+'u' for r in records)+'};\nstatic const uint32_t tensor_expected_hash[] = {'+','.join(str(r['logits_fnv1a32'])+'u' for r in records)+'};\n',encoding='ascii')
 src=[CPU/'sw/start.S',SYSTEM/'sw/tensor_model_board.c',*sorted(MODEL.glob('qw_*.c')),
 *[VENDOR/n for n in ('npu_raw.c','npu_raw_reference.c','npu_runtime.c','npu_package.c','npu_storage.c','npu_ag_platform.c','sd_card.c','npu_fat32.c','freestanding.c')],
 SYSTEM/'sw/npu_fixed_math.c',SYSTEM/'sw/npu_tensor_reference.c',SYSTEM/'sw/npu_tensor_driver.c',SYSTEM/'sw/npu_tensor_ag.c']
 headers=[*MODEL.glob('*.h'),*VENDOR.glob('*.h'),*SYSTEM.joinpath('sw').glob('npu_tensor*.h'),generated,golden]
 identity={str(x):sha(x) for x in [*src,*headers,SYSTEM/'sw/link_agent_128k.ld',Path(__file__)]}
 report={'scope':'actual_full_model_tensor_v2_firmware_build','passed':False,'quality_approved':False,'board_verified':False,
 'source_sha256':identity,'expected_source':str(expected),'expected_sha256':sha(expected),'sd_multiblock':a.sd_multiblock,
 'clock_hz':81000000,'tcm_bytes':131072,'stack_reserve_bytes':8192}
 save(out/'report.json',report)
 elf=out/'firmware.elf';binary=out/'firmware.bin';mapping=out/'firmware.map'
 flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-flto','-std=c11','-ffreestanding','-fno-builtin','-fno-pic',
 '-msmall-data-limit=0','-mno-relax','-ffunction-sections','-fdata-sections','-ffp-contract=off','-fno-fast-math',
 '-fstack-usage','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-DQW_FREESTANDING','-DQW_ENABLE_TENSOR_V2',
 '-DQW_CONTEXT_MAX=2048u','-DNPU_FAST_CRC=1','-DSD_SPI_CLOCK_HZ=81000000u','-DSD_SPI_DATA_DIVIDER=16u',
 '-I',str(MODEL),'-I',str(VENDOR),'-I',str(MODEL/'freestanding_include'),'-I',str(SYSTEM/'sw'),'-I',str(out),
 '-nostdlib','-nostartfiles','-Wl,--gc-sections','-Wl,--no-relax','-Wl,--undefined=memcpy','-Wl,--undefined=memset','-Wl,-Map,'+str(mapping),'-T',str(SYSTEM/'sw/link_agent_128k.ld')]
 if a.sd_multiblock:flags+=['-DSD_SPI_MULTIBLOCK=1','-DNPU_FAT32_MULTI_READ=1']
 try:
  command([prefix+'gcc',*flags,*src,'-lgcc','-o',elf],out,'compile')
  match=re.search(r'^\s*(0x[0-9a-fA-F]+)\s+__bss_end\s*=',mapping.read_text(),re.M)
  if not match:raise RuntimeError('TCM map missing')
  used=int(match[1],16);report['static_tcm_bytes']=used
  if used>=131072-8192:raise RuntimeError('TCM static/stack reserve overflow')
  command([prefix+'nm','-u',elf],out,'undefined')
  if (out/'undefined.log').read_text().strip():raise RuntimeError('unresolved target symbols')
  command([prefix+'readelf','-A',elf],out,'attributes');command([prefix+'readelf','-h',elf],out,'elf_header')
  report['actual_isa']=verify_arch(out)
  command([prefix+'objcopy','-O','binary',elf,binary],out,'objcopy')
  command([sys.executable,CPU/'scripts/bin2hex.py',binary,out/'firmware.hex','--words','32768'],out,'hex')
  if len((out/'firmware.hex').read_text().split())!=32768:raise RuntimeError('wrong TCM image length')
  if any(sha(Path(x))!=value for x,value in identity.items()):raise RuntimeError('source changed during build')
  report.update(passed=True,artifacts={x.name:{'sha256':sha(x),'bytes':x.stat().st_size} for x in (elf,binary,out/'firmware.hex',mapping)})
  print('TENSOR_MODEL_FIRMWARE_PASS',out,'static_tcm_bytes',used,'board_verified=false quality_approved=false',flush=True)
 except BaseException as error:report['failure']=repr(error);raise
 finally:save(out/'report.json',report)
if __name__=='__main__':main()
