"""Decode the archived PDS 2022.2-SP6.1 AG ILA WDF capture into CSV and statistics.

The vendor file is a Qt binary stream. This decoder deliberately supports only
our single 65-channel, 2048-sample profile: each named channel has one QString
of 2048 binary samples. It rejects other layouts instead of guessing.
"""
import argparse
import collections
import csv
import json
import re
import struct
from pathlib import Path


def decode_channels(data, names, depth):
    channels = {}
    for name in names:
        encoded = name.encode('utf-16-be')
        marker = struct.pack('>I', len(encoded)) + encoded
        found = []
        offset = 0
        while (offset := data.find(marker, offset)) >= 0:
            end = offset + len(marker)
            # The channel name is followed by metadata, then a Qt QString.
            for pos in range(end, min(end + 32, len(data) - 4)):
                if data[pos:pos + 4] != struct.pack('>I', depth * 2):
                    continue
                raw = data[pos + 4:pos + 4 + depth * 2]
                if len(raw) != depth * 2:
                    continue
                samples = raw.decode('utf-16-be')
                if len(samples) == depth and set(samples) <= {'0', '1'}:
                    found.append(samples)
            offset = end
        if len(found) != 1:
            raise ValueError(f'{name}: expected one sample array, found {len(found)}')
        channels[name] = tuple(map(int, found[0]))
    return channels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('wdf', type=Path)
    parser.add_argument('--probes', required=True, type=Path)
    parser.add_argument('--disassembly', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--clock-hz', type=int, default=102000000)
    args = parser.parse_args()
    probes = json.loads(args.probes.read_text(encoding='utf-8'))
    names, depth = probes['signals'], probes['depth']
    if len(names) != 65 or depth != 2048 or args.clock_hz <= 0:
        raise ValueError('Only the 65-channel / 2048-point AG capture is supported')
    channels = decode_channels(args.wdf.read_bytes(), names, depth)
    prefix = 'board/system/soc/cpu/'
    functions = [(int(a, 16), n) for a, n in re.findall(
        r'^([0-9a-f]+) <([^>]+)>:', args.disassembly.read_text(), re.M)]
    functions.sort()

    def symbol(pc):
        candidates = [(a, n) for a, n in functions if a <= pc]
        return candidates[-1][1] if candidates else 'unknown'

    rows = []
    for sample in range(depth):
        x_pc = sum(channels[prefix + f'x_pc[{b}]'][sample] << b for b in range(32))
        # m_pc[1:0] were optimized constants. FIC captures only m_pc[31:2].
        m_pc = sum(channels[prefix + f'm_pc[{b}]'][sample] << b for b in range(2, 32))
        first = channels[prefix + 'retire_valid'][sample]
        second = channels[prefix + 'retire_second_valid'][sample]
        branch = channels[prefix + 'x_dec.branch'][sample]
        rows.append(dict(sample=sample, time_ns=sample * 1e9 / args.clock_hz,
                         x_pc=f'{x_pc:08x}', m_pc=f'{m_pc:08x}',
                         retire_valid=first, retire_second_valid=second,
                         x_dec_branch=branch, x_function=symbol(x_pc)))
    if any(r['retire_second_valid'] and not r['retire_valid'] for r in rows):
        raise ValueError('Second retirement without first: check channel decoding')
    first = sum(r['retire_valid'] for r in rows)
    second = sum(r['retire_second_valid'] for r in rows)
    summary = dict(samples=depth, clock_hz=args.clock_hz,
                   nominal_window_us=depth * 1e6 / args.clock_hz,
                   first_retire_cycles=first, dual_retire_cycles=second,
                   no_retire_cycles=depth - first,
                   retired_instructions=first + second,
                   window_ipc=(first + second) / depth,
                   dual_retire_fraction=second / depth,
                   x_branch_flag_cycles=sum(r['x_dec_branch'] for r in rows),
                   x_pc_function_samples=dict(collections.Counter(r['x_function'] for r in rows)),
                   first_sample=rows[0],
                   limits='Immediate JTAG hardware capture; one short window, not whole-run IPC. '
                          'x_pc/m_pc can persist during bubbles; branch flag is not a branch event. '
                          'No dedicated hazard/mispredict/cache probes: zero-retire cycles cannot '
                          'be assigned to individual causes.')
    args.output.mkdir(parents=True, exist_ok=True)
    with (args.output / 'samples.csv').open('w', newline='', encoding='utf-8') as file:
        writer = csv.DictWriter(file, fieldnames=rows[0].keys())
        writer.writeheader()
        writer.writerows(rows)
    (args.output / 'analysis.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))


if __name__ == '__main__':
    main()
