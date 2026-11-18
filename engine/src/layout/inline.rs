//! Inline formatting: white space, line breaking around floats, vertical
//! alignment within line boxes, text measurement.
use super::block::{self, Bfc, Cursor};
use super::*;

#[derive(Clone, Debug)]
enum Item {
    /// a piece of text (no break opportunities inside), its box, trailing space flag
    Word { boxid: BoxId, text: String, space_after: bool, forced_break_after: bool },
    Atomic(BoxId),
    Open(BoxId),
    Close(BoxId),
    Break(BoxId),
    Float(BoxId),
    Abs(BoxId),
}

fn transform(s: &Style, t: &str) -> String {
    match s.text_transform {
        1 => t.to_uppercase(),
        2 => t.to_lowercase(),
        3 => {
            let mut out = String::new();
            let mut start = true;
            for c in t.chars() {
                if start && c.is_alphabetic() {
                    out.extend(c.to_uppercase());
                    start = false;
                } else {
                    out.push(c);
                    if c.is_whitespace() {
                        start = true;
                    } else if c.is_alphabetic() {
                        start = false;
                    }
                }
            }
            out
        }
        _ => t.to_string(),
    }
}

/// flattens the inline content of a block into items, collapsing white space
fn collect(l: &Layout, b: BoxId, out: &mut Vec<Item>, last_space: &mut bool) {
    for &k in &l.boxes[b].children {
        let bx = &l.boxes[k];
        let s = &bx.style;
        if matches!(s.position, Position::Absolute | Position::Fixed) && !matches!(bx.kind, Kind::Text(_)) {
            out.push(Item::Abs(k));
            continue;
        }
        if s.float != Float::None && !matches!(bx.kind, Kind::Text(_)) {
            out.push(Item::Float(k));
            continue;
        }
        match &bx.kind {
            Kind::Text(t) => {
                let t = transform(s, t);
                let ws = s.white_space;
                let collapse = matches!(ws, WhiteSpace::Normal | WhiteSpace::NoWrap | WhiteSpace::PreLine);
                let keep_newlines = matches!(ws, WhiteSpace::Pre | WhiteSpace::PreWrap | WhiteSpace::PreLine);
                let wrap = !matches!(ws, WhiteSpace::Pre | WhiteSpace::NoWrap);
                // split into words at spaces / newlines
                let mut word = String::new();
                let chars: Vec<char> = t.chars().collect();
                let mut i = 0;
                while i < chars.len() {
                    let c = chars[i];
                    if c == '\n' && keep_newlines {
                        if collapse {
                            // pre-line: spaces before a newline are removed
                            while word.ends_with(' ') {
                                word.pop();
                            }
                        }
                        out.push(Item::Word { boxid: k, text: std::mem::take(&mut word), space_after: false, forced_break_after: true });
                        *last_space = true;
                        i += 1;
                        if collapse {
                            while i < chars.len() && (chars[i] == ' ' || chars[i] == '\t') {
                                i += 1;
                            }
                        }
                        continue;
                    }
                    let is_space = c == ' ' || c == '\t' || (c == '\n' && !keep_newlines) || c == '\r' || c == '\x0C';
                    if is_space && collapse {
                        // a run of collapsible white space becomes one space (or none after a space)
                        while i < chars.len() && (chars[i] == ' ' || chars[i] == '\t' || chars[i] == '\r' || chars[i] == '\x0C' || (chars[i] == '\n' && !keep_newlines)) {
                            i += 1;
                        }
                        if !*last_space {
                            if wrap {
                                out.push(Item::Word { boxid: k, text: std::mem::take(&mut word), space_after: true, forced_break_after: false });
                            } else {
                                word.push(' ');
                            }
                            *last_space = true;
                        }
                        continue;
                    }
                    if is_space && wrap {
                        // preserved spaces (pre-wrap): each is a break opportunity after it
                        word.push(if c == '\t' { '\t' } else { ' ' });
                        out.push(Item::Word { boxid: k, text: std::mem::take(&mut word), space_after: false, forced_break_after: false });
                        *last_space = true;
                        i += 1;
                        continue;
                    }
                    word.push(if c == '\t' { '\t' } else { c });
                    *last_space = false;
                    i += 1;
                }
                if !word.is_empty() {
                    out.push(Item::Word { boxid: k, text: word, space_after: false, forced_break_after: false });
                }
            }
            Kind::Br => {
                out.push(Item::Break(k));
                *last_space = true;
            }
            Kind::Inline => {
                out.push(Item::Open(k));
                collect(l, k, out, last_space);
                out.push(Item::Close(k));
            }
            _ => {
                out.push(Item::Atomic(k));
                *last_space = false;
            }
        }
    }
}

/// measures a string in a style: (width, glyphs (id, x, font))
pub fn measure(l: &Layout, s: &Style, text: &str) -> (f32, Vec<(u16, f32, usize)>) {
    let fi = l.font_of(s);
    let font = l.fonts.font(fi);
    let k = s.font_size / font.units_per_em;
    let mut x = 0.0;
    let mut glyphs = Vec::with_capacity(text.len());
    let mut prev: Option<(usize, u16)> = None;
    for c in text.chars() {
        let (f, fidx) = if font.has_glyph(c) || c == ' ' {
            (font, fi)
        } else {
            // fall back to DejaVu Sans
            let fb = l.fonts.pick(&["sans-serif".to_string()], s.font_weight, s.italic);
            (l.fonts.font(fb), fb)
        };
        let kk = s.font_size / f.units_per_em;
        if c == '\t' {
            // tab stops every 8 spaces
            let sp = f.advance(f.glyph_index(' ')) * kk;
            let tab = sp * 8.0;
            x = ((x / tab).floor() + 1.0) * tab;
            prev = None;
            continue;
        }
        let g = f.glyph_index(c);
        if let Some((pf, pg)) = prev {
            if pf == fidx {
                x += f.kerning(pg, g) * kk;
            }
        }
        glyphs.push((g, x, fidx));
        x += f.advance(g) * kk + s.letter_spacing;
        if c == ' ' {
            x += s.word_spacing;
        }
        prev = Some((fidx, g));
    }
    let _ = k;
    (x, glyphs)
}

/// (min-content, max-content) widths of inline content
pub fn intrinsic(l: &mut Layout, b: BoxId) -> (f32, f32) {
    let mut items = vec![];
    let mut ls = true;
    collect(l, b, &mut items, &mut ls);
    let mut mn: f32 = 0.0;
    let mut mx: f32 = 0.0;
    let mut line: f32 = 0.0;
    let mut word: f32 = 0.0;
    let indent = l.boxes[b].style.text_indent.px;
    line += indent;
    for it in items {
        match it {
            Item::Word { boxid, text, space_after, forced_break_after } => {
                let s = l.boxes[boxid].style.clone();
                let (w, _) = measure(l, &s, &text);
                let (sw, _) = if space_after { measure(l, &s, " ") } else { (0.0, vec![]) };
                word += w;
                line += w + sw;
                let nowrap = matches!(s.white_space, WhiteSpace::NoWrap | WhiteSpace::Pre);
                if (space_after || !text.ends_with(' ')) && !nowrap {
                    mn = mn.max(word);
                    word = 0.0;
                } else if text.ends_with(' ') && !nowrap {
                    mn = mn.max(word);
                    word = 0.0;
                }
                if forced_break_after {
                    mn = mn.max(word);
                    word = 0.0;
                    mx = mx.max(line);
                    line = 0.0;
                }
                if space_after {
                    word = 0.0;
                }
            }
            Item::Atomic(k) => {
                let (a, c) = l.intrinsic(k);
                let s = l.boxes[k].style.clone();
                let m = s.margin[1].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0) + s.margin[3].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0);
                mn = mn.max(word);
                word = 0.0;
                mn = mn.max(a + m);
                line += c + m;
            }
            Item::Float(k) => {
                let (a, c) = l.intrinsic(k);
                let s = l.boxes[k].style.clone();
                let m = s.margin[1].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0) + s.margin[3].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0);
                mn = mn.max(a + m);
                line += c + m;
            }
            Item::Open(k) | Item::Close(k) => {
                let s = l.boxes[k].style.clone();
                let side = if matches!(it, Item::Open(_)) { 3 } else { 1 };
                let e = s.margin[side].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0) + s.border(side) + if s.padding[side].has_pct() { 0.0 } else { s.padding[side].px };
                line += e;
                word += e;
            }
            Item::Break(_) => {
                mn = mn.max(word);
                word = 0.0;
                mx = mx.max(line);
                line = 0.0;
            }
            Item::Abs(_) => {}
        }
    }
    mn = mn.max(word);
    mx = mx.max(line);
    (mn.ceil(), mx.ceil())
}

/// an entry placed on a line
#[derive(Clone, Debug)]
enum Placed {
    Text { boxid: BoxId, text: String, w: f32, space_w: f32 },
    Atomic { boxid: BoxId, w: f32 },
    Open { boxid: BoxId, w: f32 },
    Close { boxid: BoxId, w: f32 },
}

fn inline_edge(l: &Layout, k: BoxId, start: bool, cw: f32) -> f32 {
    let s = &l.boxes[k].style;
    let side = if start { 3 } else { 1 };
    s.margin[side].map(|v| v.resolve(cw)).unwrap_or(0.0) + s.border(side) + s.padding[side].resolve(cw)
}

/// lays out the lines of a block container; returns the y of the first line
pub fn layout_lines(l: &mut Layout, b: BoxId, cx: f32, cw: f32, cbh: Option<f32>, cur: &mut Cursor, bfc: &mut Bfc) -> Option<f32> {
    let mut items = vec![];
    let mut ls = true;
    collect(l, b, &mut items, &mut ls);
    let bs = l.boxes[b].style.clone();
    let strut_lh = l.line_height(&bs);
    // atomic inlines are laid out first (their sizes decide the breaks)
    for it in &items {
        if let Item::Atomic(k) = it {
            layout_atomic(l, *k, cw, cbh);
        }
    }
    let has_content = items.iter().any(|i| match i {
        Item::Word { text, forced_break_after, .. } => !text.is_empty() || *forced_break_after,
        Item::Atomic(_) | Item::Break(_) => true,
        Item::Open(k) | Item::Close(k) => {
            let s = &l.boxes[*k].style;
            s.border(1) > 0.0 || s.border(3) > 0.0 || s.padding[1].px > 0.0 || s.padding[3].px > 0.0 || s.margin[1].map_or(false, |m| m.px != 0.0) || s.margin[3].map_or(false, |m| m.px != 0.0)
        }
        _ => false,
    }) || l.boxes[b].marker.is_some();
    if !has_content {
        // only floats / positioned boxes
        for it in &items {
            match it {
                Item::Float(k) => block::place_float(l, *k, cx, cw, cbh, cur.peek(), bfc),
                Item::Abs(k) => {
                    l.boxes[*k].static_pos = (cx, cur.peek());
                    let c = l.abs_container(*k);
                    l.boxes[c].abs_children.push(*k);
                }
                _ => {}
            }
        }
        return None;
    }
    let first_y = cur.resolve();
    let mut y = first_y;
    let mut lines: Vec<Line> = vec![];
    let mut idx = 0;
    let mut first_line = true;
    let mut floats_done: std::collections::HashSet<BoxId> = std::collections::HashSet::new();
    let mut retries = 0;
    let wrap_ok = |s: &Style| !matches!(s.white_space, WhiteSpace::Pre | WhiteSpace::NoWrap);
    while idx < items.len() {
        // the line's horizontal space
        let line_h_guess = strut_lh.max(1.0);
        let (mut lx0, mut lx1) = bfc.span(y, line_h_guess, cx, cx + cw);
        let mut placed: Vec<Placed> = vec![];
        let mut width = if first_line { bs.text_indent.resolve(cw) } else { 0.0 };
        let mut forced = false;
        let mut pending_floats = vec![];
        let start_idx = idx;
        while idx < items.len() {
            let it = items[idx].clone();
            match it {
                Item::Word { boxid, text, space_after, forced_break_after } => {
                    let s = l.boxes[boxid].style.clone();
                    let t = if placed.iter().all(|p| matches!(p, Placed::Open { .. })) && wrap_ok(&s) && !matches!(s.white_space, WhiteSpace::PreWrap) {
                        text.trim_start_matches(' ').to_string()
                    } else {
                        text.clone()
                    };
                    let (w, _) = measure(l, &s, &t);
                    let sw = if space_after { measure(l, &s, " ").0 } else { 0.0 };
                    // the width of following unbreakable pieces (closing boxes, glued words)
                    let mut glued = 0.0;
                    if !space_after && !forced_break_after {
                        let mut j = idx + 1;
                        while j < items.len() {
                            match &items[j] {
                                Item::Close(k) => glued += inline_edge(l, *k, false, cw),
                                Item::Open(k) => glued += inline_edge(l, *k, true, cw),
                                Item::Word { boxid: b2, text: t2, space_after: s2, .. } => {
                                    let s2s = l.boxes[*b2].style.clone();
                                    glued += measure(l, &s2s, t2).0;
                                    if *s2 || t2.ends_with(' ') {
                                        break;
                                    }
                                }
                                _ => break,
                            }
                            j += 1;
                        }
                        if text.ends_with(' ') {
                            glued = 0.0;
                        }
                    }
                    let fits = width + w + glued <= (lx1 - lx0) + 0.01 || !wrap_ok(&s);
                    let line_empty = placed.iter().all(|p| matches!(p, Placed::Open { .. }));
                    if !fits && !line_empty {
                        break;
                    }
                    placed.push(Placed::Text { boxid, text: t, w, space_w: sw });
                    width += w + sw;
                    idx += 1;
                    if forced_break_after {
                        forced = true;
                        break;
                    }
                }
                Item::Atomic(k) => {
                    let bx = &l.boxes[k];
                    let w = bx.w + l.hsum(k);
                    let line_empty = placed.iter().all(|p| matches!(p, Placed::Open { .. }));
                    let s = l.boxes[b].style.clone();
                    if width + w > (lx1 - lx0) + 0.01 && !line_empty && wrap_ok(&s) {
                        break;
                    }
                    placed.push(Placed::Atomic { boxid: k, w });
                    width += w;
                    idx += 1;
                }
                Item::Open(k) => {
                    let w = inline_edge(l, k, true, cw);
                    placed.push(Placed::Open { boxid: k, w });
                    width += w;
                    idx += 1;
                }
                Item::Close(k) => {
                    let w = inline_edge(l, k, false, cw);
                    placed.push(Placed::Close { boxid: k, w });
                    width += w;
                    idx += 1;
                }
                Item::Break(_) => {
                    idx += 1;
                    forced = true;
                    break;
                }
                Item::Float(k) if floats_done.contains(&k) => {
                    idx += 1;
                }
                Item::Float(k) => {
                    // a float goes on this line when it fits, else below it
                    floats_done.insert(k);
                    let s = l.boxes[k].style.clone();
                    let _ = s;
                    let line_empty = placed.is_empty();
                    if line_empty {
                        block::place_float(l, k, cx, cw, cbh, y, bfc);
                        let sp = bfc.span(y, line_h_guess, cx, cx + cw);
                        lx0 = sp.0;
                        lx1 = sp.1;
                    } else {
                        // try beside the current content
                        let fl = &l.boxes[k];
                        let _ = fl;
                        let before = bfc.floats.len();
                        let mut trial = bfc.clone();
                        block::place_float(l, k, cx, cw, cbh, y, &mut trial);
                        let f = trial.floats.last().unwrap().clone();
                        if f.y <= y + 0.01 && (lx1 - lx0) - f.w >= width - 0.01 {
                            *bfc = trial;
                            let sp = bfc.span(y, line_h_guess, cx, cx + cw);
                            lx0 = sp.0;
                            lx1 = sp.1;
                        } else {
                            let _ = before;
                            pending_floats.push(k);
                        }
                    }
                    idx += 1;
                }
                Item::Abs(k) if floats_done.contains(&k) => {
                    idx += 1;
                }
                Item::Abs(k) => {
                    floats_done.insert(k);
                    l.boxes[k].static_pos = (lx0 + width, y);
                    let c = l.abs_container(k);
                    l.boxes[c].abs_children.push(k);
                    idx += 1;
                }
            }
        }
        // nothing fits next to the floats: move below them
        if placed.is_empty() && !forced && idx == start_idx {
            match bfc.next_edge(y) {
                Some(ny) => {
                    y = ny;
                    continue;
                }
                None => {
                    idx += 1;
                    continue;
                }
            }
        }
        // a line that does not fit beside floats moves down when floats end below
        if width > (lx1 - lx0) + 0.01 && (lx0 > cx || lx1 < cx + cw) && retries < 100 {
            if let Some(ny) = bfc.next_edge(y) {
                retries += 1;
                idx = start_idx;
                // undo floats placed on this attempt? they stay (placed in order)
                y = ny;
                continue;
            }
        }
        // trailing spaces hang (they do not count for alignment)
        if let Some(Placed::Text { space_w, .. }) = placed.iter_mut().rev().find(|p| matches!(p, Placed::Text { .. })) {
            width -= *space_w;
            *space_w = 0.0;
        }
        if let Some(Placed::Text { boxid, text, w, .. }) = placed.iter_mut().rev().find(|p| matches!(p, Placed::Text { .. })) {
            let s = l.boxes[*boxid].style.clone();
            if wrap_ok(&s) && !matches!(s.white_space, WhiteSpace::PreWrap) && text.ends_with(' ') {
                let t = text.trim_end_matches(' ').to_string();
                let nw = measure(l, &s, &t).0;
                width -= *w - nw;
                *w = nw;
                *text = t;
            }
        }
        let is_last = idx >= items.len() || forced;
        let line = build_line(l, b, &placed, lx0, lx1, y, width, first_line, is_last && !forced || idx >= items.len(), cw);
        y = line.top + line.height;
        lines.push(line);
        first_line = false;
        for k in pending_floats {
            block::place_float(l, k, cx, cw, cbh, y, bfc);
        }
    }
    // a list marker on an otherwise empty item still gets a line
    if lines.is_empty() {
        let line = build_line(l, b, &[], cx, cx + cw, y, 0.0, true, true, cw);
        y = line.top + line.height;
        lines.push(line);
    }
    l.boxes[b].first_baseline = lines.first().map(|ln| ln.baseline);
    l.boxes[b].baseline = lines.last().map(|ln| ln.baseline);
    l.boxes[b].lines = lines;
    cur.y = y;
    Some(first_y)
}

fn layout_atomic(l: &mut Layout, k: BoxId, cw: f32, cbh: Option<f32>) {
    l.resolve_edges(k, cw);
    let s = l.boxes[k].style.clone();
    match l.boxes[k].kind {
        Kind::Replaced { .. } => {
            block::replaced_size(l, k, cw, cbh, None);
        }
        _ => {
            let w = match s.width {
                Some(w) => {
                    let w = l.content_from_specified(k, w.resolve(cw), true);
                    l.clamp_width(k, w, cw)
                }
                None => {
                    let avail = cw - l.boxes[k].m[1] - l.boxes[k].m[3];
                    let w = block::shrink_to_fit(l, k, avail);
                    l.clamp_width(k, w, cw)
                }
            };
            block::layout_root_at(l, k, 0.0, 0.0, w, cbh);
        }
    }
}

/// the baseline of an atomic inline (relative to its margin-box top)
fn atomic_baseline(l: &Layout, k: BoxId) -> f32 {
    let bx = &l.boxes[k];
    let (_, my, _, mh) = bx.margin_box();
    let use_bottom = bx.style.overflow != Overflow::Visible || matches!(bx.kind, Kind::Replaced { .. });
    match (use_bottom, last_baseline(l, k)) {
        (false, Some(bl)) => bl - my,
        _ => mh,
    }
}
fn last_baseline(l: &Layout, k: BoxId) -> Option<f32> {
    let bx = &l.boxes[k];
    if let Some(b) = bx.baseline {
        return Some(b);
    }
    for &c in bx.children.iter().rev() {
        if l.boxes[c].style.is_out_of_flow() {
            continue;
        }
        if let Some(b) = last_baseline(l, c) {
            return Some(b);
        }
    }
    None
}

fn build_line(l: &mut Layout, b: BoxId, placed: &[Placed], lx0: f32, lx1: f32, top: f32, width: f32, first: bool, last: bool, cw: f32) -> Line {
    let bs = l.boxes[b].style.clone();
    let avail = lx1 - lx0;
    let indent = if first { bs.text_indent.resolve(cw) } else { 0.0 };
    let free = avail - width;
    let mut x = lx0 + indent;
    let mut extra_per_space = 0.0;
    match bs.text_align {
        TextAlign::Left => {}
        TextAlign::Right => x += free.max(0.0),
        TextAlign::Center => x += (free / 2.0).max(0.0),
        TextAlign::Justify => {
            if !last && free > 0.0 {
                let spaces: usize = placed.iter().map(|p| if let Placed::Text { space_w, .. } = p { (*space_w > 0.0) as usize } else { 0 }).sum();
                if spaces > 0 {
                    extra_per_space = free / spaces as f32;
                }
            }
        }
    }
    // vertical metrics relative to the line's baseline (y down): (top, bottom)
    let strut = {
        let (a, d) = l.metrics(&bs);
        let lh = l.line_height(&bs);
        let half = (lh - (a + d)) / 2.0;
        (-(a + half), d + half)
    };
    let mut min_top = strut.0;
    let mut max_bottom = strut.1;
    let mut runs = vec![];
    let mut atomics = vec![];
    // the open inline boxes and their baseline shifts
    let mut stack: Vec<(BoxId, f32, f32)> = vec![]; // box, x start, shift
    let mut frags: Vec<InlineFrag> = vec![];
    let mut open_frags: Vec<(BoxId, f32, bool)> = vec![];
    // boxes continued from the previous line are open at its start
    let mut carried: Vec<BoxId> = vec![];
    {
        // an inline box is open at line start when its Open item came on an earlier line
        // (approximated: the parents of the first text/atomic placed)
        let first_box = placed.iter().find_map(|p| match p {
            Placed::Text { boxid, .. } | Placed::Atomic { boxid, .. } => Some(*boxid),
            _ => None,
        });
        if let Some(fb) = first_box {
            let mut p = l.boxes[fb].parent;
            let mut chain = vec![];
            while let Some(x) = p {
                if x == b {
                    break;
                }
                if l.boxes[x].kind == Kind::Inline {
                    chain.push(x);
                }
                p = l.boxes[x].parent;
            }
            chain.reverse();
            // skip boxes opened on this line
            for c in chain {
                let opened_here = placed.iter().any(|q| matches!(q, Placed::Open { boxid, .. } if *boxid == c));
                if !opened_here {
                    carried.push(c);
                }
            }
        }
    }
    let shift_of = |l: &Layout, k: BoxId, parent_shift: f32| -> f32 {
        let s = &l.boxes[k].style;
        let ps = l.boxes[k].parent.map(|p| l.boxes[p].style.clone()).unwrap_or_else(|| s.clone());
        parent_shift
            + match s.vertical_align {
                VAlign::Sub => ps.font_size * 0.2,
                VAlign::Super => -ps.font_size * 0.34,
                VAlign::Len(v) => -v,
                _ => 0.0,
            }
    };
    for c in &carried {
        let sh = shift_of(l, *c, stack.last().map(|s| s.2).unwrap_or(0.0));
        stack.push((*c, x, sh));
        open_frags.push((*c, x, false));
        let s = l.boxes[*c].style.clone();
        let (a, d) = l.metrics(&s);
        let lh = l.line_height(&s);
        let half = (lh - (a + d)) / 2.0;
        min_top = min_top.min(sh - a - half);
        max_bottom = max_bottom.max(sh + d + half);
    }
    let mut atomic_top_bottom: Vec<(BoxId, VAlign, f32)> = vec![];
    for p in placed {
        match p {
            Placed::Open { boxid, w } => {
                let sh = shift_of(l, *boxid, stack.last().map(|s| s.2).unwrap_or(0.0));
                let s = l.boxes[*boxid].style.clone();
                let ml = s.margin[3].map(|v| v.resolve(cw)).unwrap_or(0.0);
                stack.push((*boxid, x, sh));
                open_frags.push((*boxid, x + ml, true));
                x += w;
                let (a, d) = l.metrics(&s);
                let lh = l.line_height(&s);
                let half = (lh - (a + d)) / 2.0;
                min_top = min_top.min(sh - a - half);
                max_bottom = max_bottom.max(sh + d + half);
            }
            Placed::Close { boxid, w } => {
                let s = l.boxes[*boxid].style.clone();
                let mr = s.margin[1].map(|v| v.resolve(cw)).unwrap_or(0.0);
                x += w;
                if let Some(pos) = open_frags.iter().rposition(|f| f.0 == *boxid) {
                    let (_, x0, first_frag) = open_frags.remove(pos);
                    frags.push(InlineFrag { boxid: *boxid, x0, x1: x - mr, top: 0.0, bottom: 0.0, first: first_frag, last: true });
                }
                if let Some(pos) = stack.iter().rposition(|s| s.0 == *boxid) {
                    stack.remove(pos);
                }
            }
            Placed::Text { boxid, text, w, space_w } => {
                let s = l.boxes[*boxid].style.clone();
                let parent = l.boxes[*boxid].parent.unwrap_or(b);
                let sh = if parent == b { 0.0 } else { stack.iter().rev().find(|e| e.0 == parent).map(|e| e.2).unwrap_or(stack.last().map(|e| e.2).unwrap_or(0.0)) };
                let (_, glyphs) = measure(l, &s, text);
                let fi = l.font_of(&s);
                let mut glyphs = glyphs;
                // justified spaces widen
                if extra_per_space > 0.0 {
                    let chars: Vec<char> = text.chars().collect();
                    let mut add = 0.0;
                    let mut gi = 0;
                    for c in &chars {
                        if *c == '\t' {
                            continue;
                        }
                        if gi < glyphs.len() {
                            glyphs[gi].1 += add;
                        }
                        if *c == ' ' {
                            add += extra_per_space;
                        }
                        gi += 1;
                    }
                }
                runs.push(TextRun { text: text.clone(), x, baseline: sh, width: *w, font: fi, size: s.font_size, style: Rc::new((*s).clone()), glyphs });
                x += w + space_w + if *space_w > 0.0 { extra_per_space } else { 0.0 };
                if parent == b {
                    // text directly in the block: the strut covers it, but its font may differ in fallbacks
                } else {
                    let (a, d) = l.metrics(&s);
                    let lh = l.line_height(&s);
                    let half = (lh - (a + d)) / 2.0;
                    min_top = min_top.min(sh - a - half);
                    max_bottom = max_bottom.max(sh + d + half);
                }
            }
            Placed::Atomic { boxid, w } => {
                let sh = stack.last().map(|e| e.2).unwrap_or(0.0);
                let s = l.boxes[*boxid].style.clone();
                let (_, _, _, mh) = l.boxes[*boxid].margin_box();
                let bl = atomic_baseline(l, *boxid);
                let ps = l.boxes[*boxid].parent.map(|p| l.boxes[p].style.clone()).unwrap_or(bs.clone());
                let (pa, pd) = l.metrics(&ps);
                let (t, bt) = match s.vertical_align {
                    VAlign::Top | VAlign::Bottom => {
                        atomic_top_bottom.push((*boxid, s.vertical_align, mh));
                        (0.0, 0.0)
                    }
                    VAlign::Middle => {
                        let f = l.fonts.font(l.font_of(&ps));
                        let xh = f.x_height * ps.font_size / f.units_per_em;
                        let mid = sh - xh / 2.0;
                        (mid - mh / 2.0, mid + mh / 2.0)
                    }
                    VAlign::TextTop => (sh - pa, sh - pa + mh),
                    VAlign::TextBottom => (sh + pd - mh, sh + pd),
                    VAlign::Sub => (sh + ps.font_size * 0.2 - bl, sh + ps.font_size * 0.2 - bl + mh),
                    VAlign::Super => (sh - ps.font_size * 0.34 - bl, sh - ps.font_size * 0.34 - bl + mh),
                    VAlign::Len(v) => (sh - v - bl, sh - v - bl + mh),
                    VAlign::Baseline => (sh - bl, sh - bl + mh),
                };
                if !matches!(s.vertical_align, VAlign::Top | VAlign::Bottom) {
                    min_top = min_top.min(t);
                    max_bottom = max_bottom.max(bt);
                }
                atomics.push((*boxid, x, t));
                x += w;
            }
        }
    }
    // boxes still open continue on the next line
    for (k, x0, first_frag) in open_frags {
        frags.push(InlineFrag { boxid: k, x0, x1: x, top: 0.0, bottom: 0.0, first: first_frag, last: false });
    }
    // top/bottom aligned atomics may grow the line
    let mut height = max_bottom - min_top;
    for (_, _, mh) in &atomic_top_bottom {
        height = height.max(*mh);
    }
    let baseline = top - min_top;
    let line_top = top;
    let line_bottom = top + height;
    // place the atomics
    let mut placed_atomics = vec![];
    for (k, ax, t) in atomics {
        let va = atomic_top_bottom.iter().find(|e| e.0 == k);
        let (_, _, _, mh) = l.boxes[k].margin_box();
        let my = match va {
            Some((_, VAlign::Top, _)) => line_top,
            Some((_, VAlign::Bottom, _)) => line_bottom - mh,
            _ => baseline + t,
        };
        let (bx_mx, bx_my, _, _) = l.boxes[k].margin_box();
        block::shift_subtree(l, k, ax - bx_mx, my - bx_my);
        placed_atomics.push(k);
    }
    // absolute positions for runs and fragments
    for r in &mut runs {
        r.baseline += baseline;
    }
    for f in &mut frags {
        let s = l.boxes[f.boxid].style.clone();
        let (a, d) = l.metrics(&s);
        let sh = shift_of(l, f.boxid, 0.0);
        f.top = baseline + sh - a - s.padding[0].resolve(cw) - s.border(0);
        f.bottom = baseline + sh + d + s.padding[2].resolve(cw) + s.border(2);
    }
    Line { top: line_top, height, baseline, runs, atomics: placed_atomics, frags }
}
