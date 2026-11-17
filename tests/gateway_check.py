#!/usr/bin/env python3
"""Exercise the real binary against a local LiteLLM protocol fixture.

No real credentials, external providers, or third-party Python packages.
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

ROOT = Path(__file__).resolve().parent.parent
BINARY = str(ROOT / "lambda")
state = {"mode": "ok", "requests": []}
models = [f"provider/model-{i:03}" for i in range(300)]
long_id = "provider/" + "long-model-" * 24
models.append(long_id)


def event(kind, **fields):
    return ("event: " + kind + "\r\ndata: " + json.dumps(
        {"type": kind, **fields}) + "\r\n\r\n").encode()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_GET(self):
        state["requests"].append((self.path, dict(self.headers), None))
        status = 401 if state["mode"] == "auth" else 200
        data = [{"id": m, "owned_by": "test"} for m in reversed(models)]
        data.append(data[0])  # duplicate deployments must not duplicate choices
        response = json.dumps({"data": data}).encode()
        if state["mode"] == "bad":
            response = b'{"models": []}'
        elif state["mode"] == "empty":
            response = b'{"data": []}'
        elif state["mode"] == "oversize":
            response = json.dumps({"data": [{"id": "x" * 512}]}).encode()
        elif state["mode"] == "capacity":
            response = json.dumps({"data": [{"id": str(i)} for i in range(1025)]}).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(response)))
        self.end_headers()
        self.wfile.write(response)

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        state["requests"].append((self.path, dict(self.headers), body))
        reply = event("message_start", message={
            "model": body["model"], "usage": {"input_tokens": 10}})
        parallel = body["model"] == "parallel-tools" and len(body["messages"]) == 1
        tool = parallel or body["model"] == "tool-test" and len(body["messages"]) == 1
        if parallel:
            for index in range(2):
                reply += event("content_block_start", index=index, content_block={
                    "type": "tool_use", "id": f"call_{index}", "name": "bash", "input": {}})
            for part in range(2):
                for index in reversed(range(2)):
                    fragment = '{"command":' if part == 0 else f'"printf tool-{index}"}}'
                    reply += event("content_block_delta", index=index, delta={"type": "input_json_delta", "partial_json": fragment})
            for index in reversed(range(2)):
                reply += event("content_block_stop", index=index)
        elif tool:
            reply += event("content_block_start", index=0, content_block={
                "type": "tool_use", "id": "call_123", "name": "bash", "input": {}})
            for part in ['{"command":', '"printf lambda-tool-ok"}']:
                reply += event("content_block_delta", index=0, delta={
                    "type": "input_json_delta", "partial_json": part})
        elif state["mode"] == "interleaved":
            reply += event("content_block_start", index=0, content_block={"type": "text", "text": ""})
            reply += event("content_block_start", index=1, content_block={"type": "thinking", "thinking": "", "signature": ""})
            reply += event("content_block_delta", index=1, delta={"type": "thinking_delta", "thinking": "private reasoning"})
            reply += event("content_block_delta", index=0, delta={"type": "text_delta", "text": "hello from litellm"})
            reply += event("content_block_stop", index=0)
            reply += event("content_block_stop", index=1)
        else:
            reply += event("content_block_start", index=0, content_block={
                "type": "thinking", "thinking": "private reasoning", "signature": "sig"})
            reply += event("content_block_stop", index=0)
            if state["mode"] != "reasoning-only":
                reply += event("content_block_start", index=1, content_block={"type": "text", "text": ""})
                for part in ["hello ", "from litellm"]:
                    reply += event("content_block_delta", index=1, delta={"type": "text_delta", "text": part})
        if not parallel and (tool or state["mode"] not in ("reasoning-only", "interleaved")):
            reply += event("content_block_stop", index=0 if tool else 1)
        reply += event("message_delta", delta={"stop_reason": "tool_use" if tool else "max_tokens" if state["mode"] == "reasoning-only" else "end_turn"},
                       usage={"output_tokens": 5})
        if state["mode"] != "incomplete":
            reply += event("message_stop")
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Transfer-Encoding", "chunked")
        self.end_headers()
        # Fragment through JSON, SSE, and HTTP chunk boundaries.
        for i in range(0, len(reply), 31):
            chunk = reply[i:i + 31]
            self.wfile.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
        self.wfile.write(b"0\r\n\r\n")


def check():
    with tempfile.TemporaryDirectory(prefix="lambda-api-") as tmp:
        server = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}/gateway"
        env = {k: v for k, v in os.environ.items()
               if k not in ("LITELLM_BASE_URL", "LITELLM_PROXY_URL", "LITELLM_API_KEY", "EXA_API_KEY", "LAMBDA_MOUSE")}
        env.update(XDG_CONFIG_HOME=tmp, NO_COLOR="1", ANTHROPIC_API_KEY="never-send-this",
                   TERM="xterm-256color")
        cfg = Path(tmp) / "lambda" / "config.json"
        cfg.parent.mkdir()
        cfg.write_text(json.dumps({"base_url": base + "/v1/", "api_key": "fixture-key"}))
        config_before = cfg.read_bytes()
        preference = cfg.parent / "default-model"

        def run(*args, code=0, extra_env=None):
            result = subprocess.run([BINARY, "--no-context", *args], env={**env, **(extra_env or {})},
                                    cwd=tmp, text=True, capture_output=True, timeout=10)
            assert result.returncode == code, (args, result.returncode, result.stdout, result.stderr)
            return result.stdout + result.stderr

        result = run("--models")
        assert result.splitlines() == sorted(models)
        path, headers, _ = state["requests"][-1]
        assert path == "/gateway/v1/models?include_metadata=true"
        assert headers["Authorization"] == "Bearer fixture-key"
        assert headers["Host"] == f"127.0.0.1:{server.server_port}"
        assert "never-send-this" not in str(headers)
        run("--models", "--litellm", base, extra_env={"LITELLM_API_KEY": "env-key"})
        assert state["requests"][-1][1]["Authorization"] == "Bearer env-key"
        run("--models", extra_env={"LITELLM_BASE_URL": base, "LITELLM_API_KEY": ""})
        assert "Authorization" not in state["requests"][-1][1]
        assert "claude-fable-5" in run("--anthropic", "--models")
        for url in ["ftp://localhost", "http://user:pass@localhost", "http://localhost:0",
                    "http://localhost:65536", "http://localhost/path?query", "http://localhost/\r\nBad: x"]:
            assert "invalid LiteLLM URL" in run("--litellm", url, "--models", code=2)
        for mode, message in [("auth", "HTTP 401"), ("bad", "data array"),
                              ("oversize", "model id"), ("capacity", "1024")]:
            state["mode"] = mode
            assert message in run("--models", code=1)
        state["mode"] = "empty"
        assert run("--models") == ""
        assert "no models" in run("--no-log", "-p", "hello", code=1)
        state["mode"] = "ok"
        assert "select a LiteLLM model" in run("--no-log", "-p", "hello", code=1)
        print("catalog: config/env/CLI, auth isolation, 301 models, long ids, validation, errors: ok")

        output = run("--no-log", "--no-thinking", "-m", long_id, "-p", "hello")
        assert "hello from litellm" in output and "private reasoning" not in output
        path, headers, body = state["requests"][-1]
        assert path == "/gateway/v1/messages" and body["model"] == long_id
        assert body["max_tokens"] == 4096 and "fallbacks" not in body
        assert "anthropic-beta" not in headers
        run("--no-log", "--max-tokens", "256", "-m", "tool-test", "-p", "test tool")
        body = state["requests"][-1][2]
        assert body["max_tokens"] == 256
        result = body["messages"][-1]["content"][0]
        assert result == {"type": "tool_result", "tool_use_id": "call_123", "content": "lambda-tool-ok"}
        print("streaming: fragmented/chunked SSE, hidden thinking, bash tool round trip: ok")

        transcript = Path(tmp) / "resume.jsonl"
        records = [
            {"t": "session", "v": 1, "model": "old-provider/model"},
            {"t": "msg", "m": {"role": "user", "content": "old prompt"}},
            {"t": "msg", "m": {"role": "assistant", "content": [
                {"type": "thinking", "thinking": "old thoughts", "signature": "old signature"},
                {"type": "text", "text": "old answer"}]}},
        ]
        transcript.write_text("".join(json.dumps(record) + "\n" for record in records))
        run("--no-log", "-r", str(transcript), "-m", "new-provider/model", "-p", "continue")
        history = state["requests"][-1][2]["messages"]
        assert history[1]["content"] == [{"type": "text", "text": "old answer"}]
        state["mode"] = "incomplete"
        assert "missing message_stop" in run("-m", "test-model", "-p", "interrupted", code=1)
        log = next((Path(tmp) / ".lambda/chats").glob("*.jsonl"))
        assert any(json.loads(line).get("t") == "drop" for line in log.read_text().splitlines())
        state["mode"] = "ok"
        print("history: cross-provider resume, truncated stream failure and rollback: ok")

        # Incomplete reasoning from older transcripts must not poison the next
        # user turn. Complete, same-model reasoning must still round-trip.
        records[0]["model"] = "test-model"
        transcript.write_text("".join(json.dumps(r) + "\n" for r in records))
        run("--no-log", "-r", str(transcript), "-m", "test-model", "-p", "again")
        assert state["requests"][-1][2]["messages"][1] == records[2]["m"]
        records[2]["m"]["content"].pop()  # reasoning with no answer/tool call
        transcript.write_text("".join(json.dumps(r) + "\n" for r in records))
        before = transcript.read_bytes()
        run("--no-log", "-r", str(transcript), "-m", "test-model", "-p", "recover")
        assert [m["role"] for m in state["requests"][-1][2]["messages"]] == ["user", "user"]
        assert transcript.read_bytes() == before
        state["mode"] = "reasoning-only"
        output = run("-r", str(transcript), "-m", "test-model", "-p", "try", code=1)
        assert "before an answer or tool call" in output
        appended = [json.loads(line) for line in transcript.read_bytes()[len(before):].splitlines()]
        assert appended[-1] == {"t": "drop", "n": 1}
        assert not any(r.get("m", {}).get("role") == "assistant" for r in appended)
        state["mode"] = "ok"
        print("history: incomplete reasoning filtered on resume, completed reasoning retained, new partial turn rolled back: ok")

        state["mode"] = "interleaved"
        run("-r", str(transcript), "-m", "test-model", "-p", "overlapping blocks")
        saved = [json.loads(line) for line in transcript.read_text().splitlines()][-1]["m"]
        assert saved["content"] == [
            {"type": "text", "text": "hello from litellm"},
            {"type": "thinking", "thinking": "private reasoning", "signature": ""},
        ], saved
        run("--no-log", "-r", str(transcript), "-m", "test-model", "-p", "again")
        assert state["requests"][-1][2]["messages"][-2] == saved
        state["mode"] = "ok"
        run("--no-log", "-m", "parallel-tools", "-p", "interleaved tools")
        body = state["requests"][-1][2]
        assert body["messages"][-1]["content"] == [
            {"type": "tool_result", "tool_use_id": f"call_{i}", "content": f"tool-{i}"} for i in range(2)]
        print("streaming: overlapping reasoning/text and interleaved tool arguments stay separate and replay intact: ok")

        # Select an entry past the old 16-item ceiling using the actual TUI.
        pid, fd = pty.fork()
        if pid == 0:
            os.chdir(tmp)
            os.execve(BINARY, [BINARY, "--no-log", "--no-context"], env)
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 30, 120, 0, 0))

        def drain(seconds=0.2):
            end = time.monotonic() + seconds
            data = b""
            while time.monotonic() < end:
                ready, _, _ = select.select([fd], [], [], 0.05)
                if ready:
                    try:
                        data += os.read(fd, 65536)
                    except OSError:
                        break
            return data

        try:
            initial = drain(1)
            assert b"select model" in initial, initial[-500:]
            assert not preference.exists()
            os.write(fd, b"\x1b")
            drain()
            assert not preference.exists(), "cancelling must not save the highlighted model"
            os.write(fd, b"/model\r")
            drain()
            assert b"\x1b[?2004h" in initial
            assert b"\x1b[?1000h" in initial
            assert b"\x1b[?1006h" in initial
            assert b"\x1b[?1000l" in initial
            os.write(fd, b"no-such-model")
            assert b"no matching models" in drain()
            os.write(fd, b"\x15MODEL-029\r")  # ctrl-u; case-insensitive filter
            drain()
            assert preference.read_text() == "provider/model-029\neffort=default\n"
            assert preference.stat().st_mode & 0o777 == 0o600
            state["requests"].clear()
            os.write(fd, b"hello\r")
            drain(0.5)
            assert state["requests"][-1][2]["model"] == "provider/model-029"
            os.write(fd, b"/model\r")
            drain(0.3)
            os.write(fd, b"model-299\r")
            drain()
            os.write(fd, b"again\r")
            drain(0.5)
            assert state["requests"][-1][2]["model"] == "provider/model-299"
            assert preference.read_text() == "provider/model-299\neffort=default\n"
            assert len(state["requests"][-1][2]["messages"]) == 3
            # A failed refresh must leave the last good catalog selectable.
            state["mode"] = "auth"
            os.write(fd, b"/model\r")
            drain(0.3)
            os.write(fd, b"model-111\r")
            drain()
            state["mode"] = "ok"
            os.write(fd, b"again\r")
            drain(0.5)
            assert state["requests"][-1][2]["model"] == "provider/model-111"
            assert preference.read_text() == "provider/model-111\neffort=default\n"
            print("picker: startup, search, no matches, >16 entries, refresh failure, mid-chat switching: ok")

            state["requests"].clear()
            # Deliberately fragment both paste delimiters, including a pause
            # longer than the normal Escape-key timeout inside the end marker.
            for chunk in [b"\x1b[2", b"00~first line\r\n", b"\tsecond line \xe2\x9c\x93\x03\x04", b"\x1b", b"[201~"]:
                os.write(fd, chunk)
                drain(0.08)
            assert not any(body is not None for _, _, body in state["requests"])
            os.write(fd, b"\r")
            drain(0.5)
            body = state["requests"][-1][2]
            assert body["messages"][-1]["content"] == "first line\n\tsecond line ✓"
            assert len([body for _, _, body in state["requests"] if body is not None]) == 1
            print("clipboard: fragmented multiline paste submitted once on Enter; mouse scrolling enabled: ok")

            os.write(fd, b"/model provider/model-042\r")
            drain()
            assert preference.read_text() == "provider/model-042\neffort=default\n"
            run("--no-log", "-p", "saved default")
            assert state["requests"][-1][2]["model"] == "provider/model-042"
            run("--no-log", "-m", "override-model", "-p", "override")
            assert state["requests"][-1][2]["model"] == "override-model"
            run("--no-log", "-r", str(transcript), "-p", "resume")
            assert state["requests"][-1][2]["model"] == "test-model"
            assert preference.read_text() == "provider/model-042\neffort=default\n"
            assert cfg.read_bytes() == config_before

            child, child_fd = pty.fork()
            if child == 0:
                os.chdir(tmp)
                os.execve(BINARY, [BINARY, "--no-log", "--no-context"], {**env, "LAMBDA_MOUSE": "0"})
            try:
                output = b""
                end = time.monotonic() + 1
                while time.monotonic() < end:
                    if select.select([child_fd], [], [], 0.05)[0]:
                        output += os.read(child_fd, 65536)
                assert b"select model" not in output and b"provider/model-042" in output
                assert b"\x1b[?1000h" not in output and b"\x1b[?1000l" in output
            finally:
                os.kill(child, signal.SIGTERM)
                os.waitpid(child, 0)
                os.close(child_fd)
            print("preferences: first selection, cancellation, switches, restart, CLI/resume precedence and config preservation: ok")
        finally:
            os.kill(pid, signal.SIGTERM)
            os.waitpid(pid, 0)
            os.close(fd)
            server.shutdown()
            server.server_close()


if __name__ == "__main__":
    check()
    print("all gateway checks passed")
