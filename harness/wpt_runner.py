"""Run the CSS reftests of the chosen web-platform-tests directories: each test
and its reference are rendered by Kestrel at 800x600 and compared pixel by
pixel (within the test's <meta name=fuzzy> allowance).

    python harness/wpt_runner.py [--kestrel PATH] [--jobs N] [--filter SUBSTR] [--json out.json]
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WPT = os.path.join(ROOT, "data", "wpt")
DIRS = ["css/CSS2/margin-padding-clear", "css/CSS2/box-display", "css/CSS2/normal-flow", "css/CSS2/borders",
        "css/CSS2/colors", "css/CSS2/backgrounds", "css/css-flexbox"]

LINK = re.compile(r"<link\b[^>]*>", re.I)
ATTR = re.compile(r"""(\w+)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+))""")
FUZZY = re.compile(r"""<meta\s+name\s*=\s*["']?fuzzy["']?\s+content\s*=\s*["']([^"']*)["']""", re.I)


def default_kestrel():
    for p in [os.environ.get("KESTREL", ""), "C:/kt/release/kestrel.exe",
              os.path.join(ROOT, "engine", "target", "release", "kestrel.exe"),
              os.path.join(ROOT, "engine", "target", "release", "kestrel")]:
        if p and os.path.exists(p):
            return p
    sys.exit("build the engine first")


def fuzzy_range(spec):
    """'maxDifference=0-2;totalPixels=0-40' or '0-2;0-40' -> (maxdiff, maxpixels)"""
    md, tp = 0, 0
    parts = [p.strip() for p in spec.split(";") if p.strip()]
    vals = []
    for p in parts:
        if ":" in p and "=" not in p.split(":")[0]:
            p = p.split(":", 1)[1]
        p = p.split("=")[-1]
        hi = p.split("-")[-1]
        try:
            vals.append(int(hi))
        except ValueError:
            vals.append(0)
    if vals:
        md = vals[0]
    if len(vals) > 1:
        tp = vals[1]
    return md, tp


def collect(filt):
    tests = []
    for d in DIRS:
        base = os.path.join(WPT, d)
        for dp, _, files in os.walk(base):
            if "/support" in dp.replace("\\", "/") or "/reference" in dp.replace("\\", "/"):
                continue
            for f in sorted(files):
                if not re.search(r"\.(x?html?|xht)$", f) or "-ref." in f or f.endswith("-notref.html"):
                    continue
                p = os.path.join(dp, f)
                if filt and filt not in p.replace("\\", "/"):
                    continue
                try:
                    src = open(p, encoding="utf-8", errors="replace").read()
                except OSError:
                    continue
                for m in LINK.finditer(src):
                    attrs = {a.group(1).lower(): (a.group(2) or a.group(3) or a.group(4) or "") for a in ATTR.finditer(m.group(0))}
                    rel = attrs.get("rel", "").lower()
                    if rel in ("match", "mismatch") and attrs.get("href"):
                        ref = os.path.normpath(os.path.join(dp, attrs["href"]))
                        if attrs["href"].startswith("/"):
                            ref = os.path.join(WPT, attrs["href"].lstrip("/"))
                        md, tp = 0, 0
                        fm = FUZZY.search(src)
                        if fm:
                            md, tp = fuzzy_range(fm.group(1))
                        tests.append((p, ref, rel, md, tp))
                        break
    return tests


def run_chunk(kestrel, chunk, results, lock, timeout=20):
    """streams results; a worker silent for `timeout` seconds is killed (the current test fails)"""
    import queue
    pending = list(chunk)
    env = dict(os.environ, KESTREL_FONTS=os.path.join(ROOT, "fonts"))
    while pending:
        p = subprocess.Popen([kestrel, "reftest-batch", WPT], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace", env=env)
        nl, tab = chr(10), chr(9)
        p.stdin.write(nl.join(tab.join([t[0], t[1], t[2], str(t[3]), str(t[4])]) for t in pending) + nl)
        p.stdin.close()
        q = queue.Queue()
        threading.Thread(target=lambda: [q.put(l) for l in p.stdout] + [q.put(None)], daemon=True).start()
        done = set()
        timed_out = False
        while True:
            try:
                line = q.get(timeout=timeout)
            except queue.Empty:
                timed_out = True
                p.kill()
                break
            if line is None:
                break
            parts = line.rstrip(nl).split(" ")
            if len(parts) >= 2:
                with lock:
                    results[parts[1]] = (parts[0] == "PASS", " ".join(parts[2:]))
                done.add(parts[1])
        p.wait()
        rest = [t for t in pending if t[0] not in done]
        if rest:
            with lock:
                results[rest[0][0]] = (False, "timeout" if timed_out else f"crash (exit {p.returncode})")
            rest = rest[1:]
        pending = rest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kestrel", default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--filter", default="")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    k = a.kestrel or default_kestrel()
    tests = collect(a.filter)
    results = {}
    lock = threading.Lock()
    n = max(1, a.jobs)
    chunks = [tests[i::n] for i in range(n)]
    ths = [threading.Thread(target=run_chunk, args=(k, c, results, lock)) for c in chunks]
    for t in ths:
        t.start()
    for t in ths:
        t.join()
    by_dir = collections.defaultdict(lambda: [0, 0])
    for t in tests:
        rel = os.path.relpath(t[0], WPT).replace("\\", "/")
        d = "/".join(rel.split("/")[:-1])
        ok = results.get(t[0], (False,))[0]
        by_dir[d][1] += 1
        by_dir[d][0] += ok
    passed = sum(v[0] for v in by_dir.values())
    print(f"WPT CSS reftests: {passed}/{len(tests)} ({100 * passed / max(1, len(tests)):.2f}%)")
    for d, (p, t) in sorted(by_dir.items()):
        print(f"  {d:45s} {p:5d}/{t:<5d} {100 * p / t:6.2f}%")
    if a.json:
        json.dump({"total": len(tests), "passed": passed, "by_dir": by_dir,
                   "results": {os.path.relpath(t[0], WPT).replace("\\", "/"): results.get(t[0], (False, "missing")) for t in tests}},
                  open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
