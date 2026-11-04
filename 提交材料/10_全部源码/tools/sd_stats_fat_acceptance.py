#!/usr/bin/env python3
"""One real-SPI fragmented FAT32 read with frozen SD statistics."""
from pathlib import Path
import subprocess
import struct

SYSTEM = Path(__file__).resolve().parents[1]
OUT = SYSTEM / 'build/sd_cold_20261102/stats_fat'

def main():
    OUT.mkdir(parents=True, exist_ok=True)
    fat_size, data_lba = 513, 1058
    image = bytearray((data_lba + 30) * 512)
    image[:3] = b'\xeb\x3c\x90'
    struct.pack_into('<H', image, 11, 512)
    image[13], image[16], image[21] = 4, 2, 0xf8
    struct.pack_into('<H', image, 14, 32)
    struct.pack_into('<II', image, 32, data_lba + 65536 * 4, fat_size)
    struct.pack_into('<IHH', image, 44, 2, 1, 6)
    image[510:512] = b'\x55\xaa'
    for fat_lba in (32, 32 + fat_size):
        for cluster, value in ((0, 0x0ffffff8), (1, 0x0fffffff), (2, 0x0fffffff), (3, 9), (9, 0x0fffffff)):
            struct.pack_into('<I', image, fat_lba * 512 + cluster * 4, value)
    entry = data_lba * 512 + 64
    image[data_lba * 512] = 0xe5
    image[data_lba * 512 + 32] = 0xe5
    image[entry:entry + 11] = b'PARAMS  BIN'
    image[entry + 11] = 0x20
    struct.pack_into('<HI', image, entry + 26, 3, 2685)
    payload = bytes((i * 43 + (i >> 5)) & 255 for i in range(2685))
    image[1062 * 512:1066 * 512] = payload[:2048]
    image[1086 * 512:1086 * 512 + 637] = payload[2048:]
    card_hex = OUT / 'fat_cluster4.hex'
    card_hex.write_text('\n'.join(f'{byte:02x}' for byte in image) + '\n', encoding='ascii')
    build = [
        'verilator', '--cc', '--exe', '--build', '-j', '4', '--top-module', 'sd_spi_test_wrapper',
        '-Wno-fatal', '--Mdir', OUT, '-CFLAGS',
        '-DSD_SPI_TEST_MMIO -DSD_SPI_MULTIBLOCK=1 -DSD_SPI_DATA_DIVIDER=16 -DNPU_FAT32_MULTI_READ=1 -DTEST_FAT32_SPI',
        'rtl/sd_spi_top.sv', 'sim/sd_spi_card_model.sv', 'sim/sd_spi_cold_tb.sv',
        SYSTEM / 'sim/sd_spi_cold_test.cpp', '-o', 'sd_spi_test']
    with (OUT / 'build.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([str(x) for x in build], cwd=SYSTEM, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(result.returncode)
    run = [OUT / 'sd_spi_test', f'+SD_DISK_IMAGE={card_hex}', f'+SD_DISK_BYTES={len(image)}']
    with (OUT / 'result.log').open('w', encoding='utf-8') as log:
        result = subprocess.run([str(x) for x in run], cwd=SYSTEM, stdout=log, stderr=subprocess.STDOUT, text=True)
    output = (OUT / 'result.log').read_text(encoding='utf-8')
    print(output, end='')
    if result.returncode or 'SD_STATS ' not in output or 'PASS actual divider16' not in output:
        raise SystemExit(result.returncode or 1)
    print('PASS_SD_STATS_FAT', OUT)

if __name__ == '__main__':
    main()
