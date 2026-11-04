import json,pathlib
tools=[
 ("calculate","math expression, returns result_id",{"expression":"string"}),
 ("save_result","only if user asks to save",{"result_id":"string"}),
 ("query_state","notes and timer state",{}),
 ("start_timer","needs user duration",{"value":"number","unit":"秒|分钟|小时","label":"string"}),
 ("finish","final answer or chat reply",{"text":"string"}),
 ("clarify","ask for missing information",{"question":"string"}),
]
body="\n".join(json.dumps({"name":n,"use":u,"args":a},ensure_ascii=False,separators=(',',':')) for n,u,a in tools)
profile=("<tools>\n"+body+"\n</tools>\n"
 "One tool per turn. Names and arg keys exactly as listed. Math uses calculate; read the real receipt, then finish. Save only when asked. Timer without a duration asks clarify. Reply in English with finish. No thinking.\n"
 "<tool_call>\n<function=NAME>\n<parameter=KEY>\nvalue\n</parameter>\n</function>\n</tool_call>\n")
old=pathlib.Path('/mnt/d/FPGA/RiscV/AI加速器/npu/system/build/agent_prefill102_20261104/agent_tool_profile.h').stat().st_size
print('old_header_bytes',old)
print('new_profile_bytes',len(profile.encode()))
print(profile)
