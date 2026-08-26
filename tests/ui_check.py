#!/usr/bin/env python3
"""ui-level regression tests. no network, no api key.

two classes of bug live here, both of which shipped once:

  render fidelity — term.c's diff renderer must leave the terminal in exactly
  the state its own back buffer describes. drift shows up as stale glyphs,
  and wide characters (cjk, emoji) are the usual cause.

  stale frames — repaints are coalesced, so a burst of scroll events can
  defer the final frame. if nothing later flushes it the screen sits on an
  old frame, which reads as the display sticking mid-scroll.
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


render_fidelity()
stale_frames()
print()
if failures:
    print(f"ui_check: {failures} failure(s)")
    sys.exit(1)
print("ui_check: ok")
