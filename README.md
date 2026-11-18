# Kestrel

A web browser engine written from scratch in six languages, with no third-party
libraries in the engine itself:

| Part | Language | What it does |
|---|---|---|
| `engine/` | Rust | HTML5 tokenizer and tree builder, CSS parser, selectors and cascade, layout (block, inline, floats, flexbox, tables, positioning), TrueType rasterizer, painter, PNG encoder |
| `js/` | C++ | **kjs**: a JavaScript interpreter (lexer, parser, bytecode compiler, VM, mark-sweep GC, builtins, its own RegExp engine), bound to the DOM |
| `imgdec/` | Zig | PNG (all color types, interlacing) and baseline JPEG decoders, with its own inflate |
| `net/` | Go | HTTP/1.1 client (chunked, gzip/deflate, keep-alive, redirects, TLS), an HTTP disk cache, and the devtools server |
| `devtools/` | TypeScript | a devtools panel: DOM tree, box-model overlay, computed style, console, network, timings |
| `harness/` | Python | conformance runners and the pixel comparison against Chrome |

About 33,000 lines in total. `engine/` is about 16,000 lines and `js/` about 14,500.

## Results

All results were measured on 2026-11-18 on Windows 11. The suites are pinned to the commits listed in `scripts/fetch_data.sh`.

### html5lib-tests (HTML parsing)

| Suite | Passed |
|---|---|
| Tokenizer | **7042 / 7042 (100%)**. 4 tests are skipped: their inputs contain lone surrogates, which UTF-8 stdin cannot carry. |
| Tree construction | **1970 / 1973 (99.85%)** |

The tree-construction tests are the copies kept in web-platform-tests (`html/syntax/parsing/resources/*.dat`). The `scripted_*` files run their `<script>` elements through kjs while parsing. The 3 failures all need `document.write`, which Kestrel does not implement.

The tokenizer runner compares tokens only. It does not check the expected parse error codes.

### test262 (JavaScript)

The suite was run on 15 directories: language expressions, statements, types, literals, function code and the arguments object, plus the Array, String, Object, Math, JSON, Number, Function, Boolean and Error built-ins. That is 30,887 tests. Each test runs in sloppy mode and in strict mode unless its flags say otherwise, and a test passes only if both modes pass. Each test has a 5 s watchdog.

| Scope | Passed |
|---|---|
| **All 30,887 tests** | **28,769 (93.14%)** |
| Tests without a `features:` tag (the ES5-era core) | 12,129 / 12,357 (98.15%) |
| language/expressions | 10,226 / 11,156 (91.66%) |
| language/statements | 8,809 / 9,399 (93.72%) |
| built-ins/Object | 3,319 / 3,411 (97.30%) |
| built-ins/Array | 2,849 / 3,083 (92.41%) |
| built-ins/String | 1,165 / 1,223 (95.26%) |
| built-ins/Math | 311 / 327 (95.11%) |
| built-ins/JSON | 119 / 166 (71.69%) |
| built-ins/Error | 45 / 93 (48.39%) |

The main failure groups:

| Failures | Cause |
|---|---|
| 447 | modules and `import.meta`, which kjs does not support |
| 249 | crashes: an access violation in some `for-of` / `for-await-of` destructuring tests. This is a known bug. |
| 154 | Proxy |
| 156 | BigInt |
| about 150 | typed arrays and ArrayBuffer |
| 58 | cross-realm tests (`$262.createRealm`) |

The full run takes about 30 s on 16 worker processes.

### WPT CSS reftests (layout and painting)

Each test is rendered at 800×600, together with its reference, and the two are compared pixel by pixel. The comparison uses WPT's fuzzy tolerance where the test declares one. Ahem is loaded automatically from the WPT `fonts/` directory.

| Directory | Passed |
|---|---|
| css/CSS2/margin-padding-clear | 612 / 683 (89.6%) |
| css/CSS2/normal-flow | 564 / 750 (75.2%) |
| css/CSS2/borders | 356 / 506 (70.4%) |
| css/CSS2/backgrounds | 236 / 339 (69.6%) |
| css/CSS2/colors | 11 / 19 (57.9%) |
| css/css-flexbox (all subdirectories) | 471 / 1029 (45.8%) |
| css/CSS2/box-display | 43 / 120 (35.8%) |
| **Total** | **2293 / 3446 (66.5%)** |

A full run takes about 28 s.

### Pixel comparison against Chrome

There are 12 hand-written test pages in `tests/pages/`, covering typography, the box model, margin collapsing, floats, flexbox, positioning, lists and tables, backgrounds, borders, inline formatting, selectors and a scripted page. Each page is rendered at 800×600 by Kestrel and by headless Chrome 157 (`--force-device-scale-factor=1`). Both use the same DejaVu fonts through `@font-face`. The Chrome screenshots are committed, so the comparison runs without Chrome.

| Page | Exact pixels | Within 32 levels | Ink IoU |
|---|---|---|---|
| 01-typography | 95.65% | 96.58% | 67.40% |
| 02-box-model | 99.18% | 99.31% | 89.74% |
| 03-margins | 98.44% | 98.65% | 57.79% |
| 04-floats | 90.38% | 93.23% | 64.09% |
| 05-flexbox | 99.32% | 99.48% | 96.95% |
| 06-positioning | 99.49% | 99.57% | 93.71% |
| 07-lists-tables | 99.07% | 99.17% | 56.10% |
| 08-backgrounds | 99.94% | 99.95% | 99.84% |
| 09-borders | 97.34% | 98.16% | 76.25% |
| 10-inline | 98.23% | 98.51% | 91.51% |
| 11-selectors | 99.45% | 99.52% | 45.58% |
| 12-script | 99.27% | 99.45% | 80.33% |
| **Mean** | **97.98%** | **98.47%** | **76.61%** |

"Exact" is inflated by the white background, which both browsers get right. **Ink IoU** is the stricter number: the intersection over union of the pixels drawn darker than near-white. Most of the ink loss comes from text placed 1 px off vertically, so glyphs overlap without lining up. Text is visibly the same, but per-pixel agreement is low. Pages 07 and 11 are mostly text. Floats (04) have real layout differences.

### Speed

A simple page renders in about 10 ms: parse 0.2 ms, style 4–8 ms, layout 0.3 ms, paint about 3 ms.

## Building

Requirements:

- Rust stable.
- A C++20 compiler: g++ on Linux. On Windows (MSVC target), clang++ from LLVM.
- Zig 0.17.
- Go 1.24.
- Node 24.
- Python 3.10+.

```bash
cd engine && cargo build --release        # also builds kjs (C++) and imgdec (Zig)
cd net && go build -o kestrel-net .       # network layer + devtools server
cd devtools && npm ci && npm run build    # devtools panel
js/build.sh                               # the standalone kjs binary (test262 runner)
```

Without Zig, the engine builds without image decoding. Without a C++ compiler, it builds without JavaScript. In both cases `build.rs` prints a warning. You can also turn either one off on purpose with `KESTREL_NO_ZIG` or `KESTREL_NO_JS`.

## Using it

```bash
kestrel render page.html -o out.png --width 800 --height 600
kestrel parse page.html                              # the DOM tree, html5lib format
kestrel-net get https://example.com/                 # one fetch through the HTTP cache (KESTREL_CACHE, default: the user cache dir)
kestrel-net serve -kestrel path/to/kestrel           # devtools at http://localhost:8091/ (pages, local HTML or any http(s) URL)
```

## Running the evaluations

```bash
scripts/fetch_data.sh                                  # html5lib-tests, test262 and WPT subsets into data/
python harness/html5lib_runner.py
python harness/test262_runner.py --show 30
python harness/wpt_runner.py
python harness/chrome_compare.py                       # --update re-shoots Chrome (needs a local Chrome)
```

## Tests

- **Rust:** unit tests for the tokenizer, tree builder, CSS syntax, values, selectors, entities and the font parser. End-to-end tests in `engine/tests/render.rs` check margin collapsing, flex-grow, floats, absolute positioning, painting, text and DOM mutation from scripts.
- **Zig:** inflate, plus decoding real PNGs.
- **Go:** chunked and gzip responses, cache freshness and revalidation, the fetch daemon.
- **TypeScript:** tree indexing, hit testing, box model and timeline.
- **CI** (`.github/workflows/ci.yml`) runs all of these. It also runs the html5lib suites, the Chrome comparison against the committed references, and a test262 sample (Math, JSON, Boolean).

## Limits (honest list)

- **HTML:** no `document.write`. No encoding sniffing: input is UTF-8.
- **CSS:** no grid, transforms, filters or animations. No `writing-mode` or bidi. Tables use a simplified auto layout; `border-collapse: collapse` only removes the spacing and does not resolve border conflicts. No text shaping: advance widths plus the TrueType `kern` table.
- **JavaScript:** no BigInt, Proxy, typed arrays or modules. WeakMap and WeakSet hold strong references. The GC runs only when no native code is on the stack, so long native loops can grow the heap. DOM layout queries (`getBoundingClientRect`, `offsetWidth`) return 0, because scripts run before layout. Timers run once, in due-time order, after the load event and before layout (`setInterval` fires once).
- **Images:** no progressive JPEG, no GIF or WebP.
- **Network:** HTTP/1.1 only. TLS uses Go's standard library.
- The Chrome references were shot with one local Chrome build. Rendering in a different Chrome version may move text by a pixel.

## Data and licenses

The code is under the MIT license (`LICENSE`).

The DejaVu fonts in `fonts/` are under their own license (`fonts/LICENSE-DejaVu`).

The conformance suites are downloaded by `scripts/fetch_data.sh` and are not committed. They are html5lib-tests (MIT), test262 (BSD) and web-platform-tests (BSD).

The PNG files in `tests/pages/chrome/` are screenshots of this repository's own test pages.
