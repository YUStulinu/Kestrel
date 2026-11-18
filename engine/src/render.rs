//! The rendering pipeline: HTML -> DOM (+ scripts) -> styles -> layout -> paint.
use crate::css::style::{self, Position, Sheet, Viewport};
use crate::font::FontSet;
use crate::html::dom::{Document, NodeData, NodeId, DOCUMENT};
use crate::html::treebuilder;
use crate::layout::{Images, Kind, Layout};
use crate::paint::{Canvas, Image, Painter};
use std::collections::HashMap;
use std::path::{Path, PathBuf};

/// fetches resources: local files (with an optional root for "/" paths), data: URLs,
/// and anything else through an optional network hook
pub struct Loader {
    pub root: Option<PathBuf>,
    pub net: Option<Box<dyn Fn(&str) -> Option<Vec<u8>>>>,
}

impl Loader {
    pub fn resolve(&self, base: &str, url: &str) -> String {
        let url = url.trim();
        if url.starts_with("data:") || url.contains("://") {
            return url.to_string();
        }
        if base.contains("://") && !base.starts_with("file://") {
            // a network base: resolve against it
            if url.starts_with("//") {
                let scheme = &base[..base.find(':').unwrap_or(0)];
                return format!("{scheme}:{url}");
            }
            let host_end = base.find("://").map(|i| i + 3).and_then(|i| base[i..].find('/').map(|j| i + j)).unwrap_or(base.len());
            if url.starts_with('/') {
                return format!("{}{}", &base[..host_end], url);
            }
            let dir = &base[..base.rfind('/').map(|i| i + 1).unwrap_or(base.len())];
            return normalize_path(&format!("{dir}{url}"));
        }
        let url = url.split('#').next().unwrap_or("").split('?').next().unwrap_or("");
        if let Some(rest) = url.strip_prefix('/') {
            if let Some(r) = &self.root {
                return r.join(rest).to_string_lossy().replace('\\', "/");
            }
        }
        let base_dir = Path::new(base).parent().map(|p| p.to_path_buf()).unwrap_or_default();
        normalize_path(&base_dir.join(url).to_string_lossy().replace('\\', "/"))
    }
    pub fn fetch(&self, url: &str) -> Option<Vec<u8>> {
        if let Some(d) = url.strip_prefix("data:") {
            let (meta, data) = d.split_once(',')?;
            if meta.ends_with(";base64") {
                return base64(data);
            }
            return Some(percent_decode(data));
        }
        if url.contains("://") && !url.starts_with("file://") {
            return self.net.as_ref().and_then(|n| n(url));
        }
        std::fs::read(url.trim_start_matches("file://")).ok()
    }
}

fn normalize_path(p: &str) -> String {
    let (prefix, rest) = match p.find("://") {
        Some(i) => {
            let host_end = p[i + 3..].find('/').map(|j| i + 3 + j).unwrap_or(p.len());
            (&p[..host_end], &p[host_end..])
        }
        None => ("", p),
    };
    let mut parts: Vec<&str> = vec![];
    let absolute = rest.starts_with('/');
    for seg in rest.split('/') {
        match seg {
            "" | "." => {}
            ".." => {
                if parts.last().is_some_and(|l| *l != "..") {
                    parts.pop();
                } else if !absolute {
                    parts.push("..");
                }
            }
            s => parts.push(s),
        }
    }
    let joined = parts.join("/");
    let drive = joined.len() > 1 && joined.as_bytes()[1] == b':';
    if absolute && !drive {
        format!("{prefix}/{joined}")
    } else {
        format!("{prefix}{joined}")
    }
}

fn percent_decode(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out = vec![];
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 2 < b.len() {
            if let Ok(v) = u8::from_str_radix(&s[i + 1..i + 3], 16) {
                out.push(v);
                i += 3;
                continue;
            }
        }
        out.push(b[i]);
        i += 1;
    }
    out
}

fn base64(s: &str) -> Option<Vec<u8>> {
    let mut out = vec![];
    let mut acc = 0u32;
    let mut bits = 0;
    for c in s.bytes() {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' | b'-' => 62,
            b'/' | b'_' => 63,
            b'=' | b' ' | b'\n' | b'\r' | b'\t' => continue,
            _ => return None,
        };
        acc = (acc << 6) | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            out.push((acc >> bits) as u8);
        }
    }
    Some(out)
}

/// a network hook backed by `kestrel-net fetch-daemon` (KESTREL_NET or next to this binary)
pub type NetLog = std::rc::Rc<std::cell::RefCell<Vec<(String, u16, usize, String)>>>;
pub fn net_hook(log: NetLog) -> Option<Box<dyn Fn(&str) -> Option<Vec<u8>>>> {
    use std::io::{BufRead, Read, Write};
    let exe = std::env::var("KESTREL_NET").ok().or_else(|| {
        let me = std::env::current_exe().ok()?;
        let dir = me.parent()?;
        for name in ["kestrel-net.exe", "kestrel-net"] {
            let p = dir.join(name);
            if p.exists() {
                return Some(p.to_string_lossy().to_string());
            }
        }
        None
    })?;
    let child = std::process::Command::new(exe)
        .arg("fetch-daemon")
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .spawn()
        .ok()?;
    let child = std::cell::RefCell::new(child);
    let reader = std::cell::RefCell::new(None::<std::io::BufReader<std::process::ChildStdout>>);
    Some(Box::new(move |url: &str| {
        let mut c = child.borrow_mut();
        let stdin = c.stdin.as_mut()?;
        writeln!(stdin, "{url}").ok()?;
        stdin.flush().ok()?;
        let mut r = reader.borrow_mut();
        if r.is_none() {
            *r = Some(std::io::BufReader::new(c.stdout.take()?));
        }
        let rd = r.as_mut()?;
        let mut head = String::new();
        rd.read_line(&mut head).ok()?;
        let parts: Vec<&str> = head.trim_end().splitn(4, ' ').collect();
        if parts.first() == Some(&"ERR") || parts.len() < 3 {
            log.borrow_mut().push((url.to_string(), 0, 0, head.trim_end().to_string()));
            return None;
        }
        let status: u16 = parts[0].parse().ok()?;
        let n: usize = parts[1].parse().ok()?;
        let mut body = vec![0u8; n];
        rd.read_exact(&mut body).ok()?;
        log.borrow_mut().push((url.to_string(), status, n, parts[2].to_string()));
        if status >= 400 {
            return None;
        }
        Some(body)
    }))
}

/// decodes PNG/JPEG bytes to RGBA (the Zig decoders when linked)
pub type Decoder = fn(&[u8]) -> Option<Image>;

pub struct RenderOptions<'a> {
    pub width: usize,
    pub height: usize,
    pub decoder: Option<Decoder>,
    pub scripts: bool,
    pub _p: std::marker::PhantomData<&'a ()>,
}

/// the default fonts: DejaVu from the fonts directory
pub fn default_fonts(dir: &Path) -> FontSet {
    let mut f = FontSet::new();
    for (file, fam, w, it) in [
        ("DejaVuSans.ttf", "DejaVu Sans", 400, false),
        ("DejaVuSans-Bold.ttf", "DejaVu Sans", 700, false),
        ("DejaVuSans-Oblique.ttf", "DejaVu Sans", 400, true),
        ("DejaVuSerif.ttf", "DejaVu Serif", 400, false),
        ("DejaVuSansMono.ttf", "DejaVu Sans Mono", 400, false),
    ] {
        let path = dir.join(file);
        f.add_cached(&path.to_string_lossy(), fam, w, it, || std::fs::read(&path).ok());
    }
    f
}

pub struct Rendered {
    pub canvas: Canvas,
    pub doc: Document,
    pub timings: Vec<(String, f64)>,
    pub console: Vec<String>,
    pub script_errors: Vec<String>,
    /// element boxes for the devtools
    pub boxes: Vec<BoxInfo>,
}

#[derive(Clone, Debug)]
pub struct BoxInfo {
    pub node: NodeId,
    pub rect: (f32, f32, f32, f32),
    pub margin: [f32; 4],
    pub border: [f32; 4],
    pub padding: [f32; 4],
    pub display: String,
    pub position: String,
    pub color: String,
    pub background: String,
    pub font: String,
}

fn text_of(doc: &Document, n: NodeId) -> String {
    doc.text_content(n)
}

/// renders a page; `base` is the document's URL or path
pub fn render(html: &str, base: &str, loader: &Loader, fonts_dir: &Path, opts: RenderOptions) -> Rendered {
    let mut timings = vec![];
    let t0 = std::time::Instant::now();
    // scripts run as their end tags are parsed, then load events and timers
    let mut console = vec![];
    let mut script_errors = vec![];
    let mut doc = if opts.scripts && crate::js::available() {
        let mut dummy = Document::new();
        let mut sc = crate::js::ScriptContext::new(&mut dummy).expect("kjs");
        let mut doc = {
            let mut hook = |d: &mut Document, script: NodeId| {
                let e = d.element(script).unwrap();
                let ty = e.attr("type").unwrap_or("").to_ascii_lowercase();
                if !(ty.is_empty() || ty.contains("javascript") || ty == "module" || ty.contains("ecmascript")) {
                    return;
                }
                let src = match e.attr("src") {
                    Some(src) => {
                        let url = loader.resolve(base, src);
                        loader.fetch(&url).map(|b| String::from_utf8_lossy(&b).to_string())
                    }
                    None => Some(text_of(d, script)),
                };
                if let Some(code) = src {
                    sc.rebind(d);
                    sc.eval(&code, "script");
                }
            };
            treebuilder::parse_with_scripts(html, &mut hook)
        };
        sc.rebind(&mut doc);
        sc.after_scripts();
        console = sc.console().to_vec();
        script_errors = sc.errors.clone();
        doc
    } else {
        treebuilder::parse(html, false)
    };
    timings.push(("parse+script".into(), t0.elapsed().as_secs_f64() * 1000.0));
    let vp = Viewport { width: opts.width as f32, height: opts.height as f32 };
    let t = std::time::Instant::now();
    // style sheets in document order
    let mut sheets: Vec<Sheet> = vec![style::ua_sheet(&vp)];
    let mut order = 1_000_000;
    let mut fonts = default_fonts(fonts_dir);
    let mut face_urls: Vec<(String, String, u16, bool)> = vec![];
    fn add_sheet(src: &str, sheet_base: &str, loader: &Loader, vp: &Viewport, order: &mut usize, sheets: &mut Vec<Sheet>, faces: &mut Vec<(String, String, u16, bool)>, depth: u32) {
        let sh = style::parse_sheet(src, 1, *order, vp);
        *order += sh.rules.len() + 1;
        // imports come first
        if depth < 4 {
            for imp in &sh.imports {
                let url = loader.resolve(sheet_base, imp);
                if let Some(b) = loader.fetch(&url) {
                    add_sheet(&String::from_utf8_lossy(&b), &url, loader, vp, order, sheets, faces, depth + 1);
                }
            }
        }
        for f in &sh.font_faces {
            faces.push((f.0.clone(), loader.resolve(sheet_base, &f.1), f.2, f.3));
        }
        sheets.push(sh);
    }
    for n in doc.descendants(DOCUMENT) {
        let Some(e) = doc.element(n) else { continue };
        if e.name == "style" {
            // XHTML tests wrap style sheets in CDATA sections
            let css = text_of(&doc, n).replace("<![CDATA[", "").replace("]]>", "");
            add_sheet(&css, base, loader, &vp, &mut order, &mut sheets, &mut face_urls, 0);
        } else if e.name == "link" && e.attr("rel").is_some_and(|r| r.split_ascii_whitespace().any(|w| w.eq_ignore_ascii_case("stylesheet"))) {
            if let Some(href) = e.attr("href") {
                let url = loader.resolve(base, href);
                if let Some(b) = loader.fetch(&url) {
                    add_sheet(&String::from_utf8_lossy(&b), &url, loader, &vp, &mut order, &mut sheets, &mut face_urls, 0);
                }
            }
        }
    }
    // the WPT tests expect the Ahem font to be installed
    if let Some(r) = &loader.root {
        let ahem = r.join("fonts").join("Ahem.ttf");
        if ahem.exists() {
            fonts.add_cached(&ahem.to_string_lossy(), "Ahem", 400, false, || std::fs::read(&ahem).ok());
        }
    }
    for (fam, url, w, it) in face_urls {
        fonts.add_cached(&url, &fam, w, it, || loader.fetch(&url));
    }
    let styles = style::cascade(&doc, &sheets, &vp);
    timings.push(("style".into(), t.elapsed().as_secs_f64() * 1000.0));
    // images
    let mut image_list: Vec<Image> = vec![];
    let mut images = Images { sizes: vec![], by_url: HashMap::new() };
    let mut want: Vec<String> = vec![];
    for n in doc.descendants(DOCUMENT) {
        if doc.is(n, "img") {
            if let Some(src) = doc.element(n).unwrap().attr("src") {
                want.push(loader.resolve(base, src));
            }
        }
    }
    for st in styles.elements.values() {
        if let Some(u) = &st.background_image {
            want.push(loader.resolve(base, u));
        }
    }
    if let Some(dec) = opts.decoder {
        for url in want {
            if images.by_url.contains_key(&url) {
                continue;
            }
            if let Some(img) = loader.fetch(&url).and_then(|b| dec(&b)) {
                images.sizes.push((img.w as u32, img.h as u32));
                images.by_url.insert(url, image_list.len());
                image_list.push(img);
            }
        }
    }
    let t = std::time::Instant::now();
    let base_s = base.to_string();
    let resolve = move |u: &str| loader.resolve(&base_s, u);
    let mut layout = Layout::new(&doc, &styles, &fonts, &images, &resolve, opts.width as f32, opts.height as f32);
    layout.run();
    apply_relative(&mut layout);
    timings.push(("layout".into(), t.elapsed().as_secs_f64() * 1000.0));
    let t = std::time::Instant::now();
    let mut painter = Painter { l: &layout, canvas: Canvas::new(opts.width, opts.height), images: &image_list };
    painter.paint_root();
    timings.push(("paint".into(), t.elapsed().as_secs_f64() * 1000.0));
    let canvas = painter.canvas;
    let mut boxes = vec![];
    for b in &layout.boxes {
        if let Some(n) = b.node {
            if b.pseudo == 0 && doc.element(n).is_some() && (b.laid_out || b.kind == Kind::Inline) {
                let s = &b.style;
                let col = |c: crate::css::values::Color| format!("rgba({}, {}, {}, {})", c.r, c.g, c.b, c.a as f32 / 255.0);
                let mut rect = b.border_box();
                if b.kind == Kind::Inline {
                    // the union of the inline box's fragments
                    let mut r: Option<(f32, f32, f32, f32)> = None;
                    for x in &layout.boxes {
                        for ln in &x.lines {
                            for f in &ln.frags {
                                if layout.boxes[f.boxid].node == Some(n) {
                                    let (a0, b0, a1, b1) = r.unwrap_or((f.x0, f.top, f.x1, f.bottom));
                                    r = Some((a0.min(f.x0), b0.min(f.top), a1.max(f.x1), b1.max(f.bottom)));
                                }
                            }
                        }
                    }
                    match r {
                        Some((a0, b0, a1, b1)) => rect = (a0, b0, a1 - a0, b1 - b0),
                        None => continue,
                    }
                }
                boxes.push(BoxInfo {
                    node: n,
                    rect,
                    margin: b.m,
                    border: b.b,
                    padding: b.p,
                    display: format!("{:?}", s.display).to_lowercase(),
                    position: format!("{:?}", s.position).to_lowercase(),
                    color: col(s.color),
                    background: col(s.background_color),
                    font: format!("{} {}px {}", s.font_weight, s.font_size, s.font_family.join(", ")),
                });
            }
        }
    }
    drop(layout);
    Rendered { canvas, doc, timings, console, script_errors, boxes }
}

/// relative positioning: moves boxes (and inline content) after layout
fn apply_relative(l: &mut Layout) {
    for b in 0..l.boxes.len() {
        let s = l.boxes[b].style.clone();
        if !matches!(s.position, Position::Relative | Position::Sticky) || l.boxes[b].node.is_none() && l.boxes[b].pseudo == 0 {
            continue;
        }
        if s.position == Position::Sticky {
            continue;
        }
        let cb = l.boxes[b].parent.unwrap_or(l.root);
        let (cw, ch) = (l.boxes[cb].w, l.boxes[cb].h);
        let dx = match (s.inset[3], s.inset[1]) {
            (Some(lf), _) => lf.resolve(cw),
            (None, Some(r)) => -r.resolve(cw),
            _ => 0.0,
        };
        let dy = match (s.inset[0], s.inset[2]) {
            (Some(t), _) => t.resolve(ch),
            (None, Some(bt)) => -bt.resolve(ch),
            _ => 0.0,
        };
        if dx == 0.0 && dy == 0.0 {
            continue;
        }
        if l.boxes[b].kind == Kind::Inline {
            // move the runs and fragments in the containing block's lines that belong to this box
            let mut block = l.boxes[b].parent;
            while let Some(x) = block {
                if l.boxes[x].kind != Kind::Inline {
                    break;
                }
                block = l.boxes[x].parent;
            }
            let Some(blk) = block else { continue };
            let inside = |l: &Layout, k: usize| {
                let mut p = Some(k);
                while let Some(x) = p {
                    if x == b {
                        return true;
                    }
                    p = l.boxes[x].parent;
                }
                false
            };
            let node_boxes: Vec<usize> = (0..l.boxes.len()).filter(|&k| inside(l, k)).collect();
            let mut lines = std::mem::take(&mut l.boxes[blk].lines);
            for ln in &mut lines {
                for r in &mut ln.runs {
                    // runs carry their style only; match by the text box ancestry through the frags' x-range
                    let _ = r;
                }
                for f in &mut ln.frags {
                    if node_boxes.contains(&f.boxid) {
                        f.x0 += dx;
                        f.x1 += dx;
                        f.top += dy;
                        f.bottom += dy;
                    }
                }
            }
            // runs: those horizontally inside this box's fragments on the same line
            for ln in &mut lines {
                let spans: Vec<(f32, f32)> = ln.frags.iter().filter(|f| f.boxid == b).map(|f| (f.x0 - dx, f.x1 - dx)).collect();
                for r in &mut ln.runs {
                    if spans.iter().any(|(a, z)| r.x >= *a - 0.01 && r.x + r.width <= *z + 0.01) {
                        r.x += dx;
                        r.baseline += dy;
                    }
                }
            }
            l.boxes[blk].lines = lines;
            for k in node_boxes {
                if k != b && l.boxes[k].laid_out {
                    crate::layout::block::shift_subtree(l, k, dx, dy);
                }
            }
        } else {
            crate::layout::block::shift_subtree(l, b, dx, dy);
        }
    }
    let _ = NodeData::Document;
}

/// compares two canvases: (differing pixels, max channel difference)
pub fn compare(a: &Canvas, b: &Canvas) -> (usize, u8) {
    let mut n = 0;
    let mut mx = 0u8;
    for i in 0..a.w.min(b.w) * a.h.min(b.h) {
        let mut d = 0u8;
        for k in 0..3 {
            d = d.max(a.px[i * 4 + k].abs_diff(b.px[i * 4 + k]));
        }
        if d > 0 {
            n += 1;
            mx = mx.max(d);
        }
    }
    (n, mx)
}

#[cfg(imgdec)]
mod zig {
    extern "C" {
        pub fn kd_decode(data: *const u8, len: usize, w: *mut u32, h: *mut u32) -> *mut u8;
        pub fn kd_free(p: *mut u8, len: usize);
    }
}

/// decodes PNG or baseline JPEG through the Zig library (None when it is not linked)
pub fn decode_image(bytes: &[u8]) -> Option<Image> {
    #[cfg(imgdec)]
    unsafe {
        let (mut w, mut h) = (0u32, 0u32);
        let p = zig::kd_decode(bytes.as_ptr(), bytes.len(), &mut w, &mut h);
        if p.is_null() {
            return None;
        }
        let n = w as usize * h as usize * 4;
        let rgba = std::slice::from_raw_parts(p, n).to_vec();
        zig::kd_free(p, n);
        return Some(Image { w: w as usize, h: h as usize, rgba });
    }
    #[cfg(not(imgdec))]
    {
        let _ = bytes;
        None
    }
}

pub fn has_image_decoders() -> bool {
    cfg!(imgdec)
}
