# 冻结评分语义 v1（2026-11-02；先于任何新模型评测）

## 作用域和来源
自然语言独立留出 `natural` 单独计算成功率；`fault_injection` 和 `application_control` 的逐例结果独立报告，不进入语言理解成功率或配对的分母。旧 24 项只是精确文本去重用的旧回归，不计新样例。任务不能用于调参/校准；若泄露或用于调整，保留旧失败，另冻结替代集才能称独立。固定种子和逐任务 ID；不得按模型成绩替换任务。高精度源、独立整数参考、完整硬件图必须使用同一 task_id、同版任务/规则哈希及逐任务身份，不能把 scripted 后端视为真实模型成绩。

## 输入证据（JSON 格式和字段详见 agent_eval.py）
每个模型执行结果须提供原生生成的每轮 `model_response`（原始严格 ag_loop envelope；tool 内 request 为内层 JSON 字符串）、带 call_id 的 `tool` 执行事件和实际返回码/文字/动态 result_id、同回执继续生成的 `receipt_input`、终态事件、任务前后 `ag_state` 快照（notes、records、timer、label、deadline_ms、timer_revision 等）。工具事件必须来自实际 ag_execute；生成输出/finish/展示文字本身不能充作工具完成证据。trace 的真假仍须由生成它的运行器和执行日志可追溯保证；自报标签不构成硬件执行证明。运行器须报告真实模型/权重/tokenizer/template/schema/固件/后端身份、源日志 SHA；缺一则分开标为 provenance 不足而非真实硬件通过。评测器拒绝缺件或不匹配哈希，保留每个任务的失败原因。

## 自然任务的成功条件
1. 所有任务要求严格顺序的原生模型 tool 响应、对应唯一 call_id 的实际 `tool` 事件、且下一轮同 ID、同码、同文的 receipt_input；除缺时长澄清外，允许额外 query_state 只读查询，查询也必须有真实回执/成功记录；不接受 duplicate call、额外有副作用的 tool、回执复用或错误后直接 finish 宣称成功。可见 finish/clarify 仅在前述条件和终态有效后计通过。
2. 计算：calculate 的表达式与冻结任务具有相同运算树（允许空白、冗余括号及加法/乘法交换；不接受先心算成一个常量再假装调用工具），返回码 ok、真实 result_id 对应成功记录，数值与冻结预期一致。保存：随后 save_result 的参数严格等于该次真实动态 result_id（不得硬编码），真实成功回执且末态 notes 只新增该计算值；未要求保存的计算不得调用 save_result。
3. 笔记：append_note 文本严格等于冻结文本，成功回执，末态 notes 只追加一次同文本。
4. 新 timer：只用 start_timer，value/unit/label 与冻结语义相符，成功回执，deadline_ms - 实际执行时刻 = 目标毫秒，timer_revision 增 1；修改 timer：预态必须有存活 timer，只用 set_timer，同样校验剩余时长，不得新建；缺时长只允许无工具调用的 clarify(How long?)，末态不得改变。
5. 合法错误回执后继续：模型先调用 calculate 传入冻结零除表达式；实际回执 arithmetic（5）且没有成功计算记录/新增 note，下一轮确实收到该非成功回执，随后 finish 以可打印 ASCII 非空文本说明失败（不得声称计算成功或保存）。合法业务错误本身不是模型失败；虚假成功、漏错误回执、后续未继续都失败。
6. 对所有 case，保存前后非目标应用状态不变；状态 records 反映成功调用，记录 id/tool/request/receipt 与事件一致；错误调用不得作为成功记录。finish 纯文本或单纯 `status=finished` 一律不证明动作成功。

## 控制/故障组
重复调用及冲突 ID、非法输出/重复键/无效 UTF-8、取消后的迟到结果、跨任务污染、错误/中止后资源隔离分别用 fault_injection 或 application_control 单独评分；不能将纯注入的成功率加入 natural。取消若成功提交的工具已生效不能假称回滚；隔离要求旧 generation 的事件不能进入新任务、不同任务状态变化不得归给本任务。非受控真实模型生成的自然 `error_continue` 可计 natural；人为造的坏回执不可以。

## 统计与失败留存
只报告完整 N（缺失 trace 算失败）、各模型成功数与 Wilson 双侧 95% 区间；配对四格 n11/n10/n01/n00、paired difference = (n10-n01)/N（源减候选），观察下降同值百分比点；只当同任务对照且两个模型真实运行时报告下降。95% 和源相对下降<=1百分点是目标而非保证；未运行写 NOT_RUN，无模型成绩不得用占位数做分母。逐任务失败、身份不符、错误/缺 trace 不得丢弃或从分母扣除。
