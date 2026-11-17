#!/usr/bin/env python3
"""Automatic conversation memory, using the real binary and a local provider."""
import http.server
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

from gateway_check import event, ROOT

BINARY = os.environ.get("LAMBDA_TEST_BINARY", str(ROOT / "lambda"))
state = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def respond(self, status, body):
        encoded = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Length", str(len(encoded)))
        self.end_headers()
        self.wfile.write(encoded)

    def do_GET(self):
        self.respond(200, {"data": [{"id": "test-model", "max_input_tokens": state["window"],
                                    "max_output_tokens": 4096}]})

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        if self.path.endswith("count_tokens"):
            state["counts"].append(body)
            if state.get("exact"):
                self.respond(200, {"input_tokens": state["exact"]})
            else:
                self.respond(404, {"error": {"message": "not supported"}})
            return
        summary = isinstance(body.get("system"), str) and body["system"].startswith("You maintain compact")
        state["summaries" if summary else "normal"].append(body)
        mode = state["mode"]
        if summary and mode == "summary-fail" and len(state["summaries"]) == 2:
            self.respond(503, {"error": {"message": "temporary summary failure"}})
            return
        chunk = body["messages"][0]["content"][1]["text"] if summary else ""
        previous = body["messages"][0]["content"][0]["text"] if summary else ""
        if ((not summary and mode == "overflow" and len(state["normal"]) == 1) or
                (summary and mode == "summary-overflow" and len(chunk) > 1500) or
                (summary and mode == "previous-memory" and len(previous) + len(chunk) > 5000)):
            self.respond(400, {"error": {"type": "context_length_exceeded", "message": "maximum context length exceeded"}})
            return
        assert not summary or ("tools" not in body and "thinking" not in body)
        usage = {"input_tokens": 20}
        if not summary and mode == "cached" and len(state["normal"]) == 1:
            usage.update(cache_read_input_tokens=7000, cache_creation_input_tokens=100)
        reply = event("message_start", message={"model": body["model"], "usage": usage})
        tool = not summary and mode in ("tool", "cached") and len(state["normal"]) == 1
        if tool:
            command = "python3 -c \"print('x'*12000)\"" if mode == "tool" else "printf already-done"
            reply += event("content_block_start", index=0, content_block={
                "type": "tool_use", "id": "call_once", "name": "bash", "input": {}})
            reply += event("content_block_delta", index=0, delta={
                "type": "input_json_delta", "partial_json": json.dumps({"command": command})})
        else:
            text = "Memory: keep the user's goal. Completed ACTION_DONE; file /tmp/task.c. Continue NEXT_STEP." if summary else "continued successfully"
            if summary and mode == "reasoning-only" and len(state["summaries"]) == 1:
                reply += event("content_block_start", index=0, content_block={"type": "thinking", "thinking": "reasoning budget used", "signature": "sig"})
            else:
                reply += event("content_block_start", index=0, content_block={"type": "text", "text": text})
        reply += event("content_block_stop", index=0)
        stop = "tool_use" if tool else ("max_tokens" if summary and mode == "truncated" else "end_turn")
        reply += event("message_delta", delta={"stop_reason": stop}, usage={"output_tokens": 40})
        if not (summary and mode == "incomplete"):
            reply += event("message_stop")
        self.send_response(200)
        self.send_header("Content-Length", str(len(reply)))
        self.end_headers()
        self.wfile.write(reply)


def reset(mode="ok", window=8192, **extra):
    state.clear()
    state.update(mode=mode, window=window, normal=[], summaries=[], counts=[], **extra)


def messages(n=16, size=2500):
    return [{"role": "user" if i % 2 == 0 else "assistant",
             "content": f"entry-{i:03} ACTION_DONE /tmp/task.c NEXT_STEP " + "abc " * (size // 4)}
            for i in range(n)]


def main():
    with tempfile.TemporaryDirectory(prefix="lambda-memory-") as tmp:
        server = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LITELLM_", "EXA_"))}
        env.update(XDG_CONFIG_HOME=tmp, NO_COLOR="1", LITELLM_BASE_URL=f"http://127.0.0.1:{server.server_port}",
                   LITELLM_API_KEY="fixture-key")

        def seed(name, content):
            file = Path(tmp) / (name + ".jsonl")
            records = [{"t": "session", "v": 1, "model": "test-model"}]
            records += [{"t": "msg", "m": m} for m in content]
            file.write_text("".join(json.dumps(r, ensure_ascii=False) + "\n" for r in records))
            return file

        def run(file=None, prompt="keep going", code=0, tools=False, extra=()):
            args = [BINARY, "--no-context", "--no-thinking", "--max-tokens", "256", "-m", "test-model"]
            if not tools:
                args += ["--no-tools"]
            if file:
                args += ["-r", str(file)]
            args += [*extra, "-p", prompt]
            result = subprocess.run(args, env=env, cwd=tmp, capture_output=True, text=True, timeout=20)
            assert result.returncode == code, (result.returncode, result.stdout[-1500:], result.stderr)
            return result.stdout + result.stderr

        def records(file):
            return [json.loads(line) for line in file.read_text().splitlines()]

        def memory(body):
            return body["messages"][0]["content"]

        try:
            reset()
            old = messages()
            file = seed("long", old)
            output = run(file)
            assert len(state["summaries"]) > 1, len(state["summaries"])
            assert len(state["normal"]) == 1
            assert "Earlier conversation memory" in memory(state["normal"][0])
            assert state["normal"][0]["messages"][-1] == {"role": "user", "content": "keep going"}
            assert "Memory: keep" not in output  # summaries never leak into user-facing replies
            chunks = "".join(b["messages"][0]["content"][1]["text"] for b in state["summaries"])
            assert "entry-000" in chunks and "ACTION_DONE" in memory(state["normal"][0])
            saved = records(file)
            assert any(r["t"] == "compact" for r in saved)
            assert all({"t": "msg", "m": m} in saved for m in old)
            reset()
            run(file, "next request")
            assert not state["summaries"]
            assert "Earlier conversation memory" in memory(state["normal"][0])
            assert any(m.get("content") == "keep going" for m in state["normal"][0]["messages"])
            # Repeated compaction updates memory rather than resurrecting old messages.
            with file.open("a") as f:
                for m in messages(12, 2500):
                    f.write(json.dumps({"t": "msg", "m": m}) + "\n")
            reset()
            run(file, "third request")
            assert state["summaries"]
            assert "Memory: keep" in state["summaries"][0]["messages"][0]["content"][0]["text"]
            reset()
            run(file, "fourth request")
            assert not state["summaries"]
            print("automatic threshold, chunked summaries, hidden output, transcript preservation, repeated resume: ok")

            for mode in ["summary-fail", "incomplete", "truncated"]:
                reset(mode)
                file = seed(mode, old)
                run(file, code=1)
                assert not any(r["t"] == "compact" for r in records(file))
                reset()
                run(file)
                assert "entry-000" in state["summaries"][0]["messages"][0]["content"][1]["text"]
            print("failed/interrupted/truncated summary: no partial commit or lost history: ok")

            reset("reasoning-only")
            file = seed("reasoning-only", old)
            run(file)
            assert state["summaries"][1]["max_tokens"] > state["summaries"][0]["max_tokens"]
            assert any(r["t"] == "compact" for r in records(file))
            print("reasoning-only summary completion: retry with a larger generation budget: ok")

            reset("overflow")
            file = seed("overflow", messages(6, 500))
            run(file)
            assert len(state["normal"]) == 2 and state["summaries"]
            assert "Earlier conversation memory" in memory(state["normal"][-1])
            reset("summary-overflow")
            file = seed("small-summary", messages(10, 2000))
            run(file)
            assert any(len(b["messages"][0]["content"][1]["text"]) <= 1500 for b in state["summaries"])
            print("provider context rejection: automatic shrink, compact, retry: ok")

            reset(exact=100)
            file = seed("exact", old)
            run(file)
            assert state["counts"] and not state["summaries"]
            assert len(state["normal"][0]["messages"]) == len(old) + 1
            print("provider count avoids unnecessary compaction when local estimate is high: ok")

            reset("tool", window=4096)
            file = seed("tool", [])
            run(file, tools=True, prompt="run the task exactly once")
            assert len(state["normal"]) == 2 and state["summaries"]
            assert state["normal"][-1]["messages"][-1]["content"] == "run the task exactly once"
            assert any(r["t"] == "compact" and r["pin"] == 0 for r in records(file))
            reset()
            run(file)
            assert state["normal"][0]["messages"][1]["content"] == "run the task exactly once"
            print("oversized tool output: current request pinned, tool pair summarised, no rerun: ok")

            reset("cached")
            file = seed("cached", messages(8, 400))
            run(file, tools=True)
            assert state["summaries"], "cache-read/creation tokens must contribute to pressure"
            print("cached input usage triggers compaction inside tool loops: ok")

            reset(window=65536)
            file = seed("message-count", messages(510, 50))
            run(file)
            assert state["summaries"], "fixed history arenas must compact before filling"
            reset(window=4096)
            file = seed("small-switch", messages(16, 1000))
            run(file)
            assert state["summaries"]
            with file.open("a") as f:
                f.write('{"t":"clear"}\n')
            reset()
            run(file)
            assert state["normal"][0]["messages"] == [{"role": "user", "content": "keep going"}]
            print("message capacity, smaller-model metadata, clear after compaction: ok")

            reset("previous-memory", window=4096)
            file = seed("oversized-memory", [])
            with file.open("a") as f:
                f.write(json.dumps({"t": "compact", "drop": 0, "pin": -1,
                                    "summary": "old memory ACTION_DONE " * 700}) + "\n")
                for m in messages(4, 200):
                    f.write(json.dumps({"t": "msg", "m": m}) + "\n")
            run(file)
            assert len(state["summaries"]) > 1
            assert "old memory" in state["summaries"][0]["messages"][0]["content"][1]["text"]
            print("switching to a small model also chunks oversized existing memory: ok")

            reset()
            file = seed("partial-checkpoint", messages(2, 100))
            with file.open("a") as f:
                f.write('{"t":"compact","drop":2,"summary":"unfinished')
            run(file, "after interruption")
            reset()
            run(file, "resume again")
            assert any(m.get("content") == "after interruption" for m in state["normal"][0]["messages"])
            assert any("entry-000" in m.get("content", "") for m in state["normal"][0]["messages"])
            print("partial checkpoint at EOF leaves old and subsequent messages resumable: ok")
        finally:
            server.shutdown()
            server.server_close()
    print("all context checks passed")


if __name__ == "__main__":
    main()
