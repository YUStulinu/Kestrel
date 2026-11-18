"""Run a subset of test262 through kjs and report pass rates.

    python harness/test262_runner.py [--kjs PATH] [--jobs N] [--filter SUBSTR] [--json out.json] [--show N]

Each worker process reads test paths on stdin and prints PASS/FAIL lines. A
crashed worker is restarted after the test that crashed it (counted as a
failure). Tests run in sloppy and strict mode unless their flags say otherwise.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile
import threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
T262 = os.path.join(ROOT, "data", "test262")
DIRS = ["test/language/expressions", "test/language/statements", "test/language/types", "test/language/literals",
        "test/language/function-code", "test/language/arguments-object", "test/built-ins/Array", "test/built-ins/String",
        "test/built-ins/Object", "test/built-ins/Math", "test/built-ins/JSON", "test/built-ins/Number",
        "test/built-ins/Function", "test/built-ins/Boolean", "test/built-ins/Error"]


def default_kjs():
    for p in [os.environ.get("KJS", ""), os.path.join(ROOT, "js", "build", "kjs.exe"), os.path.join(ROOT, "js", "build", "kjs")]:
        if p and os.path.exists(p):
            return p
    sys.exit("build kjs first (js/build.sh)")


def collect(filt):
    tests = []
    for d in DIRS:
        for dp, _, files in os.walk(os.path.join(T262, d)):
            for f in files:
                if f.endswith(".js") and "_FIXTURE" not in f:
                    rel = os.path.relpath(os.path.join(dp, f), T262).replace("\\", "/")
                    if not filt or filt in rel:
                        tests.append(rel)
    tests.sort()
    return tests


FEATURES = re.compile(r"^features:\s*\[(.*?)\]", re.M)


def features_of(path):
    with open(os.path.join(T262, path), encoding="utf-8", errors="replace") as f:
        head = f.read(4000)
    m = FEATURES.search(head)
    if not m:
        m2 = re.search(r"^features:\s*\n((?:\s+-.*\n)+)", head, re.M)
        if m2:
            return [x.strip()[1:].strip() for x in m2.group(1).splitlines() if x.strip()]
        return []
    return [x.strip() for x in m.group(1).split(",") if x.strip()]


def run_chunk(kjs, chunk, results, lock):
    pending = list(chunk)
    while pending:
        # the test list goes in through a file: a stdin pipe can deadlock against stdout on Windows
        with tempfile.TemporaryFile("w+", encoding="utf-8") as todo:
            todo.write("\n".join(pending) + "\n")
            todo.seek(0)
            p = subprocess.Popen([kjs, "--test262", T262], stdin=todo, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True, encoding="utf-8", errors="replace")
            done = set()
            for line in p.stdout:
                line = line.rstrip("\n")
                status, _, rest = line.partition(" ")
                path, _, reason = rest.partition(" :: ")
                with lock:
                    results[path] = (status == "PASS", reason)
                done.add(path)
            p.wait()
        rest = [t for t in pending if t not in done]
        if rest and p.returncode != 0:
            # the first unreported test crashed the worker
            with lock:
                results[rest[0]] = (False, f"crash (exit {p.returncode})")
            rest = rest[1:]
        elif rest:
            with lock:
                for t in rest:
                    results[t] = (False, "not reported")
            rest = []
        pending = rest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kjs", default=None)
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--filter", default="")
    ap.add_argument("--json", default=None)
    ap.add_argument("--show", type=int, default=0)
    a = ap.parse_args()
    kjs = a.kjs or default_kjs()
    tests = collect(a.filter)
    results = {}
    lock = threading.Lock()
    n = max(1, a.jobs)
    chunks = [tests[i::n] for i in range(n)]
    threads = [threading.Thread(target=run_chunk, args=(kjs, c, results, lock)) for c in chunks]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    by_dir = collections.defaultdict(lambda: [0, 0])
    es5 = [0, 0]
    reasons = collections.Counter()
    for t in tests:
        ok, why = results.get(t, (False, "missing"))
        d = "/".join(t.split("/")[1:3])
        by_dir[d][1] += 1
        if ok:
            by_dir[d][0] += 1
        else:
            reasons[re.sub(r"[0-9]+", "N", why)[:90]] += 1
        if not features_of(t):
            es5[1] += 1
            es5[0] += ok
    passed = sum(1 for t in tests if results.get(t, (False,))[0])
    print(f"test262: {passed}/{len(tests)} ({100 * passed / max(1, len(tests)):.2f}%)")
    print(f"  tests without a features tag (ES5-era core): {es5[0]}/{es5[1]} ({100 * es5[0] / max(1, es5[1]):.2f}%)")
    for d, (p, t) in sorted(by_dir.items()):
        print(f"  {d:40s} {p:6d}/{t:<6d} {100 * p / t:6.2f}%")
    if a.show:
        print("most common failure reasons:")
        for r, c in reasons.most_common(a.show):
            print(f"  {c:6d}  {r}")
    if a.json:
        json.dump({"total": len(tests), "passed": passed, "es5": es5, "by_dir": by_dir,
                   "failures": {t: results[t][1] for t in tests if t in results and not results[t][0]}},
                  open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
