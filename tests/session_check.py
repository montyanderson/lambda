#!/usr/bin/env python3
"""Real processes sharing a working directory and a transcript, without APIs."""
import concurrent.futures
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent
BINARY = str(ROOT / "lambda")


def main():
    with tempfile.TemporaryDirectory(prefix="lambda-sessions-") as tmp:
        env = {k: v for k, v in os.environ.items() if not k.startswith(("LITELLM_", "ANTHROPIC_", "EXA_"))}
        env.update(XDG_CONFIG_HOME=tmp, NO_COLOR="1")
        args = [BINARY, "--anthropic", "--no-context"]
        children = []

        def start(*extra):
            p = subprocess.Popen(args + list(extra), cwd=tmp, env=env,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            children.append(p)
            return p

        def wait_locked(path, owner):
            with path.open("r+") as f:
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    assert owner.poll() is None, owner.communicate()
                    try:
                        fcntl.lockf(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                        fcntl.lockf(f, fcntl.LOCK_UN)
                    except BlockingIOError:
                        return
                    time.sleep(0.02)
            raise AssertionError("transcript not locked")

        try:
            # Keep all starts alive together; each must own a different file.
            with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
                running = list(pool.map(lambda _: start(), range(24)))
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                logs = list((Path(tmp) / ".lambda/chats").glob("*.jsonl"))
                if len(logs) == len(running):
                    break
                time.sleep(0.02)
            assert len(logs) == len(running), (len(logs), len(running))
            for p in running:
                p.communicate(b"", timeout=5)
                assert p.returncode == 0
            for log in logs:
                records = [json.loads(line) for line in log.read_text().splitlines()]
                assert len(records) == 1 and records[0]["t"] == "session", records
            print("24 concurrent starts: separate, valid transcripts")

            log = logs[0]
            # The newest write wins even if its filename sorts before another.
            log = min(logs, key=lambda p: p.name)
            for p in logs:
                os.utime(p, ns=(1_000_000_000, 1_000_000_000))
            os.utime(log, ns=(1_000_000_001, 1_000_000_001))
            recent = start("-c")
            out, err = recent.communicate(b"", timeout=5)
            assert recent.returncode == 0 and log.name.encode() in out + err
            original = log.read_bytes()
            alias = Path(tmp) / "alias.jsonl"
            alias.symlink_to(log)
            for kill in [False, True]:
                owner = start("-r", str(log))
                wait_locked(log, owner)
                for path, extra in [(log, []), (alias, ["--no-log"])]:
                    other = start("-r", str(path), *extra, "-p", "must not be sent")
                    out, err = other.communicate(timeout=5)
                    assert other.returncode == 1 and b"already open in another lambda process" in out + err, (out, err)
                    assert log.read_bytes() == original
                if kill:
                    owner.kill()
                owner.communicate(b"" if not kill else None, timeout=5)
                after = start("-r", str(log))
                after.communicate(b"", timeout=5)
                assert after.returncode == 0
            print("resume: exclusive across paths, no-log and replay; locks released on exit and kill")

            missing = Path(tmp) / "missing.jsonl"
            p = start("-r", str(missing), "-p", "must not be sent")
            out, err = p.communicate(timeout=5)
            assert p.returncode == 1 and b"cannot resume" in out + err and not missing.exists()
        finally:
            for p in children:
                if p.poll() is None:
                    p.kill()
                    p.communicate()
    print("all session checks passed")


if __name__ == "__main__":
    main()
