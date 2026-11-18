//! Painting: the CSS 2 painting order (Appendix E, simplified) into an RGBA
//! canvas, with mitred borders, glyph coverage, images and clipping.
use crate::css::style::*;
use crate::css::values::Color;
use crate::layout::{BoxId, Kind, Layout};

pub struct Canvas {
    pub w: usize,
    pub h: usize,
    pub px: Vec<u8>, // RGBA, opaque background
}

#[derive(Clone, Copy, Debug)]
pub struct Clip {
    x0: f32,
    y0: f32,
    x1: f32,
    y1: f32,
}

pub struct Image {
    pub w: usize,
    pub h: usize,
    pub rgba: Vec<u8>,
}

impl Canvas {
    pub fn new(w: usize, h: usize) -> Self {
        Canvas { w, h, px: vec![255; w * h * 4] }
    }
    fn blend(&mut self, x: i32, y: i32, c: Color, cov: f32) {
        if x < 0 || y < 0 || x as usize >= self.w || y as usize >= self.h {
            return;
        }
        let a = c.a as f32 / 255.0 * cov;
        if a <= 0.0 {
            return;
        }
        let i = (y as usize * self.w + x as usize) * 4;
        for (k, v) in [c.r, c.g, c.b].iter().enumerate() {
            let d = self.px[i + k] as f32;
            self.px[i + k] = (d + (*v as f32 - d) * a).round().clamp(0.0, 255.0) as u8;
        }
        self.px[i + 3] = 255;
    }
    /// fills [x0,x1) x [y0,y1) with edges snapped to pixels
    pub fn fill_rect(&mut self, x0: f32, y0: f32, x1: f32, y1: f32, c: Color, clip: Clip) {
        if c.a == 0 {
            return;
        }
        let ax = x0.max(clip.x0).round() as i32;
        let ay = y0.max(clip.y0).round() as i32;
        let bx = x1.min(clip.x1).round() as i32;
        let by = y1.min(clip.y1).round() as i32;
        for y in ay.max(0)..by.min(self.h as i32) {
            for x in ax.max(0)..bx.min(self.w as i32) {
                self.blend(x, y, c, 1.0);
            }
        }
    }
    /// fills a convex polygon by sampling pixel centers
    pub fn fill_poly(&mut self, pts: &[(f32, f32)], c: Color, clip: Clip) {
        if c.a == 0 || pts.len() < 3 {
            return;
        }
        let miny = pts.iter().map(|p| p.1).fold(f32::MAX, f32::min).max(clip.y0);
        let maxy = pts.iter().map(|p| p.1).fold(f32::MIN, f32::max).min(clip.y1);
        let y0 = miny.floor() as i32;
        let y1 = maxy.ceil() as i32;
        for y in y0.max(0)..y1.min(self.h as i32) {
            let cy = y as f32 + 0.5;
            let mut xs = vec![];
            for k in 0..pts.len() {
                let (ax, ay) = pts[k];
                let (bx, by) = pts[(k + 1) % pts.len()];
                if (ay <= cy && by > cy) || (by <= cy && ay > cy) {
                    xs.push(ax + (cy - ay) / (by - ay) * (bx - ax));
                }
            }
            xs.sort_by(|a, b| a.partial_cmp(b).unwrap());
            let mut i = 0;
            while i + 1 < xs.len() {
                let xa = xs[i].max(clip.x0);
                let xb = xs[i + 1].min(clip.x1);
                // pixels whose centers are inside
                let sx = (xa - 0.5).ceil() as i32;
                let ex = (xb - 0.5).ceil() as i32;
                for x in sx.max(0)..ex.min(self.w as i32) {
                    self.blend(x, y, c, 1.0);
                }
                i += 2;
            }
        }
    }
    fn glyph(&mut self, g: &crate::font::GlyphBitmap, x: i32, y: i32, c: Color, clip: Clip) {
        for gy in 0..g.h {
            let py = y + g.top + gy as i32;
            if (py as f32) < clip.y0.floor() || (py as f32) >= clip.y1.ceil() {
                continue;
            }
            for gx in 0..g.w {
                let px = x + g.left + gx as i32;
                if (px as f32) < clip.x0.floor() || (px as f32) >= clip.x1.ceil() {
                    continue;
                }
                let cov = g.coverage[gy * g.w + gx];
                if cov > 0 {
                    self.blend(px, py, c, cov as f32 / 255.0);
                }
            }
        }
    }
    fn image(&mut self, img: &Image, x: f32, y: f32, w: f32, h: f32, clip: Clip) {
        if img.w == 0 || img.h == 0 || w <= 0.0 || h <= 0.0 {
            return;
        }
        let x0 = x.max(clip.x0).round() as i32;
        let y0 = y.max(clip.y0).round() as i32;
        let x1 = (x + w).min(clip.x1).round() as i32;
        let y1 = (y + h).min(clip.y1).round() as i32;
        for py in y0.max(0)..y1.min(self.h as i32) {
            let sy = (((py as f32 + 0.5 - y) / h * img.h as f32) as usize).min(img.h - 1);
            for px in x0.max(0)..x1.min(self.w as i32) {
                let sx = (((px as f32 + 0.5 - x) / w * img.w as f32) as usize).min(img.w - 1);
                let i = (sy * img.w + sx) * 4;
                let c = Color { r: img.rgba[i], g: img.rgba[i + 1], b: img.rgba[i + 2], a: img.rgba[i + 3] };
                self.blend(px, py, c, 1.0);
            }
        }
    }
}

const NO_CLIP: Clip = Clip { x0: -1e9, y0: -1e9, x1: 1e9, y1: 1e9 };

fn shade(c: Color, k: f32) -> Color {
    let f = |v: u8| ((v as f32) * k).clamp(0.0, 255.0) as u8;
    Color { r: f(c.r), g: f(c.g), b: f(c.b), a: c.a }
}

pub struct Painter<'a, 'b> {
    pub l: &'a Layout<'b>,
    pub canvas: Canvas,
    pub images: &'a [Image],
}

fn intersect(a: Clip, b: Clip) -> Clip {
    Clip { x0: a.x0.max(b.x0), y0: a.y0.max(b.y0), x1: a.x1.min(b.x1), y1: a.y1.min(b.y1) }
}

impl<'a, 'b> Painter<'a, 'b> {
    fn opacity(&self, b: BoxId) -> f32 {
        let mut o = 1.0;
        let mut p = Some(b);
        while let Some(x) = p {
            o *= self.l.boxes[x].style.opacity;
            p = self.l.boxes[x].parent;
        }
        o
    }
    fn col(&self, c: Color, b: BoxId) -> Color {
        let o = self.opacity(b);
        if o >= 1.0 {
            c
        } else {
            Color { a: (c.a as f32 * o).round() as u8, ..c }
        }
    }

    /// the clip from overflow:hidden ancestors (up to a stacking root)
    fn clip_of(&self, b: BoxId) -> Clip {
        let mut c = NO_CLIP;
        let mut p = self.l.boxes[b].parent;
        let abs = matches!(self.l.boxes[b].style.position, Position::Absolute | Position::Fixed);
        while let Some(x) = p {
            let bx = &self.l.boxes[x];
            // an absolutely positioned box escapes clips of non-positioned ancestors below its containing block
            if bx.style.overflow != Overflow::Visible && bx.node.is_some() && !(abs && !bx.style.is_positioned()) {
                let (px, py, pw, ph) = bx.padding_box();
                c = intersect(c, Clip { x0: px, y0: py, x1: px + pw, y1: py + ph });
            }
            p = bx.parent;
        }
        c
    }

    pub fn paint_root(&mut self) {
        // the canvas background comes from the root element (or body)
        let root = self.l.root;
        let html = self.l.boxes[root].children.first().copied();
        let mut bg = Color { r: 255, g: 255, b: 255, a: 255 };
        let mut bg_box = None;
        if let Some(h) = html {
            let hs = &self.l.boxes[h].style;
            if hs.background_color.a > 0 || hs.background_image.is_some() {
                bg_box = Some(h);
            } else if let Some(body) = self.l.boxes[h].children.iter().copied().find(|&c| self.l.boxes[c].node.is_some_and(|n| self.l.doc.is(n, "body"))) {
                if self.l.boxes[body].style.background_color.a > 0 || self.l.boxes[body].style.background_image.is_some() {
                    bg_box = Some(body);
                }
            }
        }
        if let Some(bb) = bg_box {
            let c = self.l.boxes[bb].style.background_color;
            // over white
            let a = c.a as f32 / 255.0;
            let mix = |v: u8| (255.0 + (v as f32 - 255.0) * a).round() as u8;
            bg = Color::rgb(mix(c.r), mix(c.g), mix(c.b));
        }
        let (w, h) = (self.canvas.w, self.canvas.h);
        for i in 0..w * h {
            self.canvas.px[i * 4] = bg.r;
            self.canvas.px[i * 4 + 1] = bg.g;
            self.canvas.px[i * 4 + 2] = bg.b;
            self.canvas.px[i * 4 + 3] = 255;
        }
        if let Some(bb) = bg_box {
            if self.l.boxes[bb].style.background_image.is_some() {
                // the image is painted relative to the root's box over the whole canvas
                let st = self.l.boxes[bb].style.clone();
                let area = (0.0, 0.0, w as f32, h as f32);
                self.background_image(bb, &st, area, area, NO_CLIP);
            }
        }
        self.stacking(root, bg_box);
    }

    /// paints a stacking context (or a box painted atomically) per Appendix E
    fn stacking(&mut self, root: BoxId, skip_bg: Option<BoxId>) {
        let mut positioned: Vec<(i32, usize, BoxId)> = vec![];
        let mut blocks = vec![];
        let mut floats = vec![];
        let mut inlines = vec![];
        let mut order = 0;
        self.collect(root, true, &mut positioned, &mut blocks, &mut floats, &mut inlines, &mut order);
        positioned.sort_by_key(|(z, o, _)| (*z, *o));
        // the root's own background and border
        if Some(root) != skip_bg {
            self.decorations(root);
        } else {
            self.borders(root);
        }
        for &(z, _, b) in &positioned {
            if z < 0 {
                self.stacking(b, None);
            }
        }
        for b in blocks {
            if b != root {
                self.decorations(b);
            }
        }
        for f in floats {
            self.stacking(f, None);
        }
        for b in inlines {
            self.inline_content(b);
        }
        for &(z, _, b) in &positioned {
            if z >= 0 {
                self.stacking(b, None);
            }
        }
        // outlines last
        self.outline(root);
    }

    fn collect(&self, b: BoxId, is_root: bool, pos: &mut Vec<(i32, usize, BoxId)>, blocks: &mut Vec<BoxId>, floats: &mut Vec<BoxId>, inlines: &mut Vec<BoxId>, order: &mut usize) {
        let bx = &self.l.boxes[b];
        if !bx.laid_out && !matches!(bx.kind, Kind::Text(_) | Kind::Inline | Kind::Br) && b != self.l.root {
            return;
        }
        if !is_root {
            if bx.style.is_positioned() && !matches!(bx.kind, Kind::Text(_)) {
                *order += 1;
                pos.push((bx.style.z_index.unwrap_or(0), *order, b));
                return;
            }
            if bx.style.float != Float::None && !matches!(bx.kind, Kind::Text(_)) {
                floats.push(b);
                return;
            }
            if matches!(bx.style.display, Display::InlineBlock | Display::InlineFlex | Display::InlineTable) || (matches!(bx.kind, Kind::Replaced { .. }) && bx.style.display.is_inline_level()) {
                // atomic inlines are painted with their line
                return;
            }
        }
        if !matches!(bx.kind, Kind::Inline | Kind::Text(_) | Kind::Br) {
            blocks.push(b);
            if !bx.lines.is_empty() || bx.marker.is_some() {
                inlines.push(b);
            }
        }
        for &c in &bx.children {
            self.collect(c, false, pos, blocks, floats, inlines, order);
        }
    }

    fn decorations(&mut self, b: BoxId) {
        let bx = &self.l.boxes[b];
        if !bx.style.visible || matches!(bx.kind, Kind::Inline | Kind::Text(_) | Kind::Br) {
            return;
        }
        if matches!(bx.kind, Kind::TableRow | Kind::TableRowGroup) && bx.style.background_color.a == 0 {
            return;
        }
        let clip = self.clip_of(b);
        let (x, y, w, h) = bx.border_box();
        let st = bx.style.clone();
        if st.background_color.a > 0 {
            let c = self.col(st.background_color, b);
            self.canvas.fill_rect(x, y, x + w, y + h, c, clip);
        }
        if st.background_image.is_some() {
            let pb = bx.padding_box();
            self.background_image(b, &st, pb, (x, y, w, h), clip);
        }
        self.borders(b);
        // replaced content
        if let Kind::Replaced { image: Some(i), .. } = bx.kind {
            if let Some(img) = self.images.get(i) {
                let (cx, cy, cw, ch) = (bx.x, bx.y, bx.w, bx.h);
                self.canvas.image(img, cx, cy, cw, ch, clip);
            }
        }
    }

    fn background_image(&mut self, b: BoxId, st: &Style, origin: (f32, f32, f32, f32), area: (f32, f32, f32, f32), clip: Clip) {
        let Some(url) = &st.background_image else { return };
        let full = (self.l.resolve_url)(url);
        let Some(&i) = self.l.images.by_url.get(&full) else { return };
        let Some(img) = self.images.get(i) else { return };
        let (iw, ih) = (img.w as f32, img.h as f32);
        if iw == 0.0 || ih == 0.0 {
            return;
        }
        let (ox, oy, ow, oh) = origin;
        let px = ox + st.background_position.0.px + st.background_position.0.pct / 100.0 * (ow - iw);
        let py = oy + st.background_position.1.px + st.background_position.1.pct / 100.0 * (oh - ih);
        let c = intersect(clip, Clip { x0: area.0, y0: area.1, x1: area.0 + area.2, y1: area.1 + area.3 });
        let (rx, ry) = st.background_repeat;
        let xs: Vec<f32> = if rx {
            let mut v = vec![];
            let mut x = px - ((px - c.x0) / iw).ceil().max(0.0) * iw;
            while x < c.x1 {
                v.push(x);
                x += iw;
            }
            v
        } else {
            vec![px]
        };
        let ys: Vec<f32> = if ry {
            let mut v = vec![];
            let mut y = py - ((py - c.y0) / ih).ceil().max(0.0) * ih;
            while y < c.y1 {
                v.push(y);
                y += ih;
            }
            v
        } else {
            vec![py]
        };
        let _ = b;
        for &yy in &ys {
            for &xx in &xs {
                self.canvas.image(img, xx, yy, iw, ih, c);
            }
        }
    }

    fn borders(&mut self, b: BoxId) {
        let bx = &self.l.boxes[b];
        if !bx.style.visible || matches!(bx.kind, Kind::Inline | Kind::Text(_) | Kind::Br) {
            return;
        }
        let clip = self.clip_of(b);
        let (x, y, w, h) = bx.border_box();
        let st = bx.style.clone();
        let bw = bx.b;
        let cols = [self.col(st.border_color[0], b), self.col(st.border_color[1], b), self.col(st.border_color[2], b), self.col(st.border_color[3], b)];
        self.border_box_sides(x, y, w, h, bw, &st.border_style, cols, [true, true, true, true], clip);
    }

    fn outline(&mut self, b: BoxId) {
        let bx = &self.l.boxes[b];
        let st = &bx.style;
        if matches!(st.outline_style, BorderStyle::None | BorderStyle::Hidden) || st.outline_width <= 0.0 || bx.node.is_none() {
            return;
        }
        let (x, y, w, h) = bx.border_box();
        let ow = st.outline_width;
        let c = self.col(st.outline_color, b);
        self.border_box_sides(x - ow, y - ow, w + 2.0 * ow, h + 2.0 * ow, [ow; 4], &[st.outline_style; 4], [c; 4], [true; 4], NO_CLIP);
    }

    /// the four sides of a border box as mitred trapezoids
    #[allow(clippy::too_many_arguments)]
    fn border_box_sides(&mut self, x: f32, y: f32, w: f32, h: f32, bw: [f32; 4], styles: &[BorderStyle; 4], cols: [Color; 4], sides: [bool; 4], clip: Clip) {
        let (x0, y0, x1, y1) = (x.round(), y.round(), (x + w).round(), (y + h).round());
        let (t, r, b, l) = (bw[0], bw[1], bw[2], bw[3]);
        for side in 0..4 {
            if !sides[side] || bw[side] <= 0.0 {
                continue;
            }
            let st = styles[side];
            if matches!(st, BorderStyle::None | BorderStyle::Hidden) {
                continue;
            }
            let c = cols[side];
            // the outer and inner edges of this side
            let quad: [(f32, f32); 4] = match side {
                0 => [(x0, y0), (x1, y0), (x1 - r, y0 + t), (x0 + l, y0 + t)],
                1 => [(x1, y0), (x1, y1), (x1 - r, y1 - b), (x1 - r, y0 + t)],
                2 => [(x1, y1), (x0, y1), (x0 + l, y1 - b), (x1 - r, y1 - b)],
                _ => [(x0, y1), (x0, y0), (x0 + l, y0 + t), (x0 + l, y1 - b)],
            };
            match st {
                BorderStyle::Solid => self.canvas.fill_poly(&quad, c, clip),
                BorderStyle::Double => {
                    let third = (bw[side] / 3.0).round().max(1.0);
                    if bw[side] < 3.0 {
                        self.canvas.fill_poly(&quad, c, clip);
                        continue;
                    }
                    // outer and inner thirds
                    let outer = shrink_quad(side, &quad, 0.0, third, [t, r, b, l]);
                    let inner = shrink_quad(side, &quad, bw[side] - third, bw[side], [t, r, b, l]);
                    self.canvas.fill_poly(&outer, c, clip);
                    self.canvas.fill_poly(&inner, c, clip);
                }
                BorderStyle::Dashed | BorderStyle::Dotted => {
                    // segments along the side
                    let horizontal = side % 2 == 0;
                    let thick = bw[side];
                    let len = if horizontal { x1 - x0 } else { y1 - y0 };
                    let seg = if st == BorderStyle::Dotted { thick } else { (thick * 3.0).max(3.0) };
                    let n = ((len / seg).floor() as i32).max(1);
                    let n = if n % 2 == 0 { n + 1 } else { n };
                    let unit = len / n as f32;
                    for k in 0..n {
                        if k % 2 == 1 {
                            continue;
                        }
                        let a = k as f32 * unit;
                        let (rx0, ry0, rx1, ry1) = match side {
                            0 => (x0 + a, y0, x0 + a + unit, y0 + t),
                            2 => (x0 + a, y1 - b, x0 + a + unit, y1),
                            1 => (x1 - r, y0 + a, x1, y0 + a + unit),
                            _ => (x0, y0 + a, x0 + l, y0 + a + unit),
                        };
                        let _ = horizontal;
                        self.canvas.fill_rect(rx0, ry0, rx1, ry1, c, clip);
                    }
                }
                BorderStyle::Groove | BorderStyle::Ridge => {
                    let half = (bw[side] / 2.0).round();
                    let dark = shade(c, 0.5);
                    let (o, i) = match (st, side) {
                        (BorderStyle::Groove, 0 | 3) => (dark, c),
                        (BorderStyle::Groove, _) => (c, dark),
                        (_, 0 | 3) => (c, dark),
                        _ => (dark, c),
                    };
                    let outer = shrink_quad(side, &quad, 0.0, half, [t, r, b, l]);
                    let inner = shrink_quad(side, &quad, half, bw[side], [t, r, b, l]);
                    self.canvas.fill_poly(&outer, o, clip);
                    self.canvas.fill_poly(&inner, i, clip);
                }
                BorderStyle::Inset | BorderStyle::Outset => {
                    let dark = shade(c, 0.5);
                    let use_dark = matches!((st, side), (BorderStyle::Inset, 0 | 3) | (BorderStyle::Outset, 1 | 2));
                    self.canvas.fill_poly(&quad, if use_dark { dark } else { c }, clip);
                }
                _ => {}
            }
        }
    }

    fn inline_content(&mut self, b: BoxId) {
        let bx = &self.l.boxes[b];
        let visible_block = bx.style.visible;
        let clip_box = self.clip_of(b);
        let clip = if bx.style.overflow != Overflow::Visible && bx.node.is_some() {
            let (px, py, pw, ph) = bx.padding_box();
            intersect(clip_box, Clip { x0: px, y0: py, x1: px + pw, y1: py + ph })
        } else {
            clip_box
        };
        let lines = bx.lines.clone();
        // the list marker
        if let Some(m) = &bx.marker {
            if visible_block && !lines.is_empty() {
                let st = bx.style.clone();
                let (w, glyphs) = crate::layout::inline::measure(self.l, &st, m);
                let line = &lines[0];
                let x = if st.list_inside { bx.x } else { bx.x - w };
                let c = self.col(st.color, b);
                self.text(&glyphs, x, line.baseline, st.font_size, &st, c, clip);
            }
        }
        for line in &lines {
            // inline box backgrounds and borders
            for f in &line.frags {
                let st = self.l.boxes[f.boxid].style.clone();
                if !st.visible {
                    continue;
                }
                let c = self.col(st.background_color, f.boxid);
                let pl = if f.first { st.padding[3].px + st.border(3) } else { 0.0 };
                let pr = if f.last { st.padding[1].px + st.border(1) } else { 0.0 };
                let _ = (pl, pr);
                self.canvas.fill_rect(f.x0, f.top, f.x1, f.bottom, c, clip);
                let bw = [st.border(0), if f.last { st.border(1) } else { 0.0 }, st.border(2), if f.first { st.border(3) } else { 0.0 }];
                let cols = [self.col(st.border_color[0], f.boxid), self.col(st.border_color[1], f.boxid), self.col(st.border_color[2], f.boxid), self.col(st.border_color[3], f.boxid)];
                self.border_box_sides(f.x0, f.top, f.x1 - f.x0, f.bottom - f.top, bw, &st.border_style, cols, [true, f.last, true, f.first], clip);
            }
            for r in &line.runs {
                if !r.style.visible {
                    continue;
                }
                let owner = self.owner_box_of_run(b, r);
                let c = self.col(r.style.color, owner);
                self.text(&r.glyphs, r.x, r.baseline, r.size, &r.style, c, clip);
                // decorations
                let st = &r.style;
                let deco_owner = self.decoration_style(owner);
                if let Some(ds) = deco_owner {
                    let font = self.l.fonts.font(r.font);
                    let k = st.font_size / font.units_per_em;
                    let thick = (st.font_size / 14.0).round().max(1.0);
                    let dc = self.col(ds.color, owner);
                    if ds.underline {
                        let y = (r.baseline + (font.descent * k * 0.45).max(1.0)).round();
                        self.canvas.fill_rect(r.x, y, r.x + r.width, y + thick, dc, clip);
                    }
                    if ds.line_through {
                        let y = (r.baseline - font.x_height * k / 2.0).round();
                        self.canvas.fill_rect(r.x, y, r.x + r.width, y + thick, dc, clip);
                    }
                    if ds.overline {
                        let y = (r.baseline - font.ascent * k).round();
                        self.canvas.fill_rect(r.x, y, r.x + r.width, y + thick, dc, clip);
                    }
                }
            }
            for &a in &line.atomics {
                self.stacking(a, None);
            }
        }
    }

    fn owner_box_of_run(&self, block: BoxId, r: &crate::layout::TextRun) -> BoxId {
        // the opacity source: approximate with the block
        let _ = r;
        block
    }

    /// the nearest ancestor style that draws a text decoration
    fn decoration_style(&self, b: BoxId) -> Option<Style> {
        let mut p = Some(b);
        while let Some(x) = p {
            let s = &self.l.boxes[x].style;
            if s.underline || s.line_through || s.overline {
                return Some((**s).clone());
            }
            p = self.l.boxes[x].parent;
        }
        None
    }

    #[allow(clippy::too_many_arguments)]
    fn text(&mut self, glyphs: &[(u16, f32, usize)], x: f32, baseline: f32, size: f32, st: &Style, c: Color, clip: Clip) {
        let by = baseline.round() as i32;
        let synthetic_italic = st.italic && !self.l.fonts.fonts.get(glyphs.first().map(|g| g.2).unwrap_or(0)).is_some_and(|f| f.2);
        for &(g, gx, fi) in glyphs {
            let pos = x + gx;
            let ix = pos.floor();
            let sub = (((pos - ix) * 4.0).floor() as u8).min(3);
            let bm = self.l.fonts.glyph(fi, g, size, sub, synthetic_italic);
            self.canvas.glyph(&bm, ix as i32, by, c, clip);
        }
    }
}

/// a side's trapezoid between depths d0 and d1 from the outer edge
fn shrink_quad(side: usize, q: &[(f32, f32); 4], d0: f32, d1: f32, bw: [f32; 4]) -> [(f32, f32); 4] {
    let w = bw[side].max(0.001);
    let lerp = |a: (f32, f32), b: (f32, f32), t: f32| (a.0 + (b.0 - a.0) * t, a.1 + (b.1 - a.1) * t);
    let (o0, o1, i1, i0) = (q[0], q[1], q[2], q[3]);
    let t0 = d0 / w;
    let t1 = d1 / w;
    [lerp(o0, i0, t0), lerp(o1, i1, t0), lerp(o1, i1, t1), lerp(o0, i0, t1)]
}

// ------------------------------------------------------------------ PNG
fn crc32(data: &[u8]) -> u32 {
    let mut table = [0u32; 256];
    for i in 0..256 {
        let mut c = i as u32;
        for _ in 0..8 {
            c = if c & 1 != 0 { 0xEDB88320 ^ (c >> 1) } else { c >> 1 };
        }
        table[i] = c;
    }
    let mut c = 0xFFFFFFFFu32;
    for &b in data {
        c = table[((c ^ b as u32) & 0xFF) as usize] ^ (c >> 8);
    }
    c ^ 0xFFFFFFFF
}

/// a minimal PNG encoder (RGB, stored zlib blocks)
pub fn encode_png(c: &Canvas) -> Vec<u8> {
    let mut raw = Vec::with_capacity(c.h * (c.w * 3 + 1));
    for y in 0..c.h {
        raw.push(0);
        for x in 0..c.w {
            let i = (y * c.w + x) * 4;
            raw.extend_from_slice(&c.px[i..i + 3]);
        }
    }
    let mut z = vec![0x78, 0x01];
    for (k, chunk) in raw.chunks(65535).enumerate() {
        let last = (k + 1) * 65535 >= raw.len();
        z.push(last as u8);
        let n = chunk.len() as u16;
        z.extend_from_slice(&n.to_le_bytes());
        z.extend_from_slice(&(!n).to_le_bytes());
        z.extend_from_slice(chunk);
    }
    if raw.is_empty() {
        z.extend_from_slice(&[1, 0, 0, 0xFF, 0xFF]);
    }
    let (mut a, mut b) = (1u32, 0u32);
    for &v in &raw {
        a = (a + v as u32) % 65521;
        b = (b + a) % 65521;
    }
    z.extend_from_slice(&((b << 16) | a).to_be_bytes());
    let mut out = vec![0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];
    let mut chunk = |out: &mut Vec<u8>, ty: &[u8], data: &[u8]| {
        out.extend_from_slice(&(data.len() as u32).to_be_bytes());
        let mut cd = ty.to_vec();
        cd.extend_from_slice(data);
        out.extend_from_slice(&cd);
        out.extend_from_slice(&crc32(&cd).to_be_bytes());
    };
    let mut ihdr = vec![];
    ihdr.extend_from_slice(&(c.w as u32).to_be_bytes());
    ihdr.extend_from_slice(&(c.h as u32).to_be_bytes());
    ihdr.extend_from_slice(&[8, 2, 0, 0, 0]);
    chunk(&mut out, b"IHDR", &ihdr);
    chunk(&mut out, b"IDAT", &z);
    chunk(&mut out, b"IEND", &[]);
    out
}
