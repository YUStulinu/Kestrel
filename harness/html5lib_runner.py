"""Run the html5lib tokenizer tests and the tree-construction tests (now kept
in web-platform-tests) through the kestrel binary, and report pass rates.

    python harness/html5lib_runner.py [--kestrel PATH] [--show N] [--json out.json]

Tokenizer tests compare the tokens only (the expected error codes are not
checked). Tree tests compare the whole tree dump.
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DATA = os.path.join(ROOT, "data")


def default_kestrel():
    for p in [os.environ.get("KESTREL", ""), "C:/kt/release/kestrel.exe",
              os.path.join(ROOT, "engine", "target", "release", "kestrel"),
              os.path.join(ROOT, "engine", "target", "release", "kestrel.exe")]:
        if p and os.path.exists(p):
            return p
    sys.exit("build the engine first (cargo build --release)")


def unescape(s):
    # doubleEscaped tests carry \uXXXX sequences in the strings themselves
    return re.sub(r"\\u([0-9A-Fa-f]{4})", lambda m: chr(int(m.group(1), 16)), s)


def has_surrogate(s):
    return any(0xD800 <= ord(c) <= 0xDFFF for c in s)


def normalize(tokens):
    out = []
    for t in tokens:
        if t[0] == "Character" and out and out[-1][0] == "Character":
            out[-1] = ["Character", out[-1][1] + t[1]]
        else:
            out.append(list(t))
    return out


def records(rows):
    """field, field, byte length, input: what `kestrel *-batch` reads"""
    out = b""
    for a, b, text in rows:
        body = text.encode("utf-8", "surrogatepass")
        out += f"{a}{b}{len(body)}".encode() + body
    return out


def run_tokenizer(kestrel, show):
    cases = []
    for f in sorted(glob.glob(os.path.join(DATA, "html5lib-tests", "tokenizer", "*.test"))):
        for t in json.load(open(f, encoding="utf-8")).get("tests", []):
            inp, out = t["input"], t["output"]
            if t.get("doubleEscaped"):
                inp = unescape(inp)
                out = [[unescape(x) if isinstance(x, str) else ({unescape(k): unescape(v) for k, v in x.items()} if isinstance(x, dict) else x) for x in tok] for tok in out]
            for st in t.get("initialStates", ["Data state"]):
                cases.append((os.path.basename(f), t["description"], st, t.get("lastStartTag", ""), inp, out))
    runnable = [c for c in cases if not has_surrogate(c[4])]
    payload = records((c[2], c[3], c[4]) for c in runnable)
    res = subprocess.run([kestrel, "tokenize-batch"], input=payload,
                         capture_output=True, check=True).stdout.decode("utf-8").splitlines()
    passed, fails = 0, []
    for c, line in zip(runnable, res):
        got = normalize(json.loads(line))
        want = normalize(c[5])
        # self-closing flag false is written as a missing third element
        want = [w[:3] if w[0] == "StartTag" and len(w) == 4 and not w[3] else w for w in want]
        if got == want:
            passed += 1
        else:
            fails.append((c, got))
    for c, got in fails[:show]:
        print(f"  FAIL tokenizer {c[0]} {c[1]!r} [{c[2]}]\n    input {c[4]!r}\n    want  {normalize(c[5])}\n    got   {got}")
    return {"total": len(cases), "run": len(runnable), "passed": passed,
            "skipped_surrogates": len(cases) - len(runnable)}


def parse_dat(path):
    text = open(path, encoding="utf-8", newline="\n").read()
    tests = []
    for block in re.split(r"\n(?=#data\n)", "\n" + text):
        if not block.strip():
            continue
        sections, cur = {}, None
        for line in block.split("\n"):
            if line.startswith("#") and line[1:].strip() in (
                    "data", "errors", "new-errors", "document-fragment", "script-off", "script-on", "document", "description"):
                cur = line[1:].strip()
                sections[cur] = []
            elif cur:
                sections[cur].append(line)
        if "data" not in sections:
            continue
        doc = "\n".join(sections.get("document", []))
        doc = doc.rstrip("\n")
        tests.append({
            "data": "\n".join(sections["data"]),
            "fragment": "\n".join(sections.get("document-fragment", [])).strip(),
            "scripting": "script-on" in sections,
            "document": doc,
        })
    return tests


def run_tree(kestrel, show):
    cases = []
    for f in sorted(glob.glob(os.path.join(DATA, "wpt", "html", "syntax", "parsing", "resources", "*.dat"))):
        for t in parse_dat(f):
            if os.path.basename(f).startswith("scripted_"):
                t["scripting"] = 2  # run the scripts (kjs) while parsing
            cases.append((os.path.basename(f), t))
    payload = records((t["fragment"], int(t["scripting"]), t["data"]) for _, t in cases)
    out = subprocess.run([kestrel, "parse-batch"], input=payload,
                         capture_output=True, check=True).stdout.decode("utf-8").split(chr(1) + "\n")
    passed, per_file, fails = 0, {}, []
    for (f, t), got in zip(cases, out):
        ok = got.rstrip("\n") == t["document"]
        per_file.setdefault(f, [0, 0])
        per_file[f][1] += 1
        if ok:
            passed += 1
            per_file[f][0] += 1
        else:
            fails.append((f, t, got))
    for f, t, got in fails[:show]:
        print(f"  FAIL tree {f} ctx={t['fragment']!r}\n    input {t['data']!r}\n--- want\n{t['document']}\n--- got\n{got.rstrip()}")
    return {"total": len(cases), "passed": passed, "per_file": per_file}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--kestrel", default=None)
    ap.add_argument("--show", type=int, default=0)
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    k = a.kestrel or default_kestrel()
    tok = run_tokenizer(k, a.show)
    tree = run_tree(k, a.show)
    print(f"tokenizer: {tok['passed']}/{tok['run']} ({100 * tok['passed'] / tok['run']:.2f}%), "
          f"{tok['skipped_surrogates']} skipped (lone surrogates)")
    print(f"tree construction: {tree['passed']}/{tree['total']} ({100 * tree['passed'] / tree['total']:.2f}%)")
    if a.json:
        json.dump({"tokenizer": tok, "tree": tree}, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
