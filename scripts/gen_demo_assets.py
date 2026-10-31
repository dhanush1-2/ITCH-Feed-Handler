#!/usr/bin/env python3
"""Run the real commands and render their real output as the README's images.

    gen_demo_assets.py [--only NAME[,NAME]]     tests verify compare loss cmp fuzz replay dashboard
    gen_demo_assets.py --print-fuzz             print results/fuzz.json as a table

Nothing is mocked: each image is the captured output of a command this script ran, and a command
that fails stops the script with no image written. Adapted from Project 1's script. It needs the
built tools, data/12302019.NASDAQ_ITCH50, Pillow, and for the dashboard images a Python with
Playwright, named by ITCH_PLAYWRIGHT_PYTHON. ffmpeg is not assumed, so the GIF comes from Pillow.

The full-day captures take a few minutes (a verified replay and a loss run). Their dumps go to
build/assets/, never to results/.
"""
import argparse
import functools
import http.server
import json
import os
import pty
import re
import select
import shlex
import signal
import socketserver
import struct
import subprocess
import sys
import textwrap
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = Path("docs/images")
WORK = Path("build/assets")
DAY = "data/12302019.NASDAQ_ITCH50"
VERIFY = "build/verify/tools/itch_replay"
REPLAY = "build/release/tools/itch_replay"
SIM = "build/release/tools/mold_sim"
TUI_SYMBOLS = "AAPL,MSFT,SPY,QQQ,AMD"
TUI_FROM = "09:29:50"
TUI_FIRST, TUI_LAST = "09:29:55", "09:30:25"  # the part of the feed shown in the GIF

BG, FG, PROMPT = (22, 24, 29), (222, 226, 232), (120, 200, 255)
RED, GREEN = (240, 113, 120), (127, 217, 98)
LH, PAD = 26, 20
WRAP = 110
SGR = re.compile(r"\x1b\[(\d*)m")


def font():
    from PIL import ImageFont
    return ImageFont.truetype("/System/Library/Fonts/Menlo.ttc", 20)


def spans(line):
    color, pos, out = FG, 0, []
    for m in SGR.finditer(line):
        out.append((line[pos:m.start()], color))
        color = {"31": RED, "32": GREEN}.get(m.group(1), FG)
        pos = m.end()
    out.append((line[pos:], color))
    return out


def width_of(line):
    return len(SGR.sub("", line))


def render(lines, cols=None, rows=None):
    from PIL import Image, ImageDraw
    f = font()
    cw = f.getlength("M")
    cols = cols or max(width_of(x) for x in lines)
    rows = rows or len(lines)
    img = Image.new("RGB", (int(cols * cw) + 2 * PAD, rows * LH + 2 * PAD), BG)
    d = ImageDraw.Draw(img)
    for y, line in enumerate(lines):
        x = PAD
        base = PROMPT if line.startswith("$ ") else None
        for text, color in spans(line):
            d.text((x, PAD + y * LH), text, font=f, fill=base or color)
            x += len(text) * cw
    return img


def shot(name, shown, lines):
    """One image: every command as it was run, then its output."""
    head = []
    for cmd in ([shown] if isinstance(shown, str) else shown):
        wrapped = textwrap.wrap(cmd, WRAP, subsequent_indent="    ", break_long_words=False)
        head += ["$ " + wrapped[0]] + wrapped[1:]
    wrapped_out = []
    for line in lines:
        wrapped_out += textwrap.wrap(line, WRAP, subsequent_indent="    ",
                                     break_long_words=False, drop_whitespace=False) or [""]
    render(head + wrapped_out).save(OUT / name, optimize=True)
    print("wrote", OUT / name, flush=True)


def run(cmd, env=None):
    print("+", " ".join(shlex.quote(c) for c in cmd), flush=True)
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode != 0:
        sys.stdout.write(r.stdout[-3000:])
        sys.stderr.write(r.stderr[-3000:])
        raise SystemExit(f"gen_demo_assets: exit {r.returncode} from {cmd[0]}; no image written")
    return (r.stdout + r.stderr).splitlines()


def tests():
    out = run(["ctest", "--test-dir", "build/release"])
    picks = [x for x in out if "Test " in x and "#" in x]
    short = [re.sub(r"\.{3,}\s*", " .. ", x.strip()) for x in picks[:4] + ["..."] + picks[-4:]]
    tail = [x for x in out if "tests passed" in x or "Total Test time" in x]
    shot("proof-ctest.png", "ctest --test-dir build/release   # middle of the list cut", short + [""] + tail)


def verify():
    WORK.mkdir(parents=True, exist_ok=True)
    cmd = [VERIFY, DAY, "--verify", "--symbols", "AAPL,MSFT,SPY", "--dump-state",
           str(WORK / "state_direct.bin"), "--dump-at-s", "43200"]
    shot("proof-verify.png", " ".join(cmd), [x.replace(str(ROOT) + "/", "") for x in run(cmd)])


def compare():
    cmd = ["python3", "-I", "verify/compare.py", "results/cpp_subset_12302019.json",
           "results/ref_12302019.json"]
    shot("proof-compare.png", " ".join(cmd), run(cmd))


def loss():
    WORK.mkdir(parents=True, exist_ok=True)
    cmd = [SIM, DAY, "--loss", "0.01", "--dup", "0.005", "--reorder", "16", "--burst", "0.0005,64",
           "--retransmit-loss", "0.01", "--seed", "1", "--retention", "1048576", "--dump-state",
           str(WORK / "state_loss.bin"), "--dump-at-s", "43200"]
    shot("proof-loss.png", " ".join(cmd), run(cmd))


def cmp_states():
    a, b = str(WORK / "state_direct.bin"), str(WORK / "state_loss.bin")
    # cmp prints nothing and exits 0 when the files are equal; run() stops the script otherwise.
    out = run(["cmp", a, b])
    if out:
        raise SystemExit(f"gen_demo_assets: cmp printed {out}")
    sha = run(["shasum", "-a", "256", a, b])
    size = run(["wc", "-c", a, b])[:2]
    render([f"$ cmp {a} {b}", "$ echo $?", "0", f"$ shasum -a 256 {a} {b}", *sha,
            f"$ wc -c {a} {b}", *[x.strip() for x in size]]).save(OUT / "proof-cmp.png", optimize=True)
    print("wrote", OUT / "proof-cmp.png", flush=True)


def fuzz_lines():
    lines = [f"{'target':<13}{'requested s':>12}{'executions':>14}{'crashes':>9}  commit   compiler"]
    for r in json.loads(Path("results/fuzz.json").read_text())["runs"]:
        lines.append(f"{r['target']:<13}{r['duration_s']:>12}{r['executed_units']:>14,}"
                     f"{r['crashes']:>9}  {r['meta']['commit'][:7]}  {r['meta']['compiler']}")
    lines.append("run with libFuzzer in Docker, alpine:3.20; figures as recorded in results/fuzz.json")
    return lines


def fuzz():
    cmd = ["python3", "scripts/gen_demo_assets.py", "--print-fuzz"]
    shot("proof-fuzz.png", " ".join(cmd), run(cmd))


def replay():
    """The real --tui replay under a pty, around the open, sampled twice a wall second."""
    import fcntl
    import termios
    from PIL import Image
    master, slave = pty.openpty()
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 28, 86, 0, 0))
    p = subprocess.Popen([REPLAY, DAY, "--speed", "1", "--from", TUI_FROM, "--tui", TUI_SYMBOLS],
                         stdin=slave, stdout=slave, stderr=slave, close_fds=True)
    os.close(slave)
    feed = re.compile(rb"feed (\d\d:\d\d:\d\d)")
    buf, frames, deadline = b"", [], time.time() + 180
    next_sample = 0.0
    while time.time() < deadline:
        if select.select([master], [], [], 0.05)[0]:
            try:
                buf += os.read(master, 65536)
            except OSError:
                break
        parts = buf.split(b"\x1b[H")
        if len(parts) < 3:
            continue
        latest = parts[-2]  # the last whole frame
        buf = b"\x1b[H" + parts[-1]
        m = feed.search(latest)
        if not m:
            continue
        at = m.group(1).decode()
        if at >= TUI_LAST:
            break
        if at >= TUI_FIRST and time.time() >= next_sample:
            next_sample = time.time() + 0.5
            text = re.sub(r"\x1b\[(K|J|\?\d+[hl])", "", latest.decode(errors="replace"))
            frames.append(text.replace("\r", "").rstrip("\n").split("\n"))
    os.write(master, b"q")
    # Keep reading: the replay blocks on a full pty and could never see the q otherwise.
    stop = time.time() + 30
    while p.poll() is None and time.time() < stop:
        if select.select([master], [], [], 0.05)[0]:
            try:
                os.read(master, 65536)
            except OSError:
                break
    if p.poll() is None:
        p.kill()
    rc = p.wait()
    os.close(master)
    if rc != 0 or len(frames) < 20:
        raise SystemExit(f"gen_demo_assets: --tui capture rc {rc}, {len(frames)} frames; no GIF written")
    cols = max(width_of(x) for f in frames for x in f)
    rows = max(len(f) for f in frames)
    images = [render(f, cols, rows) for f in frames]
    small = [i.resize((i.width * 2 // 3, i.height * 2 // 3)).convert("P", palette=Image.ADAPTIVE, colors=32)
             for i in images]
    small[0].save(OUT / "demo-replay.gif", save_all=True, append_images=small[1:], duration=500,
                  loop=0, optimize=True)
    print("wrote", OUT / "demo-replay.gif", "from", len(frames), "frames", flush=True)


def dashboard_capture():
    """Runs under the Playwright Python: the real page, served locally, no console errors allowed."""
    from playwright.sync_api import sync_playwright
    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory="dashboard")
    handler.func.log_message = lambda *a: None
    srv = socketserver.TCPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    with sync_playwright() as pw:
        b = pw.chromium.launch()
        ctx = b.new_context(viewport={"width": 1280, "height": 900}, color_scheme="light")
        page = ctx.new_page()
        errors = []
        page.on("console", lambda m: errors.append(m.text) if m.type == "error" else None)
        page.on("pageerror", lambda e: errors.append(str(e)))
        page.goto(f"http://127.0.0.1:{srv.server_address[1]}/index.html")
        page.wait_for_selector("body[data-ready='1']", timeout=15000)
        page.wait_for_timeout(500)
        if errors:
            raise SystemExit(f"dashboard console errors: {errors}")
        page.screenshot(path=str(OUT / "dashboard-full.png"), full_page=True)
        page.click("#symbols button:text-is('AAPL')")
        page.wait_for_timeout(300)
        page.locator("#p-book").screenshot(path=str(OUT / "dashboard-spread.png"))
        b.close()
    srv.shutdown()


def dashboard():
    python = os.environ.get("ITCH_PLAYWRIGHT_PYTHON")
    if not python:
        raise SystemExit("gen_demo_assets: set ITCH_PLAYWRIGHT_PYTHON to a Python with Playwright")
    subprocess.run([python, "scripts/gen_demo_assets.py", "--dashboard-capture"], check=True)
    from PIL import Image
    for name in ("dashboard-full.png", "dashboard-spread.png"):  # smaller files, same pixels shown
        path = OUT / name
        Image.open(path).convert("P", palette=Image.ADAPTIVE, colors=128).save(path, optimize=True)
        print("wrote", path, flush=True)


# ctest is last: tests/test_docs_fresh.py requires the other images to exist.
STEPS = {"verify": verify, "compare": compare, "loss": loss, "cmp": cmp_states, "fuzz": fuzz,
         "replay": replay, "dashboard": dashboard, "tests": tests}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--only", default="")
    ap.add_argument("--print-fuzz", action="store_true")
    ap.add_argument("--dashboard-capture", action="store_true", help=argparse.SUPPRESS)
    args = ap.parse_args()
    os.chdir(ROOT)
    OUT.mkdir(parents=True, exist_ok=True)
    if args.print_fuzz:
        print("\n".join(fuzz_lines()))
        return
    if args.dashboard_capture:
        dashboard_capture()
        return
    names = args.only.split(",") if args.only else list(STEPS)
    for name in names:
        started = time.time()
        STEPS[name]()
        print(f"{name}: {time.time() - started:.1f} s", flush=True)


if __name__ == "__main__":
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    main()
