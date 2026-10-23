"""Convert a little-endian binary to a full TCM $readmemh image."""
import argparse
from pathlib import Path
p = argparse.ArgumentParser()
p.add_argument("binary")
p.add_argument("hex")
p.add_argument("--words", type=int, default=16384)
a = p.parse_args()
data = Path(a.binary).read_bytes()
if len(data) > a.words * 4:
    raise SystemExit("firmware exceeds TCM")
data = data.ljust(a.words * 4, b"\0")
Path(a.hex).parent.mkdir(parents=True, exist_ok=True)
Path(a.hex).write_text("".join(f"{int.from_bytes(data[n:n+4], 'little'):08x}\n"
                             for n in range(0, len(data), 4)), encoding="ascii")
