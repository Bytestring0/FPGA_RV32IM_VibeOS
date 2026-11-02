#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
: "${1:?Usage: bash scripts/build_agent_accel.sh /mounted/E/artifact/root}"
OUT=$(mkdir -p -- "$1" && cd -- "$1" && pwd)
case "$OUT" in /mnt/e/*) ;; *) echo 'All artifacts must be under the designated E mount' >&2; exit 2;; esac
export TMPDIR="$OUT/tmp" PYTHONDONTWRITEBYTECODE=1
mkdir -p "$TMPDIR" "$OUT/snapshots" "$OUT/builds"
SNAP=$(python3 -B - "$ROOT" "$OUT" <<'PY'
import hashlib,json,pathlib,shutil,subprocess,sys
root,out=map(pathlib.Path,sys.argv[1:])
paths=list((root/'sw/model').rglob('*.c'))+list((root/'sw/model').rglob('*.h'))
paths+=list((root/'sw/agent').glob('*.c'))+list((root/'sw/agent').glob('*.h'))
paths+=[root/'sw/platform.h',root/'scripts/build_agent_accel.sh']
paths+=list((root/'tests').glob('model_*.c'))+list((root/'tests').glob('agent_loop*.c'))
paths.append(root/'tests/agent_qw_backend_test.c')
paths.append(root/'tests/agent_qw_real_host.c')
paths += [root/name for name in ('scripts/agent_integer_reference.py','scripts/agent_integer_graph_ops.py','scripts/agent_integer_model.py','scripts/export_agent_integer_scales.py','scripts/agent_bf16_eval.py','scripts/agent_eval.py','tests/test_agent_integer_reference.py','tests/test_agent_integer_graph_ops.py','tests/test_agent_integer_model.py','tests/test_export_agent_integer_scales.py','tests/test_agent_bf16_eval.py','tests/agent_bf16_tool_broker.c','tests/test_agent_eval.py','tests/agent_eval_tool_probe.c') if (root/name).is_file()]
entries=[]
for p in sorted(set(paths)):
 data=p.read_bytes();entries.append({'path':p.relative_to(root).as_posix(),'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
identity=hashlib.sha256(json.dumps(entries,sort_keys=True,separators=(',',':')).encode()).hexdigest()
snap=out/'snapshots'/identity
snap.mkdir(exist_ok=True)
for e in entries:
 src=root/e['path'];dst=snap/e['path'];dst.parent.mkdir(parents=True,exist_ok=True)
 if not dst.exists():shutil.copyfile(src,dst)
 if hashlib.sha256(dst.read_bytes()).hexdigest()!=e['sha256'] or hashlib.sha256(src.read_bytes()).hexdigest()!=e['sha256']:raise SystemExit('source changed during snapshot: '+e['path'])
 dst.chmod(0o444)
manifest={'format':'agent-source-snapshot-v1','identity':identity,'head':subprocess.check_output(['git','-C',str(root),'rev-parse','HEAD'],text=True).strip(),'dirty_status':subprocess.check_output(['git','-C',str(root),'status','--short'],text=True),'files':entries,'immutable_build_input':True}
(snap/'SOURCE_IDENTITY.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
print(snap)
PY
)
ID=$(basename -- "$SNAP")
BUILD="$OUT/builds/$ID"
mkdir -p "$BUILD/host" "$BUILD/rv32/obj"
printf '%s\n' "$SNAP" > "$OUT/LATEST_SNAPSHOT.txt"
printf '%s\n' "$BUILD" > "$OUT/LATEST_BUILD.txt"
exec > >(tee "$BUILD/build.log") 2>&1
cd -- "$SNAP"
core=(sw/model/qw_*.c)
vendor=(sw/model/vendor/npu_v1/npu_raw.c sw/model/vendor/npu_v1/npu_raw_reference.c sw/model/vendor/npu_v1/npu_runtime.c sw/model/vendor/npu_v1/npu_package.c sw/model/vendor/npu_v1/npu_storage.c sw/model/vendor/npu_v1/npu_ag_platform.c)
flags=(-std=c11 -Wall -Wextra -Werror -Wno-misleading-indentation -ffp-contract=off -fno-fast-math -I sw/model -I sw/agent)
CC=${CC:-gcc}
for test in model_runtime_unit model_checkpoint_unit; do
 "$CC" "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer "${core[@]}" "${vendor[@]}" "tests/$test.c" -lm -o "$BUILD/host/$test"
 "$BUILD/host/$test" > "$BUILD/host/$test.log"
done
"$CC" "${flags[@]}" -O3 -flto -fopenmp -DQW_HOST_OPENMP "${core[@]}" "${vendor[@]}" tests/model_state_machine_host.c -lm -o "$BUILD/host/model_state_machine_host"
"$CC" "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer sw/model/qw_tokenizer.c sw/model/qw_sha256.c tests/model_tokenizer_host.c -o "$BUILD/host/model_tokenizer_host"
"$CC" "${flags[@]}" -O2 sw/model/qw_tokenizer.c sw/model/qw_sha256.c tests/model_prefix_export_host.c -o "$BUILD/host/model_prefix_export_host"
TOKEN_BIN=${TOKEN_BIN:-/mnt/e/FPGA_RV32IM_Work/agent_optimization_handoff_20261102/agent_optimization_inputs_v1/tokenizer/TOKEN.BIN}
PREFIX_CASES=${R4_PREFIX_CASES:-$OUT/prefix_validation/r4_prefix_cases.bin}
"$BUILD/host/model_tokenizer_host" "$TOKEN_BIN" "$PREFIX_CASES" > "$BUILD/host/r4_prefix_parity.log"
"$CC" "${flags[@]}" -O3 -flto -fopenmp -DQW_HOST_OPENMP "${core[@]}" "${vendor[@]}" sw/agent/ag_agent.c sw/agent/ag_loop.c sw/agent/ag_qw_backend.c tests/agent_qw_real_host.c -lm -o "$BUILD/host/agent_qw_real_host"
for test in agent_loop_test agent_loop_safety; do
 "$CC" "${flags[@]}" -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer sw/agent/ag_agent.c sw/agent/ag_loop.c "tests/$test.c" -o "$BUILD/host/$test"
 "$BUILD/host/$test" > "$BUILD/host/$test.log"
done
"$CC" "${flags[@]}" -DAG_QW_BACKEND_TESTING -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer sw/model/qw_sha256.c sw/agent/ag_agent.c sw/agent/ag_loop.c sw/agent/ag_qw_backend.c tests/agent_qw_backend_test.c -o "$BUILD/host/agent_qw_backend_test"
"$BUILD/host/agent_qw_backend_test" > "$BUILD/host/agent_qw_backend_mock_only.log"
TC=${RISCV_CC:-$ROOT/build/toolchain/root/usr/bin/riscv64-unknown-elf-gcc}
BIN=${RISCV_BIN_DIR:-$ROOT/build/toolchain/root/usr/lib/riscv64-unknown-elf/bin}
RVROOT=$(dirname -- "$(dirname -- "$TC")")
AR="$RVROOT/bin/riscv64-unknown-elf-ar"
NM="$RVROOT/bin/riscv64-unknown-elf-nm"
READELF="$RVROOT/bin/riscv64-unknown-elf-readelf"
"$TC" "-B$BIN/" -print-prog-name=as > "$BUILD/rv32/assembler_path.txt"
"$TC" "-B$BIN/" -march=rv32im -mabi=ilp32 -x c -c /dev/null -o "$BUILD/rv32/probe.o"
for file in "${core[@]}" "${vendor[@]}" sw/model/vendor/npu_v1/freestanding.c sw/agent/ag_agent.c sw/agent/ag_loop.c sw/agent/ag_qw_backend.c; do
 name=$(basename -- "$file" .c)
 "$TC" "-B$BIN/" "${flags[@]}" -I sw/model/freestanding_include -DQW_FREESTANDING -Os -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -march=rv32im -mabi=ilp32 -c "$file" -o "$BUILD/rv32/obj/$name.o"
done
"$AR" rcs "$BUILD/rv32/libagent_accel.a" "$BUILD"/rv32/obj/*.o
"$READELF" -A "$BUILD/rv32/obj/qw_step.o" > "$BUILD/rv32/attributes.txt"
LIBGCC=$("$TC" "-B$BIN/" -march=rv32im -mabi=ilp32 -print-libgcc-file-name)
"$TC" "-B$BIN/" -march=rv32im -mabi=ilp32 -nostdlib -Wl,-r "$BUILD"/rv32/obj/*.o "$LIBGCC" -o "$BUILD/rv32/combined.o"
"$NM" -u "$BUILD/rv32/combined.o" > "$BUILD/rv32/undefined-symbols.txt"
[[ ! -s "$BUILD/rv32/undefined-symbols.txt" ]] || { echo 'unresolved dependency symbols'; exit 1; }
export INTEGER_REF_TEST_OUT="$BUILD/host/integer_reference_tmp"
mkdir -p "$INTEGER_REF_TEST_OUT"
python3 -B tests/test_agent_integer_reference.py > "$BUILD/host/integer_reference_tests.log" 2>&1
export AGENT_GRAPH_OPS_TEST_OUT="$BUILD/host/graph_ops_tmp"
mkdir -p "$AGENT_GRAPH_OPS_TEST_OUT"
python3 -B tests/test_agent_integer_graph_ops.py > "$BUILD/host/integer_graph_ops_tests.log" 2>&1
export AGENT_SCALE_TEST_OUT="$BUILD/host/scale_export_tmp"
mkdir -p "$AGENT_SCALE_TEST_OUT"
python3 -B tests/test_export_agent_integer_scales.py > "$BUILD/host/integer_scale_export_tests.log" 2>&1
export AGENT_FULL_GRAPH_TEST_OUT="$BUILD/host/integer_model_tmp"
mkdir -p "$AGENT_FULL_GRAPH_TEST_OUT"
python3 -B tests/test_agent_integer_model.py > "$BUILD/host/integer_model_tests.log" 2>&1
python3 -B tests/test_agent_eval.py > "$BUILD/host/evaluation_scorer_tests.log" 2>&1
"$CC" "${flags[@]}" -O1 sw/model/qw_json.c sw/agent/ag_agent.c sw/agent/ag_loop.c tests/agent_bf16_tool_broker.c -o "$BUILD/host/agent_bf16_tool_broker"
export BF16_TEST_ROOT="$BUILD/host/bf16_protocol_tmp"
export BF16_BROKER_COMMAND="[\"$BUILD/host/agent_bf16_tool_broker\"]"
python3 -B tests/test_agent_bf16_eval.py > "$BUILD/host/bf16_protocol_tests.log" 2>&1
printf 'SOURCE_IDENTITY=%s\nHOST_NUMERIC_SCORER_AND_FULL_RV32_LIBRARY_PASS (not firmware/board acceptance)\n' "$ID"
