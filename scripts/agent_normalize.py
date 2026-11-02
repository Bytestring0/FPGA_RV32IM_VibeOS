"""Native desktop tool call -> strict board v0 envelope, without execution.
Argument JSON is not repaired. result_id is the preceding calculate call ID.
"""
import json
import math

TOOLS={'calculate','save_result','append_note','query_state','start_timer',
       'set_timer','extend_timer','pause_timer','resume_timer','cancel_timer','finish','clarify'}

def strict_object(pairs):
    result={}
    for key,value in pairs:
        if key in result: raise ValueError('duplicate JSON key')
        result[key]=value
    return result

def normalize(call):
    if set(call)!={'id','type','function'} or call['type']!='function': raise ValueError('invalid call')
    ident=call['id']
    if not isinstance(ident,str) or not 1<=len(ident)<=32 or any(not (c.isascii() and (c.isalnum() or c in '_-')) for c in ident):
        raise ValueError('call ID outside board v0 bounds')
    fn=call['function']
    if set(fn)!={'name','arguments'} or fn['name'] not in TOOLS or not isinstance(fn['arguments'],str):
        raise ValueError('unsupported function')
    args=json.loads(fn['arguments'],object_pairs_hook=strict_object,
                    parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite number')))
    if not isinstance(args,dict) or len(args)>3: raise ValueError('flat arguments required')
    for value in args.values():
        if type(value) not in (str,int,float): raise ValueError('unsupported argument type')
        if isinstance(value,str):
            if '\0' in value or len(value.encode('utf-8'))>192: raise ValueError('string outside bounds')
        elif type(value) is float and not math.isfinite(value): raise ValueError('nonfinite number')
    result=json.dumps({'name':fn['name'],'arguments':args},ensure_ascii=False,allow_nan=False,separators=(',',':'))
    if len(result.encode('utf-8'))>512: raise ValueError('envelope outside bounds')
    return ident,result
