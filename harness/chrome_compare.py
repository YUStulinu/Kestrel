"""Pixel comparison against Chrome on the pages in tests/pages.

    python harness/chrome_compare.py --update   # re-shoot the Chrome references (needs a local Chrome)
    python harness/chrome_compare.py [--json out.json] [--diff DIR]

The Chrome screenshots are committed (tests/pages/chrome/*.png), so the
comparison runs without Chrome. Both browsers load the same DejaVu fonts
through @font-face. Metrics per page, over 800x600 pixels:
  exact   share of pixels with identical RGB
  near    share within 32 levels on every channel (anti-aliasing differences)
  ink     agreement of the "ink" masks (pixels darker than near-white), as
          intersection over union: where things are drawn, ignoring how
"""
import argparse
import glob
import json
import os
import struct
import subprocess
import sys
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGES = os.path.join(ROOT, "tests", "pages")
REF = os.path.join(PAGES, "chrome")
W, H = 800, 600


def read_png(path):
    """8-bit RGB/RGBA, non-interlaced PNG -> list of RGB tuples (row-major)"""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    p, idat, w, h, ct = 8, b"", 0, 0, 0
    while p < len(data):
        n = struct.unpack(">I", data[p:p + 4])[0]
        t = data[p + 4:p + 8]
        body = data[p + 8:p + 8 + n]
        if t == b"IHDR":
            w, h, depth, ct, _, _, il = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ct in (2, 6) and il == 0, f"unsupported PNG {path}"
        elif t == b"IDAT":
            idat += body
        p += 12 + n
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    out = bytearray(w * h * 3)
    prev = bytearray(stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        cur = bytearray(raw[pos + 1:pos + 1 + stride])
        pos += 1 + stride
        for i in range(stride):
            a = cur[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1:
                cur[i] = (cur[i] + a) & 255
            elif f == 2:
                cur[i] = (cur[i] + b) & 255
            elif f == 3:
                cur[i] = (cur[i] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                cur[i] = (cur[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(w):
            out[(y * w + x) * 3:(y * w + x) * 3 + 3] = cur[x * bpp:x * bpp + 3]
        prev = cur
    return w, h, out


def chrome_path():
    for p in [os.environ.get("CHROME", ""), r"C:\Program Files\Google\Chrome\Application\chrome.exe",
              r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe", "/usr/bin/google-chrome", "/usr/bin/chromium"]:
        if p and os.path.exists(p):
            return p
    sys.exit("Chrome not found (set CHROME)")


def shoot(page, out):
    url = "file:///" + os.path.abspath(page).replace("\\", "/")
    subprocess.run([chrome_path(), "--headless=new", "--disable-gpu", "--hide-scrollbars", "--force-device-scale-factor=1",
                    "--allow-file-access-from-files", "--default-background-color=ffffffff", f"--window-size={W},{H}",
                    f"--screenshot={os.path.abspath(out)}", "--virtual-time-budget=2000", url],
                   check=True, capture_output=True, timeout=60)


def kestrel_path():
    for p in [os.environ.get("KESTREL", ""), "C:/kt/release/kestrel.exe", os.path.join(ROOT, "engine", "target", "release", "kestrel")]:
        if p and os.path.exists(p):
            return p
    sys.exit("build the engine first")


def compare(a, b):
    n = W * H
    exact = near = inter = union = 0
    for i in range(n):
        pa = a[i * 3:i * 3 + 3]
        pb = b[i * 3:i * 3 + 3]
        d = max(abs(pa[0] - pb[0]), abs(pa[1] - pb[1]), abs(pa[2] - pb[2]))
        exact += d == 0
        near += d <= 32
        ia = min(pa) < 200
        ib = min(pb) < 200
        inter += ia and ib
        union += ia or ib
    return {"exact": exact / n, "near": near / n, "ink_iou": inter / union if union else 1.0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--json", default=None)
    a = ap.parse_args()
    os.makedirs(REF, exist_ok=True)
    pages = sorted(glob.glob(os.path.join(PAGES, "*.html")))
    if a.update:
        for p in pages:
            shoot(p, os.path.join(REF, os.path.basename(p)[:-5] + ".png"))
        print(f"shot {len(pages)} Chrome references")
    k = kestrel_path()
    tmp = os.path.join(ROOT, "run", "compare")
    os.makedirs(tmp, exist_ok=True)
    rows = {}
    for p in pages:
        name = os.path.basename(p)[:-5]
        out = os.path.join(tmp, name + ".png")
        subprocess.run([k, "render", p, "-o", out, "--width", str(W), "--height", str(H)], check=True, capture_output=True,
                       env=dict(os.environ, KESTREL_FONTS=os.path.join(ROOT, "fonts")))
        ref = os.path.join(REF, name + ".png")
        if not os.path.exists(ref):
            continue
        _, _, ka = read_png(out)
        rw, rh, ca = read_png(ref)
        assert (rw, rh) == (W, H), f"{ref} is {rw}x{rh}"
        rows[name] = compare(ka, ca)
        r = rows[name]
        print(f"  {name:20s} exact {100 * r['exact']:6.2f}%  near {100 * r['near']:6.2f}%  ink IoU {100 * r['ink_iou']:6.2f}%")
    if rows:
        mean = {m: sum(r[m] for r in rows.values()) / len(rows) for m in ("exact", "near", "ink_iou")}
        print(f"mean over {len(rows)} pages: exact {100 * mean['exact']:.2f}%  near {100 * mean['near']:.2f}%  ink IoU {100 * mean['ink_iou']:.2f}%")
        if a.json:
            json.dump({"pages": rows, "mean": mean}, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    main()
