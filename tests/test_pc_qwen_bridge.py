"""Host protocol tests using explicitly scripted HTTP responses; not real-model evidence."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('pc_qwen_bridge', Path(__file__).parents[1] / 'scripts' / 'pc_qwen_bridge.py')
bridge = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(bridge)


def call(name, args, native_id='native-1'):
    return {'choices': [{'finish_reason': 'tool_calls', 'message': {'role': 'assistant', 'content': '',
        'tool_calls': [{'id': native_id, 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(args)}}]}}]}


def request(seq=1, task_id=1, **kw):
    result = {'version': 1, 'task_id': task_id, 'sequence': seq, 'kind': 'task',
              'task': '计算7*8并保存结果', 'call_id': '', 'receipt_code': 0, 'receipt_text': ''}
    result.update(kw)
    return result


class ScriptedModel:
    def __init__(self, responses):
        self.responses = list(responses)
        self.requests = []

    def request(self, endpoint, body, raw_path):
        if endpoint == '/apply-template':
            result = {'prompt': 'SCRIPTED TEMPLATE FOR UNIT TEST ONLY'}
        else:
            self.requests.append(body)
            result = self.responses.pop(0)
            if isinstance(result, Exception):
                raise result
        raw_path.write_text(json.dumps(result), encoding='utf-8')
        return result


class ProtocolTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def make(self, responses, retries=1):
        model = ScriptedModel(responses)
        return bridge.Bridge(model, self.root / 'trace', retries=retries), model

    def test_main_recovers_sharing_violation_then_processes_once(self):
        channel, trace = self.root / 'channel', self.root / 'cli-trace'
        channel.mkdir()
        raw = bridge.dumps(request()).encode('utf-8')
        model = ScriptedModel([call('calculate', {'expression': '7*8'})])
        inference_request = model.request
        def model_request(endpoint, body, raw_path):
            if body is None:
                return {'status': 'ok'}
            return inference_request(endpoint, body, raw_path)
        model.request = model_request
        original = Path.read_bytes
        reads = [PermissionError('Windows sharing violation'), raw]
        def read_bytes(path):
            if path == channel / 'request.json':
                value = reads.pop(0)
                if isinstance(value, Exception):
                    raise value
                return value
            return original(path)
        argv = ['pc_qwen_bridge.py', '--channel', str(channel), '--trace', str(trace), '--once']
        with patch.object(bridge.sys, 'argv', argv), patch.object(bridge, 'HTTPModel', return_value=model), \
             patch.object(Path, 'read_bytes', read_bytes), patch.object(bridge.time, 'sleep') as backoff:
            self.assertEqual(bridge.main(), 0)
        self.assertEqual(len(model.requests), 1)
        backoff.assert_called_once_with(.02)
        stats = json.loads((trace / 'channel-read-stats.json').read_text(encoding='utf-8'))
        self.assertEqual(stats['permission_errors'], 1)
        self.assertEqual(stats['recoveries'], 1)
        self.assertEqual(json.loads((channel / 'response-1-1.json').read_text(encoding='utf-8'))['status'], 'ready')

    def test_persistent_sharing_denial_fails_at_bound(self):
        trace = self.root / 'denial-trace'
        trace.mkdir()
        reader = bridge.ChannelReader(self.root / 'request.json', trace)
        with patch.object(Path, 'read_bytes', side_effect=PermissionError('sharing violation')), \
             patch.object(bridge.time, 'monotonic', side_effect=[10, 100]), patch.object(bridge.time, 'sleep') as backoff:
            self.assertIsNone(reader.read())
            with self.assertRaises(TimeoutError):
                reader.read()
        backoff.assert_called_once_with(.02)
        stats = json.loads((trace / 'channel-read-stats.json').read_text(encoding='utf-8'))
        self.assertEqual(stats['status'], 'persistent_read_denial')
        self.assertEqual(stats['permission_errors'], 2)

    def test_missing_request_is_idle_not_a_denial(self):
        trace = self.root / 'missing-trace'
        trace.mkdir()
        reader = bridge.ChannelReader(self.root / 'request.json', trace)
        with patch.object(Path, 'read_bytes', side_effect=[FileNotFoundError(), b'valid']), patch.object(bridge.time, 'sleep'):
            self.assertIsNone(reader.read())
            self.assertEqual(reader.read(), b'valid')
        self.assertEqual(reader.missing_reads, 1)
        self.assertIsNone(reader.denied_since)

    def test_native_to_existing_rv32_string_request(self):
        normalized, _, _ = bridge.normalize(call('calculate', {'expression': '7*8'}), 'c1_1')
        self.assertIsInstance(normalized['request'], str)
        self.assertEqual(json.loads(normalized['request']), {'name': 'calculate', 'arguments': {'expression': '7*8'}})

    def test_real_receipt_identity_and_save_reference(self):
        worker, model = self.make([call('calculate', {'expression': '7*8'}),
                                  call('save_result', {'result_id': 'c1_1'}, 'native-2'),
                                  call('finish', {'text': '56，已保存'}, 'native-3')])
        first = worker.process(request())
        self.assertEqual(first['status'], 'ready')
        second = worker.process(request(2, kind='receipt', call_id='c1_1', receipt_text='56'))
        self.assertEqual(second['status'], 'ready')
        feedback = model.requests[1]['messages'][-1]
        self.assertEqual(feedback['tool_call_id'], 'native-1')
        self.assertEqual(json.loads(feedback['content'])['result_id'], 'c1_1')
        self.assertEqual(json.loads(feedback['content'])['text'], '56')
        final = worker.process(request(3, kind='receipt', call_id='c1_2', receipt_text='1'))
        self.assertEqual(json.loads(final['response']), {'type': 'finish', 'text': '56，已保存'})
        self.assertEqual(len(model.requests), 3)

    def test_replay_does_not_infer_twice_and_conflict_fails(self):
        worker, model = self.make([call('calculate', {'expression': '7*8'})])
        first = worker.process(request())
        self.assertEqual(first, worker.process(request()))
        self.assertEqual(len(model.requests), 1)
        self.assertEqual(worker.process(request(task='different'))['status'], 'failed')

    def test_mismatched_and_late_receipt_are_rejected(self):
        worker, model = self.make([call('calculate', {'expression': '7*8'}), call('clarify', {'question': '请输入时长'})])
        worker.process(request())
        bad = worker.process(request(2, kind='receipt', call_id='wrong', receipt_text='56'))
        self.assertEqual(bad['status'], 'failed')
        self.assertEqual(len(model.requests), 1)
        self.assertEqual(worker.process(request(1, task_id=2, task='设置计时'))['status'], 'ready')
        self.assertEqual(worker.process(request(3, kind='receipt', call_id='c1_1', receipt_text='56'))['status'], 'failed')
        self.assertEqual(len(model.requests), 2)

    def test_one_retry_has_raw_rejections_and_no_tool_execution(self):
        worker, model = self.make([call('wrong', {}), call('calculate', {'expression': '1/0'})])
        result = worker.process(request())
        self.assertEqual(result['status'], 'ready')
        self.assertEqual(len(model.requests), 2)
        self.assertTrue((self.root / 'trace/task-1/turn-1/attempt-0-rejected.json').exists())
        self.assertEqual(json.loads(json.loads(result['response'])['request'])['arguments']['expression'], '1/0')

    def test_unavailable_service_no_fallback(self):
        worker, model = self.make([OSError('unreachable')])
        result = worker.process(request())
        self.assertEqual(result['status'], 'failed')
        self.assertEqual(result['response'], '')
        self.assertIn('unreachable', result['error'])
        self.assertEqual(len(model.requests), 1)

    def test_utf8_limits_nonfinite_unknown_fields_and_truncation(self):
        for response in [call('finish', {'text': '中' * 33}), call('start_timer', {'value': float('nan'), 'unit': '秒', 'label': 'x'}),
                         call('calculate', {'expression': '1+2', 'extra': 'bad'})]:
            with self.assertRaises(bridge.ProtocolError):
                bridge.normalize(response, 'c1')
        response = call('finish', {'text': '56'})
        response['choices'][0]['finish_reason'] = 'length'
        with self.assertRaises(bridge.ProtocolError):
            bridge.normalize(response, 'c1')
        with self.assertRaises(bridge.ProtocolError):
            bridge.strict_json('{"x":1,"x":2}')

    def test_sequence_gap_and_task_boundary(self):
        worker, model = self.make([call('calculate', {'expression': '7*8'}), call('clarify', {'question': '设置几分钟？'})])
        worker.process(request())
        self.assertEqual(worker.process(request(3, kind='receipt', call_id='c1_1', receipt_text='56'))['status'], 'failed')
        worker.process(request(1, task_id=2, task='设置计时'))
        self.assertEqual(len(model.requests[1]['messages']), 2)
        self.assertEqual(model.requests[1]['messages'][0], {'role': 'system', 'content': bridge.SYSTEM})
        self.assertEqual(model.requests[1]['messages'][1], {'role': 'user', 'content': '设置计时'})


if __name__ == '__main__':
    unittest.main()
