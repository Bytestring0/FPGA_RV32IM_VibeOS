"""Cross-check archived decoder output and reject a damaged channel name."""
from pathlib import Path
import sys

archive = Path(__file__).resolve().parent.parent
root = archive.parents[2]
sys.path.insert(0, str(root / 'scripts'))
from analyze_ila_wdf import decode_channels

name = 'board/system/soc/cpu/x_pc[0]'
data = (archive / 'ag-coremark-ila.wdf').read_bytes()
assert decode_channels(data, [name], 2048)[name] == (0,) * 2048
encoded = name.encode('utf-16-be')
marker = len(encoded).to_bytes(4, 'big') + encoded
offset = data.index(marker)
damaged = data[:offset] + data[offset:offset + len(marker)].replace(b'x', b'q') + data[offset + len(marker):]
try:
    decode_channels(damaged, [name], 2048)
except ValueError:
    print('PASS: corrupted/missing channel is rejected')
else:
    raise AssertionError('Invalid capture accepted')
print('PASS: constant PC alignment channel has 2048 zero samples')
