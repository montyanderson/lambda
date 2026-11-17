#!/usr/bin/env python3
"""Opt-in live Lyber smoke tests. Uses the configured LiteLLM credentials and bills API usage."""
import concurrent.futures
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import time
import sys
import threading

BINARY = os.environ.get('LAMBDA_TEST_BINARY', str(Path(__file__).resolve().parent.parent / 'lambda'))
ROOT = Path(tempfile.mkdtemp(prefix='lambda-live-qa-'))
MODELS = ['lyber/kimi-k31-pro-abliterated', 'lyber/deepseek-v4.1-flash-1017-abliterated']
ENV = {k: v for k, v in os.environ.items() if not k.startswith('EXA_')}
ENV['NO_COLOR'] = '1'

def records(path):
    return [json.loads(line) for line in path.read_text().splitlines()]

def turn(directory, model, prompt, transcript=None):
    before = len(records(transcript)) if transcript else 0
    args = [BINARY, '--no-context', '--no-thinking', '--max-tokens', '2048', '-m', model]
    if transcript:
        args += ['-r', str(transcript)]
    args += ['-s', 'This is a functional test in an isolated scratch directory. Only access files in this directory. Use bash when explicitly requested; otherwise answer without tools. Keep replies brief.', '-p', prompt]
    start = time.monotonic()
    result = subprocess.run(args, cwd=directory, env=ENV, capture_output=True,
                            text=True, errors='replace', timeout=120)
    if not transcript:
        paths = []
        for path in (directory / '.lambda/chats').glob('*.jsonl'):
            try:
                candidates = records(path)
            except json.JSONDecodeError:
                continue  # a different concurrent writer may still be flushing
            if any(r.get('t') == 'msg' and r.get('m', {}).get('role') == 'user'
                   and r['m'].get('content') == prompt for r in candidates):
                paths.append(path)
        assert len(paths) == 1, paths
        transcript = paths[0]
    new = records(transcript)[before:]
    assistants = [r['m'] for r in new if r.get('t') == 'msg' and r['m'].get('role') == 'assistant']
    text = '\n'.join(b.get('text', '') for m in assistants for b in m.get('content', []) if b.get('type') == 'text')
    calls = [b for m in assistants for b in m.get('content', []) if b.get('type') == 'tool_use']
    assert result.returncode == 0, {'exit': result.returncode, 'output': (result.stdout + result.stderr)[-1600:]}
    assert assistants and any(b.get('type') == 'text' and b.get('text', '').strip() for b in assistants[-1]['content']), assistants[-1:] if assistants else new
    return transcript, {'model': model, 'reply': text, 'tool_calls': len(calls), 'seconds': round(time.monotonic() - start, 2)}

def suite(index):
    model = MODELS[index]
    other = MODELS[1-index]
    directory = ROOT / ('kimi' if index == 0 else 'deepseek')
    directory.mkdir()
    marker = f'LAMBDA-{index}-MANGO-7319'
    transcript = None
    steps = []
    prompts = [
        ('remember', f'Remember the exact marker {marker} for this conversation. Reply READY.', None),
        ('recall', 'What exact marker did I give you? Reply with it.', None),
        ('write', "Use bash to write exactly two words and a newline, 'alpha one\\n', into check.txt in this directory. Then reply CREATED.", 'alpha one\n'),
        ('read', 'Use bash to read check.txt. Reply with the contents.', 'alpha one\n'),
        ('append', "Use bash to append exactly 'beta two\\n' to check.txt. Then reply APPENDED.", 'alpha one\nbeta two\n'),
        ('count', 'Use bash to count the lines in check.txt. Reply with just the number.', 'alpha one\nbeta two\n'),
        ('recall_after_tools', 'Without tools, give the original marker and the two lines now in check.txt.', None),
        ('markdown_links', 'Return exactly two markdown list items: a link labelled Lyber to https://www.lyber.ai and a link labelled LiteLLM to https://docs.litellm.ai.', None),
        ('tools_after_many_turns', f'Use bash to read check.txt again. Then reply PASS {marker} and the file contents.', 'alpha one\nbeta two\n'),
        ('switch_model', 'Without tools, give the original marker and both file lines from our conversation.', None),
        ('switch_back_and_tool', f'Use bash to count the lines in check.txt again. Then reply PASS {marker} and the count.', 'alpha one\nbeta two\n'),
        ('binary_tool_output', 'Run this exact bash command once: python3 -c "import os; os.write(1, bytes([255,0,194])+chr(163).encode()+b\' after-binary\')". Then reply BINARY_OK. The odd bytes are intentional for a harness test.', None),
        ('recall_after_binary', 'Without tools, repeat the original conversation marker.', None),
    ]
    try:
        for number, (name, prompt, expected_file) in enumerate(prompts, 1):
            selected = other if name == 'switch_model' else model
            transcript, detail = turn(directory, selected, prompt, transcript)
            reply = detail['reply']
            if name == 'remember': assert 'READY' in reply.upper(), reply
            if name in ['recall', 'recall_after_tools', 'tools_after_many_turns', 'switch_model', 'switch_back_and_tool']:
                assert marker in reply, reply
            if expected_file is not None:
                assert (directory / 'check.txt').read_text() == expected_file, (directory / 'check.txt').read_text()
                assert detail['tool_calls'] >= 1, (name, detail)
            if name in ['read', 'recall_after_tools', 'tools_after_many_turns', 'switch_model']:
                assert 'alpha one' in reply, reply
            if name in ['recall_after_tools', 'tools_after_many_turns', 'switch_model']:
                assert 'beta two' in reply, reply
            if name in ['count', 'switch_back_and_tool']:
                assert re.search(r'\b2\b', reply), reply
            if name == 'markdown_links':
                assert '[Lyber](https://www.lyber.ai)' in reply and '[LiteLLM](https://docs.litellm.ai)' in reply, reply
                assert len(re.findall(r'^\s*[-*+]\s', reply, re.M)) == 2, reply
            if name == 'binary_tool_output':
                assert 'BINARY_OK' in reply and detail['tool_calls'] >= 1, detail
                outputs = [b.get('content', '') for r in records(transcript)
                           if r.get('t') == 'msg' and isinstance(r.get('m', {}).get('content'), list)
                           for b in r['m']['content'] if b.get('type') == 'tool_result']
                assert any('\0' in out and '�' in out and 'after-binary' in out for out in outputs), outputs[-3:]
            if name == 'recall_after_binary':
                assert marker in reply, reply
            steps.append({'step': number, 'name': name, 'passed': True, **detail})
            print(json.dumps({'suite': directory.name, 'step': number, 'name': name, 'passed': True, 'tools': detail['tool_calls'], 'seconds': detail['seconds']}), flush=True)
    except Exception as error:
        steps.append({'step': len(steps)+1, 'passed': False, 'error': str(error)})
        print(json.dumps({'suite': directory.name, 'passed': False, 'error': str(error)}), flush=True)
    return {'suite': directory.name, 'directory': str(directory), 'transcript': str(transcript), 'steps': steps, 'passed': len(steps)==len(prompts) and all(step['passed'] for step in steps)}

def shared_directory_suite():
    directory = ROOT / 'shared'
    directory.mkdir()
    barrier = threading.Barrier(2)

    def worker(index):
        model = MODELS[index]
        marker = f'SHARED-{index}-KIWI-9183'
        filename = f'session-{index}.txt'
        steps = []
        transcript = None
        try:
            barrier.wait(timeout=5)
            start = time.monotonic()
            transcript, detail = turn(directory, model,
                f'Remember marker {marker}. Use bash to write exactly {marker} followed by a newline to {filename} in this directory. Reply READY.')
            end = time.monotonic()
            assert (directory / filename).read_text() == marker + '\n'
            assert detail['tool_calls'] >= 1
            steps.append({'name': 'concurrent_start_and_write', 'passed': True, **detail})
            transcript, detail = turn(directory, model,
                f'Use bash to read {filename}. Reply with the marker from our conversation and the file contents.', transcript)
            assert marker in detail['reply'] and f'SHARED-{1-index}' not in detail['reply']
            assert detail['tool_calls'] >= 1
            steps.append({'name': 'independent_resume_and_recall', 'passed': True, **detail})
            user_text = str([r['m'] for r in records(transcript) if r.get('t') == 'msg' and r['m']['role'] == 'user'])
            assert f'SHARED-{1-index}' not in user_text
            print(json.dumps({'suite': 'shared', 'model': model, 'passed': True, 'prompts': len(steps)}), flush=True)
            return {'suite': f'shared-{index}', 'passed': True, 'steps': steps,
                    'transcript': str(transcript), 'start': start, 'end': end}
        except Exception as error:
            print(json.dumps({'suite': 'shared', 'model': model, 'passed': False, 'error': str(error)}), flush=True)
            steps.append({'passed': False, 'error': str(error)})
            return {'suite': f'shared-{index}', 'passed': False, 'steps': steps}

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(worker, range(2)))
    if all(r['passed'] for r in results):
        assert results[0]['transcript'] != results[1]['transcript']
        assert max(r['start'] for r in results) < min(r['end'] for r in results), 'requests did not overlap'
    return results


def main():
    print('report directory: ' + str(ROOT), flush=True)
    results = []
    if '--shared-only' not in sys.argv:
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(suite, range(2)))
    results += shared_directory_suite()
    (ROOT / 'report.json').write_text(json.dumps(results, indent=2)+'\n')
    passed = all(r['passed'] for r in results)
    print(json.dumps({'report': str(ROOT / 'report.json'), 'passed': passed,
                      'prompts_passed': sum(sum(s['passed'] for s in r['steps']) for r in results)}), flush=True)
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
