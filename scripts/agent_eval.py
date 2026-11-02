r"""Frozen independent holdout scorer; NEVER invokes a model or a tool adapter.

Input trace contract: evaluation/TRACE_CONTRACT_v1.md. Fail closed on absent evidence.
CLI: python -B scripts/agent_eval.py --evaluation E:\...\evaluation [--results raw_results.json] [--output E:\...\evaluation\report.json]
"""
from __future__ import annotations

import argparse
import ast
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import sys
from typing import Any

ROLES = ('high_precision', 'integer_reference', 'hardware_graph')
IDENTITY = ('model_sha256', 'tokenizer_sha256', 'template_sha256', 'tools_schema_sha256',
            'runtime_sha256', 'backend', 'source_log_sha256', 'source_log_path')
SCHEMA = {
    'calculate': {'expression'}, 'save_result': {'result_id'}, 'append_note': {'text'},
    'start_timer': {'value','unit','label'}, 'set_timer': {'value','unit'},
    'extend_timer': {'value','unit'}, 'query_state': set(), 'pause_timer': set(),
    'resume_timer': set(), 'cancel_timer': set(),
}
CODE = {'ok':0, 'bad_json':1, 'bad_args':2, 'unsupported':3, 'limit':4,
        'arithmetic':5, 'conflict':6, 'not_found':7, 'timer_state':8, 'storage':9}
TIMER = {0:'idle',1:'running',2:'paused',3:'finished'}

class InvalidEvidence(ValueError):
    pass

def require(condition: bool, reason: str) -> None:
    if not condition: raise InvalidEvidence(reason)

def pairs_no_duplicates(pairs):
    result = {}
    for key,value in pairs:
        if key in result: raise InvalidEvidence('duplicate_json_key:' + key)
        result[key] = value
    return result

def parse_json(raw, label):
    require(isinstance(raw, str), label + ':must_be_raw_string')
    try:
        return json.loads(raw, object_pairs_hook=pairs_no_duplicates,
                          parse_constant=lambda x: (_ for _ in ()).throw(InvalidEvidence('nonfinite_json')))
    except (ValueError, UnicodeError) as exc:
        raise InvalidEvidence(label + ':invalid_json:' + str(exc)) from exc

def parse_response(raw):
    require(type(raw) is str, 'native_response:must_be_raw_string')
    try: require(len(raw.encode('utf-8'))<=768, 'native_response:too_long')
    except UnicodeError as exc: raise InvalidEvidence('native_response:invalid_utf8') from exc
    r = parse_json(raw, 'native_response')
    require(type(r) is dict, 'native_response:not_object')
    if r.get('type') == 'tool':
        require(set(r) == {'type','id','request'}, 'native_response:invalid_tool_envelope')
        require(isinstance(r['id'],str) and re.fullmatch(r'[a-zA-Z0-9_-]{1,32}',r['id']) is not None,
                'native_response:invalid_call_id')
        c = parse_json(r['request'], 'native_tool_request')
        require(type(c) is dict and set(c) == {'name','arguments'}, 'native_tool_request:invalid_schema')
        name,args = c['name'],c['arguments']
        require(name in SCHEMA and type(args) is dict and set(args)==SCHEMA[name], 'native_tool_request:unknown_tool_or_args')
        if name in ('calculate','save_result','append_note'):
            require(all(type(v) is str for v in args.values()), 'native_tool_request:invalid_field_type')
        elif name in ('start_timer','set_timer','extend_timer'):
            require(type(args['value']) in (float,int) and math.isfinite(args['value']) and args['value']>0
                    and type(args['unit']) is str and args['unit'] in ('秒','分钟','小时'),
                    'native_tool_request:invalid_timer_args')
            if name=='start_timer': require(type(args['label']) is str and bool(args['label']), 'invalid_timer_label')
        return r, c
    require(r.get('type') in ('finish','clarify'), 'native_response:invalid_type')
    require(set(r) == {'type','text'}, 'native_response:invalid_terminal_envelope')
    require(isinstance(r['text'],str) and bool(r['text']) and all(32 <= ord(x) <= 126 for x in r['text']),
            'terminal_text:printable_ascii_required')
    return r,None

def code_int(x):
    if type(x) is int and x in CODE.values(): return x
    if type(x) is str and x in CODE: return CODE[x]
    raise InvalidEvidence('invalid_receipt_code')

def state(state, label):
    require(type(state) is dict, label+':missing')
    for k in ('notes','records','timer','label','deadline_ms','remaining_ms','timer_revision'):
        require(k in state, label+':missing_'+k)
    require(type(state['notes']) is list and all(type(s) is str for s in state['notes']), label+':invalid_notes')
    require(type(state['records']) is list and all(type(r) is dict for r in state['records']), label+':invalid_records')
    require(state['timer'] in TIMER.values() or state['timer'] in TIMER, label+':bad_timer')
    for k in ('deadline_ms','remaining_ms','timer_revision'):
        require(type(state[k]) is int and state[k]>=0,label+':invalid_'+k)
    require(type(state['label']) is str,label+':invalid_label')
    if 'record_count' in state: require(state['record_count']==len(state['records']),label+':record_count_mismatch')
    if 'note_count' in state: require(state['note_count']==len(state['notes']),label+':note_count_mismatch')
    return state

def timer_name(v):
    return TIMER[v] if type(v) is int and v in TIMER else v

def same_expression(actual, expected):
    def node(n):
        if isinstance(n,ast.Expression):return node(n.body)
        if isinstance(n,ast.Constant) and type(n.value) in (int,float):return ('number',str(n.value))
        if isinstance(n,ast.UnaryOp) and isinstance(n.op,(ast.UAdd,ast.USub)):
            return node(n.operand) if isinstance(n.op,ast.UAdd) else ('neg',node(n.operand))
        if isinstance(n,ast.BinOp) and isinstance(n.op,(ast.Add,ast.Sub,ast.Mult,ast.Div)):
            left,right=node(n.left),node(n.right)
            if isinstance(n.op,(ast.Add,ast.Mult)):left,right=sorted((left,right),key=repr)
            return (type(n.op).__name__,left,right)
        raise InvalidEvidence('unsupported_arithmetic_expression')
    try:return node(ast.parse(actual.strip(),mode='eval'))==node(ast.parse(expected.strip(),mode='eval'))
    except (SyntaxError,ValueError,TypeError,InvalidEvidence):return False

def exact_no_extra_side_effects(before,after, family):
    if family not in ('calculate_save','append_note'):
        require(after['notes']==before['notes'], 'state:unrequested_note_change')
    if family not in ('timer_start','timer_set'):
        for k in ('timer','label','deadline_ms','remaining_ms','timer_revision'):
            require(after[k]==before[k], 'state:unrequested_timer_change_'+k)

def score_case(case: dict, trace: dict) -> dict:
    """Return pass plus all failure reasons; never use model finish as a substitute for receipts."""
    errors = []
    try:
        _score_case(case, trace)
    except (InvalidEvidence, TypeError, KeyError, IndexError, OverflowError) as e:
        errors.append(str(e))
    return {'task_id':case['task_id'],'cohort':case['cohort'],'family':case.get('family'),
            'passed':not errors,'errors':errors}

def _score_case(case, trace):
    require(case['cohort']=='natural', 'control_requires_separate_harness')
    require(type(trace) is dict and trace.get('task_id')==case['task_id'], 'wrong_or_missing_task_id')
    require(trace.get('case_sha256')==case['content_sha256'], 'case_content_hash_mismatch')
    gen = trace.get('generation')
    require(type(gen) is int and gen>0, 'missing_generation')
    pre = state(trace.get('initial_state'),'initial_state')
    post = state(trace.get('final_state'),'final_state')
    require(pre['notes']==[], 'initial_state:not_independent_empty_notes')
    if case['initial_state']=='running_timer':
        require(timer_name(pre['timer'])=='running' and pre['timer_revision']>=1, 'initial_state:timer_not_running')
        require(type(pre['deadline_ms']) is int and pre['deadline_ms']>0, 'initial_state:timer_deadline_missing')
        require(len(pre['records'])==1 and pre['records'][0].get('tool')=='start_timer'
                and code_int(pre['records'][0].get('receipt',{}).get('code'))==0,
                'initial_state:missing_real_timer_setup_record')
    else:
        require(pre['records']==[] and timer_name(pre['timer'])=='idle' and pre['timer_revision']==0,
                'initial_state:must_be_reset')
    turns = trace.get('turns')
    require(type(turns) is list and 1<=len(turns)<=16, 'missing_or_excess_turns')
    expected = case['expected']; family = case['family']; names = expected['tools']
    require(len(turns)>=len(names)+1, 'wrong_number_of_turns')
    ids=set(); observed=[];action_index=0
    for i,turn in enumerate(turns):
        require(type(turn) is dict and type(turn.get('model_response')) is str, f'turn_{i}:missing_raw_model_response')
        require(turn.get('generation')==gen and turn.get('turn')==i, f'turn_{i}:generation_or_turn_mismatch')
        raw,c = parse_response(turn['model_response'])
        if i:
            prior=turns[i-1]['tool_event']
            receipt_input=turn.get('receipt_input')
            require(type(receipt_input) is dict, f'turn_{i}:missing_receipt_input')
            require(set(receipt_input)>={'kind','call_id','code','text','generation'}, f'turn_{i}:bad_receipt_input')
            require(receipt_input['kind']=='receipt' and receipt_input['call_id']==prior['id']
                    and code_int(receipt_input['code'])==code_int(prior['code'])
                    and receipt_input['text']==prior['text'] and receipt_input['generation']==gen,
                    f'turn_{i}:receipt_not_echoed_to_same_session')
        else:
            require('receipt_input' not in turn, 'initial_turn_has_receipt')
        if i<len(turns)-1:
            require(raw['type']=='tool', f'turn_{i}:unexpected_tool')
            if c['name']=='query_state':
                require(family!='timer_clarify', 'missing_duration_allows_only_clarify')
            else:
                require(action_index<len(names) and c['name']==names[action_index], f'turn_{i}:unexpected_tool')
                action_index+=1
            require('terminal_event' not in turn, f'turn_{i}:premature_terminal')
            require(raw['id'] not in ids, f'turn_{i}:duplicate_call_id')
            ids.add(raw['id'])
            event=turn.get('tool_event')
            require(type(event) is dict, f'turn_{i}:missing_real_tool_event')
            require(event.get('kind')=='tool' and event.get('origin')=='ag_execute' and event.get('id')==raw['id']
                    and event.get('generation')==gen, f'turn_{i}:tool_event_not_bound_to_execution')
            require(event.get('request')==raw['request'], f'turn_{i}:tool_event_request_mismatch')
            require(type(event.get('text')) is str and type(event.get('now_ms')) is int,
                    f'turn_{i}:missing_real_receipt_or_clock')
            assert_code=5 if family=='error_continue' and c['name']=='calculate' else 0
            require(code_int(event.get('code'))==assert_code, f'turn_{i}:unexpected_receipt_code')
            observed.append((raw,c,event))
        else:
            require('tool_event' not in turn, 'terminal_turn_has_unexpected_tool')
            required='clarify' if family=='timer_clarify' else 'finish'
            require(raw['type']==required, 'missing_expected_terminal')
            if family=='timer_clarify':
                require(raw['text']==expected['question'], 'clarification_question_mismatch')
            elif family=='error_continue':
                text=raw['text'].lower()
                require(any(w in text for w in ('error','fail','zero','invalid','cannot','division','arithmetic'))
                        and not any(w in text for w in ('saved','success','completed')), 'error_not_explained_or_false_success')
            elif family in ('calculate_save','calculate_only'):
                require(re.search(r'(?<![0-9.])'+re.escape(expected['value'])+r'(?![0-9.])',raw['text']) is not None,
                        'terminal_wrong_calculation_value')
            terminal=turn.get('terminal_event')
            require(type(terminal) is dict and terminal.get('kind')==('clarification' if required=='clarify' else 'final')
                    and terminal.get('generation')==gen and terminal.get('text')==raw['text'],
                    'terminal_event_not_bound_to_model_response')
    require(action_index==len(names), 'requested_actions_not_all_executed')
    require(trace.get('final_status')==('clarify' if family=='timer_clarify' else 'finished'),
            'wrong_final_loop_status')
    require(len(post['records'])==len(pre['records'])+sum(code_int(e['code'])==0 for _,_,e in observed),
            'state:wrong_committed_record_count')
    require(post['records'][:len(pre['records'])]==pre['records'], 'state:prior_records_changed')
    recorded=post['records'][len(pre['records']):]
    for idx,(raw,c,event) in enumerate(observed):
        if code_int(event['code'])!=0: continue
        record=recorded.pop(0)
        require(record.get('id')==event['id'] and record.get('tool')==c['name']
                and record.get('request')==raw['request'], f'committed_record_{idx}:id_tool_or_request_mismatch')
        rr=record.get('receipt')
        require(type(rr) is dict and code_int(rr.get('code'))==0 and rr.get('result')==event['text'],
                f'committed_record_{idx}:receipt_mismatch')
    exact_no_extra_side_effects(pre,post,family)
    observed=[x for x in observed if x[1]['name']!='query_state']
    if family in ('calculate_save','calculate_only'):
        raw,c,evt=observed[0]
        require(same_expression(c['arguments']['expression'],expected['expression']), 'wrong_expression')
        require(evt['text']==expected['value'], 'calculator_result_mismatch')
        rid=evt.get('result_id',raw['id'])
        require(type(rid) is str and rid==raw['id'], 'result_id_not_bound_to_committed_calculate')
        if family=='calculate_save':
            _,sc,save_evt=observed[1]
            require(sc['arguments']['result_id']==rid, 'save_result_did_not_use_real_result_id')
            require(post['notes']==pre['notes']+[evt['text']], 'saved_note_does_not_equal_real_calculated_value')
            require(save_evt['text']==str(len(post['notes'])), 'save_result_receipt_not_note_count')
    if family=='append_note':
        require(observed[0][1]['arguments']['text']==expected['text'], 'wrong_note_text')
        require(post['notes']==pre['notes']+[expected['text']], 'note_not_committed_exactly_once')
        require(observed[0][2]['text']==str(len(post['notes'])), 'note_receipt_not_actual_count')
    if family in ('timer_start','timer_set'):
        _,c,evt=observed[0]
        args=c['arguments']
        require(args['value']==expected['value'] and args['unit']==expected['unit'], 'wrong_timer_duration_or_unit')
        if family=='timer_start':
            require(args['label']==expected['label'] and post['label']==expected['label'], 'wrong_timer_label')
        else:
            require(post['label']==pre['label'], 'existing_timer_label_changed')
        require(timer_name(post['timer'])=='running' and post['timer_revision']==pre['timer_revision']+1,
                'timer_state_or_revision_mismatch')
        require(post['deadline_ms']-evt['now_ms']==expected['duration_ms'], 'timer_deadline_not_expected_duration')
        require(post['remaining_ms']==expected['duration_ms'] and evt['text']==str(expected['duration_ms']),
                'timer_receipt_or_remaining_mismatch')
    if family=='error_continue':
        require(same_expression(observed[0][1]['arguments']['expression'],expected['expression']), 'error_case_wrong_expression')
        require(post['notes']==pre['notes'] and all(r.get('tool')=='query_state'
                for r in post['records'][len(pre['records']):]), 'error_case_committed_side_effect')
    if family=='timer_clarify':
        require(post==pre,'clarification_changed_application_state')

def score_control(case, trace):
    """Control/fault evidence is separate from natural language model evidence."""
    result={'task_id':case['task_id'],'cohort':case['cohort'],'scenario':case['scenario'],
            'passed':False,'errors':[]}
    try:
        require(type(trace) is dict and trace.get('task_id')==case['task_id']
                and trace.get('case_sha256')==case['content_sha256'], 'control_identity_mismatch')
        pre=state(trace.get('initial_state'),'initial_state');post=state(trace.get('final_state'),'final_state')
        events=trace.get('events');require(type(events) is list,'control_missing_events')
        require(all(type(e) is dict and type(e.get('now_ms')) is int for e in events), 'control_missing_event_clock')
        require([e['now_ms'] for e in events]==sorted(e['now_ms'] for e in events), 'control_event_order_invalid')
        tools=[e for e in events if e.get('kind')=='tool']
        require(all(e.get('origin')=='ag_execute' and isinstance(e.get('id'),str)
                    and isinstance(e.get('request'),str) and isinstance(e.get('text'),str) for e in tools),
                'control_missing_real_execution_receipts')
        name=case['scenario']
        if name in ('duplicate_successful_call','conflicting_call_id'):
            require(len(tools)==2 and tools[0]['id']==tools[1]['id'], 'control_requires_two_same_id_calls')
            a,b=tools;require(code_int(a['code'])==0,'control_first_call_not_successful')
            require(len(post['records'])==len(pre['records'])+1 and post['records'][:-1]==pre['records'],
                    'duplicate_or_conflict_changed_record_count')
            rec=post['records'][-1]
            require(rec.get('id')==a['id'] and rec.get('request')==a['request']
                    and rec.get('receipt',{}).get('result')==a['text'], 'control_first_commit_not_preserved')
            call=parse_json(a['request'],'control_request')
            require(call.get('name')=='append_note','control_fixture_must_use_append_note')
            require(post['notes']==pre['notes']+[call['arguments']['text']], 'duplicate_committed_note_more_than_once')
            require(all(pre[k]==post[k] for k in ('timer','label','deadline_ms','remaining_ms','timer_revision')),
                    'control_unrelated_timer_mutation')
            if name=='duplicate_successful_call':
                require(a['request']==b['request'] and code_int(b['code'])==0 and a['text']==b['text'],
                        'duplicate_receipt_not_identical')
            else: require(a['request']!=b['request'] and code_int(b['code'])==6,'conflicting_id_not_rejected')
        elif name in ('invalid_json_response','invalid_tool_schema','duplicate_json_key','malformed_utf8'):
            require(not tools and pre==post and trace.get('final_status')=='protocol_error',
                    'illegal_output_not_rejected_without_side_effect')
            if name=='malformed_utf8':
                try: bytes.fromhex(trace['raw_response_hex']).decode('utf-8',errors='strict')
                except UnicodeDecodeError: pass
                else: raise InvalidEvidence('control_input_was_valid_utf8')
            else:
                try:parse_response(trace.get('raw_response'))
                except InvalidEvidence:pass
                else:raise InvalidEvidence('control_input_was_valid_protocol')
        elif name in ('wrong_receipt_id','fabricated_result_id'):
            require(not tools and pre==post and trace.get('final_status') in ('protocol_error','backend_error'),
                    'fabricated_receipt_not_rejected')
            injections=[e for e in events if e.get('kind')=='injected_receipt']
            require(len(injections)==1 and injections[0].get('call_id') not in {r.get('id') for r in pre['records']},
                    'missing_actual_invalid_receipt_injection')
        else:
            gen=trace.get('generation');require(type(gen) is int and gen>0,'control_missing_generation')
            cancels=[e for e in events if e.get('kind')=='cancel']
            acknowledgements=[e for e in events if e.get('kind')=='quiescent_ack']
            reuses=[e for e in events if e.get('kind')=='reuse']
            for reuse in reuses:
                require(any(a['now_ms']<=reuse['now_ms'] and a.get('generation')==reuse.get('retired_generation')
                            and a.get('accepted') is True for a in acknowledgements), 'reuse_before_proven_quiescence')
            if name.startswith('cancel_'):
                require(len(cancels)==1 and trace.get('final_status')=='cancelled','cancel_not_observed')
                cancel=cancels[0]
                require(cancel.get('generation')==gen and all(e['now_ms']<=cancel['now_ms'] for e in tools),
                        'tool_executed_after_cancel')
                if name=='cancel_pending_late_result':
                    require(not tools and pre==post,'cancel_pending_had_side_effects')
                    require(any(e.get('kind')=='backend_result' and e.get('generation')==gen and e.get('accepted') is False
                                and e['now_ms']>=cancel['now_ms'] for e in events),'missing_late_result_rejection')
                else:
                    require(len(tools)==1 and code_int(tools[0]['code'])==0 and len(post['records'])==len(pre['records'])+1,
                            'cancel_rolled_back_or_lost_committed_action')
                    c=parse_json(tools[0]['request'],'cancel_commit_request')
                    require(c.get('name')=='append_note' and post['notes']==pre['notes']+[c['arguments']['text']],
                            'cancel_committed_note_not_retained')
            elif name in ('stale_generation','cross_task_note_isolation','cross_task_timer_isolation'):
                require(not tools and pre==post,'cross_task_side_effect')
                require(any(e.get('kind')=='backend_result' and e.get('generation')!=gen and e.get('accepted') is False
                            for e in events),'missing_stale_result_rejection')
                inputs=[e for e in events if e.get('kind')=='model_input']
                require(all(e.get('generation')==gen for e in inputs),'old_task_context_leaked')
            elif name=='failure_quiescence_gate':
                require(not tools and pre==post and reuses and acknowledgements,'missing_quiescence_gate_evidence')
                require(any(e.get('kind')=='reuse_rejected' for e in events),'missing_early_reuse_rejection')
            else:raise InvalidEvidence('unsupported_control_scenario')
        result['passed']=True
    except (InvalidEvidence,ValueError,TypeError,KeyError,IndexError) as exc:
        result['errors'].append(str(exc))
    return result

def wilson(success, total, z=1.959963984540054):
    if not total: return None
    p=success/total; zz=z*z; denominator=1+zz/total
    center=(p+zz/(2*total))/denominator
    radius=z*math.sqrt((p*(1-p)+zz/(4*total))/total)/denominator
    return [max(0,center-radius),min(1,center+radius)]

def pair(a,b):
    aa={r['task_id']:r['passed'] for r in a}; bb={r['task_id']:r['passed'] for r in b}
    require(aa.keys()==bb.keys(), 'paired_case_ids_mismatch')
    counts=Counter((aa[k],bb[k]) for k in aa)
    n=len(aa); n11=counts[True,True]; n10=counts[True,False]
    n01=counts[False,True]; n00=counts[False,False]
    # Bonferroni two-sided paired difference: 97.5% Wilson for each discordant cell.
    z=2.241402727604947
    p10=wilson(n10,n,z); p01=wilson(n01,n,z)
    return {'n':n,'n11':n11,'n10':n10,'n01':n01,'n00':n00,
            'source_minus_candidate_percentage_points':100*(n10-n01)/n,
            'observed_decline_percentage_points':100*(n10-n01)/n,
            'target_decline_at_most_one_percentage_point_observed':100*(n10-n01)/n<=1,
            'paired_difference_95pct_conservative_ci_percentage_points':
                [100*(p10[0]-p01[1]),100*(p10[1]-p01[0])],
            'ci_method':'Bonferroni 97.5% Wilson intervals on paired discordant rates; simultaneous >=95%'}

def sha(data): return hashlib.sha256(data).hexdigest()

def evaluate_suite(cases, results=None, manifest=None):
    natural=cases['natural']; ids=[c['task_id'] for c in natural]
    require(len(natural)>=200 and len(ids)==len(set(ids)), 'invalid_frozen_task_list')
    report={'task_count':len(ids),'control_count':len(cases['controls']),
            'freeze_sha256':(manifest or {}).get('tasks_sha256'), 'models':{}, 'pairs':{},
            'controls':{'status':'NOT_RUN','count':len(cases['controls']),
                        'note':'Controls/fault injection are not part of natural success-rate denominator.'}}
    if results is None:
        for role in ROLES: report['models'][role]={'status':'NOT_RUN','success_count':None,'wilson_95pct':None}
        for role in ROLES[1:]: report['pairs'][role]={'status':'NOT_RUN'}
        return report
    require(type(results) is dict and results.get('freeze_sha256')==(manifest or {}).get('tasks_sha256'),
            'results_freeze_hash_mismatch')
    require(results.get('rules_sha256')==(manifest or {}).get('rules_sha256'), 'results_rules_hash_mismatch')
    runs=results.get('runs',{})
    require(type(runs) is dict and set(runs)<=set(ROLES), 'results_unknown_model_role')
    for role in ROLES:
        run=runs.get(role)
        if not run or run.get('status')=='NOT_RUN':
            report['models'][role]={'status':'NOT_RUN','success_count':None,'wilson_95pct':None}
            continue
        require(run.get('status')=='RUN' and run.get('mode')=='real_generation',role+':no_real_model_provenance')
        identity=run.get('identity')
        require(type(identity) is dict and all(identity.get(k) for k in IDENTITY),role+':identity_missing')
        source=Path(identity['source_log_path']); require(source.is_file() and sha(source.read_bytes())==identity['source_log_sha256'],
                role+':source_log_absent_or_sha_mismatch')
        raw=run.get('cases')
        require(type(raw) is list and all(type(x) is dict for x in raw),role+':case_list_missing')
        supplied={x.get('task_id'):x for x in raw}
        require(len(supplied)==len(raw) and set(supplied)<=set(ids),role+':duplicate_or_unknown_case_id')
        rows=[]
        for case in natural:
            trace=supplied.get(case['task_id'])
            if trace is None:
                rows.append({'task_id':case['task_id'],'cohort':'natural','family':case['family'],
                             'passed':False,'errors':['missing_trace']})
            else: rows.append(score_case(case,trace))
        successes=sum(x['passed'] for x in rows)
        report['models'][role]={'status':'RUN','n':len(rows),'success_count':successes,
                                'success_rate':successes/len(rows),'wilson_95pct':wilson(successes,len(rows)),
                                'identity':identity,'rows':rows,
                                'failed_task_ids':[x['task_id'] for x in rows if not x['passed']],
                                'target_success_at_least_95pct_observed':successes/len(rows)>=.95}
    control_run=results.get('control_run')
    if control_run and control_run.get('status')=='RUN':
        raw=control_run.get('cases',[])
        require(type(raw) is list,'invalid_control_case_list')
        control_ids={c['task_id'] for c in cases['controls']}
        traces={c.get('task_id'):c for c in raw}
        require(len(traces)==len(raw) and set(traces)<=control_ids,'duplicate_or_unknown_control_id')
        control_rows=[]
        for c in cases['controls']:
            if c['task_id'] in traces: control_rows.append(score_control(c,traces[c['task_id']]))
            else: control_rows.append({'task_id':c['task_id'],'cohort':c['cohort'],'scenario':c['scenario'],
                                       'passed':False,'errors':['missing_control_trace']})
        report['controls']={'status':'RUN','count':len(control_rows),'rows':control_rows,
                            'success_count':sum(r['passed'] for r in control_rows),
                            'note':'Control evidence only; excluded from natural model success rates.'}
    for candidate in ROLES[1:]:
        baseline=report['models']['high_precision']; other=report['models'][candidate]
        report['pairs'][candidate]=(pair(baseline['rows'],other['rows'])
            if baseline['status']=='RUN' and other['status']=='RUN' else {'status':'NOT_RUN'})
    return report

def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--evaluation',type=Path,required=True)
    ap.add_argument('--results',type=Path,help='real model execution traces; omit to emit NOT_RUN')
    ap.add_argument('--output',type=Path,help='output must live within evaluation directory')
    args=ap.parse_args()
    folder=args.evaluation.resolve()
    sys.path.insert(0,str(folder))
    from freeze_tasks import verify
    manifest=verify(Path(__file__).resolve().parents[1])
    require(folder==(Path(__import__('freeze_tasks').ROOT).resolve()), 'evaluation_dir_mismatch')
    cases=json.loads((folder/'holdout_v1.json').read_text(encoding='utf-8'))
    raw=json.loads(args.results.read_text(encoding='utf-8')) if args.results else None
    report=evaluate_suite(cases,raw,manifest)
    if args.output:
        target=args.output.resolve()
        require(target.is_relative_to(folder), 'output_must_be_in_evaluation_directory')
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_text(json.dumps(report,ensure_ascii=False,sort_keys=True,indent=2,allow_nan=False)+'\n',encoding='utf-8')
    print(json.dumps({'task_count':report['task_count'],
                      'models':{k:{a:v.get(a) for a in ('status','success_count','wilson_95pct')}
                                for k,v in report['models'].items()},
                      'pairs':report['pairs'],'controls':report['controls']},ensure_ascii=False))

if __name__=='__main__':
    try:main()
    except (InvalidEvidence,ValueError,OSError) as exc:
        print('EVALUATION_INVALID:',exc,file=sys.stderr)
        raise SystemExit(2)
