#!/usr/bin/env python3
"""Native Herdr lifecycle and resume checks. Local Unix/HTTP fixtures only."""
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import signal
import socketserver
import struct
import subprocess
import tempfile
import termios
import threading
import time

from reliability_check import BINARY, Fixture, reply, block


class Herdr:
    def __init__(self, root):
        self.records = []
        self.mode = 'ok'
        self.path = str(root / 'herdr.sock')
        owner = self

        class Handler(socketserver.StreamRequestHandler):
            def handle(self):
                if owner.mode == 'disconnect': return
                request = json.loads(self.rfile.readline(65536))
                owner.records.append(request)
                if owner.mode == 'stall':
                    time.sleep(.3)
                    return
                response = {'id': request['id'], 'result': {'type': 'fixture'}}
                if owner.mode == 'error': response = {'id': request['id'], 'error': {'code': 'not_found'}}
                try: self.wfile.write(json.dumps(response).encode() + b'\n')
                except BrokenPipeError: pass

        class Server(socketserver.ThreadingUnixStreamServer):
            daemon_threads = True

        self.server = Server(self.path, Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.env = dict(HERDR_ENV='1', HERDR_PANE_ID='w1:p1',
                        HERDR_BIN_PATH='/not-invoked/herdr', HERDR_SOCKET_PATH=self.path)

    def states(self):
        return [r['params']['state'] for r in self.records if r['method'] == 'pane.report_agent']

    def last(self):
        return self.records[-1]['params']

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class Terminal:
    def __init__(self, f, args=()):
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.chdir(f.directory)
            os.execve(BINARY, f.args + list(args), f.env)
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ, struct.pack('HHHH', 30, 120, 0, 0))
        self.output = bytearray()

    def drain(self):
        if select.select([self.fd], [], [], .02)[0]:
            try: self.output.extend(os.read(self.fd, 65536))
            except OSError: pass

    def until(self, predicate):
        end = time.monotonic() + 6
        while time.monotonic() < end:
            self.drain()
            if predicate(): return
        raise AssertionError(bytes(self.output[-1500:]))

    def send(self, text):
        os.write(self.fd, text.encode())

    def quit(self):
        self.send('/quit\r')
        result = []
        def exited():
            pid, status = os.waitpid(self.pid, os.WNOHANG)
            if pid: result.append(status)
            return bool(pid)
        self.until(exited)
        self.pid = 0
        assert result == [0], result

    def close(self):
        if self.pid:
            os.kill(self.pid, signal.SIGKILL)
            os.waitpid(self.pid, 0)
        os.close(self.fd)


def check(f, h):
    original = dict(f.env)
    f.env.update(h.env)
    gate, entered = threading.Event(), threading.Event()
    def respond(body, n):
        if n == 1:
            entered.set()
            assert gate.wait(6)
        return reply()
    f.reset(respond)
    system = "private instructions with 'quotes'\nand a newline"
    t = Terminal(f, ['--no-tools', '--no-fallback', '--max-tokens', '512', '-s', system, '-e', 'low'])
    try:
        t.until(lambda: h.states() == ['idle'])
        initial = h.last()
        argv = initial['resume_argv']
        assert argv[:2] == ['lambda', '-r']
        assert Path(argv[2]).is_absolute() and Path(argv[2]).exists()
        assert initial['agent_session_path'] == argv[2]
        assert '--no-tools' in argv and '--no-thinking' in argv and '--no-fallback' in argv
        assert argv[argv.index('--max-tokens')+1] == '512'
        assert argv[argv.index('-e')+1] == 'low'
        assert '--context-window' in argv and '--no-context' in argv
        assert '-s' not in argv and system not in json.dumps(h.records)
        t.send('first\r')
        t.until(entered.is_set)
        assert h.states()[-1] == 'working'
        gate.set()
        t.until(lambda: h.states()[-1] == 'idle' and len(h.states()) >= 3)
        t.send('/model fixture-two\r')
        t.until(lambda: h.last().get('resume_argv', [])[4:5] == ['fixture-two'])
        t.send('/tools\r')
        t.until(lambda: '--no-tools' not in h.last().get('resume_argv', ['--no-tools']))
        t.send('/thinking\r')
        t.until(lambda: '--no-thinking' not in h.last().get('resume_argv', ['--no-thinking']))
        t.send('/effort high\r')
        t.until(lambda: 'high' in h.last().get('resume_argv', []))
        restore = h.last()['resume_argv']
        t.quit()
        assert h.records[-1]['method'] == 'pane.release_agent'
    finally:
        gate.set()
        t.close()
    last_seq = h.last()['seq']
    f.reset(lambda b, n: reply())
    result = subprocess.run([BINARY, *restore[1:], '-p', 'restored'], env=f.env,
                            cwd=f.directory, capture_output=True, timeout=6)
    assert result.returncode == 0, result.stderr
    assert f.requests[0]['model'] == 'fixture-two'
    assert system in json.dumps(f.requests[0]['system']).replace('\\n', '\n')
    assert any('first' == m['content'] for m in f.requests[0]['messages'])
    assert h.records[-1]['method'] == 'pane.release_agent'
    assert h.last()['seq'] > last_seq
    assert not f.errors, f.errors
    print('lifecycle: idle/working/release, live settings and executable transcript restore: ok', flush=True)

    # No false completion between a tool loop and the next queued prompt.
    h.records.clear()
    tool_started, tool_release = f.directory / 'tool-started', f.directory / 'tool-release'
    command = 'touch tool-started; while [ ! -e tool-release ]; do sleep .05; done; printf tool-ok'
    f.reset(lambda b, n: reply(block(0, 'tool_use', args={'command': command})) if n == 1 else reply())
    t = Terminal(f, ['--no-log'])
    try:
        t.until(lambda: h.states() == ['idle'])
        t.send('use a tool\r')
        t.until(tool_started.exists)
        assert h.states()[-1] == 'working'
        t.send('queued prompt\r')
        t.until(lambda: b'queued prompt' in t.output)
        tool_release.touch()
        t.until(lambda: len(f.requests) == 3 and h.states()[-1] == 'idle')
        assert h.states()[0] == h.states()[-1] == 'idle'
        assert all(s == 'working' for s in h.states()[1:-1]), h.states()
        t.quit()
    finally:
        tool_release.touch(); t.close()
    print('tools and queued turns: stay working until all pending work finishes: ok', flush=True)

    # Failed turns block; a subsequent successful turn clears that state.
    h.records.clear()
    f.reset(lambda b, n: (500, b'{"error":{"message":"fixture error"}}') if n == 1 else reply())
    t = Terminal(f, ['--no-log'])
    try:
        t.until(lambda: h.states() == ['idle'])
        t.send('fail\r')
        t.until(lambda: h.states()[-1] == 'blocked')
        assert 'turn failed' in h.last()['message']
        t.send('retry\r')
        t.until(lambda: len(f.requests) == 2 and h.states()[-1] == 'idle')
        t.quit()
        assert all('resume_argv' not in r['params'] for r in h.records)
    finally: t.close()

    # Cancellation pauses queued work and keeps Herdr blocked until resumed.
    h.records.clear(); gate.clear(); entered.clear()
    f.reset(respond)
    t = Terminal(f, ['--no-log'])
    try:
        t.until(lambda: h.states() == ['idle'])
        t.send('first\r')
        t.until(entered.is_set)
        t.send('second\r')
        t.until(lambda: b'second' in t.output)
        t.send('\x03')
        t.until(lambda: h.states()[-1] == 'blocked')
        assert 'queued prompts paused' in h.last()['message']
        assert len(f.requests) == 1
        gate.set()
        t.send('\r')
        t.until(lambda: len(f.requests) == 2 and h.states()[-1] == 'idle')
        t.quit()
    finally:
        gate.set(); t.close()
    print('errors: blocked on failure/paused queue, retry and cancellation recovery: ok', flush=True)

    # Model selection is a real decision, and does not remain blocked on Escape.
    h.records.clear()
    t = Terminal(f, ['--no-log'])
    try:
        t.until(lambda: h.states() == ['idle'])
        t.send('/model\r')
        t.until(lambda: h.states()[-1] == 'blocked')
        assert h.last()['message'] == 'select a model'
        t.send('\x1b')
        t.until(lambda: h.states()[-1] == 'idle')
        t.quit()
    finally: t.close()

    # Herdr must never reject a state report because its resume argv is unsafe.
    for name in ["quote's.jsonl", 'control\nname.jsonl', 'safe space λ.jsonl']:
        transcript = f.directory / name
        transcript.write_text('{"t":"session","model":"fixture"}\n')
        h.records.clear()
        f.run('-r', str(transcript), '-p', 'unsafe path')
        assert h.states() == ['working', 'idle']
        for record in h.records:
            if record['method'] == 'pane.report_agent':
                assert ('resume_argv' in record['params']) == name.startswith('safe')
    h.records.clear()
    f.run('--no-log', '-p', 'one shot')
    assert h.states() == ['working', 'idle']
    assert h.records[-1]['method'] == 'pane.release_agent'
    print('picker, unsafe resume paths, no-log and one-shot lifecycle: ok', flush=True)

    # Missing environment never reports, including version/help invocations.
    for key in h.env:
        f.env = {**original, **h.env}
        del f.env[key]
        h.records.clear()
        f.run('--no-log', '-p', 'not in herdr')
        assert not h.records, (key, h.records)
    f.env = {**original, **h.env, 'HERDR_ENV': '0'}
    f.run('--no-log', '-p', 'not in herdr')
    assert not h.records
    f.env = {**original, **h.env}
    f.run('--version')
    assert not h.records
    f.env['HERDR_SOCKET_PATH'] = str(f.directory / 'missing.sock')
    f.run('--no-log', '-p', 'missing socket')
    assert not h.records
    f.env.update(h.env)
    for mode in ['error', 'disconnect', 'stall']:
        h.mode = mode
        start = time.monotonic()
        f.run('--no-log', '-p', mode)
        assert time.monotonic() - start < 2, mode
    h.mode = 'ok'
    print('isolation: missing environment/socket, rejected/disconnected/stalled server: ok', flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix='lambda-herdr-') as directory:
        f = Fixture(directory)
        f.env = {k: v for k, v in f.env.items() if not k.startswith('HERDR_')}
        h = Herdr(Path(directory))
        try: check(f, h)
        finally:
            h.close()
            f.close()
    print('all herdr checks passed', flush=True)


if __name__ == '__main__': main()
