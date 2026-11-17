#!/usr/bin/env python3
"""Reasoning discovery, transactional pickers, persistence and wire settings."""
import json
from pathlib import Path
import tempfile
from reliability_check import Fixture, reply
from herdr_check import Terminal


def main():
    with tempfile.TemporaryDirectory(prefix='lambda-reasoning-') as directory:
        f = Fixture(directory)
        f.args = f.args[:-2]  # remove the fixture's explicit model
        f.env = {k: v for k, v in f.env.items() if not k.startswith('HERDR_')}
        names = ['alias-adaptive', 'alias-limited', 'alias-plain', 'alias-legacy', 'alias-openai', 'alias-multi', 'alias-unknown']
        ladder = ['none', 'minimal', 'low', 'medium', 'high', 'xhigh', 'max']
        groups = [{'model_group': n, 'supports_reasoning': True,
                   'supported_reasoning_efforts': ladder,
                   'supported_openai_params': ['reasoning_effort']} for n in names if n != 'alias-unknown']
        info = [
            {'model_name': 'alias-adaptive', 'model_info': {'supports_reasoning': True, 'supports_adaptive_thinking': True,
             'thinking_always_on': True, 'supports_minimal_reasoning_effort': False, 'default_reasoning_effort': 'medium'}},
            {'model_name': 'alias-limited', 'model_info': {'supports_reasoning': True, 'reasoning_effort_levels': ['none', 'low', 'high'],
             'supports_max_reasoning_effort': True}},
            {'model_name': 'alias-plain', 'model_info': {'supports_reasoning': False}},
            {'model_name': 'alias-legacy', 'model_info': {'supports_reasoning': True, 'litellm_provider': 'anthropic',
             'reasoning_effort_levels': ['none', 'minimal', 'low', 'medium', 'high']}},
            {'model_name': 'alias-openai', 'model_info': {'supports_reasoning': True, 'reasoning_effort_levels': ['none', 'low', 'high']}},
            {'model_name': 'alias-multi', 'model_info': {'supports_reasoning': True, 'reasoning_effort_levels': ['low', 'high']}},
            {'model_name': 'alias-multi', 'model_info': {'supports_reasoning': True, 'reasoning_effort_levels': ['high', 'max']}},
        ]
        # The real proxy's deployment response is larger than the old 2 MiB
        # catalog cap. Irrelevant metadata must not hide the always-on flags.
        info[0]['description'] = 'x' * (2 << 20)
        requests = []
        def get(handler):
            requests.append(handler.path)
            if handler.path == '/v1/models?include_metadata=true':
                data = [{'id': n, 'max_input_tokens': 1000000} for n in names]
            elif handler.path == '/model_group/info': data = groups
            elif handler.path == '/model/info': data = info
            else: handler.send_error(404); return
            payload = json.dumps({'data': data}).encode()
            handler.send_response(200)
            handler.send_header('Content-Length', str(len(payload)))
            handler.end_headers()
            handler.wfile.write(payload)
        f.server.RequestHandlerClass.do_GET = get
        pref = Path(directory) / 'lambda/default-model'
        config_before = (pref.parent / 'config.json').read_bytes()
        t = None
        try:
            t = Terminal(f, ['--no-log'])
            t.until(lambda: b'select model' in t.output)
            t.output.clear(); t.send('alias-adaptive\r')
            t.until(lambda: b'reasoning effort' in t.output)
            assert b'provider decides' in t.output and b'advertised: medium' in t.output
            assert b'disable reasoning' not in t.output and b'minimal' not in t.output
            assert not pref.exists()
            t.send('high\r'); t.until(lambda: pref.exists())
            assert pref.read_text() == 'alias-adaptive\neffort=high\n'
            t.send('first\r'); t.until(lambda: len(f.requests) == 1)
            assert f.requests[-1]['thinking'] == {'type': 'adaptive'}
            assert f.requests[-1]['output_config'] == {'effort': 'high'}
            t.output.clear(); t.send('/model alias-limited\r')
            t.until(lambda: b'reasoning effort' in t.output)
            assert b'medium' not in t.output and b'xhigh' not in t.output and b'max' not in t.output
            t.send('\x1b')
            t.until(lambda: b'type a message' in t.output or b'enter send' in t.output)
            assert pref.read_text() == 'alias-adaptive\neffort=high\n'
            t.output.clear(); t.send('/model alias-limited\r')
            t.until(lambda: b'reasoning effort' in t.output)
            t.send('low\r'); t.until(lambda: pref.read_text() == 'alias-limited\neffort=low\n')
            t.send('/effort high\r'); t.until(lambda: pref.read_text() == 'alias-limited\neffort=high\n')
            t.quit(); t.close(); t = None
            assert pref.stat().st_mode & 0o777 == 0o600
            assert (pref.parent / 'config.json').read_bytes() == config_before
            print('picker: model then effort, metadata restrictions, cancellation, /effort and atomic paired defaults: ok', flush=True)

            f.reset(lambda b, n: reply())
            f.run('--no-log', '-p', 'saved')
            assert f.requests[-1]['model'] == 'alias-limited' and f.requests[-1]['output_config']['effort'] == 'high'
            f.run('--no-log', '-e', 'low', '-p', 'override effort')
            assert f.requests[-1]['output_config']['effort'] == 'low'
            f.run('--no-log', '-m', 'alias-openai', '-p', 'override model')
            assert 'thinking' not in f.requests[-1]
            assert pref.read_text() == 'alias-limited\neffort=high\n'
            f.run('--no-log', '-m', 'alias-limited', '-e', 'max', '-p', 'invalid', code=2)
            f.run('--no-log', '-m', 'alias-adaptive', '-e', 'none', '-p', 'always on', code=2)
            f.run('--no-log', '-m', 'alias-plain', '-e', 'high', '-p', 'no reasoning', code=2)
            f.run('--no-log', '-m', 'alias-multi', '-e', 'low', '-p', 'mixed deployments', code=2)
            f.run('--no-log', '-m', 'alias-multi', '-e', 'high', '-p', 'common level')
            assert f.requests[-1]['output_config']['effort'] == 'high'
            f.run('--no-log', '-m', 'alias-legacy', '-e', 'none', '-p', 'off')
            assert f.requests[-1]['thinking'] == {'type': 'disabled'} and 'output_config' not in f.requests[-1]
            f.run('--no-log', '-m', 'alias-openai', '-e', 'none', '-p', 'off')
            assert f.requests[-1]['output_config']['effort'] == 'none'
            f.run('--no-log', '-m', 'alias-legacy', '-e', 'high', '-p', 'legacy translation')
            assert f.requests[-1]['thinking']['type'] == 'adaptive' and f.requests[-1]['output_config']['effort'] == 'high'
            f.run('--no-log', '-m', 'alias-legacy', '-e', 'minimal', '-p', 'native minimal maps to low')
            assert f.requests[-1]['output_config']['effort'] == 'low'
            print('wire: defaults/overrides, adaptive and legacy interface, reasoning off, unsupported and multi-deployment levels: ok', flush=True)

            log = Path(directory) / 'resume.jsonl'
            log.write_text(json.dumps({'t':'session','model':'alias-adaptive'})+'\n'+json.dumps({'t':'meta','k':'effort','v':'medium'})+'\n')
            f.run('--no-log', '-r', str(log), '-p', 'resume')
            assert f.requests[-1]['model'] == 'alias-adaptive' and f.requests[-1]['output_config']['effort'] == 'medium'
            f.run('--no-log', '-r', str(log), '-e', 'max', '-p', 'explicit beats transcript')
            assert f.requests[-1]['output_config']['effort'] == 'max'
            f.run('--no-log', '-r', str(log), '-m', 'alias-plain', '-p', 'new model resets effort')
            assert 'thinking' not in f.requests[-1]
            assert pref.read_text() == 'alias-limited\neffort=high\n'

            pref.write_text('alias-limited\n')
            t = Terminal(f, ['--no-log'])
            t.until(lambda: b'reasoning effort' in t.output)
            assert b'select model' not in t.output
            t.send('default\r'); t.until(lambda: pref.read_text() == 'alias-limited\neffort=default\n')
            t.output.clear(); t.send('/model alias-unknown\r')
            t.until(lambda: pref.read_text() == 'alias-unknown\neffort=default\n')
            assert b'reasoning options unavailable' in t.output
            t.quit(); t.close(); t = None
            pref.unlink()
            t = Terminal(f, ['--no-log', '-e', 'high'])
            t.until(lambda: b'select model' in t.output)
            t.output.clear(); t.send('alias-limited\r')
            t.until(lambda: b'reasoning effort' in t.output)
            t.send('low\r'); t.until(lambda: pref.exists())
            assert pref.read_text() == 'alias-limited\neffort=low\n'
            f.requests.clear(); t.send('CLI override after startup selection\r')
            t.until(lambda: len(f.requests) == 1)
            assert f.requests[0]['output_config']['effort'] == 'high'
            t.quit(); t.close(); t = None
            print('resume/migration: transcript and CLI precedence; old model-only files ask once; missing capabilities keep default: ok', flush=True)
        finally:
            if t: t.close()
            f.close()
    print('all reasoning checks passed', flush=True)


if __name__ == '__main__': main()
