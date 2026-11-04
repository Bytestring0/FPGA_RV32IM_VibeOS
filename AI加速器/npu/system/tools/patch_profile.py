import pathlib,re,json
p=pathlib.Path('/mnt/d/FPGA/RiscV/AI加速器/npu/system/tools/build_agent_interactive.py')
s=p.read_text(encoding='utf-8')
start=s.index('        profile="# Tools')
end=s.index('\n',start)
old=s[start:end]
assert 'calculate' in old
new='''        _tools=[("calculate","math expression, returns result_id",{"expression":"string"}),
                ("save_result","only if user asks to save",{"result_id":"string"}),
                ("query_state","notes and timer state",{}),
                ("start_timer","needs user duration",{"value":"number","unit":"\u79d2|\u5206\u949f|\u5c0f\u65f6","label":"string"}),
                ("finish","final answer or chat reply",{"text":"string"}),
                ("clarify","ask for missing information",{"question":"string"})]
        profile="<tools>\\n"+"\\n".join(json.dumps({"name":n,"use":u,"args":a},ensure_ascii=False,separators=(',',':')) for n,u,a in _tools)+"\\n</tools>\\n"+"One tool per turn. Names and arg keys exactly as listed. Math uses calculate; read the real receipt, then finish. Save only when asked. Timer without a duration asks clarify. Reply in English with finish. No thinking.\\n<tool_call>\\n<function=NAME>\\n<parameter=KEY>\\nvalue\\n</parameter>\\n</function>\\n</tool_call>\\n"'''
s=s[:start]+new+s[end:]
s=s.replace("    if a.simple_profile:\n        import json\n","    if a.simple_profile:\n        import json\n",1)
p.write_text(s,encoding='utf-8')
print('PROFILE_LINE_REPLACED old_bytes',len(old))
