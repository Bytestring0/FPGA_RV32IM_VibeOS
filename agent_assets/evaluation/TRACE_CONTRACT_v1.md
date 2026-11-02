# Trace 输入契约 v1：score_case 的最小真实证据

`python -B scripts/agent_eval.py --evaluation E:\FPGA_RV32IM_Work\deepseek_agent_accel_20261102\evaluation --results E:\...\evaluation\run_results.json --output E:\...\evaluation\quality_report.json`。不提供 `--results` 输出 NOT_RUN；不会运行模型。原始 trace、运行日志、评分输出一律保存 E/evaluation。

`run_results.json`：`{"freeze_sha256": "manifest.tasks_sha256", "rules_sha256": "manifest.rules_sha256", "runs": {"high_precision": {"status":"RUN","mode":"real_generation","identity":{"model_sha256":"...","tokenizer_sha256":"...","template_sha256":"...","tools_schema_sha256":"...","runtime_sha256":"...","backend":"cpu_real_model","source_log_path":"E:\\...\\native.log","source_log_sha256":"64位hex"},"cases":[...]}, "integer_reference":..., "hardware_graph":...}}`。未测 role 写 `status=NOT_RUN` 或省略；只能真实模型生成标 `real_generation`，scripted/故障注入不得冒充。模型记录缺失的自然任务算失败，绝不自动减少分母。保存各方完整运行日志，用 source_log_path 的真实 SHA 作最低可追溯要求；字段自报不能独立证明硬件。

每个自然 case：`{"task_id":"冻结ID","case_sha256":"冻结case.content_sha256","generation":1,"initial_state":{...},"final_state":{...},"final_status":"finished|clarify","turns":[...]}`。

- `initial_state`/`final_state` 必须为从 `ag_runtime.state` 直接采集（不可从显示文本推断）的 `notes:[...]`, `records:[...]`, `timer:idle|running|paused|finished`, `label`, `deadline_ms`, `remaining_ms`, `timer_revision`，可含 `note_count`/`record_count`，存在则必须匹配数组长度。独立任务先清空 runtime；修改计时器任务由真实 `start_timer` 预置已运行计时器，起点状态记录应保留该预置记录，不视为本自然任务模型生成行为。
- 每轮 `turns[i]` 包含 `generation` 和零基 `turn`，`model_response` 是 **原始 JSON 字符串**，如 `"{\"type\":\"tool\",\"id\":\"c1\",\"request\":\"{\\\"name\\\":\\\"calculate\\\",\\\"arguments\\\":{\\\"expression\\\":\\\"12+3\\\"}}\"}"`（示意；实际 JSON 按运行器正确序列化）；`finish/clarify` 为 `{"type":"finish","text":"15"}` 的 JSON 字符串。只接受 native ag_loop envelope，不以归一化文本纠错。
- 工具轮附真实 `tool_event`：`{"kind":"tool","origin":"ag_execute","id":"c1","generation":1,"request":"原始内层 request 字符串","code":0,"text":"15","now_ms":1000,"result_id":"c1"}`；计算的 result_id 若单独回执中未提供，可由 `ag_state.records` 中匹配的成功 calculate 的 `id` 映射，**不能凭模型文本发明 ID**。
- 下一轮有 `receipt_input`：`{"kind":"receipt","call_id":"c1","generation":1,"code":0,"text":"15"}`，这必须就是送入模型上下文的回执。工具完成状态记录每条含 `id`,`tool`,`request`,`receipt:{"code":0,"result":"15"}`，必须对应真实成功事件。错误工具调用 (如 1/0) 不成功入库，但错误事件和回执输入都要保留。
- 终态轮仅有 `terminal_event:{"kind":"final|clarification","generation":1,"text":"与原生响应相同"}`，且 `final_status` 对应 ag_loop。不可用单个 finish 事件代替工具与状态证据。

自然类别的 `initial_state` 默认 notes/records 空、timer idle、revision 0；`timer_set` 用预置的真实 start_timer 成功记录、timer running、revision>=1。所有 case 保留同一冻结内容，不给模型传 expected/答案；评测器只在输出完成后读取 expected。

控制/故障类在 `holdout_v1.json.controls` 中独立列 14 项，不以自然任务分母计算。可在结果顶层另传 `control_run:{"status":"RUN","cases":[...]}`。每条含 `task_id`,`case_sha256`,`initial_state`,`final_state`,`final_status`,`events`；按单调 `now_ms` 排序，tool 事件必须是实际 `ag_execute` 的 `id/request/code/text/origin`。重复/冲突两例夹具调用 `append_note` 两次用同 ID：前者同请求同回执只提交一次；后者不同请求必须 conflict，原提交保留。非法输出例另含 `raw_response` 或 `raw_response_hex`，必须实际拒绝且无状态变化。取消和隔离例事件须包含真实 `cancel/backend_result(accepted=false)/quiescent_ack(accepted=true)/reuse(retired_generation)/reuse_rejected/model_input` 的 generation 与时刻；评分器检查取消后不执行新工具、迟到 generation 不改新状态、未 quiescent 不复用、已成功提交不能回滚。缺真实证据就失败/NOT_RUN，不接受抽象 `passed=true`。不能把这类的脚本注入当成自然模型生成。
