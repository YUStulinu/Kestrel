//! Block formatting: widths (CSS 2 §10.3), margin collapsing, floats and
//! clearance, block formatting context roots, absolute positioning.
use super::*;

#[derive(Clone, Copy, Debug, Default)]
pub struct Strut {
    pos: f32,
    neg: f32,
}
impl Strut {
    pub fn add(&mut self, m: f32) {
        if m > 0.0 {
            self.pos = self.pos.max(m);
        } else {
            self.neg = self.neg.min(m);
        }
    }
    pub fn with(mut self, m: f32) -> Strut {
        self.add(m);
        self
    }
    pub fn join(mut self, o: Strut) -> Strut {
        self.pos = self.pos.max(o.pos);
        self.neg = self.neg.min(o.neg);
        self
    }
    pub fn resolve(&self) -> f32 {
        self.pos + self.neg
    }
}

/// the flow position inside a block container: the bottom of the last in-flow
/// content and the margins waiting to collapse
#[derive(Clone, Copy, Debug)]
pub struct Cursor {
    pub y: f32,
    pub strut: Strut,
}
impl Cursor {
    /// the position where content placed now would start (consumes the margins)
    pub fn resolve(&mut self) -> f32 {
        self.y += self.strut.resolve();
        self.strut = Strut::default();
        self.y
    }
    pub fn peek(&self) -> f32 {
        self.y + self.strut.resolve()
    }
}

#[derive(Clone, Debug)]
pub struct FloatRect {
    pub x: f32,
    pub y: f32,
    pub w: f32,
    pub h: f32,
    pub left: bool,
}

#[derive(Clone, Debug, Default)]
pub struct Bfc {
    pub floats: Vec<FloatRect>,
}

impl Bfc {
    pub fn new() -> Self {
        Bfc { floats: vec![] }
    }
    /// the free horizontal span [l, r) within [x0, x1) for the band [y, y+h)
    pub fn span(&self, y: f32, h: f32, x0: f32, x1: f32) -> (f32, f32) {
        let mut l = x0;
        let mut r = x1;
        let h = h.max(0.01);
        for f in &self.floats {
            if f.y < y + h && f.y + f.h > y && f.h > 0.0 {
                if f.left {
                    l = l.max(f.x + f.w);
                } else {
                    r = r.min(f.x);
                }
            }
        }
        (l, r)
    }
    /// the lowest float bottom below y that changes the span (the next place to try)
    pub fn next_edge(&self, y: f32) -> Option<f32> {
        self.floats.iter().map(|f| f.y + f.h).filter(|&b| b > y + 0.001).fold(None, |m, b| Some(m.map_or(b, |m: f32| m.min(b))))
    }
    pub fn clear_y(&self, c: Clear) -> f32 {
        let mut y = f32::MIN;
        for f in &self.floats {
            let hit = match c {
                Clear::Left => f.left,
                Clear::Right => !f.left,
                Clear::Both => true,
                Clear::None => false,
            };
            if hit {
                y = y.max(f.y + f.h);
            }
        }
        y
    }
    pub fn bottom(&self) -> f32 {
        self.floats.iter().map(|f| f.y + f.h).fold(f32::MIN, f32::max)
    }
}

pub fn is_bfc_root(l: &Layout, b: BoxId) -> bool {
    let bx = &l.boxes[b];
    let s = &bx.style;
    b == l.root
        || bx.parent == Some(l.root)
        || s.float != Float::None
        || matches!(s.position, Position::Absolute | Position::Fixed)
        || s.overflow != Overflow::Visible
        || matches!(s.display, Display::InlineBlock | Display::FlowRoot | Display::TableCell | Display::TableCaption | Display::Flex | Display::InlineFlex | Display::Table | Display::InlineTable)
        || matches!(bx.kind, Kind::Flex | Kind::Table | Kind::TableCell | Kind::TableCaption | Kind::Replaced { .. })
        || l.boxes[b].parent.is_some_and(|p| matches!(l.boxes[p].kind, Kind::Flex))
}

/// the used width of an in-flow block box: (x of content box, content width, margins left/right)
pub fn block_width(l: &mut Layout, b: BoxId, cb_x: f32, cb_w: f32) {
    l.resolve_edges(b, cb_w);
    let s = l.boxes[b].style.clone();
    let bx = &l.boxes[b];
    let edges = bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3];
    let ml_auto = s.margin[3].is_none();
    let mr_auto = s.margin[1].is_none();
    let (mut ml, mut mr) = (bx.m[3], bx.m[1]);
    let w = match s.width {
        None => {
            if ml_auto {
                ml = 0.0;
            }
            if mr_auto {
                mr = 0.0;
            }
            if matches!(bx.kind, Kind::Table) {
                // tables shrink to fit
                let (mn, mx) = l.intrinsic(b);
                let avail = cb_w - ml - mr;
                (mx.min(avail).max(mn) - edges).max(0.0)
            } else {
                (cb_w - ml - mr - edges).max(0.0)
            }
        }
        Some(w) => l.content_from_specified(b, w.resolve(cb_w), true),
    };
    let w = l.clamp_width(b, w, cb_w);
    if s.width.is_some() || s.min_width.is_some() || s.max_width.is_some() || matches!(l.boxes[b].kind, Kind::Table) {
        let rest = cb_w - w - edges;
        if ml_auto && mr_auto {
            ml = ((rest) / 2.0).max(0.0);
            mr = rest - ml;
        } else if ml_auto {
            ml = rest - mr;
        } else if mr_auto {
            mr = rest - ml;
        } else {
            // over-constrained: the right margin gives way (ltr)
            mr = rest - ml;
        }
    }
    let bx = &mut l.boxes[b];
    bx.m[3] = ml;
    bx.m[1] = mr;
    bx.w = w;
    bx.x = cb_x + ml + bx.b[3] + bx.p[3];
}

fn collapses_top(l: &Layout, b: BoxId) -> bool {
    let bx = &l.boxes[b];
    !is_bfc_root(l, b) && bx.b[0] == 0.0 && bx.p[0] == 0.0 && matches!(bx.kind, Kind::Block)
}
fn collapses_bottom(l: &Layout, b: BoxId, cbh: Option<f32>) -> bool {
    let bx = &l.boxes[b];
    !is_bfc_root(l, b)
        && bx.b[2] == 0.0
        && bx.p[2] == 0.0
        && matches!(bx.kind, Kind::Block)
        && l.specified_height(b, cbh).is_none()
        && bx.style.min_height.map_or(true, |m| m.resolve(cbh.unwrap_or(0.0)) <= 0.0)
}

/// lays out one box in a block formatting context at the cursor
pub fn layout_block_level(l: &mut Layout, b: BoxId, cb_x: f32, cb_w: f32, cb_h: Option<f32>, cursor: &mut Cursor, bfc: &mut Bfc, parent: BoxId) {
    let s = l.boxes[b].style.clone();
    if matches!(s.position, Position::Absolute | Position::Fixed) {
        l.boxes[b].static_pos = (cb_x, cursor.peek());
        let c = l.abs_container(b);
        l.boxes[c].abs_children.push(b);
        return;
    }
    if s.float != Float::None {
        place_float(l, b, cb_x, cb_w, cb_h, cursor.peek(), bfc);
        return;
    }
    let _ = parent;
    block_width(l, b, cb_x, cb_w);
    let bfc_root = is_bfc_root(l, b);
    // floats beside a BFC root: it is narrowed or moved down to fit
    let mt = l.boxes[b].m[0];
    let mut top_strut = cursor.strut.with(mt);
    let mut clearance_y = None;
    if s.clear != Clear::None {
        let cy = bfc.clear_y(s.clear);
        let hyp = cursor.y + top_strut.resolve();
        if cy > hyp {
            clearance_y = Some(cy);
        }
    }
    let collapse_top = collapses_top(l, b) && clearance_y.is_none();
    let cbh_for_children = l.specified_height(b, cb_h);
    let mut inner;
    let border_top;
    if collapse_top {
        // the margins keep collapsing into the first child; our top is where they resolve
        inner = Cursor { y: cursor.y, strut: top_strut };
        border_top = None;
    } else {
        let y = match clearance_y {
            Some(cy) => cy,
            None => cursor.y + top_strut.resolve(),
        };
        top_strut = Strut::default();
        let _ = top_strut;
        inner = Cursor { y: y + l.boxes[b].b[0] + l.boxes[b].p[0], strut: Strut::default() };
        border_top = Some(y);
    }
    if bfc_root && !bfc.floats.is_empty() && !matches!(l.boxes[b].kind, Kind::Table) {
        // avoid the floats: shrink into the free span at this position
        let y = border_top.unwrap_or_else(|| inner.peek());
        let bx = &l.boxes[b];
        let outer_w = bx.w + bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3];
        let (fl, fr) = bfc.span(y, 1.0, cb_x, cb_x + cb_w);
        if fl > cb_x || fr < cb_x + cb_w {
            let avail = fr - fl;
            if s.width.is_none() {
                let edges = bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3];
                let ml = bx.m[3].max(0.0);
                let mr = bx.m[1].max(0.0);
                let w = (avail - ml - mr - edges).max(0.0);
                let w = l.clamp_width(b, w, cb_w);
                let bx = &mut l.boxes[b];
                bx.w = w;
                bx.x = fl + ml + bx.b[3] + bx.p[3];
            } else if outer_w <= avail {
                let bx = &mut l.boxes[b];
                let ml = bx.m[3].max(0.0);
                bx.x = bx.x.max(fl + ml + bx.b[3] + bx.p[3]);
            }
        }
    }
    let content_x = l.boxes[b].x;
    let content_w = l.boxes[b].w;
    let mut own_bfc_storage = Bfc::new();
    let first_resolved;
    let kind = l.boxes[b].kind.clone();
    {
        let bfc_ref: &mut Bfc = if bfc_root { &mut own_bfc_storage } else { bfc };
        let before = inner;
        match kind {
            Kind::Flex => {
                let y = inner.resolve();
                l.boxes[b].y = y;
                flex::layout(l, b, cbh_for_children);
                inner.y = y + l.boxes[b].h;
                first_resolved = Some(y);
            }
            Kind::Table => {
                let y = inner.resolve();
                l.boxes[b].y = y;
                table::layout(l, b, cbh_for_children);
                inner.y = y + l.boxes[b].h;
                first_resolved = Some(y);
            }
            Kind::Replaced { .. } => {
                let y = inner.resolve();
                l.boxes[b].y = y;
                replaced_size(l, b, cb_w, cb_h, Some(content_w));
                inner.y = y + l.boxes[b].h;
                first_resolved = Some(y);
            }
            _ => {
                first_resolved = layout_children(l, b, content_x, content_w, cbh_for_children, &mut inner, bfc_ref);
            }
        }
        let _ = before;
    }
    // the border-box top
    let top = match border_top {
        Some(t) => t,
        None => first_resolved.unwrap_or_else(|| cursor.y + cursor.strut.with(mt).resolve()),
    };
    let content_top = top + l.boxes[b].b[0] + l.boxes[b].p[0];
    l.boxes[b].y = content_top;
    let collapse_bottom = collapses_bottom(l, b, cb_h) && !bfc_root;
    let mb = l.boxes[b].m[2];
    let empty = first_resolved.is_none() && collapse_top && collapse_bottom && l.boxes[b].marker.is_none();
    let mut content_bottom;
    let out_strut;
    if empty {
        content_bottom = content_top;
        out_strut = inner.strut.with(mb);
    } else if collapse_bottom {
        content_bottom = inner.y;
        out_strut = inner.strut.with(mb);
    } else {
        content_bottom = if first_resolved.is_some() || inner.strut.resolve() != 0.0 { inner.y + inner.strut.resolve() } else { inner.y };
        out_strut = Strut::default().with(mb);
    }
    if bfc_root {
        // a BFC root contains its floats
        let fb = own_bfc_storage.bottom();
        if fb > content_bottom {
            content_bottom = fb;
        }
    }
    let auto_h = (content_bottom - content_top).max(0.0);
    let h = match l.specified_height(b, cb_h) {
        Some(h) => h,
        None => auto_h,
    };
    let h = if matches!(kind, Kind::Flex | Kind::Table | Kind::Replaced { .. }) { l.boxes[b].h.max(if l.specified_height(b, cb_h).is_some() { h } else { 0.0 }) } else { h };
    let h = l.clamp_height(b, h, cb_h);
    l.boxes[b].h = h;
    l.boxes[b].laid_out = true;
    // own absolutely positioned descendants
    if s.is_positioned() {
        let abs = std::mem::take(&mut l.boxes[b].abs_children);
        for a in abs {
            layout_absolute(l, a, b);
        }
    }
    if empty {
        cursor.strut = cursor.strut.with(mt).join(out_strut);
        if clearance_y.is_some() {
            cursor.y = top;
        }
    } else {
        cursor.y = content_top + h + l.boxes[b].p[2] + l.boxes[b].b[2];
        cursor.strut = out_strut;
    }
}

/// lays out the children of a block container; returns where the first content resolved
pub fn layout_children(l: &mut Layout, b: BoxId, cx: f32, cw: f32, cbh: Option<f32>, cur: &mut Cursor, bfc: &mut Bfc) -> Option<f32> {
    let kids = l.boxes[b].children.clone();
    let inline_ctx = !kids.is_empty() && kids.iter().all(|&k| !l.is_block_level_box(k));
    if inline_ctx {
        return inline::layout_lines(l, b, cx, cw, cbh, cur, bfc);
    }
    let mut first = None;
    for k in kids {
        let before_y = cur.y;
        let before_strut = cur.strut.resolve();
        layout_block_level(l, k, cx, cw, cbh, cur, bfc, b);
        if first.is_none() {
            let bx = &l.boxes[k];
            if !bx.style.is_out_of_flow() && bx.laid_out {
                // did this child produce a position (not collapsed through)?
                if cur.y != before_y || (cur.strut.resolve() != before_strut && false) {
                    let (_, by, _, _) = bx.border_box();
                    first = Some(by);
                }
            }
        }
    }
    first
}

/// sizes a replaced element (img etc.): (content width, height)
pub fn replaced_size(l: &mut Layout, b: BoxId, cb_w: f32, cb_h: Option<f32>, fill_w: Option<f32>) {
    let (iw, ih) = match l.boxes[b].kind {
        Kind::Replaced { iw, ih, .. } => (iw, ih),
        _ => (0.0, 0.0),
    };
    let s = l.boxes[b].style.clone();
    let w_spec = s.width.map(|w| l.content_from_specified(b, w.resolve(cb_w), true));
    let h_spec = l.specified_height(b, cb_h);
    let ratio = if iw > 0.0 && ih > 0.0 { Some(iw / ih) } else { None };
    let (w, h) = match (w_spec, h_spec) {
        (Some(w), Some(h)) => (w, h),
        (Some(w), None) => (w, ratio.map(|r| w / r).unwrap_or(ih)),
        (None, Some(h)) => (ratio.map(|r| h * r).unwrap_or(iw), h),
        (None, None) => (iw, ih),
    };
    let _ = fill_w;
    let w = l.clamp_width(b, w, cb_w);
    let h = l.clamp_height(b, h, cb_h);
    l.boxes[b].w = w;
    l.boxes[b].h = h;
    l.boxes[b].laid_out = true;
}

/// shrink-to-fit content width for floats, inline-blocks and absolutely positioned boxes
pub fn shrink_to_fit(l: &mut Layout, b: BoxId, avail: f32) -> f32 {
    let (mn, mx) = l.intrinsic(b);
    let bx = &l.boxes[b];
    let edges = bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3];
    let w = mx.min(avail.max(mn));
    (w - edges).max(0.0)
}

/// lays out a box as the root of its own formatting context at (border-box x, y)
pub fn layout_root_at(l: &mut Layout, b: BoxId, x: f32, y: f32, content_w: f32, cb_h: Option<f32>) {
    let bx = &mut l.boxes[b];
    bx.w = content_w;
    bx.x = x + bx.b[3] + bx.p[3];
    bx.y = y + bx.b[0] + bx.p[0];
    let content_x = bx.x;
    let content_y = bx.y;
    let kind = bx.kind.clone();
    let own_h = l.specified_height(b, cb_h);
    match kind {
        Kind::Flex => {
            flex::layout(l, b, own_h);
        }
        Kind::Table => {
            table::layout(l, b, own_h);
        }
        Kind::Replaced { .. } => {
            replaced_size(l, b, content_w, cb_h, None);
            l.boxes[b].w = content_w.max(l.boxes[b].w).min(l.boxes[b].w.max(content_w));
        }
        _ => {
            let mut bfc = Bfc::new();
            let mut cur = Cursor { y: content_y, strut: Strut::default() };
            let first = layout_children(l, b, content_x, content_w, own_h, &mut cur, &mut bfc);
            let mut bottom = if first.is_some() { cur.y + cur.strut.resolve() } else { content_y };
            bottom = bottom.max(bfc.bottom());
            let auto_h = (bottom - content_y).max(0.0);
            let h = own_h.unwrap_or(auto_h);
            l.boxes[b].h = l.clamp_height(b, h, cb_h);
        }
    }
    if let Some(h) = own_h {
        if !matches!(kind, Kind::Replaced { .. }) {
            l.boxes[b].h = l.clamp_height(b, h, cb_h);
        }
    }
    l.boxes[b].laid_out = true;
    if l.boxes[b].style.is_positioned() {
        let abs = std::mem::take(&mut l.boxes[b].abs_children);
        for a in abs {
            layout_absolute(l, a, b);
        }
    }
}

pub fn place_float(l: &mut Layout, b: BoxId, cb_x: f32, cb_w: f32, cb_h: Option<f32>, y: f32, bfc: &mut Bfc) {
    l.resolve_edges(b, cb_w);
    let s = l.boxes[b].style.clone();
    let content_w = match (s.width, &l.boxes[b].kind) {
        (Some(w), _) => {
            let cw = l.content_from_specified(b, w.resolve(cb_w), true);
            l.clamp_width(b, cw, cb_w)
        }
        (None, Kind::Replaced { .. }) => {
            replaced_size(l, b, cb_w, cb_h, None);
            l.boxes[b].w
        }
        _ => {
            let avail = cb_w - l.boxes[b].m[1] - l.boxes[b].m[3];
            let w = shrink_to_fit(l, b, avail);
            l.clamp_width(b, w, cb_w)
        }
    };
    // lay out at a provisional position to learn the height
    layout_root_at(l, b, 0.0, 0.0, content_w, cb_h);
    let bx = &l.boxes[b];
    let ow = content_w + l.hsum(b);
    let oh = bx.h + l.vsum(b);
    let left = s.float == Float::Left;
    // a float may not be higher than earlier floats
    let mut ty = y;
    for f in &bfc.floats {
        ty = ty.max(f.y);
    }
    if s.clear != Clear::None {
        ty = ty.max(bfc.clear_y(s.clear));
    }
    loop {
        let (fl, fr) = bfc.span(ty, oh, cb_x, cb_x + cb_w);
        if fr - fl >= ow - 0.01 || (fl <= cb_x && fr >= cb_x + cb_w) {
            let x = if left { fl } else { fr - ow };
            let bx = &l.boxes[b];
            let (dx, dy) = (x + bx.m[3] - (bx.x - bx.p[3] - bx.b[3]), ty + bx.m[0] - (bx.y - bx.p[0] - bx.b[0]));
            shift_subtree(l, b, dx, dy);
            bfc.floats.push(FloatRect { x, y: ty, w: ow, h: oh, left });
            return;
        }
        match bfc.next_edge(ty) {
            Some(n) => ty = n,
            None => {
                let x = if left { cb_x } else { cb_x + cb_w - ow };
                let bx = &l.boxes[b];
                let (dx, dy) = (x + bx.m[3] - (bx.x - bx.p[3] - bx.b[3]), ty + bx.m[0] - (bx.y - bx.p[0] - bx.b[0]));
                shift_subtree(l, b, dx, dy);
                bfc.floats.push(FloatRect { x, y: ty, w: ow, h: oh, left });
                return;
            }
        }
    }
}

/// moves a laid-out subtree (boxes, lines and runs)
pub fn shift_subtree(l: &mut Layout, b: BoxId, dx: f32, dy: f32) {
    if dx == 0.0 && dy == 0.0 {
        return;
    }
    let mut stack = vec![b];
    while let Some(x) = stack.pop() {
        let bx = &mut l.boxes[x];
        bx.x += dx;
        bx.y += dy;
        if let Some(bl) = bx.baseline.as_mut() {
            *bl += dy;
        }
        if let Some(bl) = bx.first_baseline.as_mut() {
            *bl += dy;
        }
        bx.static_pos.0 += dx;
        bx.static_pos.1 += dy;
        for line in &mut bx.lines {
            line.top += dy;
            line.baseline += dy;
            for r in &mut line.runs {
                r.x += dx;
                r.baseline += dy;
            }
            for f in &mut line.frags {
                f.x0 += dx;
                f.x1 += dx;
                f.top += dy;
                f.bottom += dy;
            }
        }
        stack.extend(bx.children.iter().copied());
    }
}

/// CSS 2 §10.3.7 / §10.6.4 (simplified): absolutely positioned boxes
pub fn layout_absolute(l: &mut Layout, b: BoxId, cb: BoxId) {
    let fixed = l.boxes[b].style.position == Position::Fixed;
    let (cx, cy, cw, ch) = if cb == l.root || fixed {
        (0.0, 0.0, l.vw, l.vh)
    } else {
        l.boxes[cb].padding_box()
    };
    l.resolve_edges(b, cw);
    let s = l.boxes[b].style.clone();
    let [top, right, bottom, left] = s.inset;
    let (sx, sy) = l.boxes[b].static_pos;
    let hedges = { let x = &l.boxes[b]; x.b[1] + x.b[3] + x.p[1] + x.p[3] };
    let ml = l.boxes[b].m[3];
    let mr = l.boxes[b].m[1];
    let replaced = matches!(l.boxes[b].kind, Kind::Replaced { .. });
    // width
    let width = if replaced {
        replaced_size(l, b, cw, Some(ch), None);
        Some(l.boxes[b].w)
    } else {
        s.width.map(|w| l.clamp_width(b, l.content_from_specified(b, w.resolve(cw), true), cw))
    };
    let (x, w) = match (left, width, right) {
        (Some(lf), Some(w), _) => (cx + lf.resolve(cw) + ml, w),
        (Some(lf), None, Some(rt)) => {
            let lv = lf.resolve(cw);
            let w = (cw - lv - rt.resolve(cw) - ml - mr - hedges).max(0.0);
            (cx + lv + ml, l.clamp_width(b, w, cw))
        }
        (Some(lf), None, None) => {
            let lv = lf.resolve(cw);
            let w = shrink_to_fit(l, b, cw - lv - ml - mr);
            (cx + lv + ml, l.clamp_width(b, w, cw))
        }
        (None, Some(w), Some(rt)) => (cx + cw - rt.resolve(cw) - mr - w - hedges, w),
        (None, None, Some(rt)) => {
            let rv = rt.resolve(cw);
            let w = shrink_to_fit(l, b, cw - rv - ml - mr);
            let w = l.clamp_width(b, w, cw);
            (cx + cw - rv - mr - w - hedges, w)
        }
        (None, Some(w), None) => (sx + ml, w),
        (None, None, None) => {
            let w = shrink_to_fit(l, b, (cx + cw - sx) - ml - mr);
            (sx + ml, l.clamp_width(b, w, cw))
        }
    };
    // center with auto margins when both insets and the width are set
    let mut x = x;
    if let (Some(lf), Some(_), Some(rt)) = (left, width, right) {
        if s.margin[3].is_none() || s.margin[1].is_none() {
            let free = cw - lf.resolve(cw) - rt.resolve(cw) - w - hedges;
            let (a, _) = match (s.margin[3].is_none(), s.margin[1].is_none()) {
                (true, true) => (free.max(0.0) / 2.0, 0.0),
                (true, false) => (free - mr, 0.0),
                _ => (ml, 0.0),
            };
            x = cx + lf.resolve(cw) + a;
            l.boxes[b].m[3] = a;
        }
    }
    let vedges = { let bx = &l.boxes[b]; bx.b[0] + bx.b[2] + bx.p[0] + bx.p[2] };
    let mt = l.boxes[b].m[0];
    let mb = l.boxes[b].m[2];
    let spec_h = if replaced { Some(l.boxes[b].h) } else { l.specified_height(b, Some(ch)) };
    let fill_h = match (top, bottom, spec_h) {
        (Some(t), Some(bt), None) => Some((ch - t.resolve(ch) - bt.resolve(ch) - mt - mb - vedges).max(0.0)),
        _ => None,
    };
    // lay out at the provisional top, then move vertically
    let ty0 = match top {
        Some(t) => cy + t.resolve(ch) + mt,
        None => sy + mt,
    };
    {
        let saved = l.boxes[b].style.clone();
        if let Some(fh) = fill_h {
            let mut st = (*saved).clone();
            st.height = Some(Len::px(if st.box_sizing == BoxSizing::BorderBox { fh + vedges } else { fh }));
            l.boxes[b].style = Rc::new(st);
        }
        layout_root_at(l, b, x, ty0, w, Some(ch));
        l.boxes[b].style = saved;
    }
    let h = l.boxes[b].h;
    if top.is_none() {
        if let Some(bt) = bottom {
            let want = cy + ch - bt.resolve(ch) - mb - h - vedges;
            let (_, cur_y, _, _) = l.boxes[b].border_box();
            shift_subtree(l, b, 0.0, want - cur_y);
        }
    }
}
