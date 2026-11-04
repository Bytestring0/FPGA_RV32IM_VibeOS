"""Bounded directed integer-head64KiB capacity regression, no full model."""
from pathlib import Path
import subprocess
SYSTEM=Path(__file__).resolve().parents[1];MODEL=SYSTEM/'integration/agent_runtime_20261101/sw/model'
out=SYSTEM/'build/integer_head_sram_20261103';out.mkdir(parents=True,exist_ok=True)
command=['gcc','-O2','-ffunction-sections','-fdata-sections','-Wl,--gc-sections','-DQW_ENABLE_TENSOR_V2','-DQW_PARTIAL_SRAM','-I'+str(MODEL),'-I'+str(SYSTEM/'sw'),str(SYSTEM/'sim/test_integer_head_sram.c'),str(MODEL/'qw_int_head.c'),str(MODEL/'vendor/npu_v1/npu_raw.c'),str(SYSTEM/'sw/npu_fixed_math.c'),'-lm','-o',str(out/'test')]
with (out/'build.log').open('w') as log:subprocess.run(command,stdout=log,stderr=subprocess.STDOUT,check=True,timeout=30)
r=subprocess.run([out/'test'],capture_output=True,text=True,timeout=10);(out/'test.log').write_text(r.stdout+r.stderr);print(r.stdout+r.stderr);r.check_returncode()
