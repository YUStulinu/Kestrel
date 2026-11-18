//! TrueType fonts: the tables needed for layout and drawing (head, hhea,
//! hmtx, cmap, loca, glyf, kern, OS/2), and an anti-aliased rasterizer that
//! accumulates signed coverage per pixel.
use std::collections::HashMap;

pub struct Font {
    data: Vec<u8>,
    pub units_per_em: f32,
    pub ascent: f32, // in font units
    pub descent: f32, // positive
    pub line_gap: f32,
    pub x_height: f32,
    num_hmetrics: usize,
    hmtx: usize,
    loca: usize,
    glyf: usize,
    long_loca: bool,
    cmap: HashMap<u32, u16>,
    kern: HashMap<(u16, u16), i16>,
    num_glyphs: usize,
}

fn u16_at(d: &[u8], o: usize) -> u16 {
    if o + 2 > d.len() {
        return 0;
    }
    u16::from_be_bytes([d[o], d[o + 1]])
}
fn i16_at(d: &[u8], o: usize) -> i16 {
    u16_at(d, o) as i16
}
fn u32_at(d: &[u8], o: usize) -> u32 {
    if o + 4 > d.len() {
        return 0;
    }
    u32::from_be_bytes([d[o], d[o + 1], d[o + 2], d[o + 3]])
}

#[derive(Clone, Copy, Debug)]
pub struct Point {
    pub x: f32,
    pub y: f32,
}

impl Font {
    pub fn parse(data: Vec<u8>) -> Option<Font> {
        let d = &data;
        let n = u16_at(d, 4) as usize;
        let mut tables = HashMap::new();
        for i in 0..n {
            let r = 12 + 16 * i;
            let tag = String::from_utf8_lossy(&d[r..r + 4]).to_string();
            tables.insert(tag, u32_at(d, r + 8) as usize);
        }
        let head = *tables.get("head")?;
        let hhea = *tables.get("hhea")?;
        let maxp = *tables.get("maxp")?;
        let units_per_em = u16_at(d, head + 18) as f32;
        let long_loca = i16_at(d, head + 50) == 1;
        let mut ascent = i16_at(d, hhea + 4) as f32;
        let mut descent = -(i16_at(d, hhea + 6) as f32);
        let mut line_gap = i16_at(d, hhea + 8) as f32;
        let num_hmetrics = u16_at(d, hhea + 34) as usize;
        let mut x_height = units_per_em * 0.5;
        if let Some(&os2) = tables.get("OS/2") {
            // prefer the typographic metrics when USE_TYPO_METRICS is set; Chrome uses hhea otherwise
            let version = u16_at(d, os2);
            let fs_selection = u16_at(d, os2 + 62);
            if fs_selection & (1 << 7) != 0 {
                ascent = i16_at(d, os2 + 68) as f32;
                descent = -(i16_at(d, os2 + 70) as f32);
                line_gap = i16_at(d, os2 + 72) as f32;
            }
            if version >= 2 {
                x_height = i16_at(d, os2 + 86) as f32;
            }
        }
        let mut f = Font {
            units_per_em,
            ascent,
            descent,
            line_gap,
            x_height,
            num_hmetrics,
            hmtx: *tables.get("hmtx")?,
            loca: *tables.get("loca")?,
            glyf: *tables.get("glyf")?,
            long_loca,
            cmap: HashMap::new(),
            kern: HashMap::new(),
            num_glyphs: u16_at(d, maxp + 4) as usize,
            data: vec![],
        };
        if let Some(&cmap) = tables.get("cmap") {
            f.cmap = parse_cmap(d, cmap);
        }
        if let Some(&kern) = tables.get("kern") {
            // version 0, format 0 subtables
            let nt = u16_at(d, kern + 2) as usize;
            let mut o = kern + 4;
            for _ in 0..nt {
                let len = u16_at(d, o + 2) as usize;
                let cov = u16_at(d, o + 4);
                if cov >> 8 == 0 && cov & 1 == 1 {
                    let np = u16_at(d, o + 6) as usize;
                    for k in 0..np {
                        let p = o + 14 + 6 * k;
                        f.kern.insert((u16_at(d, p), u16_at(d, p + 2)), i16_at(d, p + 4));
                    }
                }
                o += len;
            }
        }
        f.data = data;
        Some(f)
    }

    pub fn glyph_index(&self, c: char) -> u16 {
        self.cmap.get(&(c as u32)).copied().unwrap_or(0)
    }
    pub fn has_glyph(&self, c: char) -> bool {
        self.cmap.contains_key(&(c as u32))
    }
    pub fn advance(&self, g: u16) -> f32 {
        let i = (g as usize).min(self.num_hmetrics.saturating_sub(1));
        u16_at(&self.data, self.hmtx + 4 * i) as f32
    }
    pub fn kerning(&self, a: u16, b: u16) -> f32 {
        self.kern.get(&(a, b)).copied().unwrap_or(0) as f32
    }

    fn glyph_range(&self, g: u16) -> Option<(usize, usize)> {
        if g as usize >= self.num_glyphs {
            return None;
        }
        let d = &self.data;
        let (a, b) = if self.long_loca {
            (u32_at(d, self.loca + 4 * g as usize) as usize, u32_at(d, self.loca + 4 * g as usize + 4) as usize)
        } else {
            (u16_at(d, self.loca + 2 * g as usize) as usize * 2, u16_at(d, self.loca + 2 * g as usize + 2) as usize * 2)
        };
        if b <= a {
            return None;
        }
        Some((self.glyf + a, self.glyf + b))
    }

    /// the outline as closed contours of line segments, in font units (y up)
    pub fn outline(&self, g: u16, depth: u32) -> Vec<Vec<Point>> {
        let Some((o, _end)) = self.glyph_range(g) else { return vec![] };
        let d = &self.data;
        let ncont = i16_at(d, o);
        if ncont < 0 {
            if depth > 4 {
                return vec![];
            }
            // composite glyph
            let mut out = vec![];
            let mut p = o + 10;
            loop {
                let flags = u16_at(d, p);
                let gi = u16_at(d, p + 2);
                p += 4;
                let (dx, dy) = if flags & 1 != 0 {
                    let v = (i16_at(d, p) as f32, i16_at(d, p + 2) as f32);
                    p += 4;
                    v
                } else {
                    let v = (d[p] as i8 as f32, d[p + 1] as i8 as f32);
                    p += 2;
                    v
                };
                let (mut a, mut b, mut c, mut dd) = (1.0, 0.0, 0.0, 1.0);
                let f2 = |x: i16| x as f32 / 16384.0;
                if flags & 8 != 0 {
                    a = f2(i16_at(d, p));
                    dd = a;
                    p += 2;
                } else if flags & 0x40 != 0 {
                    a = f2(i16_at(d, p));
                    dd = f2(i16_at(d, p + 2));
                    p += 4;
                } else if flags & 0x80 != 0 {
                    a = f2(i16_at(d, p));
                    b = f2(i16_at(d, p + 2));
                    c = f2(i16_at(d, p + 4));
                    dd = f2(i16_at(d, p + 6));
                    p += 8;
                }
                for contour in self.outline(gi, depth + 1) {
                    out.push(
                        contour
                            .iter()
                            .map(|q| Point { x: a * q.x + c * q.y + dx, y: b * q.x + dd * q.y + dy })
                            .collect(),
                    );
                }
                if flags & 0x20 == 0 {
                    break;
                }
            }
            return out;
        }
        let ncont = ncont as usize;
        let mut ends = vec![];
        for i in 0..ncont {
            ends.push(u16_at(d, o + 10 + 2 * i) as usize);
        }
        let npts = ends.last().map(|e| e + 1).unwrap_or(0);
        let ilen = u16_at(d, o + 10 + 2 * ncont) as usize;
        let mut p = o + 12 + 2 * ncont + ilen;
        let mut flags = Vec::with_capacity(npts);
        while flags.len() < npts {
            let f = d[p];
            p += 1;
            flags.push(f);
            if f & 8 != 0 {
                let r = d[p];
                p += 1;
                for _ in 0..r {
                    flags.push(f);
                }
            }
        }
        let mut xs = Vec::with_capacity(npts);
        let mut v: i32 = 0;
        for &f in &flags {
            if f & 2 != 0 {
                let dx = d[p] as i32;
                p += 1;
                v += if f & 16 != 0 { dx } else { -dx };
            } else if f & 16 == 0 {
                v += i16_at(d, p) as i32;
                p += 2;
            }
            xs.push(v);
        }
        let mut ys = Vec::with_capacity(npts);
        v = 0;
        for &f in &flags {
            if f & 4 != 0 {
                let dy = d[p] as i32;
                p += 1;
                v += if f & 32 != 0 { dy } else { -dy };
            } else if f & 32 == 0 {
                v += i16_at(d, p) as i32;
                p += 2;
            }
            ys.push(v);
        }
        // contours with quadratic curves -> polylines
        let mut out = vec![];
        let mut start = 0;
        for &e in &ends {
            let pts: Vec<(Point, bool)> = (start..=e).map(|i| (Point { x: xs[i] as f32, y: ys[i] as f32 }, flags[i] & 1 != 0)).collect();
            start = e + 1;
            if pts.is_empty() {
                continue;
            }
            out.push(flatten_quadratic(&pts));
        }
        out
    }
}

fn mid(a: Point, b: Point) -> Point {
    Point { x: (a.x + b.x) / 2.0, y: (a.y + b.y) / 2.0 }
}

fn flatten_quadratic(pts: &[(Point, bool)]) -> Vec<Point> {
    let n = pts.len();
    // start at an on-curve point (or the midpoint of two off-curve points)
    let first_on = pts.iter().position(|p| p.1);
    let (start_pt, rot) = match first_on {
        Some(i) => (pts[i].0, i),
        None => (mid(pts[0].0, pts[1 % n].0), 0),
    };
    let mut out = vec![start_pt];
    let mut cur = start_pt;
    let mut ctrl: Option<Point> = None;
    let seq: Vec<(Point, bool)> = (1..=n).map(|k| pts[(rot + k) % n]).collect();
    let quad = |out: &mut Vec<Point>, a: Point, c: Point, b: Point| {
        let steps = 8;
        for s in 1..=steps {
            let t = s as f32 / steps as f32;
            let mt = 1.0 - t;
            out.push(Point { x: mt * mt * a.x + 2.0 * mt * t * c.x + t * t * b.x, y: mt * mt * a.y + 2.0 * mt * t * c.y + t * t * b.y });
        }
    };
    for (p, on) in seq {
        match (on, ctrl) {
            (true, None) => {
                out.push(p);
                cur = p;
            }
            (true, Some(c)) => {
                quad(&mut out, cur, c, p);
                cur = p;
                ctrl = None;
            }
            (false, None) => ctrl = Some(p),
            (false, Some(c)) => {
                let m = mid(c, p);
                quad(&mut out, cur, c, m);
                cur = m;
                ctrl = Some(p);
            }
        }
    }
    if let Some(c) = ctrl {
        quad(&mut out, cur, c, start_pt);
    }
    out
}

fn parse_cmap(d: &[u8], cmap: usize) -> HashMap<u32, u16> {
    let n = u16_at(d, cmap + 2) as usize;
    let mut best: Option<(usize, u16)> = None;
    for i in 0..n {
        let r = cmap + 4 + 8 * i;
        let pid = u16_at(d, r);
        let eid = u16_at(d, r + 2);
        let off = cmap + u32_at(d, r + 4) as usize;
        let fmt = u16_at(d, off);
        let score = match (pid, eid, fmt) {
            (3, 10, 12) | (0, 4, 12) | (0, 6, 12) => 3,
            (3, 1, 4) | (0, 3, 4) | (0, 1, 4) => 2,
            (3, 0, 4) => 1,
            _ => 0,
        };
        if score > 0 && best.map_or(true, |b| score > b.1 as usize as u16 as usize) {
            best = Some((off, score as u16));
        }
    }
    let mut m = HashMap::new();
    let Some((off, _)) = best else { return m };
    match u16_at(d, off) {
        4 => {
            let segx2 = u16_at(d, off + 6) as usize;
            let ends = off + 14;
            let starts = ends + segx2 + 2;
            let deltas = starts + segx2;
            let ranges = deltas + segx2;
            for s in 0..segx2 / 2 {
                let end = u16_at(d, ends + 2 * s) as u32;
                let start = u16_at(d, starts + 2 * s) as u32;
                let delta = u16_at(d, deltas + 2 * s);
                let ro = u16_at(d, ranges + 2 * s) as usize;
                if start > end {
                    continue;
                }
                for c in start..=end {
                    if c == 0xFFFF {
                        continue;
                    }
                    let g = if ro == 0 {
                        (c as u16).wrapping_add(delta)
                    } else {
                        let p = ranges + 2 * s + ro + 2 * (c - start) as usize;
                        let g = u16_at(d, p);
                        if g == 0 {
                            0
                        } else {
                            g.wrapping_add(delta)
                        }
                    };
                    if g != 0 {
                        m.insert(c, g);
                    }
                }
            }
        }
        12 => {
            let ng = u32_at(d, off + 12) as usize;
            for k in 0..ng {
                let p = off + 16 + 12 * k;
                let (s, e, g) = (u32_at(d, p), u32_at(d, p + 4), u32_at(d, p + 8));
                for c in s..=e.min(s + 0x10000) {
                    m.insert(c, (g + c - s) as u16);
                }
            }
        }
        _ => {}
    }
    m
}

/// a rasterized glyph: coverage 0..255, with the bitmap's offset from the pen position (y down)
pub struct GlyphBitmap {
    pub w: usize,
    pub h: usize,
    pub left: i32,
    pub top: i32,
    pub coverage: Vec<u8>,
}

/// rasterizes contours (already scaled to pixels, y down, offset by a subpixel x) by signed area accumulation
pub fn rasterize(contours: &[Vec<Point>]) -> GlyphBitmap {
    let mut minx = f32::MAX;
    let mut miny = f32::MAX;
    let mut maxx = f32::MIN;
    let mut maxy = f32::MIN;
    for c in contours {
        for p in c {
            minx = minx.min(p.x);
            miny = miny.min(p.y);
            maxx = maxx.max(p.x);
            maxy = maxy.max(p.y);
        }
    }
    if minx > maxx {
        return GlyphBitmap { w: 0, h: 0, left: 0, top: 0, coverage: vec![] };
    }
    let left = minx.floor() as i32;
    let top = miny.floor() as i32;
    let w = (maxx.ceil() as i32 - left + 1).max(1) as usize;
    let h = (maxy.ceil() as i32 - top + 1).max(1) as usize;
    let mut acc = vec![0f32; w * h + 1];
    for c in contours {
        for k in 0..c.len() {
            let a = c[k];
            let b = c[(k + 1) % c.len()];
            line(&mut acc, w, h, a.x - left as f32, a.y - top as f32, b.x - left as f32, b.y - top as f32);
        }
    }
    let mut cov = vec![0u8; w * h];
    let mut sum = 0.0;
    for i in 0..w * h {
        if i % w == 0 {
            sum = 0.0;
        }
        sum += acc[i];
        cov[i] = (sum.abs().min(1.0) * 255.0 + 0.5) as u8;
    }
    GlyphBitmap { w, h, left, top, coverage: cov }
}

// the font-rs accumulation: each line adds signed area to the cells it crosses
fn line(acc: &mut [f32], w: usize, h: usize, x0: f32, y0: f32, x1: f32, y1: f32) {
    if (y0 - y1).abs() < 1e-9 {
        return;
    }
    let (dir, x0, y0, x1, y1) = if y0 < y1 { (1.0, x0, y0, x1, y1) } else { (-1.0, x1, y1, x0, y0) };
    let dxdy = (x1 - x0) / (y1 - y0);
    let mut x = x0;
    let y0i = y0.max(0.0) as usize;
    if y0 < 0.0 {
        x -= y0 * dxdy;
    }
    let ymax = (y1.ceil() as usize).min(h);
    for y in y0i..ymax {
        let linestart = y * w;
        let dy = ((y + 1) as f32).min(y1) - (y as f32).max(y0);
        let xnext = x + dxdy * dy;
        let d = dy * dir;
        let (xa, xb) = if x < xnext { (x, xnext) } else { (xnext, x) };
        let xaf = xa.floor();
        let xai = xaf as i32;
        let xbc = xb.ceil();
        let xbi = xbc as i32;
        if xbi <= xai + 1 {
            let xmf = 0.5 * (x + xnext) - xaf;
            let i = linestart as i32 + xai;
            if i >= 0 && (i as usize) < acc.len() {
                acc[i as usize] += d - d * xmf;
            }
            if i + 1 >= 0 && ((i + 1) as usize) < acc.len() {
                acc[(i + 1) as usize] += d * xmf;
            }
        } else {
            let s = (xb - xa).recip();
            let x0f = xa - xaf;
            let a0 = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
            let x1f = xb - xbc + 1.0;
            let am = 0.5 * s * x1f * x1f;
            let i0 = linestart as i32 + xai;
            let put = |acc: &mut [f32], i: i32, v: f32| {
                if i >= 0 && (i as usize) < acc.len() {
                    acc[i as usize] += v;
                }
            };
            put(acc, i0, d * a0);
            if xbi == xai + 2 {
                put(acc, i0 + 1, d * (1.0 - a0 - am));
            } else {
                let a1 = s * (1.5 - x0f);
                put(acc, i0 + 1, d * (a1 - a0));
                for xi in xai + 2..xbi - 1 {
                    put(acc, linestart as i32 + xi, d * s);
                }
                let a2 = a1 + (xbi - xai - 3) as f32 * s;
                put(acc, linestart as i32 + xbi - 1, d * (1.0 - a2 - am));
            }
            put(acc, linestart as i32 + xbi, d * am);
        }
        x = xnext;
    }
}

/// the fonts known to the renderer, matched by family, weight and style
pub struct FontSet {
    pub fonts: Vec<(String, u16, bool, std::rc::Rc<Font>)>, // family (lowercase), weight, italic, font
    glyph_cache: std::cell::RefCell<HashMap<(usize, u16, u32, u8), std::rc::Rc<GlyphBitmap>>>,
}

impl FontSet {
    pub fn new() -> Self {
        FontSet { fonts: vec![], glyph_cache: Default::default() }
    }
    pub fn add(&mut self, family: &str, weight: u16, italic: bool, data: Vec<u8>) -> bool {
        match Font::parse(data) {
            Some(f) => {
                self.fonts.push((family.to_ascii_lowercase(), weight, italic, std::rc::Rc::new(f)));
                true
            }
            None => false,
        }
    }
    /// a font loaded once per thread (by its path or URL)
    pub fn add_cached(&mut self, key: &str, family: &str, weight: u16, italic: bool, load: impl FnOnce() -> Option<Vec<u8>>) -> bool {
        thread_local! {
            static CACHE: std::cell::RefCell<HashMap<String, Option<std::rc::Rc<Font>>>> = Default::default();
        }
        let f = CACHE.with(|c| {
            if let Some(f) = c.borrow().get(key) {
                return f.clone();
            }
            let f = load().and_then(Font::parse).map(std::rc::Rc::new);
            c.borrow_mut().insert(key.to_string(), f.clone());
            f
        });
        match f {
            Some(f) => {
                self.fonts.push((family.to_ascii_lowercase(), weight, italic, f));
                true
            }
            None => false,
        }
    }
    /// the best font for a family list (generic families map to DejaVu)
    pub fn pick(&self, families: &[String], weight: u16, italic: bool) -> usize {
        for fam in families {
            let f = fam.to_ascii_lowercase();
            let f = match f.as_str() {
                "serif" | "times" | "times new roman" | "georgia" => "dejavu serif".to_string(),
                "sans-serif" | "arial" | "helvetica" | "verdana" | "system-ui" | "-apple-system" | "sans" => "dejavu sans".to_string(),
                "monospace" | "courier" | "courier new" | "consolas" => "dejavu sans mono".to_string(),
                _ => f,
            };
            let mut best: Option<(usize, i32)> = None;
            for (i, (fam2, w, it, _)) in self.fonts.iter().enumerate() {
                if *fam2 != f {
                    continue;
                }
                let score = (*w as i32 - weight as i32).abs() + if *it != italic { 1000 } else { 0 };
                if best.map_or(true, |b| score < b.1) {
                    best = Some((i, score));
                }
            }
            if let Some((i, _)) = best {
                return i;
            }
        }
        // the default: serif like browsers, else the first font
        self.fonts.iter().position(|f| f.0 == "dejavu serif").unwrap_or(0)
    }
    pub fn font(&self, i: usize) -> &Font {
        &self.fonts[i].3
    }
    /// a rasterized glyph at a size (px) and a subpixel x offset (quarters)
    pub fn glyph(&self, font: usize, g: u16, size: f32, sub: u8, synthetic_italic: bool) -> std::rc::Rc<GlyphBitmap> {
        let key = (font, g, (size * 64.0) as u32, sub | if synthetic_italic { 8 } else { 0 });
        if let Some(b) = self.glyph_cache.borrow().get(&key) {
            return b.clone();
        }
        let f = self.font(font);
        let scale = size / f.units_per_em;
        let dx = sub as f32 / 4.0;
        let contours: Vec<Vec<Point>> = f
            .outline(g, 0)
            .iter()
            .map(|c| {
                c.iter()
                    .map(|p| {
                        let y = -p.y * scale;
                        let x = p.x * scale + dx + if synthetic_italic { -y * 0.2 } else { 0.0 };
                        Point { x, y }
                    })
                    .collect()
            })
            .collect();
        let b = std::rc::Rc::new(rasterize(&contours));
        self.glyph_cache.borrow_mut().insert(key, b.clone());
        b
    }
}

impl Default for FontSet {
    fn default() -> Self {
        Self::new()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn ahem_metrics_and_square() {
        let p = concat!(env!("CARGO_MANIFEST_DIR"), "/../data/wpt/fonts/Ahem.ttf");
        let Ok(data) = std::fs::read(p) else { return };
        let f = Font::parse(data).unwrap();
        assert_eq!(f.units_per_em, 1000.0);
        assert_eq!(f.ascent, 800.0);
        assert_eq!(f.descent, 200.0);
        let g = f.glyph_index('X');
        assert_eq!(f.advance(g), 1000.0);
        // 'X' in Ahem is a full em square: at 10px, 10x10 fully covered pixels
        let scale = 10.0 / f.units_per_em;
        let c: Vec<Vec<Point>> = f.outline(g, 0).iter().map(|c| c.iter().map(|p| Point { x: p.x * scale, y: -p.y * scale }).collect()).collect();
        let b = rasterize(&c);
        let full = b.coverage.iter().filter(|&&v| v == 255).count();
        assert_eq!(full, 100);
    }
}
