//! Layout: the box tree built from the DOM and computed styles, and the
//! formatting contexts (block with floats and margin collapsing, inline,
//! flex, table) that give every box its position and size.
pub mod block;
pub mod flex;
pub mod inline;
pub mod table;

use crate::css::style::*;
use crate::css::values::Len;
use crate::font::FontSet;
use crate::html::dom::{Document, NodeData, NodeId, Ns};
use std::collections::HashMap;
use std::rc::Rc;

pub type BoxId = usize;

#[derive(Clone, Debug, PartialEq)]
pub enum Kind {
    /// a block container (block, list-item, inline-block, flow-root, anonymous block, table cell)
    Block,
    /// a non-atomic inline box
    Inline,
    Text(String),
    Replaced { image: Option<usize>, iw: f32, ih: f32 },
    Br,
    Flex,
    Table,
    TableRow,
    TableRowGroup,
    TableCell,
    TableCaption,
}

#[derive(Clone, Debug)]
pub struct TextRun {
    pub text: String,
    pub x: f32,
    pub baseline: f32,
    pub width: f32,
    pub font: usize,
    pub size: f32,
    pub style: Rc<Style>,
    /// glyph ids and x offsets (relative to x)
    pub glyphs: Vec<(u16, f32, usize)>, // glyph, x offset, font index (fallbacks)
}

#[derive(Clone, Debug)]
pub struct InlineFrag {
    pub boxid: BoxId,
    pub x0: f32,
    pub x1: f32,
    pub top: f32,
    pub bottom: f32,
    pub first: bool,
    pub last: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Line {
    pub top: f32,
    pub height: f32,
    pub baseline: f32,
    pub runs: Vec<TextRun>,
    pub atomics: Vec<BoxId>,
    pub frags: Vec<InlineFrag>,
}

#[derive(Clone, Debug)]
pub struct LBox {
    pub node: Option<NodeId>,
    pub style: Rc<Style>,
    pub kind: Kind,
    pub children: Vec<BoxId>,
    pub parent: Option<BoxId>,
    pub pseudo: u8, // 0 none, 1 ::before, 2 ::after, 3 anonymous
    // results (absolute, content box)
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub m: [f32; 4],
    pub b: [f32; 4],
    pub p: [f32; 4],
    pub lines: Vec<Line>,
    pub baseline: Option<f32>,
    pub first_baseline: Option<f32>,
    pub marker: Option<String>,
    pub colspan: usize,
    /// absolutely positioned descendants laid out by this box (its containing block)
    pub abs_children: Vec<BoxId>,
    pub static_pos: (f32, f32),
    pub laid_out: bool,
}

impl LBox {
    pub fn border_box(&self) -> (f32, f32, f32, f32) {
        (self.x - self.p[3] - self.b[3], self.y - self.p[0] - self.b[0], self.w + self.p[1] + self.p[3] + self.b[1] + self.b[3], self.h + self.p[0] + self.p[2] + self.b[0] + self.b[2])
    }
    pub fn margin_box(&self) -> (f32, f32, f32, f32) {
        let (x, y, w, h) = self.border_box();
        (x - self.m[3], y - self.m[0], w + self.m[1] + self.m[3], h + self.m[0] + self.m[2])
    }
    pub fn padding_box(&self) -> (f32, f32, f32, f32) {
        (self.x - self.p[3], self.y - self.p[0], self.w + self.p[1] + self.p[3], self.h + self.p[0] + self.p[2])
    }
}

pub struct Images {
    pub sizes: Vec<(u32, u32)>,
    pub by_url: HashMap<String, usize>,
}

pub struct Layout<'a> {
    pub doc: &'a Document,
    pub styles: &'a Styles,
    pub fonts: &'a FontSet,
    pub boxes: Vec<LBox>,
    pub root: BoxId,
    pub vw: f32,
    pub vh: f32,
    pub images: &'a Images,
    pub resolve_url: &'a dyn Fn(&str) -> String,
    pub counters: HashMap<String, i32>,
}

pub fn is_block_level(d: Display) -> bool {
    matches!(
        d,
        Display::Block
            | Display::ListItem
            | Display::Flex
            | Display::Table
            | Display::FlowRoot
            | Display::TableRowGroup
            | Display::TableHeaderGroup
            | Display::TableFooterGroup
            | Display::TableRow
            | Display::TableCell
            | Display::TableCaption
    )
}

impl<'a> Layout<'a> {
    pub fn new(doc: &'a Document, styles: &'a Styles, fonts: &'a FontSet, images: &'a Images, resolve_url: &'a dyn Fn(&str) -> String, vw: f32, vh: f32) -> Self {
        let mut l = Layout { doc, styles, fonts, boxes: vec![], root: 0, vw, vh, images, resolve_url, counters: HashMap::new() };
        // the initial containing block
        let icb_style = Style { display: Display::Block, ..Style::default() };
        l.root = l.new_box(None, Rc::new(icb_style), Kind::Block);
        if let Some(html) = doc.document_element() {
            let kids = l.build(html);
            for k in kids {
                l.boxes[k].parent = Some(l.root);
                l.boxes[l.root].children.push(k);
            }
        }
        l
    }

    pub fn new_box(&mut self, node: Option<NodeId>, style: Rc<Style>, kind: Kind) -> BoxId {
        self.boxes.push(LBox {
            node,
            style,
            kind,
            children: vec![],
            parent: None,
            pseudo: 0,
            x: 0.0,
            y: 0.0,
            w: 0.0,
            h: 0.0,
            m: [0.0; 4],
            b: [0.0; 4],
            p: [0.0; 4],
            lines: vec![],
            baseline: None,
            first_baseline: None,
            marker: None,
            colspan: 1,
            abs_children: vec![],
            static_pos: (0.0, 0.0),
            laid_out: false,
        });
        self.boxes.len() - 1
    }

    fn kind_for(&self, n: NodeId, s: &Style) -> Kind {
        let e = self.doc.element(n).unwrap();
        if e.ns == Ns::Html {
            match e.name.as_str() {
                "img" | "input" | "canvas" | "video" | "iframe" | "embed" | "object" | "svg" => {
                    if e.name == "img" {
                        let src = e.attr("src").unwrap_or("");
                        let url = (self.resolve_url)(src);
                        let img = self.images.by_url.get(&url).copied();
                        let (iw, ih) = img.map(|i| (self.images.sizes[i].0 as f32, self.images.sizes[i].1 as f32)).unwrap_or((0.0, 0.0));
                        return Kind::Replaced { image: img, iw, ih };
                    }
                    if e.name == "input" || e.name == "iframe" || e.name == "canvas" || e.name == "video" {
                        let (iw, ih) = match e.name.as_str() {
                            "iframe" => (300.0, 150.0),
                            "canvas" | "video" => (300.0, 150.0),
                            _ => (0.0, 0.0),
                        };
                        if e.name != "input" {
                            return Kind::Replaced { image: None, iw, ih };
                        }
                    }
                }
                "br" => return Kind::Br,
                _ => {}
            }
        }
        if e.ns == Ns::Svg && e.name == "svg" {
            return Kind::Replaced { image: None, iw: 300.0, ih: 150.0 };
        }
        match s.display {
            Display::Inline => Kind::Inline,
            Display::Flex | Display::InlineFlex => Kind::Flex,
            Display::Table | Display::InlineTable => Kind::Table,
            Display::TableRow => Kind::TableRow,
            Display::TableRowGroup | Display::TableHeaderGroup | Display::TableFooterGroup => Kind::TableRowGroup,
            Display::TableCell => Kind::TableCell,
            Display::TableCaption => Kind::TableCaption,
            _ => Kind::Block,
        }
    }

    /// the boxes generated by a node (none for display:none, several for display:contents)
    fn build(&mut self, n: NodeId) -> Vec<BoxId> {
        match &self.doc.nodes[n].data {
            NodeData::Text(t) => {
                if t.is_empty() {
                    return vec![];
                }
                let parent_style = self.doc.parent(n).and_then(|p| self.styles.elements.get(&p)).cloned().unwrap_or_default();
                let mut st = parent_style;
                st.display = Display::Inline;
                // inline-only properties do not apply to the anonymous text box
                st.margin = [Some(Len::ZERO); 4];
                st.padding = [Len::ZERO; 4];
                st.border_style = [BorderStyle::None; 4];
                st.background_color = crate::css::values::Color::TRANSPARENT;
                st.background_image = None;
                st.position = Position::Static;
                st.width = None;
                st.height = None;
                let b = self.new_box(Some(n), Rc::new(st), Kind::Text(t.clone()));
                vec![b]
            }
            NodeData::Element(e) => {
                let Some(s) = self.styles.elements.get(&n) else { return vec![] };
                if s.display == Display::None {
                    return vec![];
                }
                if e.ns == Ns::Html && e.name == "template" {
                    return vec![];
                }
                let style = Rc::new(s.clone());
                let mut kids = vec![];
                // ::before / ::after
                if let Some(bs) = self.styles.before.get(&n) {
                    if let Some(b) = self.pseudo_box(n, bs, 1) {
                        kids.push(b);
                    }
                }
                let kind = self.kind_for(n, s);
                let replaced = matches!(kind, Kind::Replaced { .. });
                if !replaced && kind != Kind::Br {
                    let children: Vec<NodeId> = self.doc.children(n).to_vec();
                    for c in children {
                        kids.extend(self.build(c));
                    }
                }
                if let Some(a) = self.styles.after.get(&n) {
                    if let Some(b) = self.pseudo_box(n, a, 2) {
                        kids.push(b);
                    }
                }
                if s.display == Display::Contents {
                    return kids;
                }
                let b = self.new_box(Some(n), style.clone(), kind.clone());
                if let Some(cs) = e.attr("colspan") {
                    self.boxes[b].colspan = cs.trim().parse::<usize>().unwrap_or(1).clamp(1, 1000);
                }
                if s.display == Display::ListItem && s.list_style != ListStyle::None {
                    let n_item = self.list_index(n);
                    self.boxes[b].marker = Some(marker_text(s.list_style, n_item));
                }
                self.attach(b, kids);
                vec![b]
            }
            _ => vec![],
        }
    }

    fn list_index(&self, n: NodeId) -> i32 {
        let Some(p) = self.doc.parent(n) else { return 1 };
        let mut start = 1;
        if let Some(pe) = self.doc.element(p) {
            if pe.name == "ol" {
                if let Some(s) = pe.attr("start").and_then(|v| v.trim().parse::<i32>().ok()) {
                    start = s;
                }
            }
        }
        let mut idx = start;
        for &c in self.doc.children(p) {
            if c == n {
                break;
            }
            if let Some(cs) = self.styles.elements.get(&c) {
                if cs.display == Display::ListItem {
                    idx += 1;
                }
            }
        }
        if let Some(v) = self.doc.element(n).and_then(|e| e.attr("value")).and_then(|v| v.trim().parse::<i32>().ok()) {
            idx = v;
        }
        idx
    }

    fn pseudo_box(&mut self, n: NodeId, st: &Style, which: u8) -> Option<BoxId> {
        let content = st.content.as_ref()?;
        if st.display == Display::None {
            return None;
        }
        let mut text = String::new();
        let e = self.doc.element(n).unwrap();
        for c in content {
            match c {
                ContentItem::Text(t) => text.push_str(t),
                ContentItem::Attr(a) => text.push_str(e.attr(a).unwrap_or("")),
                ContentItem::Counter(_) => text.push('1'),
                ContentItem::OpenQuote => text.push('\u{201C}'),
                ContentItem::CloseQuote => text.push('\u{201D}'),
            }
        }
        let style = Rc::new(st.clone());
        let kind = match st.display {
            Display::Inline => Kind::Inline,
            Display::Flex | Display::InlineFlex => Kind::Flex,
            _ => Kind::Block,
        };
        let b = self.new_box(Some(n), style.clone(), kind);
        self.boxes[b].pseudo = which;
        if !text.is_empty() {
            let mut ts = st.clone();
            ts.display = Display::Inline;
            ts.margin = [Some(Len::ZERO); 4];
            ts.padding = [Len::ZERO; 4];
            ts.border_style = [BorderStyle::None; 4];
            ts.background_color = crate::css::values::Color::TRANSPARENT;
            ts.position = Position::Static;
            ts.width = None;
            ts.height = None;
            let t = self.new_box(None, Rc::new(ts), Kind::Text(text));
            self.attach(b, vec![t]);
        }
        Some(b)
    }

    /// attaches children, wrapping inline runs in anonymous blocks when needed
    fn attach(&mut self, b: BoxId, kids: Vec<BoxId>) {
        let kind = self.boxes[b].kind.clone();
        let is_container = matches!(kind, Kind::Block | Kind::TableCell | Kind::TableCaption);
        let any_block = kids.iter().any(|&k| self.is_block_level_box(k));
        if matches!(kind, Kind::Flex) {
            // every in-flow child is a flex item; text runs get anonymous blocks
            let mut out = vec![];
            let mut run = vec![];
            for k in kids {
                if matches!(self.boxes[k].kind, Kind::Text(_)) || (matches!(self.boxes[k].kind, Kind::Inline | Kind::Br) && !self.boxes[k].style.is_out_of_flow()) {
                    run.push(k);
                } else {
                    if !run.is_empty() {
                        if let Some(a) = self.anon_block(b, std::mem::take(&mut run)) {
                            out.push(a);
                        }
                    }
                    out.push(k);
                }
            }
            if !run.is_empty() {
                if let Some(a) = self.anon_block(b, run) {
                    out.push(a);
                }
            }
            for k in &out {
                self.boxes[*k].parent = Some(b);
            }
            self.boxes[b].children = out;
            return;
        }
        if matches!(kind, Kind::Table | Kind::TableRowGroup | Kind::TableRow) {
            table::attach_table_children(self, b, kids);
            return;
        }
        if (is_container || kind == Kind::Inline) && any_block {
            if kind == Kind::Inline {
                // block inside inline: the inline behaves as a block container
                self.boxes[b].kind = Kind::Block;
            }
            let mut out = vec![];
            let mut run = vec![];
            for k in kids {
                if self.is_block_level_box(k) {
                    if !run.is_empty() {
                        if let Some(a) = self.anon_block(b, std::mem::take(&mut run)) {
                            out.push(a);
                        }
                    }
                    out.push(k);
                } else {
                    run.push(k);
                }
            }
            if !run.is_empty() {
                if let Some(a) = self.anon_block(b, run) {
                    out.push(a);
                }
            }
            for k in &out {
                self.boxes[*k].parent = Some(b);
            }
            self.boxes[b].children = out;
        } else {
            for k in &kids {
                self.boxes[*k].parent = Some(b);
            }
            self.boxes[b].children = kids;
        }
    }

    pub fn is_block_level_box(&self, k: BoxId) -> bool {
        let bx = &self.boxes[k];
        match bx.kind {
            Kind::Text(_) | Kind::Br => false,
            _ => {
                // floats and absolutely positioned boxes stay where they are (in an anonymous block if needed)
                if bx.style.is_out_of_flow() {
                    return false;
                }
                is_block_level(bx.style.display)
            }
        }
    }

    fn collapsible_ws_only(&self, k: BoxId) -> bool {
        match &self.boxes[k].kind {
            Kind::Text(t) => {
                matches!(self.boxes[k].style.white_space, WhiteSpace::Normal | WhiteSpace::NoWrap | WhiteSpace::PreLine)
                    && t.chars().all(|c| c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\x0C')
            }
            _ => false,
        }
    }

    fn anon_block(&mut self, parent: BoxId, run: Vec<BoxId>) -> Option<BoxId> {
        if run.iter().all(|&k| self.collapsible_ws_only(k)) {
            return None;
        }
        let ps = &self.boxes[parent].style;
        let mut st = Style { display: Display::Block, ..Style::default() };
        // inherited properties
        st.color = ps.color;
        st.font_family = ps.font_family.clone();
        st.font_size = ps.font_size;
        st.font_weight = ps.font_weight;
        st.italic = ps.italic;
        st.line_height = ps.line_height;
        st.text_align = ps.text_align;
        st.white_space = ps.white_space;
        st.visible = ps.visible;
        st.letter_spacing = ps.letter_spacing;
        st.word_spacing = ps.word_spacing;
        st.text_transform = ps.text_transform;
        st.list_style = ps.list_style;
        let a = self.new_box(None, Rc::new(st), Kind::Block);
        self.boxes[a].pseudo = 3;
        for k in &run {
            self.boxes[*k].parent = Some(a);
        }
        self.boxes[a].children = run;
        Some(a)
    }

    // ------------------------------------------------------------ helpers
    pub fn font_of(&self, s: &Style) -> usize {
        self.fonts.pick(&s.font_family, s.font_weight, s.italic)
    }
    /// ascent and descent (px) of the style's primary font
    pub fn metrics(&self, s: &Style) -> (f32, f32) {
        let f = self.fonts.font(self.font_of(s));
        let k = s.font_size / f.units_per_em;
        (f.ascent * k, f.descent * k)
    }
    pub fn line_height(&self, s: &Style) -> f32 {
        match s.line_height {
            LineHeight::Px(p) => p,
            LineHeight::Number(n) => n * s.font_size,
            LineHeight::Normal => {
                let f = self.fonts.font(self.font_of(s));
                let k = s.font_size / f.units_per_em;
                ((f.ascent + f.descent + f.line_gap) * k).round().max(0.0)
            }
        }
    }

    /// the margin/border/padding of a box against a containing block width
    pub fn resolve_edges(&mut self, b: BoxId, cbw: f32) {
        let s = self.boxes[b].style.clone();
        let inline_text = matches!(self.boxes[b].kind, Kind::Text(_));
        for k in 0..4 {
            self.boxes[b].b[k] = if inline_text { 0.0 } else { s.border(k) };
            self.boxes[b].p[k] = if inline_text { 0.0 } else { s.padding[k].resolve(cbw).max(0.0) };
            self.boxes[b].m[k] = s.margin[k].map(|m| m.resolve(cbw)).unwrap_or(0.0);
        }
        if matches!(self.boxes[b].kind, Kind::TableRow | Kind::TableRowGroup) {
            self.boxes[b].p = [0.0; 4];
            self.boxes[b].m = [0.0; 4];
        }
        if self.boxes[b].kind == Kind::TableCell {
            self.boxes[b].m = [0.0; 4];
        }
    }
    pub fn hsum(&self, b: BoxId) -> f32 {
        let x = &self.boxes[b];
        x.m[1] + x.m[3] + x.b[1] + x.b[3] + x.p[1] + x.p[3]
    }
    pub fn vsum(&self, b: BoxId) -> f32 {
        let x = &self.boxes[b];
        x.m[0] + x.m[2] + x.b[0] + x.b[2] + x.p[0] + x.p[2]
    }
    /// content width from a specified width (box-sizing aware)
    pub fn content_from_specified(&self, b: BoxId, spec: f32, horizontal: bool) -> f32 {
        let x = &self.boxes[b];
        if x.style.box_sizing == BoxSizing::BorderBox {
            let edge = if horizontal { x.b[1] + x.b[3] + x.p[1] + x.p[3] } else { x.b[0] + x.b[2] + x.p[0] + x.p[2] };
            (spec - edge).max(0.0)
        } else {
            spec
        }
    }
    pub fn clamp_width(&self, b: BoxId, w: f32, cbw: f32) -> f32 {
        let s = &self.boxes[b].style;
        let mut w = w;
        if let Some(mx) = s.max_width {
            w = w.min(self.content_from_specified(b, mx.resolve(cbw), true));
        }
        if let Some(mn) = s.min_width {
            w = w.max(self.content_from_specified(b, mn.resolve(cbw), true));
        }
        w.max(0.0)
    }
    pub fn clamp_height(&self, b: BoxId, h: f32, cbh: Option<f32>) -> f32 {
        let s = &self.boxes[b].style;
        let mut h = h;
        if let Some(mx) = s.max_height {
            if !mx.has_pct() || cbh.is_some() {
                h = h.min(self.content_from_specified(b, mx.resolve(cbh.unwrap_or(0.0)), false));
            }
        }
        if let Some(mn) = s.min_height {
            if !mn.has_pct() || cbh.is_some() {
                h = h.max(self.content_from_specified(b, mn.resolve(cbh.unwrap_or(0.0)), false));
            }
        }
        h.max(0.0)
    }
    /// a definite specified height, if any (percentages need a definite containing block height)
    pub fn specified_height(&self, b: BoxId, cbh: Option<f32>) -> Option<f32> {
        let s = &self.boxes[b].style;
        let h = s.height?;
        if h.has_pct() {
            let c = cbh?;
            return Some(self.content_from_specified(b, h.resolve(c), false));
        }
        Some(self.content_from_specified(b, h.px, false))
    }

    // ------------------------------------------------------------ intrinsic sizes
    /// (min-content, max-content) border-box widths of a box
    pub fn intrinsic(&mut self, b: BoxId) -> (f32, f32) {
        self.resolve_edges(b, 0.0);
        let s = self.boxes[b].style.clone();
        let edges = self.boxes[b].b[1] + self.boxes[b].b[3] + self.boxes[b].p[1] + self.boxes[b].p[3];
        if let Some(w) = s.width {
            if !w.has_pct() {
                let cw = self.content_from_specified(b, w.px, true);
                let cw = self.clamp_width(b, cw, 0.0);
                return (cw + edges, cw + edges);
            }
        }
        let (mn, mx) = match self.boxes[b].kind.clone() {
            Kind::Replaced { iw, ih, .. } => {
                let w = match (s.width, s.height) {
                    (None, Some(h)) if !h.has_pct() && ih > 0.0 => h.px * iw / ih,
                    _ => iw,
                };
                (w, w)
            }
            Kind::Flex => flex::intrinsic(self, b),
            Kind::Table => table::intrinsic(self, b),
            _ => {
                let kids = self.boxes[b].children.clone();
                let has_inline = kids.iter().any(|&k| !self.is_block_level_box(k));
                if has_inline && !kids.iter().all(|&k| self.is_block_level_box(k) || self.boxes[k].style.is_out_of_flow() && !matches!(self.boxes[k].kind, Kind::Text(_))) {
                    inline::intrinsic(self, b)
                } else {
                    let mut mn: f32 = 0.0;
                    let mut mx: f32 = 0.0;
                    let mut float_row: f32 = 0.0;
                    for k in kids {
                        if self.boxes[k].style.position == Position::Absolute || self.boxes[k].style.position == Position::Fixed {
                            continue;
                        }
                        let (a, c) = self.intrinsic(k);
                        let m = self.boxes[k].m[1].max(0.0) + self.boxes[k].m[3].max(0.0);
                        let ms = &self.boxes[k].style;
                        let mm = ms.margin[1].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0)
                            + ms.margin[3].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0);
                        let _ = m;
                        if ms.float != Float::None {
                            float_row += c + mm;
                            mx = mx.max(float_row);
                        } else {
                            float_row = 0.0;
                            mx = mx.max(c + mm);
                        }
                        mn = mn.max(a + mm);
                    }
                    (mn, mx)
                }
            }
        };
        let mut mn = mn;
        let mut mx = mx;
        if let Some(m) = s.min_width {
            if !m.has_pct() {
                let v = self.content_from_specified(b, m.px, true);
                mn = mn.max(v);
                mx = mx.max(v);
            }
        }
        if let Some(m) = s.max_width {
            if !m.has_pct() {
                let v = self.content_from_specified(b, m.px, true);
                mn = mn.min(v);
                mx = mx.min(v);
            }
        }
        (mn + edges, mx.max(mn) + edges)
    }

    // ------------------------------------------------------------ entry
    pub fn run(&mut self) {
        let root = self.root;
        self.boxes[root].x = 0.0;
        self.boxes[root].y = 0.0;
        self.boxes[root].w = self.vw;
        self.boxes[root].h = self.vh;
        let kids = self.boxes[root].children.clone();
        let mut bfc = block::Bfc::new();
        let mut cursor = block::Cursor { y: 0.0, strut: block::Strut::default() };
        for k in kids {
            block::layout_block_level(self, k, 0.0, self.vw, Some(self.vh), &mut cursor, &mut bfc, root);
        }
        let abs = std::mem::take(&mut self.boxes[root].abs_children);
        for a in abs {
            block::layout_absolute(self, a, root);
        }
    }

    /// the containing block of absolutely positioned descendants: the nearest positioned ancestor
    pub fn abs_container(&self, b: BoxId) -> BoxId {
        let fixed = self.boxes[b].style.position == Position::Fixed;
        let mut p = self.boxes[b].parent;
        while let Some(x) = p {
            if x == self.root {
                return x;
            }
            if !fixed && self.boxes[x].style.is_positioned() && self.boxes[x].node.is_some() {
                return x;
            }
            p = self.boxes[x].parent;
        }
        self.root
    }
}

pub fn marker_text(ls: ListStyle, n: i32) -> String {
    fn alpha(mut n: i32, upper: bool) -> String {
        let mut s = vec![];
        while n > 0 {
            n -= 1;
            s.push((if upper { b'A' } else { b'a' } + (n % 26) as u8) as char);
            n /= 26;
        }
        s.iter().rev().collect()
    }
    fn roman(mut n: i32, upper: bool) -> String {
        let t = [(1000, "m"), (900, "cm"), (500, "d"), (400, "cd"), (100, "c"), (90, "xc"), (50, "l"), (40, "xl"), (10, "x"), (9, "ix"), (5, "v"), (4, "iv"), (1, "i")];
        let mut s = String::new();
        for (v, r) in t {
            while n >= v {
                s.push_str(r);
                n -= v;
            }
        }
        if upper {
            s.to_uppercase()
        } else {
            s
        }
    }
    match ls {
        ListStyle::None => String::new(),
        ListStyle::Disc => "\u{2022} ".into(),
        ListStyle::Circle => "\u{25E6} ".into(),
        ListStyle::Square => "\u{25AA} ".into(),
        ListStyle::Decimal => format!("{n}. "),
        ListStyle::LowerAlpha => format!("{}. ", alpha(n, false)),
        ListStyle::UpperAlpha => format!("{}. ", alpha(n, true)),
        ListStyle::LowerRoman => format!("{}. ", roman(n, false)),
        ListStyle::UpperRoman => format!("{}. ", roman(n, true)),
    }
}

/// the first baseline of a box, relative to its margin-box top (None when it has no lines)
pub fn inline_baseline(l: &Layout, b: BoxId) -> Option<f32> {
    fn first(l: &Layout, b: BoxId) -> Option<f32> {
        let bx = &l.boxes[b];
        if let Some(x) = bx.first_baseline {
            return Some(x);
        }
        if matches!(bx.kind, Kind::Replaced { .. }) {
            return None;
        }
        for &c in &bx.children {
            if l.boxes[c].style.is_out_of_flow() {
                continue;
            }
            if let Some(x) = first(l, c) {
                return Some(x);
            }
        }
        None
    }
    let (_, my, _, _) = l.boxes[b].margin_box();
    first(l, b).map(|x| x - my)
}
