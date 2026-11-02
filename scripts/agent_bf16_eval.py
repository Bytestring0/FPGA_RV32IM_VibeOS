#!/usr/bin/env python3
"""Offline official-BF16 Agent runner; tools and terminal events come from real C.

The server uses raw /completion with exact token IDs, never a chat template.
No scoring, answer extraction, argument repair or scripted tool results live here.
Only explicit --case-id selections or --all-natural run model tasks.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import queue
import re
import socket
import struct
import subprocess
import threading
import time
import urllib.error
import urllib.request
from urllib.parse import urlsplit

OFFICIAL_GGUF_SHA256 = '8eede05870d39309724ccbfed488a9d8a77424a5128c86474d12e9d241ad58d0'
OFFICIAL_GGUF_BYTES = 1557662720
SOURCE_REVISION = '2fc06364715b967f1860aea9cf38778875588b17'
SOURCE_CHECKPOINT_SHA256 = '04b1c301231dd422b8860db31311ab2721511346a32cb1e079c4c4e5f1fe4696'
DEFAULT_OUTPUT_ROOT = Path(r'E:\FPGA_RV32IM_Work\deepseek_agent_accel_20261102\high_precision')
CODES = ('ok', 'bad_json', 'bad_args', 'unsupported', 'limit', 'arithmetic',
         'conflict', 'not_found', 'timer_state', 'storage')
NAME = re.compile(r'[A-Za-z_][A-Za-z_0-9]{0,30}')
NUMBER = re.compile(r'-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?')
VOCAB_SIZE = 248320
NATIVE_STOP_TOKEN = 248046


class ProtocolError(ValueError):
    pass


def dumps(value):
    return json.dumps(value, ensure_ascii=False, separators=(',', ':'), allow_nan=False)


def strict_json(raw):
    def pairs(items):
        out = {}
        for key, value in items:
            if key in out:
                raise ProtocolError('duplicate JSON key: ' + key)
            out[key] = value
        return out
    def invalid(value):
        raise ProtocolError('nonfinite JSON number: ' + value)
    return json.loads(raw, object_pairs_hook=pairs, parse_constant=invalid)


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False) + '\n', encoding='utf-8')


def text_bytes(value, maximum, name, empty=True):
    if not isinstance(value, str) or '\0' in value or (not empty and not value):
        raise ProtocolError(name + ': invalid text')
    try:
        encoded = value.encode('utf-8')
    except UnicodeError as exc:
        raise ProtocolError(name + ': invalid UTF-8') from exc
    if len(encoded) > maximum:
        raise ProtocolError(name + ': byte limit')
    return encoded


def native_to_envelope(raw, generation, turn):
    """Match ag_qw_backend.c native_to_json, preserving raw values verbatim."""
    text_bytes(raw, 2048, 'native response', empty=False)
    start = raw.find('<tool_call>')
    if start < 0 or len(raw[:start].encode('utf-8')) > 512:
        raise ProtocolError('missing native block or reasoning prefix too long')
    if any(tag in raw[:start] for tag in ('<tool_call', '</tool_call', '<function=', '<parameter=')):
        raise ProtocolError('protocol tags in reasoning')
    at = start
    def take(literal):
        nonlocal at
        if not raw.startswith(literal, at):
            raise ProtocolError('expected native framing: ' + repr(literal))
        at += len(literal)
    def name():
        nonlocal at
        match = NAME.match(raw, at)
        if not match:
            raise ProtocolError('invalid native name')
        at = match.end()
        return match.group(0)
    take('<tool_call>\n<function=')
    tool = name()
    take('>\n')
    args = {}
    while not raw.startswith('</function>', at):
        if len(args) == 3:
            raise ProtocolError('too many parameters')
        take('<parameter=')
        key = name()
        if key in args:
            raise ProtocolError('duplicate native parameter')
        take('>\n')
        end = raw.find('\n</parameter>', at)
        if end < 0:
            raise ProtocolError('unterminated parameter')
        value = raw[at:end]
        at = end
        take('\n</parameter>\n')
        if key == 'value' and tool in ('start_timer', 'set_timer', 'extend_timer'):
            if len(value) > 24 or not NUMBER.fullmatch(value):
                raise ProtocolError('timer value is outside native numeric subset')
            number = strict_json(value)
            if isinstance(number, float) and not math.isfinite(number):
                raise ProtocolError('nonfinite timer value')
            # Preserve the original numeric lexeme, including trailing zeroes.
            args[key] = ('number', value)
        else:
            args[key] = ('text', value)
    take('</function>\n</tool_call>')
    if any(char not in ' \n\r\t' for char in raw[at:]):
        raise ProtocolError('suffix after native tool block')
    if tool in ('finish', 'clarify'):
        wanted = 'text' if tool == 'finish' else 'question'
        if set(args) != {wanted}:
            raise ProtocolError('terminal parameter mismatch')
        value = args[wanted][1]
        text_bytes(value, 96, 'terminal text')
        envelope = {'type': tool, 'text': value}
    else:
        pieces = [dumps(key) + ':' + (value if kind == 'number' else dumps(value))
                  for key, (kind, value) in args.items()]
        request = '{"name":' + dumps(tool) + ',"arguments":{' + ','.join(pieces) + '}}'
        text_bytes(request, 512, 'native inner request', empty=False)
        envelope = {'type': 'tool', 'id': f'c{generation}_{turn}', 'request': request}
    encoded = dumps(envelope)
    text_bytes(encoded, 768, 'ag_loop envelope', empty=False)
    return encoded


def trusted_fragment(text):
    text_bytes(text, 512, 'task', empty=False)
    if any(tag in text for tag in ('<|', '<tool_call', '</tool_call', '<tool_response', '</tool_response')):
        raise ProtocolError('untrusted framing in task or receipt')
    return text


def task_fragment(prompt):
    return '<|im_start|>user\n' + trusted_fragment(prompt) + '<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'


def receipt_fragment(event):
    code = event['code']
    if type(code) is not int or not 0 <= code < len(CODES):
        raise ProtocolError('unknown receipt code')
    trusted_fragment(event['text'])
    receipt = {'ok': code == 0, 'code': CODES[code], 'text': event['text'], 'call_id': event['id']}
    if code == 0 and strict_json(event['request'])['name'] == 'calculate':
        if event.get('result_id') != event['id']:
            raise ProtocolError('successful calculate lacks real recorded result_id')
        receipt['result_id'] = event['result_id']
    return '\n<|im_start|>user\n<tool_response>\n' + dumps(receipt) + '\n</tool_response><|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n'


def valid_ids(value):
    if not isinstance(value, list) or not value or any(type(t) is not int or not 0 <= t < VOCAB_SIZE for t in value):
        raise ProtocolError('invalid or empty token ID array')
    return value


def read_prefix(system_path, prefix_path, metadata_path, token_bin):
    metadata = strict_json(Path(metadata_path).read_text(encoding='utf-8'))
    system = Path(system_path).read_bytes()
    if hashlib.sha256(system).hexdigest() != metadata['utf8_sha256']:
        raise ProtocolError('frozen system UTF-8 hash mismatch')
    if sha256(token_bin) != metadata['tokenizer_bin_sha256']:
        raise ProtocolError('frozen TOKEN.BIN identity mismatch')
    prefix = Path(prefix_path).read_bytes()
    if len(prefix) < 8 or len(prefix) % 4 or hashlib.sha256(prefix).hexdigest() != metadata['token_ids_file_sha256']:
        raise ProtocolError('frozen PREFIX.BIN identity mismatch')
    words = struct.unpack('<' + 'I' * (len(prefix) // 4), prefix)
    if words[0] != len(words) - 1 or words[0] != metadata['token_count']:
        raise ProtocolError('frozen prefix count mismatch')
    ids = valid_ids(list(words[1:]))
    if hashlib.sha256(prefix[4:]).hexdigest() != metadata['token_ids_sha256']:
        raise ProtocolError('frozen prefix token ID hash mismatch')
    return system.decode('utf-8'), ids, metadata


def verify_official_model(model, manifest_path):
    model = Path(model).resolve()
    manifest_path = Path(manifest_path).resolve()
    manifest = strict_json(manifest_path.read_text(encoding='utf-8'))
    found = [entry for entry in manifest['files']
             if entry.get('sha256') == OFFICIAL_GGUF_SHA256
             and entry.get('bytes') == OFFICIAL_GGUF_BYTES
             and (manifest_path.parent / entry['path']).resolve() == model]
    if len(found) != 1 or model.stat().st_size != OFFICIAL_GGUF_BYTES:
        raise ProtocolError('model must be the manifest-bound official BF16 GGUF')
    return {'model_sha256': OFFICIAL_GGUF_SHA256, 'model_bytes': OFFICIAL_GGUF_BYTES,
            'model_path': str(model), 'model_mtime_ns': model.stat().st_mtime_ns,
            'model_hash_evidence': 'existing release manifest + current stat; payload not rehashed',
            'model_manifest_path': str(manifest_path), 'model_manifest_sha256': sha256(manifest_path),
            'source_revision': SOURCE_REVISION, 'source_checkpoint_sha256': SOURCE_CHECKPOINT_SHA256}


class EvidenceLog:
    def __init__(self, path):
        self.path = Path(path)
        self.stream = self.path.open('x', encoding='utf-8', newline='\n')
    def add(self, kind, **fields):
        self.stream.write(dumps({'kind': kind, **fields}) + '\n')
        self.stream.flush()
    def close(self):
        self.stream.close()


class Tokenizer:
    def __init__(self, command, model, output, log, timeout=120):
        self.command, self.model, self.output, self.log, self.timeout = list(command), str(model), Path(output), log, timeout
        self.count = 0
    def encode(self, text):
        self.count += 1
        stem = self.output / ('tokenize-%06d' % self.count)
        path = stem.with_suffix('.utf8')
        path.write_bytes(text.encode('utf-8'))
        # b11835 TOKENIZE defaults parse_special=true; its --parse-special flag is IMATRIX-only.
        command = self.command + ['-m', self.model, '-f', str(path), '--ids', '--no-bos', '--no-escape', '--offline', '--log-disable']
        result = subprocess.run(command, capture_output=True, timeout=self.timeout)
        stem.with_suffix('.stdout').write_bytes(result.stdout)
        stem.with_suffix('.stderr').write_bytes(result.stderr)
        self.log.add('tokenizer', command=command, returncode=result.returncode,
                     input_sha256=sha256(path), stdout_sha256=hashlib.sha256(result.stdout).hexdigest())
        if result.returncode:
            raise ProtocolError('tokenizer process failed')
        return valid_ids(strict_json(result.stdout.decode('utf-8').strip()))


class Broker:
    def __init__(self, command, output, log, timeout=10):
        self.log, self.timeout = log, timeout
        self.errors = (Path(output) / 'broker.stderr').open('xb')
        self.process = subprocess.Popen(list(command), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=self.errors)
        self.lines = queue.Queue()
        def reader():
            try:
                while True:
                    line = self.process.stdout.readline(1024 * 1024)
                    if not line:
                        break
                    self.lines.put(line)
            finally:
                self.lines.put(None)
        self.thread = threading.Thread(target=reader, daemon=True)
        self.thread.start()
        self.log.add('broker_started', command=list(command), pid=self.process.pid)
    def exchange(self, line):
        raw = (line + '\n').encode('ascii')
        self.process.stdin.write(raw)
        self.process.stdin.flush()
        try:
            answer = self.lines.get(timeout=self.timeout)
        except queue.Empty as exc:
            raise ProtocolError('broker timed out') from exc
        if answer is None:
            raise ProtocolError('broker closed before receipt')
        self.log.add('broker_exchange', input=line, output=answer.decode('utf-8'))
        return strict_json(answer.decode('utf-8'))
    @staticmethod
    def hex(text):
        return text.encode('utf-8').hex()
    def reset(self, task, timeout_ms, max_turns):
        return self.exchange(f'RESET\t{timeout_ms}\t{max_turns}\t{self.hex(task)}')
    def setup(self, now_ms, ident, request):
        return self.exchange(f'SETUP\t{now_ms}\t{self.hex(ident)}\t{self.hex(request)}')
    def step(self, now_ms, response):
        return self.exchange(f'STEP\t{now_ms}\t{self.hex(response)}')
    def reject(self, now_ms, raw):
        return self.exchange(f'REJECT\t{now_ms}\t{self.hex(raw)}')
    def snapshot(self):
        return self.exchange('SNAPSHOT')
    def close(self):
        try:
            if self.process.poll() is None:
                self.process.stdin.write(b'QUIT\n')
                self.process.stdin.flush()
                self.process.wait(timeout=5)
        except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
            self.process.kill()
            self.process.wait(timeout=5)
        finally:
            self.process.stdin.close()
            self.thread.join(timeout=5)
            self.process.stdout.close()
            self.errors.close()
        if self.process.returncode:
            raise ProtocolError('broker exit code ' + str(self.process.returncode))


class LlamaServer:
    def __init__(self, url, log, timeout=600):
        parsed = urlsplit(url)
        if parsed.scheme != 'http' or parsed.hostname not in ('127.0.0.1', 'localhost', '::1') or parsed.path not in ('', '/') or parsed.username:
            raise ProtocolError('server must be an explicit local HTTP endpoint')
        self.url, self.log, self.timeout = url.rstrip('/'), log, timeout
        self.opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    def request(self, path, payload=None):
        raw = None if payload is None else dumps(payload).encode('utf-8')
        request = urllib.request.Request(self.url + path, data=raw, headers={'Content-Type': 'application/json'})
        with self.opener.open(request, timeout=self.timeout) as result:
            data = result.read(4 * 1024 * 1024 + 1)
        if len(data) > 4 * 1024 * 1024:
            raise ProtocolError('server response too large')
        return strict_json(data.decode('utf-8'))
    def complete(self, ids, max_tokens):
        payload = {'prompt': valid_ids(ids), 'n_predict': max_tokens, 'temperature': 0,
                   'seed': 42, 'repeat_penalty': 1.0, 'cache_prompt': False,
                   'return_tokens': True, 'stream': False, 'n_keep': 0}
        self.log.add('model_request', url=self.url + '/completion', payload=payload)
        result = self.request('/completion', payload)
        self.log.add('model_result', result=result)
        text_bytes(result.get('content'), 2048, 'model content', empty=False)
        tokens = valid_ids(result.get('tokens'))
        if result.get('truncated') or result.get('stop_type') != 'eos' or tokens[-1] != NATIVE_STOP_TOKEN:
            raise ProtocolError('generation must terminate at native 248046, without truncation')
        return result['content'], tokens, result


def run_case(case, broker, backend, tokenizer, prefix_ids, max_tokens=192,
             context=2048, max_turns=16, timeout_ms=3600000, clock=time.monotonic):
    """case is a whitelist projection: expected/scoring answers are never accepted."""
    allowed = {'task_id', 'cohort', 'family', 'prompt', 'initial_state', 'content_sha256'}
    if set(case) - allowed or case.get('cohort', 'natural') != 'natural':
        raise ProtocolError('runner accepts natural input fields only')
    task = trusted_fragment(case['prompt'])
    start = clock()
    now = lambda: max(0, int((clock() - start) * 1000))
    reset = broker.reset(task, timeout_ms, max_turns)
    initial = reset['final_state']
    generation = reset['generation']
    trace = {'task_id': case['task_id'], 'case_sha256': case['content_sha256'],
             'generation': generation, 'initial_state': initial, 'final_state': initial,
             'final_status': reset['status'], 'turns': []}
    if case.get('initial_state', 'empty') == 'running_timer':
        setup = broker.setup(now(), 'preset', dumps({'name': 'start_timer', 'arguments':
                             {'value': 120, 'unit': '分钟', 'label': 'Existing'}}))
        if setup['event']['code'] != 0:
            raise ProtocolError('real timer preset failed')
        trace['setup_events'] = [setup['event']]
        trace['initial_state'] = initial = setup['final_state']
        trace['final_state'] = initial
    elif case.get('initial_state', 'empty') != 'empty':
        raise ProtocolError('unsupported initial state')
    ids = list(prefix_ids) + tokenizer.encode(task_fragment(task))
    previous = None
    try:
        for turn in range(max_turns):
            if len(ids) >= context:
                raise ProtocolError('context capacity reached')
            row = {'generation': generation, 'turn': turn, 'input_token_count': len(ids),
                   'input_token_ids_sha256': hashlib.sha256(struct.pack('<' + 'I' * len(ids), *ids)).hexdigest()}
            if previous is not None:
                row['receipt_input'] = {'kind': 'receipt', 'generation': generation,
                                        'call_id': previous['id'], 'code': previous['code'], 'text': previous['text']}
            raw, generated, native = backend.complete(ids, min(max_tokens, context - len(ids)))
            row.update(raw_response=raw, generated_token_ids=generated,
                       generation_metadata={key: native.get(key) for key in ('stop_type', 'tokens_predicted', 'tokens_evaluated', 'truncated', 'timings')})
            try:
                envelope = native_to_envelope(raw, generation, turn)
            except ProtocolError as exc:
                row['parse_error'] = str(exc)
                envelope = ''
                reply = broker.reject(now(), raw)
            else:
                reply = broker.step(now(), envelope)
            row['model_response'] = envelope
            row['state_before'] = reply['initial_state']
            row['state_after'] = reply['final_state']
            event = reply.get('event')
            if event is not None:
                if event['kind'] == 'tool':
                    parsed = strict_json(envelope)
                    if event['id'] != parsed['id'] or event['request'] != parsed['request']:
                        raise ProtocolError('broker executed a different request')
                    row['tool_event'] = event
                elif event['kind'] in ('final', 'clarification'):
                    row['terminal_event'] = event
                else:
                    row['error_event'] = event
            trace['turns'].append(row)
            trace['final_status'] = reply['status']
            trace['final_state'] = reply['final_state']
            if reply['status'] != 'running':
                return trace
            if not event or event['kind'] != 'tool':
                raise ProtocolError('running C loop lacks tool event')
            previous = event
            ids += generated + tokenizer.encode(receipt_fragment(previous))
        limited = broker.step(now(), '{}')
        trace['final_status'] = limited['status']
        trace['final_state'] = limited['final_state']
        trace['loop_limit_event'] = limited.get('event')
    except Exception as exc:
        trace['runner_error'] = type(exc).__name__ + ': ' + str(exc)
        trace['final_status'] = 'backend_error'
        trace['final_state'] = broker.snapshot()['final_state']
    return trace


def select_cases(document, selected, all_natural=False):
    cases = document['natural']
    if not all_natural and not selected:
        raise ProtocolError('select explicit case IDs or --all-natural')
    ids = set(selected)
    if len(ids) != len(selected):
        raise ProtocolError('duplicate selected case ID')
    chosen = cases if all_natural else [case for case in cases if case['task_id'] in ids]
    if not all_natural and {case['task_id'] for case in chosen} != ids:
        raise ProtocolError('selected case ID missing')
    fields = ('task_id', 'cohort', 'family', 'prompt', 'initial_state', 'content_sha256')
    return [{key: case[key] for key in fields if key in case} for case in chosen]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cli', type=Path, required=True, help='Pinned llama-server binary; server is started separately')
    ap.add_argument('--server-url', required=True, help='Explicit already-managed local llama-server URL')
    ap.add_argument('--server-log', type=Path, required=True)
    ap.add_argument('--tokenizer', type=Path, required=True)
    ap.add_argument('--model', type=Path, required=True)
    ap.add_argument('--model-manifest', type=Path, required=True)
    ap.add_argument('--system', type=Path, required=True)
    ap.add_argument('--prefix', type=Path, required=True)
    ap.add_argument('--prompt-metadata', type=Path, required=True)
    ap.add_argument('--token-bin', type=Path, required=True)
    broker_args = ap.add_mutually_exclusive_group(required=True)
    broker_args.add_argument('--broker-command', help='JSON argv array; shells may require escaping')
    broker_args.add_argument('--broker-command-file', type=Path, help='UTF-8 JSON argv array; recommended on Windows')
    ap.add_argument('--broker-artifact', type=Path, help='Broker binary to hash; inferred for a plain executable or /mnt/X argv')
    ap.add_argument('--tasks', type=Path, required=True)
    ap.add_argument('--freeze-manifest', type=Path, required=True)
    ap.add_argument('--case-id', action='append', default=[])
    ap.add_argument('--all-natural', action='store_true')
    ap.add_argument('--out', type=Path, required=True, help='New directory under E high_precision')
    ap.add_argument('--max-tokens', type=int, default=192)
    ap.add_argument('--context', type=int, default=2048)
    ap.add_argument('--max-turns', type=int, default=16)
    ap.add_argument('--timeout', type=int, default=3600)
    args = ap.parse_args(argv)
    if not 1 <= args.max_turns <= 16 or not 1 <= args.max_tokens < args.context or args.context != 2048 or not 1 <= args.timeout <= 86400:
        raise ProtocolError('invalid fixed-context generation limits')
    output = args.out.resolve()
    if not output.is_relative_to(DEFAULT_OUTPUT_ROOT.resolve()) or output == DEFAULT_OUTPUT_ROOT.resolve():
        raise ProtocolError('all outputs must be in a new E high_precision subdirectory')
    identity = verify_official_model(args.model, args.model_manifest)
    identity['runner_source_path'] = str(Path(__file__).resolve())
    identity['runner_source_sha256'] = sha256(Path(__file__))
    system, prefix_ids, metadata = read_prefix(args.system, args.prefix, args.prompt_metadata, args.token_bin)
    freeze = strict_json(args.freeze_manifest.read_text(encoding='utf-8'))
    if sha256(args.tasks) != freeze['tasks_sha256']:
        raise ProtocolError('frozen task document hash mismatch')
    cases = select_cases(strict_json(args.tasks.read_text(encoding='utf-8')), args.case_id, args.all_natural)
    command = strict_json(args.broker_command_file.read_text(encoding='utf-8')
                          if args.broker_command_file else args.broker_command)
    if not isinstance(command, list) or not command or any(not isinstance(x, str) or not x or '\0' in x for x in command):
        raise ProtocolError('invalid broker command argv')
    broker_artifact = args.broker_artifact
    if broker_artifact is None:
        candidate = command[-1]
        mount = re.fullmatch(r'/mnt/([a-z])/(.+)', candidate)
        if mount and os.name == 'nt':
            candidate = mount.group(1).upper() + ':/' + mount.group(2)
        broker_artifact = Path(candidate)
    if not broker_artifact.is_file():
        raise ProtocolError('provide --broker-artifact for binary identity')
    identity.update(broker_command=command, broker_artifact_path=str(broker_artifact.resolve()),
                    broker_sha256=sha256(broker_artifact))
    output.mkdir(parents=True, exist_ok=False)
    log = EvidenceLog(output / 'native.jsonl')
    broker = None
    traces = []
    error = None
    try:
        version = subprocess.run([str(args.cli), '--version'], capture_output=True, timeout=30)
        (output / 'runtime-version.stdout').write_bytes(version.stdout)
        (output / 'runtime-version.stderr').write_bytes(version.stderr)
        version_text = (version.stdout + version.stderr).decode('utf-8', 'strict')
        if version.returncode or '11835' not in version_text or 'f64a49819' not in version_text:
            raise ProtocolError('runtime is not pinned b11835/f64a49819')
        log.add('identity', **identity, cli=str(args.cli), runtime_version=version_text,
                server_url=args.server_url, server_log_path=str(args.server_log),
                server_log_sha256_at_start=sha256(args.server_log), freeze=freeze,
                prompt_metadata_sha256=sha256(args.prompt_metadata))
        tokenizer = Tokenizer([str(args.tokenizer)], args.model, output, log)
        actual_prefix = tokenizer.encode(system)
        write_json(output / 'prefix-alignment.json', {'expected_count': len(prefix_ids), 'actual_count': len(actual_prefix),
                   'exact': actual_prefix == prefix_ids, 'expected_ids': prefix_ids, 'actual_ids': actual_prefix})
        if actual_prefix != prefix_ids:
            raise ProtocolError('llama-tokenize prefix differs from frozen C IDs')
        backend = LlamaServer(args.server_url, log, args.timeout)
        health = backend.request('/health')
        properties = backend.request('/props')
        log.add('server_health', health=health, properties=properties)
        # /props binds the live server model to the read-only manifest artifact.
        live_path = properties.get('model_path')
        live_build = str(properties.get('build_info', ''))
        if '11835' not in live_build or 'f64a49819' not in live_build:
            raise ProtocolError('live server build differs from pinned runtime')
        if not live_path or Path(live_path).resolve() != args.model.resolve():
            raise ProtocolError('live server /props model_path does not match official artifact')
        if (properties.get('model_ftype') != 'BF16' or properties.get('total_slots') != 1
                or properties.get('default_generation_settings', {}).get('n_ctx') != args.context):
            raise ProtocolError('live server must be BF16, single-slot, and fixed 2048 context')
        broker = Broker(command, output, log)
        for case in cases:
            log.add('case_start', case=case)
            trace = run_case(case, broker, backend, tokenizer, prefix_ids, args.max_tokens,
                             args.context, args.max_turns, args.timeout * 1000)
            traces.append(trace)
            write_json(output / ('case-%04d.json' % len(traces)), trace)
            log.add('case_end', task_id=case['task_id'], final_status=trace['final_status'])
    except Exception as exc:
        error = type(exc).__name__ + ': ' + str(exc)
        log.add('runner_error', error=error)
    finally:
        if broker is not None:
            try:
                broker.close()
            except Exception as exc:
                error = error or ('broker close: ' + str(exc))
                log.add('broker_close_error', error=str(exc))
        log.close()
    identity.update(tokenizer_sha256=metadata['tokenizer_bin_sha256'],
                    tokenizer_runtime_sha256=sha256(args.tokenizer), template_sha256=metadata['utf8_sha256'],
                    tools_schema_sha256=metadata['schema_file_sha256'], runtime_sha256=sha256(args.cli),
                    backend='cpu_real_model', source_log_path=str(log.path), source_log_sha256=sha256(log.path),
                    prompt_metadata_sha256=sha256(args.prompt_metadata), prefix_file_sha256=sha256(args.prefix),
                    prefix_token_ids_sha256=metadata['token_ids_sha256'],
                    tokenizer_alignment='native llama-tokenize prefix compared by every ID',
                    model_role='high_precision', runtime_version=locals().get('version_text'),
                    server_url=args.server_url, server_log_path=str(args.server_log),
                    server_log_sha256=sha256(args.server_log),
                    generation_config={'context': args.context, 'max_tokens': args.max_tokens,
                                       'max_turns': args.max_turns, 'temperature': 0, 'seed': 42},
                    freeze_manifest_path=str(args.freeze_manifest), freeze_manifest_sha256=sha256(args.freeze_manifest))
    result = {'freeze_sha256': freeze['tasks_sha256'], 'rules_sha256': freeze['rules_sha256'],
              'evaluation_scope': freeze.get('scope', 'frozen_natural_tasks'),
              'runs': {'high_precision': {'status': 'RUN' if traces else 'NOT_RUN',
                        'mode': 'real_generation' if traces else 'not_run', 'identity': identity, 'cases': traces}},
              'control_run': {'status': 'NOT_RUN', 'cases': []}}
    if error:
        result['runner_error'] = error
    write_json(output / 'run_results.json', result)
    print(dumps({'output': str(output / 'run_results.json'), 'cases': len(traces), 'error': error}))
    return 1 if error else 0


if __name__ == '__main__':
    raise SystemExit(main())
