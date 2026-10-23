"""Calculate precise board throughput from captured UART cycles, never integer seconds."""
import argparse
import json
import re
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('log', type=Path)
parser.add_argument('--clock-hz', required=True, type=int,
                    help='Actual verified CPU clock, not a failed timing target')
parser.add_argument('--luts', type=int, help='Implemented LUT count for the same bitstream/system boundary')
args = parser.parse_args()
s = args.log.read_text(encoding='utf-8-sig')
def field(pattern):
    match = re.search(pattern, s)
    if not match:
        raise SystemExit(f'Incomplete log: {pattern}')
    return match[1]
seed = int(field(r'seedcrc\s+:\s+0x([0-9a-fA-F]+)'), 16)
crcs = tuple(int(field(name + r'\s+:\s+0x([0-9a-fA-F]+)'), 16)
             for name in ('crclist', 'crcmatrix', 'crcstate'))
reference = {0xe9f5: (0xe714, 0x1fd7, 0x8e3a), 0x18f2: (0xe3c1, 0x0747, 0x8d84)}
if seed not in reference or crcs != reference[seed] or 'ERROR' in s or 'Correct operation validated' not in s:
    raise SystemExit('CoreMark CRC/duration validation failed')
cycles = int(field(r'MEASURED_CYCLES_HEX=([0-9a-fA-F]{16})'), 16)
iterations = int(field(r'Iterations\s+:\s+(\d+)'))
if args.clock_hz <= 0 or cycles <= 0 or iterations <= 0 or (args.luts is not None and args.luts <= 0):
    raise SystemExit('Clock, cycles, iterations and LUTs must be positive')
seconds = cycles / args.clock_hz
if seconds < 10:
    raise SystemExit(f'Run too short at the supplied actual clock: {seconds:.6f} seconds')
result = dict(iterations=iterations, measured_cycles=cycles, supplied_clock_hz=args.clock_hz,
              seconds=seconds, coremark=iterations/seconds,
              coremark_per_mhz=iterations*1e6/cycles, crcs_valid=True)
if args.luts:
    result.update(implemented_luts=args.luts, coremark_per_lut=result['coremark']/args.luts)
print(json.dumps(result, indent=2))
print('Clock and bitstream provenance must be verified separately; this parser does not measure frequency.')
