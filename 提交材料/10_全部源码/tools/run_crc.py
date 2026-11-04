"""Portable current CRC16/CRC32 bit/table equivalence batch; no media IO."""
import argparse
from pathlib import Path
import subprocess

SYSTEM=Path(__file__).resolve().parents[1]

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    args=parser.parse_args();out=args.output.resolve()
    if out.exists() or not out.is_relative_to(SYSTEM/'build'):
        parser.error('fresh output under system/build required')
    out.mkdir(parents=True)
    for vendor in (0,1):
        for mode in (None,0,1):
            label=f'{vendor}_{mode}';exe=out/('crc_'+label)
            flags=[f'-DCRC_CHECK_VENDOR={vendor}']
            if mode is not None:flags.append(f'-DNPU_FAST_CRC={mode}')
            with (out/(label+'.log')).open('w') as log:
                subprocess.run(['gcc','-std=c99','-O2','-Wall','-Wextra','-Werror',
                                *flags,str(SYSTEM/'sim/test_crc.c'),'-o',str(exe)],
                               stdout=log,stderr=subprocess.STDOUT,check=True)
                subprocess.run([str(exe)],stdout=log,stderr=subprocess.STDOUT,check=True)
    print('PASS_CRC_CANONICAL_VENDOR modes=default,0,1')

if __name__=='__main__':main()
