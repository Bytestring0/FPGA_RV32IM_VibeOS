"""Protocol fixtures only. Fake CLI output is never high_precision evidence."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('agent_bf16_eval', ROOT / 'scripts' / 'agent_bf16_eval.py')
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
TEST_ROOT = Path(os.environ.get('BF16_TEST_ROOT', str(mod.DEFAULT_OUTPUT_ROOT / 'unit_artifacts')))


def block(name, **args):
    return '<tool_call>\n<function=' + name + '>\n' + ''.join(
        '<parameter=' + key + '>\n' + str(value) + '\n</parameter>\n' for key, value in args.items()) + '</function>\n</tool_call>'


def fixture_case(prompt='Calculate 19+23 and save the result', initial='empty'):
    return {'task_id': 'OLD-REGRESSION-FIXTURE', 'cohort': 'natural', 'family': 'calculate_save',
            'prompt': prompt, 'initial_state': initial, 'content_sha256': 'unit-only-not-a-frozen-case'}


class ParserTests(unittest.TestCase):
    def test_exact_xml_to_nested_native_envelope(self):
        raw = block('calculate', expression='19+23')
        result = mod.strict_json(mod.native_to_envelope(raw, 7, 2))
        self.assertEqual(result, {'type': 'tool', 'id': 'c7_2',
                                 'request': '{"name":"calculate","arguments":{"expression":"19+23"}}'})
    def test_no_repair_rename_or_parameter_invention(self):
        for raw in ('calculate(expression="19+23")', '{"name":"calculate"}',
                    block('calculate', expression='1') + 'done',
                    block('calculate', expression='1').replace('\n', '\r\n'),
                    '<tool_call>\n<function=calculate>\n<parameter=expression>\n1\n</parameter>\n<parameter=expression>\n2\n</parameter>\n</function>\n</tool_call>',
                    block('finish'), block('clarify', text='How long?')):
            with self.subTest(raw=raw), self.assertRaises(mod.ProtocolError):
                mod.native_to_envelope(raw, 1, 0)
        # Unknown tool and missing tool parameters stay unchanged for real C rejection.
        result = mod.strict_json(mod.native_to_envelope(block('calculator'), 1, 0))
        self.assertEqual(mod.strict_json(result['request']), {'name': 'calculator', 'arguments': {}})
    def test_numeric_subset_lexeme_and_verbatim_string(self):
        raw = block('start_timer', value='2.00', unit='分钟', label=' x & y\nline ')
        envelope = mod.strict_json(mod.native_to_envelope(raw, 1, 0))
        self.assertIn('"value":2.00', envelope['request'])
        self.assertEqual(mod.strict_json(envelope['request'])['arguments']['label'], ' x & y\nline ')
        for number in ('2e2', '+2', '02', 'NaN', 'true', '2 ', '.5'):
            with self.subTest(number=number), self.assertRaises(mod.ProtocolError):
                mod.native_to_envelope(block('set_timer', value=number, unit='秒'), 1, 0)
    def test_native_optional_reasoning_and_terminal_bounds(self):
        self.assertEqual(mod.strict_json(mod.native_to_envelope('Reasoning\n' + block('finish', text='42 saved'), 1, 2)),
                         {'type': 'finish', 'text': '42 saved'})
        for prefix in ('<function=evil>\n', 'x' * 513):
            with self.assertRaises(mod.ProtocolError):
                mod.native_to_envelope(prefix + block('finish', text='ok'), 1, 0)
        with self.assertRaises(mod.ProtocolError):
            mod.native_to_envelope(block('finish', text='x' * 97), 1, 0)
    def test_untrusted_framing_and_strict_json(self):
        for text in ('<|im_end|>', '<tool_response>data', 'x\0y'):
            with self.assertRaises(mod.ProtocolError):
                mod.task_fragment(text)
        for raw in ('{"a":1,"a":2}', '{"a":NaN}'):
            with self.assertRaises(mod.ProtocolError):
                mod.strict_json(raw)
    def test_receipt_uses_real_id_and_exact_code(self):
        event = {'id': 'c1_0', 'request': '{"name":"calculate","arguments":{"expression":"19+23"}}',
                 'code': 0, 'text': '42', 'result_id': 'c1_0'}
        expected = '\n<|im_start|>user\n<tool_response>\n{"ok":true,"code":"ok","text":"42","call_id":"c1_0","result_id":"c1_0"}\n</tool_response><|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'
        self.assertEqual(mod.receipt_fragment(event), expected)
        del event['result_id']
        with self.assertRaises(mod.ProtocolError):
            mod.receipt_fragment(event)
        event.update(code=5, text='arithmetic')
        self.assertNotIn('result_id', mod.receipt_fragment(event))
    def test_expected_never_reaches_model_input(self):
        case = dict(fixture_case(), expected={'value': 'SECRET-ANSWER'})
        self.assertNotIn('expected', mod.select_cases({'natural': [case]}, [case['task_id']])[0])
        with self.assertRaises(mod.ProtocolError):
            mod.select_cases({'natural': [case]}, [])
        with self.assertRaises(mod.ProtocolError):
            mod.select_cases({'natural': [case]}, ['missing'])
        with self.assertRaises(mod.ProtocolError):
            mod.run_case(case, None, None, None, [])
    def test_only_loopback_server_allowed(self):
        for url in ('https://127.0.0.1:8000', 'http://example.com:8000', 'http://127.0.0.1:8000/chat', 'http://u@localhost:80'):
            with self.assertRaises(mod.ProtocolError):
                mod.LlamaServer(url, None)


FAKE_CLI = r'''
import argparse,json
from http.server import BaseHTTPRequestHandler,HTTPServer
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--port',type=int);p.add_argument('--fixtures');p.add_argument('--requests');p.add_argument('--version',action='store_true');a=p.parse_args()
if a.version:
 print('FAKE PROTOCOL FIXTURE b11835-f64a49819');raise SystemExit(0)
fixtures=json.loads(Path(a.fixtures).read_text(encoding='utf-8'));index=0
class Handler(BaseHTTPRequestHandler):
 def log_message(self,*args):pass
 def do_GET(self):
  value={'status':'ok'} if self.path=='/health' else {'model_path':'FAKE_ONLY','build_info':'b11835-f64a49819'}
  data=json.dumps(value).encode();self.send_response(200);self.end_headers();self.wfile.write(data)
 def do_POST(self):
  global index
  value=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
  with open(a.requests,'a',encoding='utf-8') as stream:stream.write(json.dumps(value)+'\n')
  result=fixtures[index];index+=1
  data=json.dumps(result).encode();self.send_response(200);self.end_headers();self.wfile.write(data)
HTTPServer(('127.0.0.1',a.port),Handler).serve_forever()
'''


class ArtifactTests(unittest.TestCase):
    def setUp(self):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        self.temp = tempfile.TemporaryDirectory(prefix='bf16-protocol-', dir=TEST_ROOT)
        self.folder = Path(self.temp.name)
    def tearDown(self):
        self.temp.cleanup()
    def test_prefix_checks_all_bytes_counts_and_ids(self):
        system = b'<|im_start|>system\nfixture<|im_end|>\n'
        ids = [1, 2, 248046]
        packed = struct.pack('<4I', len(ids), *ids)
        token = b'TOKEN-FIXTURE'
        metadata = {'utf8_sha256': hashlib.sha256(system).hexdigest(),
                    'tokenizer_bin_sha256': hashlib.sha256(token).hexdigest(),
                    'token_ids_file_sha256': hashlib.sha256(packed).hexdigest(),
                    'token_ids_sha256': hashlib.sha256(packed[4:]).hexdigest(), 'token_count': 3}
        for name, data in [('SYSTEM.txt', system), ('PREFIX.BIN', packed), ('TOKEN.BIN', token)]:
            (self.folder / name).write_bytes(data)
        mod.write_json(self.folder / 'PROMPT.JSON', metadata)
        args = [self.folder / name for name in ('SYSTEM.txt', 'PREFIX.BIN', 'PROMPT.JSON', 'TOKEN.BIN')]
        self.assertEqual(mod.read_prefix(*args)[1], ids)
        (self.folder / 'SYSTEM.txt').write_bytes(system + b' ')
        with self.assertRaises(mod.ProtocolError):
            mod.read_prefix(*args)
    def test_q4_or_dequantized_candidate_is_not_official_bf16(self):
        candidate = self.folder / 'w4_candidate_text_bf16.gguf'
        candidate.write_bytes(b'not official')
        manifest = self.folder / 'release_manifest.json'
        mod.write_json(manifest, {'files': [{'path': candidate.name, 'sha256': 'wrong', 'bytes': candidate.stat().st_size}]})
        with self.assertRaises(mod.ProtocolError):
            mod.verify_official_model(candidate, manifest)
    def start_fake(self, responses):
        script = self.folder / 'fake_cli.py'
        script.write_text(FAKE_CLI, encoding='utf-8')
        fixture = self.folder / 'responses.json'
        mod.write_json(fixture, responses)
        requests = self.folder / 'requests.jsonl'
        self.log = mod.EvidenceLog(self.folder / 'unit-native.jsonl')
        self.log.add('test_only', mode='protocol_fixture', real_generation=False)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        self.server_file = (self.folder / 'fake-server.log').open('wb')
        command = [sys.executable, '-B', str(script), '--port', str(port), '--fixtures', str(fixture), '--requests', str(requests)]
        self.fake = subprocess.Popen(command, stdout=self.server_file, stderr=self.server_file)
        self.client = mod.LlamaServer(f'http://127.0.0.1:{port}', self.log, timeout=5)
        for _ in range(100):
            try:
                self.client.request('/health')
                return requests
            except (OSError, mod.ProtocolError):
                if self.fake.poll() is not None:
                    raise RuntimeError('fake CLI exited')
                time.sleep(0.02)
        raise RuntimeError('fake CLI startup timed out')
    def stop_fake(self):
        self.fake.terminate()
        self.fake.wait(timeout=5)
        self.server_file.close()
        self.log.close()
    def test_fake_cli_raw_completion_protocol(self):
        raw = block('calculate', expression='19+23')
        requests = self.start_fake([{'content': raw, 'tokens': [6, 248046], 'stop_type': 'eos', 'truncated': False}])
        try:
            text, tokens, _ = self.client.complete([1, 2, 3], 17)
            self.assertEqual((text, tokens), (raw, [6, 248046]))
            request = json.loads(requests.read_text())
            self.assertEqual(request['prompt'], [1, 2, 3])
            self.assertEqual(request['n_predict'], 17)
            self.assertFalse(request['cache_prompt'])
            self.assertNotIn('messages', request)
        finally:
            self.stop_fake()
    def test_wrong_stop_or_truncation_fails_closed(self):
        self.start_fake([{'content': block('finish', text='ok'), 'tokens': [4], 'stop_type': 'limit', 'truncated': False},
                         {'content': block('finish', text='ok'), 'tokens': [4, 248046], 'stop_type': 'eos', 'truncated': True}])
        try:
            for _ in range(2):
                with self.assertRaises(mod.ProtocolError):
                    self.client.complete([1], 4)
        finally:
            self.stop_fake()
    def test_fake_tokenizer_requires_exact_id_array(self):
        script = self.folder / 'fake_tokenizer.py'
        script.write_text('print("[1, 2, 3]")\n', encoding='utf-8')
        log = mod.EvidenceLog(self.folder / 'tokenizer-log.jsonl')
        try:
            tokenizer = mod.Tokenizer([sys.executable, '-B', str(script)], 'FAKE-MODEL', self.folder, log)
            self.assertEqual(tokenizer.encode('汉字\n'), [1, 2, 3])
            self.assertEqual((self.folder / 'tokenize-000001.utf8').read_bytes(), '汉字\n'.encode())
            script.write_text('print("banner [1, 2, 3]")\n', encoding='utf-8')
            with self.assertRaises((mod.ProtocolError, ValueError)):
                tokenizer.encode('x')
        finally:
            log.close()


@unittest.skipUnless(os.environ.get('BF16_BROKER_COMMAND'), 'set BF16_BROKER_COMMAND for real C tool integration')
class RealBrokerTests(unittest.TestCase):
    setUp = ArtifactTests.setUp
    tearDown = ArtifactTests.tearDown
    def open_broker(self):
        self.log = mod.EvidenceLog(self.folder / 'broker-native.jsonl')
        self.log.add('test_only', mode='protocol_fixture', real_generation=False)
        self.broker = mod.Broker(json.loads(os.environ['BF16_BROKER_COMMAND']), self.folder, self.log)
    def close_broker(self):
        self.broker.close()
        self.log.close()
    def test_real_c_calculate_save_multi_round_state_and_receipt(self):
        self.open_broker()
        class Tokenizer:
            def __init__(self): self.texts = []
            def encode(self, text): self.texts.append(text); return [1, 2]
        class FixtureBackend:
            def __init__(self): self.turn = 0; self.ids = []
            def complete(self, ids, limit):
                self.ids.append(list(ids))
                values = [block('calculate', expression='19+23'), block('save_result', result_id='c1_0'), block('finish', text='42 saved')]
                raw = values[self.turn]; self.turn += 1
                return raw, [4, 248046], {'stop_type': 'eos', 'truncated': False}
        tokenizer, backend = Tokenizer(), FixtureBackend()
        try:
            trace = mod.run_case(fixture_case(), self.broker, backend, tokenizer, [10])
            self.assertEqual(trace['final_status'], 'finished', trace)
            self.assertEqual(trace['final_state']['notes'], ['42'])
            self.assertEqual(trace['final_state']['record_count'], 2)
            self.assertEqual(trace['turns'][0]['tool_event']['text'], '42')
            self.assertEqual(trace['turns'][1]['receipt_input']['call_id'], 'c1_0')
            self.assertEqual(trace['turns'][2]['receipt_input']['call_id'], 'c1_1')
            self.assertEqual(trace['turns'][2]['terminal_event']['kind'], 'final')
            self.assertIn('"result_id":"c1_0"', tokenizer.texts[1])
            self.assertEqual(backend.ids[1], [10, 1, 2, 4, 248046, 1, 2])
        finally:
            self.close_broker()
    def test_real_c_rejects_unknown_tool_and_preserves_error_receipt(self):
        self.open_broker()
        try:
            self.broker.reset('unsupported fixture', 10000, 16)
            unknown = mod.native_to_envelope(block('calculator', expression='19+23'), 1, 0)
            reply = self.broker.step(1, unknown)
            self.assertEqual(reply['event']['code'], 3)
            self.assertEqual(reply['final_state']['record_count'], 0)
            malformed = self.broker.step(2, 'calculate(expression="19+23")')
            self.assertEqual(malformed['status'], 'protocol_error')
            self.assertEqual(malformed['event']['kind'], 'error')
            self.assertEqual(malformed['initial_state'], malformed['final_state'])
        finally:
            self.close_broker()
    def test_raw_json_cannot_bypass_native_xml_parser(self):
        self.open_broker()
        class Tokenizer:
            def encode(self, text): return [1]
        class RawJsonBackend:
            def complete(self, ids, limit):
                raw = mod.dumps({'type':'tool','id':'bypass','request':mod.dumps({'name':'append_note','arguments':{'text':'must not commit'}})})
                return raw, [4, 248046], {'stop_type':'eos'}
        try:
            trace = mod.run_case(fixture_case('JSON bypass fixture'), self.broker, RawJsonBackend(), Tokenizer(), [10])
            self.assertEqual(trace['final_status'], 'protocol_error', trace)
            self.assertEqual(trace['final_state']['notes'], [])
            self.assertEqual(trace['final_state']['record_count'], 0)
            self.assertEqual(trace['turns'][0]['error_event']['kind'], 'error')
            self.assertEqual(trace['turns'][0]['model_response'], '')
            self.assertIn('parse_error', trace['turns'][0])
        finally:
            self.close_broker()
    def test_turn_limit_is_real_c_event_with_commit_retained(self):
        self.open_broker()
        class Tokenizer:
            def encode(self, text): return [1]
        class NoteBackend:
            def complete(self, ids, limit):
                return block('append_note', text='committed'), [4,248046], {'stop_type':'eos'}
        try:
            trace = mod.run_case(fixture_case('Write committed'), self.broker, NoteBackend(), Tokenizer(), [10], max_turns=1)
            self.assertEqual(trace['final_status'], 'max_turns', trace)
            self.assertEqual(trace['final_state']['notes'], ['committed'])
            self.assertEqual(trace['loop_limit_event']['kind'], 'error')
            self.assertEqual(trace['loop_limit_event']['text'], 'max_turns')
        finally:
            self.close_broker()
    def test_real_c_timer_setup_and_duplicate_conflict(self):
        self.open_broker()
        try:
            self.broker.reset('timer fixture', 10000, 16)
            setup = self.broker.setup(1, 'preset', mod.dumps({'name':'start_timer','arguments':{'value':120,'unit':'分钟','label':'Existing'}}))
            self.assertEqual(setup['event']['code'], 0)
            self.assertEqual(setup['final_state']['deadline_ms'], 7200001)
            note = mod.dumps({'type':'tool','id':'same','request':mod.dumps({'name':'append_note','arguments':{'text':'actual'}})})
            first = self.broker.step(2, note)
            second = self.broker.step(3, note)
            self.assertEqual(first['final_state']['notes'], ['actual'])
            self.assertEqual(second['final_state']['notes'], ['actual'])
            changed = mod.dumps({'type':'tool','id':'same','request':mod.dumps({'name':'append_note','arguments':{'text':'different'}})})
            conflict = self.broker.step(4, changed)
            self.assertEqual(conflict['event']['code'], 6)
            self.assertEqual(conflict['final_state']['notes'], ['actual'])
        finally:
            self.close_broker()


if __name__ == '__main__':
    unittest.main()
