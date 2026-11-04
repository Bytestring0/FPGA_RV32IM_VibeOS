#!/usr/bin/env python3
"""Build a data-checked firmware replacement for an existing UART asset loader."""
import argparse
from pathlib import Path
import subprocess
import struct

SYSTEM=Path(__file__).resolve().parents[1]
def run(args):subprocess.run(list(map(str,args)),check=True)

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--baseline',type=Path,required=True)
 p.add_argument('--firmware',type=Path,required=True)
 p.add_argument('--output',type=Path,required=True)
 p.add_argument('--entry',type=lambda x:int(x,0),default=0xbf009a30)
 p.add_argument('--font-offset',type=lambda x:int(x,0),default=0x89e8)
 p.add_argument('--cache-ranges',type=lambda x:int(x,0),default=0x17a0)
 a=p.parse_args();out=a.output.resolve();out.mkdir(parents=True,exist_ok=True)
 image=a.firmware.read_bytes()
 if not image or len(image)>65536:raise ValueError('TCM image must be 1..65536 bytes')
 (out/'image.bin').write_bytes(image+b'\0'*(-len(image)%4))
 blob=out/'image.S';blob.write_text('.section .rodata.tcm_image,"a",@progbits\n.balign 4\n.globl tcm_image_start,tcm_image_end\ntcm_image_start:\n.incbin "'+str(out/'image.bin')+'"\ntcm_image_end:\n')
 stage2=out/'stage2.ld';stage2.write_text('OUTPUT_ARCH(riscv)\nENTRY(tcm_chainload)\nSECTIONS { .image 0x80010000 : { *(.text.chainload) *(.rodata.tcm_image) } }\n')
 flags=['-march=rv32im_zicsr_zifencei','-mabi=ilp32','-Os','-ffreestanding','-fno-builtin','-fno-pic','-msmall-data-limit=0','-mno-relax','-nostdlib','-nostartfiles','-Wl,--no-relax']
 run(['riscv64-unknown-elf-gcc',*flags,'-T',stage2,SYSTEM/'sw/tcm_chainload.S',blob,'-o',out/'stage2.elf'])
 run(['riscv64-unknown-elf-objcopy','-O','binary',out/'stage2.elf',out/'stage2.bin'])
 font=(out/'stage2.bin').read_bytes();(out/'FONT.BIN').write_bytes(font.ljust(1048576,b'\0'))
 stage1=out/'stage1.ld';stage1.write_text('OUTPUT_ARCH(riscv)\nENTRY(uart_tcm_handoff)\nSECTIONS { .entry '+hex(a.entry)+' : { *(.text.entry) *(.text*) *(.rodata*) } }\n')
 run(['riscv64-unknown-elf-gcc',*flags,'-DHANDOFF_FONT_OFFSET='+hex(a.font_offset),'-DHANDOFF_CACHE_RANGES='+hex(a.cache_ranges),'-T',stage1,SYSTEM/'sw/uart_tcm_handoff.c','-o',out/'stage1.elf'])
 run(['riscv64-unknown-elf-objcopy','-O','binary',out/'stage1.elf',out/'stage1.bin'])
 resources=bytearray((a.baseline/'APPRES.BIN').read_bytes());entry=(out/'stage1.bin').read_bytes();offset=a.entry-0xbf000000
 if len(entry)>1024 or offset<0 or offset+len(entry)>len(resources):raise ValueError('handoff entry exceeds reserved callback span')
 resources[offset:offset+len(entry)]=entry;(out/'APPRES.BIN').write_bytes(resources)
 print('UART_TCM_HANDOFF_BUILD_PASS image_bytes='+str(len(image))+' entry_bytes='+str(len(entry))+' resource_bytes='+str(len(resources))+' routing_changes=0 board_verified=0')
if __name__=='__main__':main()
