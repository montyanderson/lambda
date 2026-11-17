#!/usr/bin/env python3
"""Deterministic failure/interleaving tests against the real lambda process.

No provider credentials. Every request is decoded strictly before inspecting
history. HTTP gates let tests place cancellation and input at specific phases.
"""
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import struct
import subprocess
import tempfile
import termios
import threading
import time

BINARY = str(Path(__file__).resolve().parent.parent / "lambda")


def event(kind, **fields):
    return ("data: " + json.dumps({"type": kind, **fields}, ensure_ascii=False) + "\n\n").encode()


def block(index, kind="text", text="ok", ident=None, name="bash", args=None, initial=False):
    if kind == "tool_use":
        fields = dict(type=kind, id=ident or f"call_{index}", name=name, input=args if initial else {})
    else:
        fields = dict(type=kind, text=text)
    result = event("content_block_start", index=index, content_block=fields)
    if kind == "tool_use" and not initial:
        raw = args if isinstance(args, str) else json.dumps(args or {})
        for i in range(0, len(raw), 7):
            result += event("content_block_delta", index=index, delta=dict(type="input_json_delta", partial_json=raw[i:i+7]))
    return result + event("content_block_stop", index=index)


def reply(*blocks):
    return (event("message_start", message={"model": "fixture", "usage": {"input_tokens": 10}})
            + b"".join(blocks or [block(0)])
            + event("message_delta", delta={"stop_reason": "end_turn"}, usage={"output_tokens": 10})
            + event("message_stop"))


def valid_history(body):
    pending = []
    for msg in body["messages"]:
        content = msg["content"]
        if pending:
            assert msg["role"] == "user" and isinstance(content, list), msg
            results = [b["tool_use_id"] for b in content if b["type"] == "tool_result"]
            assert results == pending, (results, pending)
            pending = []
        elif isinstance(content, list):
            assert not any(b["type"] == "tool_result" for b in content), content
        if msg["role"] == "assistant" and isinstance(content, list):
            pending = [b["id"] for b in content if b["type"] == "tool_use"]
    assert not pending, pending


class Fixture:
    def __init__(self, directory):
        self.directory = Path(directory)
        self.requests = []
        self.errors = []
        self.respond = lambda body, n: reply()
        self.active = self.max_active = 0
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args): pass

            def do_GET(self):
                data = json.dumps({"data": [{"id": "fixture", "max_input_tokens": 1000000}]}).encode()
                self.send_response(200)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_POST(self):
                try:
                    raw = self.rfile.read(int(self.headers["Content-Length"]))
                    body = json.loads(raw.decode("utf-8"))
                    # json.loads accepts surrogate escapes; strict UTF-8 does not.
                    json.dumps(body, ensure_ascii=False).encode("utf-8")
                    valid_history(body)
                    owner.requests.append(body)
                    owner.active += 1
                    owner.max_active = max(owner.max_active, owner.active)
                    try:
                        response = owner.respond(body, len(owner.requests))
                    finally:
                        owner.active -= 1
                    if isinstance(response, tuple):
                        status, response = response
                    else:
                        status = 200
                    self.send_response(status)
                    self.send_header("Content-Type", "text/event-stream" if status == 200 else "application/json")
                    self.send_header("Content-Length", str(len(response)))
                    self.end_headers()
                    self.wfile.write(response)
                except (BrokenPipeError, ConnectionResetError):
                    pass
                except BaseException as error:
                    owner.errors.append(repr(error))
                    self.send_error(400)

        self.server = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(("LITELLM_", "ANTHROPIC_", "EXA_"))}
        self.env.update(XDG_CONFIG_HOME=directory, NO_COLOR="1", TERM="xterm-256color")
        config = self.directory / "lambda/config.json"
        config.parent.mkdir()
        config.write_text(json.dumps({"base_url": f"http://127.0.0.1:{self.server.server_port}", "api_key": "fixture"}))
        self.args = [BINARY, "--no-context", "--context-window", "1000000", "--no-thinking", "-m", "fixture"]

    def run(self, *args, code=0):
        p = subprocess.run(self.args + list(args), cwd=self.directory, env=self.env,
                           capture_output=True, timeout=12)
        output = (p.stdout + p.stderr).decode("utf-8", "replace")
        assert p.returncode == code, (p.returncode, output[-3000:])
        assert not self.errors, self.errors
        return output

    def reset(self, callback):
        self.requests.clear()
        self.errors.clear()
        self.respond = callback

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def saved(path):
    live = []
    for line in path.read_text().splitlines():
        r = json.loads(line)
        if r["t"] == "msg": live.append(r["m"])
        elif r["t"] == "drop" and r["n"]: del live[-r["n"]:]
        elif r["t"] == "clear": live.clear()
    return live


def wait_for(predicate, timeout=5):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if predicate(): return
        time.sleep(.01)
    raise AssertionError("timed out waiting for fixture condition")


def check(f):
    # Includes embedded NUL: C string functions must not discard its suffix.
    command = "python3 -c \"import os; os.write(1, bytes([255,0,237,160,128])+ '£ ✓ 中文 😀'.encode())\""
    f.reset(lambda b, n: reply(block(0, "tool_use", args={"command": command})) if n == 1 else reply())
    f.run("-p", "binary output")
    result = f.requests[-1]["messages"][-1]["content"][0]["content"]
    assert result == "�\0���£ ✓ 中文 😀", repr(result)
    for log in (f.directory / ".lambda/chats").glob("*.jsonl"): saved(log)

    f.reset(lambda b, n: reply(block(0, "tool_use", args={"command": "python3 -c \"import os; os.write(1, b'a'*65406 + '😀'.encode() + b'x'*1000)\""})) if n == 1 else reply())
    f.run("-p", "truncated Unicode")
    result = f.requests[-1]["messages"][-1]["content"][0]["content"]
    assert result.endswith("�\n[output truncated]"), repr(result[-50:])
    old = f.directory / "malformed.jsonl"
    old.write_bytes(b'{"t":"session","model":"fixture"}\n{"t":"msg","m":{"role":"user","content":"old \xc2\xe2\x80\xa6 \\ud800"}}\n')
    before = old.read_bytes()
    f.reset(lambda b, n: reply())
    f.run("--no-log", "-r", str(old), "-p", "resume")
    assert f.requests[0]["messages"][0]["content"] == "old �… �"
    assert old.read_bytes() == before
    print("encoding: binary/NUL tools, truncated Unicode, malformed old transcript: ok", flush=True)

    for initial in [False, True]:
        f.reset(lambda b, n: reply(*(block(i, "tool_use", args={"command": f"printf result-{i}"}, initial=initial) for i in range(8))) if n == 1 else reply())
        f.run("--no-log", "-p", "eight calls")
        assert [r["content"] for r in f.requests[-1]["messages"][-1]["content"]] == [f"result-{i}" for i in range(8)]
    for args in [{}, {"command": 123}, {"command": "echo bad\0echo worse"}]:
        f.reset(lambda b, n: reply(block(0, "tool_use", args=args)) if n == 1 else reply())
        f.run("--no-log", "-p", "bad command")
        result = f.requests[-1]["messages"][-1]["content"][0]
        assert result["is_error"] and "requires" in result["content"]
    f.reset(lambda b, n: reply(block(0, "tool_use", name="unknown", args={})) if n == 1 else reply())
    f.run("--no-log", "-p", "unknown tool")
    assert f.requests[-1]["messages"][-1]["content"][0]["is_error"]
    f.reset(lambda b, n: reply(block(0, "tool_use", args={"command": "touch must-not-run"})) if n == 1 else reply())
    f.run("--no-log", "--no-tools", "-p", "disabled bash")
    assert "disabled" in f.requests[-1]["messages"][-1]["content"][0]["content"]
    assert not (f.directory / "must-not-run").exists()

    bad = {
        "too many calls": reply(*(block(i, "tool_use", args={"command": "touch must-not-run"}) for i in range(9))),
        "duplicate ids": reply(block(0, "tool_use", args={"command": "touch must-not-run"}), block(1, "tool_use", ident="call_0", args={})),
        "oversized id": reply(block(0, "tool_use", ident="x"*65, args={})),
        "invalid JSON args": reply(block(0, "tool_use", args='{ "command": "touch must-not-run" "other": true }')),
        "array args": reply(block(0, "tool_use", args='[]')),
        "truncated args": reply(block(0, "tool_use", args='{"command":')),
        "delta before start": event("content_block_delta", index=0, delta={"type":"text_delta","text":"bad"}) + reply(),
        "wrong delta type": event("content_block_start", index=0, content_block={"type":"text","text":""}) + event("content_block_delta", index=0, delta={"type":"input_json_delta","partial_json":"{}"}) + event("content_block_stop",index=0) + event("message_stop"),
        "duplicate start": block(0) + reply(block(0)),
        "duplicate stop": reply() + event("message_stop"),
        "float index": reply(block(.5)),
        "negative index": reply(block(-1)),
        "missing block stop": reply().replace(event("content_block_stop",index=0),b""),
        "malformed event": b'data: {"type":"ping" nope}\n\n' + reply(),
        "EOF before message stop": reply().replace(event("message_stop"),b""),
    }
    for name, data in bad.items():
        f.reset(lambda b, n, data=data: data)
        output = f.run("--no-log", "-p", name, code=1)
        assert "error" in output or "incomplete" in output, (name, output)
        assert len(f.requests) == 1 and not (f.directory / "must-not-run").exists(), name
    multiline = reply().replace(b'data: {"type": "message_stop"}', b'data: {"type":\r\ndata: "message_stop"}')
    f.reset(lambda b, n: multiline)
    f.run("--no-log", "-p", "multiline SSE")
    print("streams: 8 tool calls, initial/delta args, 15 malformed streams, error results, multiline SSE: ok", flush=True)

    # The first tool ran; a later HTTP failure must not erase its result.
    for status in [429, 500]:
        f.reset(lambda b,n: reply(block(0,"tool_use",args={"command": f"echo once >> side-effect-{status}"})) if n == 1 else (status,b'{"error":{"message":"fixture failure"}}'))
        logs = set((f.directory / ".lambda/chats").glob("*.jsonl"))
        f.run("-p", "write then fail", code=1)
        log, = set((f.directory / ".lambda/chats").glob("*.jsonl")) - logs
        history = saved(log)
        assert len(history) == 3 and history[-1]["content"][0]["type"] == "tool_result", history
        f.reset(lambda b,n: reply())
        f.run("-r", str(log), "-p", "continue")
        assert f.requests[0]["messages"][:3] == history
        assert (f.directory / f"side-effect-{status}").read_text() == "once\n"
    # SIGINT while a child has closed stdout used to hang in blocking waitpid.
    marker = f.directory / "second-started"
    commands = ["printf first", "touch second-started; exec 1>&- 2>&-; trap '' TERM; sleep 30", "touch must-not-run"]
    f.reset(lambda b,n: reply(*(block(i,"tool_use",args={"command":c}) for i,c in enumerate(commands))))
    logs = set((f.directory / ".lambda/chats").glob("*.jsonl"))
    p = subprocess.Popen(f.args + ["-p","interrupt tools"],cwd=f.directory,env=f.env,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    try:
        wait_for(marker.exists)
        p.send_signal(signal.SIGINT)
        out,err = p.communicate(timeout=4)
        assert p.returncode == 1, (out,err)
    finally:
        if p.poll() is None: p.kill(); p.communicate()
    log, = set((f.directory / ".lambda/chats").glob("*.jsonl")) - logs
    history = saved(log)
    results = history[-1]["content"]
    assert results[0]["content"] == "first" and not results[0].get("is_error")
    assert results[1]["is_error"] and "cancelled before execution" in results[2]["content"]
    assert not (f.directory / "must-not-run").exists() and len(f.requests) == 1
    f.reset(lambda b,n: reply())
    f.run("-r",str(log),"-p","continue after interrupt")
    assert f.requests[0]["messages"][:3] == history

    # A killed process may leave a durable tool_use without a result. Recover
    # with an unknown outcome, never replay the command automatically.
    unfinished = f.directory / "unfinished.jsonl"
    unfinished.write_text(json.dumps({"t":"session","model":"fixture"})+"\n"+json.dumps({"t":"msg","m":{"role":"assistant","content":[{"type":"tool_use","id":"crash","name":"bash","input":{"command":"touch must-not-run"}}]}})+"\n")
    f.reset(lambda b,n: reply())
    f.run("-r",str(unfinished),"-p","inspect state")
    assert "Outcome unknown" in f.requests[0]["messages"][1]["content"][0]["content"]
    assert not (f.directory / "must-not-run").exists()
    print("failures: 429/500 preserve side effects; SIGINT cancels closed-output child and remaining tools; crash recovery: ok", flush=True)


def terminal_isolation_check(f):
    # Git hooks/credential helpers can open /dev/tty even when fd 0/1/2
    # have been redirected. Run real git under Lambda's controlling PTY.
    repo = f.directory / "terminal-git"
    env = {**f.env, "GIT_CONFIG_GLOBAL": "/dev/null", "GIT_CONFIG_NOSYSTEM": "1"}
    subprocess.run(["git", "init", "-q", str(repo)], env=env, check=True)
    ready, release = repo / "hook-ready", repo / "hook-release"
    hook = repo / ".git/hooks/pre-commit"
    hook.write_text('''#!/usr/bin/env python3
import os, pathlib, time
print("hook-stdout-captured", flush=True)
os.write(2, b"hook-stderr-captured\\n")
try:
    fd = os.open("/dev/tty", os.O_RDWR | os.O_NONBLOCK)
except OSError:
    print("controlling-terminal-unavailable", flush=True)
else:
    os.write(fd, b"GIT-TERMINAL-BYPASS\\n")
    os.close(fd)
pathlib.Path("hook-ready").touch()
while not pathlib.Path("hook-release").exists(): time.sleep(.01)
''')
    hook.chmod(0o700)
    command = ("git -C terminal-git -c core.hooksPath=.git/hooks -c commit.gpgSign=false "
               "-c user.name=Fixture -c user.email=fixture@example.invalid commit --allow-empty -qm fixture")
    class PasswordServer(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args): pass
        def do_GET(self):
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="git fixture"')
            self.send_header("Content-Length", "0")
            self.end_headers()
    auth = http.server.ThreadingHTTPServer(("127.0.0.1", 0), PasswordServer)
    threading.Thread(target=auth.serve_forever, daemon=True).start()
    auth_command = ("LC_ALL=C GIT_TERMINAL_PROMPT=1 GIT_ASKPASS='' SSH_ASKPASS='' "
                    "git -c core.askPass= -c credential.helper= -c credential.username=fixture-user ls-remote "
                    f"http://fixture-user@127.0.0.1:{auth.server_port}/repo.git")
    def respond(body, n):
        if n == 1: return reply(block(0, "tool_use", args={"command": command}))
        if n == 2: return reply(block(0, "tool_use", args={"command": auth_command}))
        return reply()
    f.reset(respond)
    pid, fd = pty.fork()
    if pid == 0:
        os.chdir(f.directory)
        os.execve(BINARY, f.args + ["--no-log"], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))
    output = bytearray()
    def until(predicate):
        end = time.monotonic() + 6
        while time.monotonic() < end:
            if select.select([fd], [], [], .02)[0]:
                output.extend(os.read(fd, 65536))
            if predicate(): return
        raise AssertionError(bytes(output[-1500:]))
    try:
        until(lambda: b"type a message" in output)
        os.write(fd, b"commit using git\r")
        until(ready.exists)
        os.write(fd, b"queued during git\r")
        until(lambda: b"queued during git" in output)
        release.touch()
        until(lambda: len(f.requests) == 4)
        result = f.requests[1]["messages"][-1]["content"][0]["content"]
        assert "hook-stdout-captured" in result and "hook-stderr-captured" in result, result
        assert "GIT-TERMINAL-BYPASS".encode() not in output, "git bypassed tool capture through /dev/tty"
        assert "controlling-terminal-unavailable" in result, result
        auth_result = f.requests[2]["messages"][-1]["content"][0]
        assert auth_result["is_error"] and "could not read Password" in auth_result["content"], auth_result
        assert f.requests[3]["messages"][-1]["content"] == "queued during git"
        assert not f.errors, f.errors
    finally:
        release.touch()
        os.kill(pid, signal.SIGTERM)
        os.waitpid(pid, 0)
        os.close(fd)
        auth.shutdown()
        auth.server_close()
    print("terminal: git hook capture, /dev/tty isolation, HTTP password failure returned to agent, queued input preserved: ok", flush=True)


def queue_check(f):
    entered, release = threading.Event(), threading.Event()
    status = [200]
    def respond(body, n):
        if n == 1:
            entered.set()
            assert release.wait(8), "test did not release request"
            if status[0] != 200: return status[0], b'{"error":{"message":"fixture failure"}}'
        return reply()
    for mode in ["fifo", "error", "cancel", "full"]:
        entered.clear(); release.clear(); status[0] = 500 if mode == "error" else 200
        f.reset(respond)
        f.max_active = 0
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(f.directory)
            os.execve(BINARY, f.args + ["--no-log"], f.env)
        fcntl.ioctl(fd,termios.TIOCSWINSZ,struct.pack("HHHH",30,120,0,0))
        output = bytearray()
        def drain(seconds=.15):
            end = time.monotonic()+seconds
            while time.monotonic()<end:
                ready,_,_=select.select([fd],[],[],.01)
                if ready:
                    try: output.extend(os.read(fd,65536))
                    except OSError: return
        def until(predicate):
            end=time.monotonic()+6
            while time.monotonic()<end:
                drain(.03)
                if predicate(): return
            raise AssertionError((mode,bytes(output[-1500:])))
        try:
            until(lambda: b"type a message" in output)
            os.write(fd,b"first\r")
            until(entered.is_set)
            count = 17 if mode == "full" else 3
            for i in range(count): os.write(fd,f"queued-{i}\r".encode())
            drain(.3)
            assert len(f.requests)==1, (mode,len(f.requests))
            if mode == "cancel":
                os.write(fd,b"\x03")
                until(lambda: b"queued prompts paused" in output)
            release.set()
            if mode in ("error","cancel"):
                until(lambda: b"queued prompts paused" in output)
                drain(.3)
                assert len(f.requests)==1
                os.write(fd,b"\r")
                until(lambda: len(f.requests)==4)
            else:
                expected = 17 if mode == "full" else 4
                until(lambda:len(f.requests)==expected)
                drain(.3)
                assert len(f.requests)==expected
                if mode == "full":
                    assert b"draft kept" in output
                    os.write(fd,b"\r")
                    until(lambda:len(f.requests)==18)
            prompts=[r["messages"][-1]["content"] for r in f.requests]
            assert prompts==["first"]+[f"queued-{i}" for i in range(count)],(mode,prompts)
            if mode != "cancel": assert f.max_active==1
            assert not f.errors,f.errors
        finally:
            release.set()
            os.kill(pid,signal.SIGTERM)
            os.waitpid(pid,0)
            os.close(fd)
    print("queue: gated FIFO, no overlapping requests, pause on errors/cancel, explicit resume, full draft retained: ok",flush=True)


def main():
    with tempfile.TemporaryDirectory(prefix="lambda-reliability-") as directory:
        f=Fixture(directory)
        try:
            check(f)
            terminal_isolation_check(f)
            queue_check(f)
        finally: f.close()
    print("all reliability checks passed",flush=True)

if __name__=="__main__": main()
