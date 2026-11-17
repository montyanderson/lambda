#!/usr/bin/env python3
"""Full transcript resume, long streamed messages and allocation failure."""
import json
import os
from pathlib import Path
import resource
import subprocess
import tempfile

from reliability_check import BINARY, Fixture, event, reply


def main():
    with tempfile.TemporaryDirectory(prefix='lambda-full-history-') as directory:
        f = Fixture(directory)
        f.env = {k: v for k, v in f.env.items() if not k.startswith('HERDR_')}
        try:
            path = Path(directory) / 'large.jsonl'
            records = [{'t': 'session', 'model': 'fixture'}]
            for i in range(5000):
                records.append({'t': 'msg', 'm': {'role': 'user' if i % 2 == 0 else 'assistant',
                                                'content': f'original-{i:04d} ' + 'data ' * 20}})
            records.append({'t': 'compact', 'drop': 4994, 'pin': 0, 'summary': 'earlier conversation summary'})
            path.write_text(''.join(json.dumps(r) + '\n' for r in records))
            output = f.run('-r', str(path), '-p', 'continue')
            assert '5000 messages restored' in output
            assert 'original-0000' in output and 'original-2500' in output and 'original-4999' in output
            sent = f.requests[-1]['messages']
            assert len(sent) < 20 and 'earlier conversation summary' in sent[0]['content']
            assert not any('original-2500' in str(m) for m in sent)
            f.reset(lambda b, n: reply())
            output = f.run('-r', str(path), '-p', 'continue again')
            assert '5002 messages restored' in output and 'original-2500' in output
            print('resume: all 5000 originals retained/replayed; compact context sent; repeated resume: ok', flush=True)

            # One text block, one message and one disk record exceed the old
            # 256 KiB/400 KiB/1 MiB buffers, using valid small SSE events.
            text = 'large-reply-start ' + 'abcde ' * 200000 + ' large-reply-end'
            stream = event('message_start', message={'usage': {'input_tokens': 10}})
            stream += event('content_block_start', index=0, content_block={'type': 'text', 'text': ''})
            for start in range(0, len(text), 16000):
                stream += event('content_block_delta', index=0, delta={'type': 'text_delta', 'text': text[start:start+16000]})
            stream += event('content_block_stop', index=0)
            stream += event('message_delta', delta={'stop_reason': 'end_turn'}, usage={'output_tokens': 200000})
            stream += event('message_stop')
            f.reset(lambda b, n: stream)
            output = f.run('-p', 'long reply', '--max-tokens', '250000')
            assert 'large-reply-end' in output
            transcript = next((Path(directory) / '.lambda/chats').glob('*.jsonl'))
            saved = [json.loads(line) for line in transcript.read_text().splitlines()]
            assistant = [r['m'] for r in saved if r['t'] == 'msg' and r['m']['role'] == 'assistant']
            assert assistant[0]['content'][0]['text'] == text
            f.reset(lambda b, n: reply())
            output = f.run('-r', str(transcript), '-p', 'check full reply')
            assert 'large-reply-end' in output
            assert f.requests[0]['messages'][1]['content'][0]['text'] == text
            print('stream/replay: a 1.2 MB reply is retained, logged and sent back verbatim: ok', flush=True)

            before = path.read_bytes()
            def limit():
                resource.setrlimit(resource.RLIMIT_AS, (256 << 20, 256 << 20))
            result = subprocess.run(f.args + ['--context-window', '2147483647', '-r', str(path), '-p', 'must not run'],
                                    cwd=directory, env=f.env, capture_output=True, timeout=8, preexec_fn=limit)
            assert result.returncode == 1, result.stderr
            assert b'not enough memory' in result.stdout + result.stderr
            assert path.read_bytes() == before
            assert not f.errors, f.errors
            print('allocation failure: explicit error, original transcript unchanged: ok', flush=True)
        finally:
            f.close()
    print('all full-history checks passed', flush=True)


if __name__ == '__main__': main()
