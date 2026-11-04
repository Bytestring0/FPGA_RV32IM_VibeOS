#!/usr/bin/env python3
"""Build real CoreMark in TCM and a DDR resource entry for the existing board."""
import argparse
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[4]
SYSTEM=Path(__file__).resolve().parents[1]

def run(args):
    subprocess.run(list(map(str,args)),check=True)

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--output',required=True,type=Path)
    p.add_argument('--baseline',required=True,type=Path)
    p.add_argument('--entry',type=lambda s:int(s,0),default=0xbf009ab0)
    p.add_argument('--clock-hz',type=int,default=102000000)
    p.add_argument('--iterations',type=int,default=10000)
    a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
    flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-O3','-ffreestanding','-fno-builtin','-msmall-data-limit=0','-mno-relax','-ffunction-sections','-fdata-sections']
    gcc='riscv64-unknown-elf-gcc';objcopy='riscv64-unknown-elf-objcopy'
    src=[ROOT/'third_party/coremark'/name for name in ('core_list_join.c','core_main.c','core_matrix.c','core_state.c','core_util.c')]
    run([gcc,*flags,'-I'+str(ROOT/'third_party/coremark'),'-I'+str(ROOT/'sw/coremark'),'-I'+str(ROOT/'sw'),'-DPERFORMANCE_RUN=1','-DITERATIONS='+str(a.iterations),'-DCPU_HZ='+str(a.clock_hz)+'u','-nostdlib','-nostartfiles','-Wl,--gc-sections','-Wl,--no-relax','-T',ROOT/'sw/link.ld',ROOT/'sw/start.S',*src,ROOT/'sw/coremark/core_portme.c',ROOT/'sw/coremark/ee_printf.c',SYSTEM/'sw/freestanding.c','-lgcc','-o',out/'coremark.elf'])
    run([objcopy,'-O','binary',out/'coremark.elf',out/'coremark.bin'])
    payload=(out/'coremark.bin').read_bytes();(out/'coremark.bin').write_bytes(payload+b'\0'*(-len(payload)%4))
    blob=out/'payload.S';blob.write_text('.section .rodata.tcm_image,"a",@progbits\n.balign 4\n.globl tcm_image_start,tcm_image_end\ntcm_image_start:\n.incbin "'+str(out/'coremark.bin')+'"\ntcm_image_end:\n',encoding='utf-8')
    linker=out/'chain.ld';linker.write_text('OUTPUT_ARCH(riscv)\nENTRY(tcm_chainload)\nSECTIONS { .entry 0xbf000000 : { *(.text.chainload) *(.rodata.tcm_image) } }\n',encoding='utf-8')
    run([gcc,*flags,'-nostdlib','-nostartfiles','-Wl,--no-relax','-T',linker,SYSTEM/'sw/tcm_chainload.S',blob,'-o',out/'chain.elf'])
    run([objcopy,'-O','binary',out/'chain.elf',out/'chain.bin'])
    resources=bytearray((a.baseline/'APPRES.BIN').read_bytes());offset=a.entry-0xbf000000;chain=(out/'chain.bin').read_bytes()
    if offset<0 or offset+8>len(resources) or len(chain)>0x8800:raise ValueError('chain image overlaps required pre-entry code')
    import struct
    delta=0xbf000000-a.entry;upper=(delta+0x800)>>12;lower=delta-(upper<<12)
    trampoline=struct.pack('<II',((upper&0xfffff)<<12)|(5<<7)|0x17,((lower&0xfff)<<20)|(5<<15)|0x67)
    resources[:len(chain)]=chain;resources[offset:offset+8]=trampoline;(out/'APPRES.BIN').write_bytes(resources)
    (out/'FONT.BIN').write_bytes((a.baseline/'FONT.BIN').read_bytes())
    print('COREMARK_CHAIN_BUILD_PASS iterations='+str(a.iterations)+' clock_hz='+str(a.clock_hz)+' execution=TCM benchmark_unchanged=1 board_measured=0')

if __name__=='__main__':main()
