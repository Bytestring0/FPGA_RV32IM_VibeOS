# Agent integration branch

The RV32 Agent executes tools in sw/agent. The Qwen model, cooperative stepping, prefix checkpoints and the existing NPU v1 adapter are in sw/model. tests/board_desktop.c is the desktop application source. Board RTL, bitstreams and trained weights are maintained separately.

Run the host executor tests from the repository root:
    gcc -std=c11 sw/agent/ag_agent.c sw/agent/ag_loop.c tests/agent_loop_test.c -o /tmp/agent-loop && /tmp/agent-loop

For the host reference and RV32IM library use scripts/build_agent_accel.sh in WSL, with an E-drive output directory, TOKEN_BIN pointing to the delivered tokenizer asset, R4_PREFIX_CASES pointing to agent_assets/prefix_validation/r4_prefix_cases.bin, and RISCV_CC/RISCV_BIN_DIR pointing to the bare-metal toolchain.

The R5 software reference has demonstrated calculation and saving a real result. The frozen evaluation tasks, integer graph candidates and software evidence do not establish full task quality, board throughput or hardware integration.
