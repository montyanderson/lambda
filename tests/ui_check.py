#!/usr/bin/env python3
"""ui-level regression tests. no network, no api key.

two classes of bug live here, both of which shipped once:

  render fidelity — term.c's diff renderer must leave the terminal in exactly
  the state its own back buffer describes. drift shows up as stale glyphs,
  and wide characters (cjk, emoji) are the usual cause.

  stale frames — repaints are coalesced, so a burst of scroll events can
  defer the final frame. if nothing later flushes it the screen sits on an
  old frame, which reads as the display sticking mid-scroll.

  markdown tables and the /model picker are laid out against the frame
  width, so they are checked on a real screen rather than in isolation.
"""
import fcntl, os, pty, select, struct, subprocess, sys, termios, time

try:
    import pyte
except ImportError:
    print("pyte is required: pip install pyte", file=sys.stderr)
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HARNESS = os.path.join(ROOT, "build", "tests", "term_harness")
LAMBDA = os.path.join(ROOT, "lambda")
failures = 0


def fail(msg):
    global failures
    failures += 1
    print(f"  FAIL {msg}")


def render_fidelity():
    """term.c's output, replayed through an independent emulator, must match
    term.c's own back buffer."""
    print("render fidelity (term.c vs an independent emulator)")
    for W, H, rounds, seed in [(80, 24, 30, 1), (120, 40, 25, 2),
                               (200, 50, 20, 3), (60, 20, 35, 4)]:
        dump = f"/tmp/lambda_termdump_{seed}.txt"
        esc = subprocess.run([HARNESS, str(W), str(H), str(rounds), str(seed),
                              dump], capture_output=True).stdout
        screen = pyte.Screen(W, H)
        pyte.ByteStream(screen).feed(esc)

        want = {}
        with open(dump) as f:
            for line in f:
                y, x, cp = line.split()
                want[(int(y), int(x))] = chr(int(cp))
        got = {(y, x): d
               for y in range(H) for x in range(W)
               if (d := screen.buffer[y][x].data) and d != " "}

        bad = [(k, want.get(k), got.get(k))
               for k in set(want) | set(got) if want.get(k) != got.get(k)]
        if bad:
            fail(f"{W}x{H}: {len(bad)} cells differ, e.g. {bad[:3]}")
        else:
            print(f"  {W}x{H}: clean")


def stale_frames():
    """after a burst of scrolling, what is on screen must equal what a
    guaranteed full repaint produces."""
    print("stale frames after scroll bursts")
    ROWS, COLS = 24, 80
    pid, fd = pty.fork()
    if pid == 0:
        os.environ["TERM"] = "xterm-256color"
        os.environ["LAMBDA_SELFTEST_FILL"] = "150"
        os.environ.pop("ANTHROPIC_API_KEY", None)
        os.execv(LAMBDA, ["lambda", "--no-log", "--no-context"])
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))
    screen = pyte.Screen(COLS, ROWS)
    stream = pyte.ByteStream(screen)

    def pump(sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([fd], [], [], 0.02)
            if r:
                try:
                    d = os.read(fd, 65536)
                except OSError:
                    return
                if not d:
                    return
                stream.feed(d)

    def snap():
        return ["".join(c if (c := screen.buffer[y][x].data) else " "
                        for x in range(COLS)) for y in range(ROWS)]

    pump(1.5)
    for seq, n, label in [(b"\x1b[5~", 6, "PgUp x6"),
                          (b"\x1b[6~", 4, "PgDn x4"),
                          (b"\x1b[<64;10;10M", 12, "wheel-up x12"),
                          (b"\x1b[<65;10;10M", 10, "wheel-down x10")]:
        for _ in range(n):          # faster than the coalescing window
            os.write(fd, seq)
            time.sleep(0.004)
        pump(1.0)
        settled = snap()
        # a resize forces an unconditional full repaint: ground truth
        fcntl.ioctl(fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS - 1, 0, 0))
        pump(0.4)
        fcntl.ioctl(fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", ROWS, COLS, 0, 0))
        pump(0.8)
        truth = snap()
        diff = [i for i, (a, b) in enumerate(zip(settled, truth)) if a != b]
        if diff:
            fail(f"{label}: {len(diff)} rows stale, first row {diff[0]}:\n"
                 f"       on screen: {settled[diff[0]][:60]!r}\n"
                 f"       should be: {truth[diff[0]][:60]!r}")
        else:
            print(f"  {label}: clean")
    os.write(fd, b"\x04")
    pump(1.0)
    try:
        os.kill(pid, 9)
        os.waitpid(pid, 0)
    except OSError:
        pass


class Term:
    """lambda on a pty, with an independent emulator watching the output."""

    def __init__(self, rows, cols, env=None):
        self.rows, self.cols = rows, cols
        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            os.environ["TERM"] = "xterm-256color"
            os.environ.pop("ANTHROPIC_API_KEY", None)
            os.environ.update(env or {})
            os.execv(LAMBDA, ["lambda", "--no-log", "--no-context"])
        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", rows, cols, 0, 0))
        self.screen = pyte.Screen(cols, rows)
        self.stream = pyte.ByteStream(self.screen)

    def pump(self, sec):
        end = time.time() + sec
        while time.time() < end:
            r, _, _ = select.select([self.fd], [], [], 0.02)
            if r:
                try:
                    d = os.read(self.fd, 65536)
                except OSError:
                    return
                if not d:
                    return
                self.stream.feed(d)

    def send(self, data, sec=0.6):
        os.write(self.fd, data)
        self.pump(sec)

    def rows_text(self):
        return ["".join(c if (c := self.screen.buffer[y][x].data) else " "
                        for x in range(self.cols)) for y in range(self.rows)]

    def close(self):
        try:
            os.write(self.fd, b"\x04")
            self.pump(0.3)
            os.kill(self.pid, 9)
            os.waitpid(self.pid, 0)
        except OSError:
            pass


def markdown_tables():
    """a table must come out rectangular, with its bars in one column, at
    every frame width."""
    print("markdown tables")
    for COLS in (100, 76, 52, 40, 34):
        t = Term(24, COLS, {"LAMBDA_SELFTEST_MD": "1"})
        t.pump(1.5)
        rows = [r for r in t.rows_text() if "│" in r and ("┬" in r or "┼" in r
                or "┴" in r or r.count("│") > 2)]
        t.close()
        if len(rows) < 6:
            fail(f"{COLS} cols: found {len(rows)} table rows, want 6")
            continue
        # the transcript frame's own bars sit at column 0 and COLS-1; the
        # table's are everything in between, and must line up
        cols_of = [tuple(i for i, ch in enumerate(r)
                         if ch in "│┬┼┴├┤╭╮╰╯" and 0 < i < COLS - 1)
                   for r in rows]
        if len(set(cols_of)) != 1:
            fail(f"{COLS} cols: table bars do not line up: "
                 f"{sorted(set(cols_of))[:2]}")
        else:
            print(f"  {COLS} cols: {len(rows)} rows, bars at {cols_of[0]}")


def fenced_table():
    """a table inside a ``` block is code, and must not be reformatted."""
    print("tables inside code fences")
    t = Term(24, 80, {"LAMBDA_SELFTEST_MD": "2"})
    t.pump(1.5)
    rows = t.rows_text()
    t.close()
    body = "\n".join(rows)
    if "| fenced | table |" not in body:
        fail("the fenced table's source text is not on screen")
    if any(ch in body for ch in "┬┼┴"):
        fail("a fenced table was laid out as a table")
    else:
        print("  left verbatim")


def model_picker():
    """/model opens a list, and choosing from it retitles the frame."""
    print("model picker")
    t = Term(24, 80)
    t.pump(1.5)
    t.send(b"/model\r", 0.8)
    body = "\n".join(t.rows_text())
    if "select model" not in body:
        fail("/model did not open the picker")
    if "claude-opus-5" not in body:
        fail("the picker did not list the shortlist")
    if "esc cancel" not in body:
        fail("the picker did not say how to get out")

    t.send(b"\x1b", 0.6)  # esc: nothing changes
    if "select model" in "\n".join(t.rows_text()):
        fail("esc did not close the picker")
    if "claude-fable-5" not in t.rows_text()[0]:
        fail("cancelling the picker changed the model")

    t.send(b"/model\r", 0.8)
    t.send(b"\x1b[B", 0.4)  # down one, onto claude-opus-5
    t.send(b"\r", 0.8)
    top = t.rows_text()[0]
    if "claude-opus-5" not in top:
        fail(f"picking a model did not retitle the frame: {top.strip()!r}")
    else:
        print("  opened, cancelled, and switched model")
    t.close()


render_fidelity()
stale_frames()
markdown_tables()
fenced_table()
model_picker()
print()
if failures:
    print(f"ui_check: {failures} failure(s)")
    sys.exit(1)
print("ui_check: ok")
