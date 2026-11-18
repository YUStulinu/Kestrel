//! Tables: anonymous table objects, the automatic column-width algorithm
//! (min/max content per column, colspan), row heights, cell alignment.
//! The separated borders model; border-collapse only removes the spacing.
use super::block;
use super::*;

fn anon(l: &mut Layout, parent: BoxId, kind: Kind, display: Display, kids: Vec<BoxId>) -> BoxId {
    let ps = l.boxes[parent].style.clone();
    let mut st = Style { display, ..Style::default() };
    st.color = ps.color;
    st.font_family = ps.font_family.clone();
    st.font_size = ps.font_size;
    st.font_weight = ps.font_weight;
    st.italic = ps.italic;
    st.line_height = ps.line_height;
    st.text_align = ps.text_align;
    st.white_space = ps.white_space;
    st.visible = ps.visible;
    st.border_collapse = ps.border_collapse;
    st.border_spacing = ps.border_spacing;
    st.vertical_align = if display == Display::TableCell { VAlign::Baseline } else { ps.vertical_align };
    let a = l.new_box(None, Rc::new(st), kind);
    l.boxes[a].pseudo = 3;
    for k in &kids {
        l.boxes[*k].parent = Some(a);
    }
    l.boxes[a].children = kids;
    a
}

fn is_ws_text(l: &Layout, k: BoxId) -> bool {
    matches!(&l.boxes[k].kind, Kind::Text(t) if t.chars().all(|c| c.is_ascii_whitespace()))
}

pub fn attach_table_children(l: &mut Layout, b: BoxId, kids: Vec<BoxId>) {
    let kind = l.boxes[b].kind.clone();
    let mut out = vec![];
    let mut run: Vec<BoxId> = vec![];
    let flush = |l: &mut Layout, run: &mut Vec<BoxId>, out: &mut Vec<BoxId>| {
        if run.is_empty() {
            return;
        }
        if run.iter().all(|&k| is_ws_text(l, k)) {
            run.clear();
            return;
        }
        let r = std::mem::take(run);
        let wrapped = match kind {
            Kind::TableRow => anon(l, b, Kind::TableCell, Display::TableCell, r),
            Kind::TableRowGroup => {
                let c = anon(l, b, Kind::TableCell, Display::TableCell, r);
                anon(l, b, Kind::TableRow, Display::TableRow, vec![c])
            }
            _ => {
                let c = anon(l, b, Kind::TableCell, Display::TableCell, r);
                anon(l, b, Kind::TableRow, Display::TableRow, vec![c])
            }
        };
        out.push(wrapped);
    };
    for k in kids {
        let kk = l.boxes[k].kind.clone();
        let fits = match kind {
            Kind::Table => matches!(kk, Kind::TableRowGroup | Kind::TableRow | Kind::TableCaption) || l.boxes[k].style.display == Display::TableColumn || l.boxes[k].style.display == Display::TableColumnGroup,
            Kind::TableRowGroup => matches!(kk, Kind::TableRow),
            Kind::TableRow => matches!(kk, Kind::TableCell),
            _ => true,
        };
        // consecutive cells directly in a table or group share one anonymous row
        if !fits && matches!(kk, Kind::TableCell) && matches!(kind, Kind::Table | Kind::TableRowGroup) {
            flush(l, &mut run, &mut out);
            if let Some(&last) = out.last() {
                if l.boxes[last].pseudo == 3 && l.boxes[last].kind == Kind::TableRow {
                    l.boxes[k].parent = Some(last);
                    l.boxes[last].children.push(k);
                    continue;
                }
            }
            let r = anon(l, b, Kind::TableRow, Display::TableRow, vec![k]);
            out.push(r);
            continue;
        }
        if fits {
            flush(l, &mut run, &mut out);
            out.push(k);
        } else if l.boxes[k].style.is_out_of_flow() && !matches!(kk, Kind::Text(_)) {
            flush(l, &mut run, &mut out);
            out.push(k);
        } else {
            run.push(k);
        }
    }
    flush(l, &mut run, &mut out);
    for k in &out {
        l.boxes[*k].parent = Some(b);
    }
    l.boxes[b].children = out;
}

/// the rows of a table in order: (row box, its group)
fn rows(l: &Layout, t: BoxId) -> Vec<(BoxId, Option<BoxId>)> {
    let mut head = vec![];
    let mut body = vec![];
    let mut foot = vec![];
    for &k in &l.boxes[t].children {
        match l.boxes[k].kind {
            Kind::TableRow => body.push((k, None)),
            Kind::TableRowGroup => {
                let target = match l.boxes[k].style.display {
                    Display::TableHeaderGroup if head.is_empty() => &mut head,
                    Display::TableFooterGroup if foot.is_empty() => &mut foot,
                    _ => &mut body,
                };
                for &r in &l.boxes[k].children {
                    if l.boxes[r].kind == Kind::TableRow {
                        target.push((r, Some(k)));
                    }
                }
            }
            _ => {}
        }
    }
    head.extend(body);
    head.extend(foot);
    head
}

fn cells(l: &Layout, r: BoxId) -> Vec<BoxId> {
    l.boxes[r].children.iter().copied().filter(|&c| l.boxes[c].kind == Kind::TableCell).collect()
}

fn spacing(s: &Style) -> (f32, f32) {
    if s.border_collapse {
        (0.0, 0.0)
    } else {
        s.border_spacing
    }
}

/// column (min, max) widths
fn columns(l: &mut Layout, t: BoxId) -> Vec<(f32, f32)> {
    let rs = rows(l, t);
    let mut cols: Vec<(f32, f32)> = vec![];
    let mut spans = vec![];
    for (r, _) in &rs {
        let mut c = 0;
        for cell in cells(l, *r) {
            let span = l.boxes[cell].colspan;
            let (mn, mx) = l.intrinsic(cell);
            if span == 1 {
                if cols.len() <= c {
                    cols.resize(c + 1, (0.0, 0.0));
                }
                cols[c].0 = cols[c].0.max(mn);
                cols[c].1 = cols[c].1.max(mx);
            } else {
                spans.push((c, span, mn, mx));
            }
            c += span;
        }
        if cols.len() < c {
            cols.resize(c, (0.0, 0.0));
        }
    }
    let hs = spacing(&l.boxes[t].style).0;
    for (c, span, mn, mx) in spans {
        let end = (c + span).min(cols.len());
        let cur_mn: f32 = cols[c..end].iter().map(|x| x.0).sum::<f32>() + hs * (span - 1) as f32;
        let cur_mx: f32 = cols[c..end].iter().map(|x| x.1).sum::<f32>() + hs * (span - 1) as f32;
        let n = (end - c).max(1) as f32;
        if mn > cur_mn {
            for x in &mut cols[c..end] {
                x.0 += (mn - cur_mn) / n;
            }
        }
        if mx > cur_mx {
            for x in &mut cols[c..end] {
                x.1 += (mx - cur_mx) / n;
            }
        }
    }
    for x in cols.iter_mut() {
        x.1 = x.1.max(x.0);
    }
    cols
}

pub fn intrinsic(l: &mut Layout, t: BoxId) -> (f32, f32) {
    let cols = columns(l, t);
    let hs = spacing(&l.boxes[t].style).0;
    let n = cols.len() as f32;
    let sp = if cols.is_empty() { 0.0 } else { hs * (n + 1.0) };
    let mn: f32 = cols.iter().map(|c| c.0).sum::<f32>() + sp;
    let mx: f32 = cols.iter().map(|c| c.1).sum::<f32>() + sp;
    (mn, mx)
}

pub fn layout(l: &mut Layout, t: BoxId, own_h: Option<f32>) {
    let s = l.boxes[t].style.clone();
    let (hs, vs) = spacing(&s);
    let cols = columns(l, t);
    let n = cols.len();
    let tw = l.boxes[t].w;
    let avail = (tw - if n > 0 { hs * (n as f32 + 1.0) } else { 0.0 }).max(0.0);
    let sum_min: f32 = cols.iter().map(|c| c.0).sum();
    let sum_max: f32 = cols.iter().map(|c| c.1).sum();
    let widths: Vec<f32> = if n == 0 {
        vec![]
    } else if sum_max <= avail {
        let extra = avail - sum_max;
        if sum_max > 0.0 {
            cols.iter().map(|c| c.1 + extra * c.1 / sum_max).collect()
        } else {
            cols.iter().map(|_| avail / n as f32).collect()
        }
    } else if sum_min <= avail {
        let r = if sum_max > sum_min { (avail - sum_min) / (sum_max - sum_min) } else { 0.0 };
        cols.iter().map(|c| c.0 + (c.1 - c.0) * r).collect()
    } else {
        cols.iter().map(|c| c.0).collect()
    };
    let tx = l.boxes[t].x;
    let mut y = l.boxes[t].y;
    // captions on top
    for &k in &l.boxes[t].children.clone() {
        if l.boxes[k].kind == Kind::TableCaption {
            l.resolve_edges(k, tw);
            let w = (tw - l.hsum(k)).max(0.0);
            block::layout_root_at(l, k, tx + l.boxes[k].m[3], y + l.boxes[k].m[0], w, None);
            let (_, _, _, mh) = l.boxes[k].margin_box();
            y += mh;
        }
    }
    let rows_list = rows(l, t);
    y += if rows_list.is_empty() { 0.0 } else { vs };
    let mut group_extent: HashMap<BoxId, (f32, f32)> = HashMap::new();
    let mut occupied: Vec<usize> = vec![0; n.max(1)];
    let _ = &mut occupied;
    for (r, g) in &rows_list {
        let r = *r;
        l.resolve_edges(r, tw);
        let row_cells = cells(l, r);
        let mut x = tx + hs;
        let mut c = 0;
        let mut row_h: f32 = l.specified_height(r, None).unwrap_or(0.0);
        let mut placed = vec![];
        for cell in row_cells {
            let span = l.boxes[cell].colspan;
            let end = (c + span).min(n);
            let w: f32 = widths.get(c..end).map(|v| v.iter().sum::<f32>()).unwrap_or(0.0) + hs * (span.saturating_sub(1)) as f32;
            l.resolve_edges(cell, tw);
            let cw = (w - l.hsum(cell)).max(0.0);
            // the cell's specified height acts as a minimum
            let saved = l.boxes[cell].style.clone();
            let spec = l.specified_height(cell, None);
            let mut st = (*saved).clone();
            st.height = None;
            l.boxes[cell].style = Rc::new(st);
            block::layout_root_at(l, cell, x, y, cw, None);
            l.boxes[cell].style = saved;
            let content_h = l.boxes[cell].h;
            let h = spec.map_or(content_h, |sh| sh.max(content_h));
            l.boxes[cell].h = h;
            let bx = &l.boxes[cell];
            row_h = row_h.max(h + bx.b[0] + bx.b[2] + bx.p[0] + bx.p[2]);
            placed.push((cell, content_h));
            x += w + hs;
            c += span;
        }
        // stretch cells to the row height and align their content
        for (cell, content_h) in placed {
            let bx = &l.boxes[cell];
            let edges = bx.b[0] + bx.b[2] + bx.p[0] + bx.p[2];
            let new_h = row_h - edges;
            let free = new_h - content_h;
            let off = match bx.style.vertical_align {
                VAlign::Middle => free / 2.0,
                VAlign::Bottom => free,
                _ => 0.0,
            };
            if off > 0.0 {
                let kids = l.boxes[cell].children.clone();
                for k in kids {
                    block::shift_subtree(l, k, 0.0, off);
                }
                let lines = std::mem::take(&mut l.boxes[cell].lines);
                let mut lines = lines;
                for ln in &mut lines {
                    ln.top += off;
                    ln.baseline += off;
                    for r in &mut ln.runs {
                        r.baseline += off;
                    }
                    for f in &mut ln.frags {
                        f.top += off;
                        f.bottom += off;
                    }
                }
                l.boxes[cell].lines = lines;
            }
            l.boxes[cell].h = new_h;
        }
        // the row box spans its cells
        let rb = &mut l.boxes[r];
        rb.x = tx + hs;
        rb.y = y;
        rb.w = (tw - 2.0 * hs).max(0.0);
        rb.h = row_h;
        rb.laid_out = true;
        if let Some(g) = g {
            let e = group_extent.entry(*g).or_insert((y, y + row_h));
            e.1 = y + row_h;
        }
        y += row_h + vs;
    }
    for (g, (top, bottom)) in group_extent {
        let gb = &mut l.boxes[g];
        gb.x = tx + hs;
        gb.y = top;
        gb.w = (tw - 2.0 * hs).max(0.0);
        gb.h = bottom - top;
        gb.laid_out = true;
    }
    let content_h = y - l.boxes[t].y;
    let h = own_h.map_or(content_h, |sh| sh.max(content_h));
    l.boxes[t].h = h;
    l.boxes[t].laid_out = true;
}
