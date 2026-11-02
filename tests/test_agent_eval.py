import importlib.util
import copy
import json
from pathlib import Path
import unittest

spec=importlib.util.spec_from_file_location('agent_eval',Path(__file__).resolve().parents[1]/'scripts'/'agent_eval.py')
evalmod=importlib.util.module_from_spec(spec);spec.loader.exec_module(evalmod)

def js(obj): return json.dumps(obj,ensure_ascii=False,separators=(',',':'))
def base_state():
    return dict(notes=[],records=[],timer='idle',label='',deadline_ms=0,remaining_ms=0,timer_revision=0,
                record_count=0,note_count=0)
def case(family,expected,pre='empty'):
    return dict(task_id='H26-TEST-001',cohort='natural',family=family,initial_state=pre,
                content_sha256='frozen-test-case-sha',expected=expected)
def record(cid,tool,request,text):
    return dict(id=cid,tool=tool,request=request,receipt=dict(code=0,result=text))
def toolturn(cid,tool,args,text,at=1000,code=0,idx=0,receipt_input=None):
    request=js(dict(name=tool,arguments=args))
    r=dict(generation=7,turn=idx,model_response=js(dict(type='tool',id=cid,request=request)),
           tool_event=dict(kind='tool',origin='ag_execute',id=cid,generation=7,request=request,
                           code=code,text=text,now_ms=at))
    if receipt_input:r['receipt_input']=receipt_input
    return r
def echoed(turn):
    e=turn['tool_event']
    return dict(kind='receipt',call_id=e['id'],code=e['code'],text=e['text'],generation=7)
def terminal(text,idx,previous=None,kind='finish'):
    r=dict(generation=7,turn=idx,model_response=js(dict(type=kind,text=text)),
           terminal_event=dict(kind='final' if kind=='finish' else 'clarification',generation=7,text=text))
    if previous:r['receipt_input']=echoed(previous)
    return r
def trace(c,turns,pre=None,post=None,status='finished'):
    return dict(task_id=c['task_id'],case_sha256=c['content_sha256'],generation=7,turns=turns,
                initial_state=pre or base_state(), final_state=post or base_state(),final_status=status)

class EvalTests(unittest.TestCase):
    def assertPass(self,c,t):
        r=evalmod.score_case(c,t);self.assertTrue(r['passed'],r['errors'])
    def assertFail(self,c,t,needle):
        r=evalmod.score_case(c,t);self.assertFalse(r['passed']);self.assertIn(needle,';'.join(r['errors']))
    def calc_save(self):
        c=case('calculate_save',dict(expression='17*9',value='153',tools=['calculate','save_result']))
        a=toolturn('real_calc_8','calculate',dict(expression='17*9'),'153')
        b=toolturn('save_2','save_result',dict(result_id='real_calc_8'),'1',idx=1,receipt_input=echoed(a))
        end=terminal('153 saved',2,b)
        post=base_state();post.update(notes=['153'],note_count=1,record_count=2,
            records=[record('real_calc_8','calculate',a['tool_event']['request'],'153'),
                     record('save_2','save_result',b['tool_event']['request'],'1')])
        return c,trace(c,[a,b,end],post=post)
    def test_dynamic_real_result_id_save_and_committed_state(self):
        c,t=self.calc_save();self.assertPass(c,t)
    def test_finish_cannot_substitute_real_calls(self):
        c,t=self.calc_save();t['turns']=[terminal('153 saved',0)];self.assertFail(c,t,'wrong_number_of_turns')
    def test_wrong_result_id_even_when_finish_claims_saved(self):
        c,t=self.calc_save();turn=t['turns'][1];turn['model_response']=js(dict(type='tool',id='save_2',
                              request=js(dict(name='save_result',arguments=dict(result_id='invented')))))
        turn['tool_event']['request']=evalmod.parse_response(turn['model_response'])[0]['request']
        t['final_state']['records'][1]['request']=turn['tool_event']['request']
        self.assertFail(c,t,'save_result_did_not_use_real_result_id')
    def test_missing_real_execution_and_corrupt_receipt(self):
        c,t=self.calc_save();del t['turns'][0]['tool_event'];self.assertFail(c,t,'missing_real_tool_event')
        c,t=self.calc_save();t['turns'][1]['receipt_input']['text']='wrong';self.assertFail(c,t,'receipt_not_echoed')
    def test_duplicate_call_id_and_commit_does_not_pass(self):
        c,t=self.calc_save();second=t['turns'][1];second['model_response']=js(dict(type='tool',id='real_calc_8',
                    request=js(dict(name='save_result',arguments=dict(result_id='real_calc_8')))))
        self.assertFail(c,t,'duplicate_call_id')
    def test_note_exact_once(self):
        c=case('append_note',dict(text='实验：保存原文。',tools=['append_note']))
        a=toolturn('n1','append_note',dict(text=c['expected']['text']),'1')
        post=base_state();post.update(notes=[c['expected']['text']],note_count=1,record_count=1,
                     records=[record('n1','append_note',a['tool_event']['request'],'1')])
        t=trace(c,[a,terminal('Note saved',1,a)],post=post);self.assertPass(c,t)
        t['final_state']['notes'].append('extra');t['final_state']['note_count']=2
        self.assertFail(c,t,'note_not_committed_exactly_once')
    def test_timer_start_checks_real_clock_and_revision(self):
        c=case('timer_start',dict(value=9,unit='分钟',label='Focus 1',duration_ms=540000,tools=['start_timer']))
        a=toolturn('timer_1','start_timer',dict(value=9,unit='分钟',label='Focus 1'),'540000',at=1000)
        post=base_state();post.update(timer='running',label='Focus 1',deadline_ms=541000,
                    remaining_ms=540000,timer_revision=1,record_count=1,
                    records=[record('timer_1','start_timer',a['tool_event']['request'],'540000')])
        t=trace(c,[a,terminal('Timer started',1,a)],post=post);self.assertPass(c,t)
        t['final_state']['deadline_ms']-=1000;self.assertFail(c,t,'timer_deadline_not_expected_duration')
    def test_set_existing_timer_requires_existing_committed_setup(self):
        c=case('timer_set',dict(value=45,unit='秒',duration_ms=45000,tools=['set_timer']),pre='running_timer')
        pre=base_state();pre.update(timer='running',label='Existing',timer_revision=1,
                   deadline_ms=7200000,remaining_ms=7200000,record_count=1,
                   records=[record('seed','start_timer',js(dict(name='start_timer',
                            arguments=dict(value=2,unit='小时',label='Existing'))),'7200000')])
        a=toolturn('set_1','set_timer',dict(value=45,unit='秒'),'45000',at=1000)
        post=copy.deepcopy(pre);post.update(timer_revision=2,deadline_ms=46000,
                       remaining_ms=45000,record_count=2)
        post['records'].append(record('set_1','set_timer',a['tool_event']['request'],'45000'))
        t=trace(c,[a,terminal('Timer changed',1,a)],pre,post);self.assertPass(c,t)
        t['initial_state']['records']=[];t['initial_state']['record_count']=0
        self.assertFail(c,t,'missing_real_timer_setup_record')
    def test_missing_duration_requires_clarification_without_mutation(self):
        c=case('timer_clarify',dict(question='How long?',tools=[]))
        t=trace(c,[terminal('How long?',0,kind='clarify')],status='clarify')
        self.assertPass(c,t)
        t['final_state']['timer']='running';self.assertFail(c,t,'unrequested_timer_change')
    def test_real_arithmetic_error_then_model_continues(self):
        c=case('error_continue',dict(expression='(51+17)/0',receipt_code=5,tools=['calculate']))
        a=toolturn('bad1','calculate',dict(expression='(51+17)/0'),'arithmetic',code=5)
        t=trace(c,[a,terminal('Arithmetic error: division by zero',1,a)])
        self.assertPass(c,t)
        t['turns'][1]['receipt_input']['code']=0;self.assertFail(c,t,'receipt_not_echoed')
    def test_native_invalid_duplicate_json_and_role_report(self):
        c,t=self.calc_save();t['turns'][0]['model_response']='{"type":"tool","type":"tool"}'
        self.assertFail(c,t,'duplicate_json_key')
        cases=dict(natural=[dict(c,task_id=f'H26-TEST-{i:03d}') for i in range(200)], controls=[])
        # A missing model is explicitly NOT_RUN, not a fabricated score.
        summary=evalmod.evaluate_suite(cases,None,dict(tasks_sha256='a'))
        self.assertEqual(summary['models']['high_precision']['status'],'NOT_RUN')
    def test_expression_equivalence_keeps_real_operations(self):
        self.assertTrue(evalmod.same_expression(' (9 * 17) ', '17*9'))
        self.assertFalse(evalmod.same_expression('153', '17*9'))
        self.assertFalse(evalmod.same_expression('17-9','9-17'))
    def test_controls_reject_illegal_output_and_cross_generation_mutation(self):
        for scenario,raw in [('invalid_json_response','not json'),
                              ('duplicate_json_key','{"type":"finish","type":"finish","text":"ok"}'),
                              ('invalid_tool_schema','{"type":"tool","id":"x","request":"{}"}')]:
            c=dict(task_id='C1',content_sha256='control-sha',cohort='fault_injection',scenario=scenario)
            t=dict(task_id='C1',case_sha256='control-sha',initial_state=base_state(),final_state=base_state(),
                   final_status='protocol_error',events=[],raw_response=raw)
            self.assertTrue(evalmod.score_control(c,t)['passed'])
            t['final_state']['notes']=['leak'];self.assertFalse(evalmod.score_control(c,t)['passed'])
        c=dict(task_id='C2',content_sha256='x',cohort='application_control',scenario='cancel_pending_late_result')
        t=dict(task_id='C2',case_sha256='x',generation=7,initial_state=base_state(),final_state=base_state(),
               final_status='cancelled',events=[dict(kind='cancel',generation=7,now_ms=1),
                     dict(kind='backend_result',generation=7,now_ms=2,accepted=False)])
        self.assertTrue(evalmod.score_control(c,t)['passed'])
        t['events'].append(dict(kind='reuse',retired_generation=7,now_ms=3))
        self.assertFalse(evalmod.score_control(c,t)['passed'])
    def test_wilson_and_paired_discordant_difference(self):
        a=[dict(task_id=f't{i}',passed=x) for i,x in enumerate([True,True,False,False])]
        b=[dict(task_id=f't{i}',passed=x) for i,x in enumerate([True,False,True,False])]
        p=evalmod.pair(a,b)
        self.assertEqual((p['n11'],p['n10'],p['n01'],p['n00']),(1,1,1,1))
        self.assertEqual(p['source_minus_candidate_percentage_points'],0)
        self.assertLess(evalmod.wilson(0,200)[0],evalmod.wilson(0,200)[1])

if __name__=='__main__':unittest.main()
