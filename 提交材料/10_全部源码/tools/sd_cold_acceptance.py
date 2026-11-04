#!/usr/bin/env python3
"""One SD-only acceptance batch: real C -> MMIO -> SPI RTL -> card waveform.
No board connection, media write, firmware execution or long model run.
Use --case solely to repair/recheck a failed case; reports never merge old PASSes.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import time
import sys

SYSTEM = Path(__file__).resolve().parents[1]
OUT = SYSTEM / 'build/sd_cold_20261102'
SOURCES = ['sw/sd_card.c', 'sw/sd_card.h', 'rtl/sd_spi_top.sv',
           'sim/sd_spi_card_model.sv', 'sim/sd_spi_cold_test.cpp',
           'sim/sd_spi_cold_tb.sv', 'tools/sd_cold_acceptance.py']
VENDOR = 'integration/agent_runtime_20261101/sw/model/vendor/npu_v1/'
CASES = [
    ('hw_cmd17', [], ['-DSD_SPI_MULTIBLOCK=0'], []),
    ('hw_multi', [], [], []),
    ('hw_sdsc', ['-GSDHC=0'], [], []),
    ('legacy_auto', ['-GENABLE_CRC16=0'], [], []),
    ('forced_software', [], ['-DSD_SPI_HW_CRC=0', '-DNPU_FAST_CRC=1'], []),
    ('hw_crc_error', ['-GCORRUPT_CRC=1'], ['-DEXPECT_CRC_ERROR'], []),
    ('sw_crc_error', ['-GENABLE_CRC16=0', '-GCORRUPT_CRC=1'], ['-DEXPECT_CRC_ERROR'], []),
    ('hw_token_timeout', ['-GSTALL_READ=1'], ['-DEXPECT_TOKEN_TIMEOUT'], []),
    ('hw_stop_busy', [], ['-DEXPECT_STOP_BUSY_TIMEOUT'], ['+SD_STOP_BUSY_BYTES=-1']),
    ('hw_invalid_valid', [], ['-DEXPECT_INVALID_CRC_VALID'], []),
    ('reject_cmd18', ['-GREJECT_CMD18=1'], ['-DEXPECT_REJECT_CMD18=1'], []),
    ('reject_busy_timeout', ['-GREJECT_CMD18=1'], ['-DEXPECT_REJECT_CMD18=1', '-DEXPECT_REJECT_BUSY_TIMEOUT'], ['+SD_STOP_BUSY_BYTES=-1']),
    ('hw_div16', [], ['-DSD_SPI_DATA_DIVIDER=16'], []),
]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log, timeout=240):
    started = time.monotonic()
    result = subprocess.run([str(a) for a in command], cwd=SYSTEM,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace', timeout=timeout)
    log.write_text(result.stdout, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'{log}: exit {result.returncode}\n{result.stdout[-3000:]}')
    return result.stdout, time.monotonic() - started


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--case', choices=[c[0] for c in CASES])
    parser.add_argument('--repair-built', action='store_true', help='run executables already linked in a failed build batch; verify source identity first')
    args = parser.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    source_hashes = {name: sha(SYSTEM / name) for name in SOURCES}
    protected = ['sw/npu_package.c', VENDOR + 'npu_package.c']
    report = {'passed': False, 'scope': 'SD_ONLY_REAL_SPI_RTL_HOST_DRIVER',
              'board_verified': False, 'native_4bit': False, 'dma': False,
              'source_sha256': source_hashes, 'cases': {},
              'protected_crc32_sha256': {name: sha(SYSTEM / name) for name in protected}}
    selected = [c for c in CASES if not args.case or c[0] == args.case]
    if args.repair_built:
        earlier = json.loads((OUT / 'report.json').read_text())
        for name, digest in earlier['source_sha256'].items():
            if name != 'tools/sd_cold_acceptance.py' and source_hashes[name] != digest:
                raise RuntimeError('cannot reuse built executable after source change: ' + name)
        selected = [c for c in selected if not earlier['cases'].get(c[0], {}).get('passed', False)]
        report['repair_of'] = 'report.json'
        report['initial_report_sha256'] = sha(OUT / 'report.json')
    target_report = OUT / ('repair_report.json' if args.repair_built else f'repair_{args.case}.json' if args.case else 'report.json')
    failures = []
    try:
        for name in ('sd_card.c', 'sd_card.h'):
            if sha(SYSTEM / ('sw/' + name)) != sha(SYSTEM / (VENDOR + name)):
                raise RuntimeError('canonical/vendor SD source identity mismatch: ' + name)
        report['vendor_identity_matches'] = True
        cc = ['gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-pedantic',
              '-ffreestanding', '-fno-builtin', '-I', SYSTEM / 'sw']
        for label, flags in [('default', []), ('reference', ['-DSD_SPI_HW_CRC=0']),
                             ('multi', ['-DSD_SPI_MULTIBLOCK=1', '-DNPU_FAST_CRC=1'])]:
            run([*cc, *flags, '-c', 'sw/sd_card.c', '-o', OUT / ('sd_driver_' + label + '.o')], OUT / (label + '_compile.log'))
        run(['iverilog', '-g2012', '-s', 'tb_sd_spi', '-o', OUT / 'tb.vvp',
             'rtl/sd_spi_top.sv', 'sim/sd_spi_card_model.sv', 'sim/tb_sd_spi.sv'], OUT / 'rtl_compile.log')
        run(['vvp', OUT / 'tb.vvp'], OUT / 'rtl_run.log')
        # Existing independent CRC16/CRC32 bit-vs-table test; six local host builds.
        run([sys.executable, SYSTEM/'tools/run_crc.py', '--output', OUT/'crc_reference'], OUT / 'crc_reference.log')
        for label, params, flags, runtime in selected:
            folder = OUT / label
            folder.mkdir(exist_ok=True)
            cflags = ' '.join(['-DSD_SPI_TEST_MMIO', '-DSD_SPI_MULTIBLOCK=1', *flags])
            # Later -D wins for CMD17-only case, avoid a redefinition warning.
            if '-DSD_SPI_MULTIBLOCK=0' in flags:
                cflags = cflags.replace('-DSD_SPI_MULTIBLOCK=1 ', '')
            try:
                if args.repair_built:
                    if not (folder / 'sd_spi_test').is_file():
                        raise RuntimeError('initial failed build did not link executable')
                    build_seconds = 0
                else:
                    _, build_seconds = run(['verilator', '--cc', '--exe', '--build', '-j', '4',
                        '--top-module', 'sd_spi_test_wrapper', '-Wno-fatal', '--Mdir', folder,
                        '-CFLAGS', cflags, '-o', 'sd_spi_test', *params, 'rtl/sd_spi_top.sv',
                        'sim/sd_spi_card_model.sv', 'sim/sd_spi_cold_tb.sv',
                        SYSTEM / 'sim/sd_spi_cold_test.cpp'], folder / 'build.log')
                text, seconds = run([folder / 'sd_spi_test', *runtime], folder / 'result.log')
                if 'PASS ' not in text:
                    raise RuntimeError('missing explicit PASS')
                report['cases'][label] = {'passed': True, 'build_seconds': build_seconds,
                    'host_test_seconds': seconds,
                    'command': {'params': params, 'cflags': cflags, 'runtime': runtime},
                    'measurements': [json.loads(line.split(' ', 1)[1]) for line in text.splitlines()
                                     if line.startswith(('SD_TRANSPORT ', 'SD_CRC '))]}
                print('PASS_SD_COLD', label, flush=True)
            except Exception as exc:
                report['cases'][label] = {'passed': False, 'failure': str(exc)}
                failures.append(label)
                print('FAIL_SD_COLD', label, str(exc), flush=True)
        report['unchanged_during_batch'] = source_hashes == {name: sha(SYSTEM / name) for name in SOURCES}
        report['crc32_unchanged'] = report['protected_crc32_sha256'] == {name: sha(SYSTEM / name) for name in protected}
        report['passed'] = not failures and report['unchanged_during_batch'] and report['crc32_unchanged']
        if not report['passed']:
            raise RuntimeError('failed cases: ' + ', '.join(failures))
    except BaseException as exc:
        report['failure'] = repr(exc)
        raise
    finally:
        target_report.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print('PASS_SD_COLD_SUITE', target_report, flush=True)


if __name__ == '__main__':
    main()
