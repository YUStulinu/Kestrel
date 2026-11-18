//! Flexbox (CSS Flexible Box Layout §9): base sizes, line collection,
//! resolving flexible lengths, cross sizes, alignment.
use super::block;
use super::*;

struct Item {
    b: BoxId,
    base: f32,       // flex base size (content box, main axis)
    hypo: f32,       // hypothetical main size (content)
    target: f32,     // resolved main size (content)
    frozen: bool,
    min: f32,
    max: f32,
    main_edges: f32, // margins + border + padding on the main axis
    cross_edges: f32,
    cross: f32,      // content cross size
    grow: f32,
    shrink: f32,
    auto_margins_main: (bool, bool),
    auto_margins_cross: (bool, bool),
    baseline: f32,
}

fn is_row(s: &Style) -> bool {
    matches!(s.flex_direction, FlexDirection::Row | FlexDirection::RowReverse)
}

pub fn intrinsic(l: &mut Layout, b: BoxId) -> (f32, f32) {
    let s = l.boxes[b].style.clone();
    let kids: Vec<BoxId> = l.boxes[b].children.iter().copied().filter(|&k| !matches!(l.boxes[k].style.position, Position::Absolute | Position::Fixed)).collect();
    let mut mn: f32 = 0.0;
    let mut mx: f32 = 0.0;
    let gap = s.column_gap.px;
    for (i, k) in kids.iter().enumerate() {
        let (a, c) = l.intrinsic(*k);
        let ms = &l.boxes[*k].style;
        let m = ms.margin[1].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0) + ms.margin[3].map(|v| if v.has_pct() { 0.0 } else { v.px }).unwrap_or(0.0);
        if is_row(&s) {
            mx += c + m + if i > 0 { gap } else { 0.0 };
            if s.flex_wrap {
                mn = mn.max(a + m);
            } else {
                mn += a + m + if i > 0 { gap } else { 0.0 };
            }
        } else {
            mn = mn.max(a + m);
            mx = mx.max(c + m);
        }
    }
    (mn, mx)
}

/// lays out the flex container's items; the container's content x, y and width are set
pub fn layout(l: &mut Layout, b: BoxId, own_h: Option<f32>) {
    let s = l.boxes[b].style.clone();
    let row = is_row(&s);
    let (cx, cy, cw) = (l.boxes[b].x, l.boxes[b].y, l.boxes[b].w);
    let mut kids: Vec<BoxId> = vec![];
    for &k in &l.boxes[b].children.clone() {
        if matches!(l.boxes[k].style.position, Position::Absolute | Position::Fixed) {
            l.boxes[k].static_pos = (cx, cy);
            let c = l.abs_container(k);
            l.boxes[c].abs_children.push(k);
        } else {
            kids.push(k);
        }
    }
    kids.sort_by_key(|&k| l.boxes[k].style.order);
    let main_avail = if row { Some(cw) } else { own_h };
    let cross_avail = if row { own_h } else { Some(cw) };
    let main_gap = if row { s.column_gap.resolve(cw) } else { s.row_gap.resolve(own_h.unwrap_or(0.0)) };
    let cross_gap = if row { s.row_gap.resolve(own_h.unwrap_or(0.0)) } else { s.column_gap.resolve(cw) };
    // 1. base sizes
    let mut items: Vec<Item> = vec![];
    for &k in &kids {
        l.resolve_edges(k, cw);
        let ks = l.boxes[k].style.clone();
        let bx = &l.boxes[k];
        let (main_edges, cross_edges) = if row {
            (bx.m[1] + bx.m[3] + bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3], bx.m[0] + bx.m[2] + bx.b[0] + bx.b[2] + bx.p[0] + bx.p[2])
        } else {
            (bx.m[0] + bx.m[2] + bx.b[0] + bx.b[2] + bx.p[0] + bx.p[2], bx.m[1] + bx.m[3] + bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3])
        };
        let main_border_edges = if row { bx.b[1] + bx.b[3] + bx.p[1] + bx.p[3] } else { bx.b[0] + bx.b[2] + bx.p[0] + bx.p[2] };
        let spec_main = if row {
            ks.width.map(|w| l.content_from_specified(k, w.resolve(cw), true))
        } else {
            l.specified_height(k, main_avail)
        };
        let basis = match ks.flex_basis {
            Some(fb) if !(fb.has_pct() && main_avail.is_none()) => Some(l.content_from_specified(k, fb.resolve(main_avail.unwrap_or(0.0)), row)),
            _ if !ks.flex_basis_content => spec_main,
            _ => None,
        };
        let content_main = |l: &mut Layout| -> f32 {
            if row {
                let (_, mx) = l.intrinsic(k);
                (mx - main_border_edges).max(0.0)
            } else {
                // lay out at the cross size to learn the height
                let w = match ks.width {
                    Some(w) => l.content_from_specified(k, w.resolve(cw), true),
                    None => (cw - cross_edges).max(0.0),
                };
                block::layout_root_at(l, k, 0.0, 0.0, w, None);
                l.boxes[k].h
            }
        };
        let base = match basis {
            Some(v) => v,
            None => content_main(l),
        };
        // min/max on the main axis; min auto is the content size for visible overflow
        let (min_spec, max_spec) = if row { (ks.min_width, ks.max_width) } else { (ks.min_height, ks.max_height) };
        let min = match min_spec {
            Some(m) => l.content_from_specified(k, m.resolve(main_avail.unwrap_or(0.0)), row),
            None => {
                if ks.overflow == Overflow::Visible {
                    let content_min = if row {
                        let (mn, _) = l.intrinsic(k);
                        (mn - main_border_edges).max(0.0)
                    } else {
                        content_main(l)
                    };
                    let spec_cap = spec_main.unwrap_or(f32::MAX);
                    content_min.min(spec_cap)
                } else {
                    0.0
                }
            }
        };
        let max = max_spec.map(|m| l.content_from_specified(k, m.resolve(main_avail.unwrap_or(0.0)), row)).unwrap_or(f32::MAX);
        let hypo = base.max(min).min(max.max(min));
        let am = if row { (ks.margin[3].is_none(), ks.margin[1].is_none()) } else { (ks.margin[0].is_none(), ks.margin[2].is_none()) };
        let ac = if row { (ks.margin[0].is_none(), ks.margin[2].is_none()) } else { (ks.margin[3].is_none(), ks.margin[1].is_none()) };
        items.push(Item {
            b: k,
            base,
            hypo,
            target: hypo,
            frozen: false,
            min,
            max: max.max(min),
            main_edges,
            cross_edges,
            cross: 0.0,
            grow: ks.flex_grow,
            shrink: ks.flex_shrink,
            auto_margins_main: am,
            auto_margins_cross: ac,
            baseline: 0.0,
        });
    }
    // 2. lines
    let mut lines: Vec<Vec<usize>> = vec![];
    {
        let limit = main_avail.unwrap_or(f32::MAX);
        let mut cur: Vec<usize> = vec![];
        let mut used = 0.0;
        for (i, it) in items.iter().enumerate() {
            let outer = it.hypo + it.main_edges;
            let g = if cur.is_empty() { 0.0 } else { main_gap };
            if s.flex_wrap && !cur.is_empty() && used + g + outer > limit + 0.01 {
                lines.push(std::mem::take(&mut cur));
                used = 0.0;
            }
            used += if cur.is_empty() { 0.0 } else { main_gap } + outer;
            cur.push(i);
        }
        if !cur.is_empty() || lines.is_empty() {
            lines.push(cur);
        }
    }
    // 3. resolve flexible lengths per line
    let container_main = match main_avail {
        Some(m) => m,
        None => {
            // an indefinite main size (column without height): the content size
            let mut mx: f32 = 0.0;
            for ln in &lines {
                let sum: f32 = ln.iter().map(|&i| items[i].hypo + items[i].main_edges).sum::<f32>() + main_gap * (ln.len().saturating_sub(1)) as f32;
                mx = mx.max(sum);
            }
            let mx = l.clamp_height(b, mx, None);
            mx
        }
    };
    for ln in &lines {
        resolve_flexible(&mut items, ln, container_main, main_gap);
    }
    // 4. cross sizes: lay out each item at its main size
    for it in items.iter_mut() {
        let k = it.b;
        let ks = l.boxes[k].style.clone();
        if row {
            block::layout_root_at(l, k, 0.0, 0.0, it.target, cross_avail);
            it.cross = l.boxes[k].h;
            if let Some(h) = l.specified_height(k, cross_avail) {
                it.cross = l.clamp_height(k, h, cross_avail);
            }
            it.baseline = super::inline_baseline(l, k).unwrap_or(l.boxes[k].h + l.boxes[k].p[0] + l.boxes[k].b[0] + l.boxes[k].m[0]);
        } else {
            let w = match ks.width {
                Some(w) => l.content_from_specified(k, w.resolve(cw), true),
                None => {
                    let align = if ks.align_self == Align::Auto { s.align_items } else { ks.align_self };
                    let stretch = matches!(align, Align::Stretch | Align::Normal) && !it.auto_margins_cross.0 && !it.auto_margins_cross.1;
                    if stretch {
                        (cw - it.cross_edges).max(0.0)
                    } else {
                        let (_, mx) = l.intrinsic(k);
                        let edges = l.boxes[k].b[1] + l.boxes[k].b[3] + l.boxes[k].p[1] + l.boxes[k].p[3];
                        (mx - edges).max(0.0).min((cw - it.cross_edges).max(0.0))
                    }
                }
            };
            let w = l.clamp_width(k, w, cw);
            it.cross = w;
        }
    }
    // 5. line cross sizes
    let single_line = !s.flex_wrap;
    let mut line_cross: Vec<f32> = vec![];
    let mut line_base: Vec<f32> = vec![];
    for ln in &lines {
        let mut mx: f32 = 0.0;
        let mut above: f32 = 0.0;
        let mut below: f32 = 0.0;
        for &i in ln {
            let it = &items[i];
            let ks = &l.boxes[it.b].style;
            let align = if ks.align_self == Align::Auto { s.align_items } else { ks.align_self };
            if row && align == Align::Baseline {
                above = above.max(it.baseline);
                below = below.max(it.cross + it.cross_edges - it.baseline);
            } else {
                mx = mx.max(it.cross + it.cross_edges);
            }
        }
        line_cross.push(mx.max(above + below));
        line_base.push(above);
    }
    if single_line {
        if let Some(c) = cross_avail {
            if row {
                line_cross[0] = l.clamp_height(b, c, None);
            } else {
                line_cross[0] = c;
            }
        } else if row {
            line_cross[0] = l.clamp_height(b, line_cross[0], None);
        }
    }
    // 6. align-content (multi-line with a definite cross size)
    let total_cross: f32 = line_cross.iter().sum::<f32>() + cross_gap * (lines.len().saturating_sub(1)) as f32;
    let container_cross = if row {
        match own_h {
            Some(h) => h,
            None => l.clamp_height(b, total_cross, None),
        }
    } else {
        cw
    };
    let mut cross_pos = 0.0;
    let mut cross_between = cross_gap;
    if !single_line {
        let free = container_cross - total_cross;
        let n = lines.len() as f32;
        match s.align_content {
            Align::FlexEnd => cross_pos = free,
            Align::Center => cross_pos = free / 2.0,
            Align::SpaceBetween if n > 1.0 && free > 0.0 => cross_between += free / (n - 1.0),
            Align::SpaceAround if free > 0.0 => {
                cross_pos = free / n / 2.0;
                cross_between += free / n;
            }
            Align::SpaceEvenly if free > 0.0 => {
                cross_pos = free / (n + 1.0);
                cross_between += free / (n + 1.0);
            }
            Align::Stretch | Align::Normal if free > 0.0 => {
                let add = free / n;
                for c in line_cross.iter_mut() {
                    *c += add;
                }
            }
            _ => {}
        }
    }
    let reverse_main = matches!(s.flex_direction, FlexDirection::RowReverse | FlexDirection::ColumnReverse);
    let mut line_offsets = vec![];
    {
        let mut p = cross_pos;
        for (li, _) in lines.iter().enumerate() {
            line_offsets.push(p);
            p += line_cross[li] + cross_between;
        }
    }
    if s.wrap_reverse {
        for (li, off) in line_offsets.iter_mut().enumerate() {
            *off = container_cross - *off - line_cross[li];
        }
    }
    // 7. main-axis placement and cross alignment
    for (li, ln) in lines.iter().enumerate() {
        let used: f32 = ln.iter().map(|&i| items[i].target + items[i].main_edges).sum::<f32>() + main_gap * (ln.len().saturating_sub(1)) as f32;
        let mut free = container_main - used;
        let auto_count: usize = ln.iter().map(|&i| items[i].auto_margins_main.0 as usize + items[i].auto_margins_main.1 as usize).sum();
        let mut start = 0.0;
        let mut between = main_gap;
        let mut auto_each = 0.0;
        if auto_count > 0 && free > 0.0 {
            auto_each = free / auto_count as f32;
            free = 0.0;
        }
        let n = ln.len() as f32;
        let jc = s.justify_content;
        match jc {
            Align::FlexEnd => start = free,
            Align::Center => start = free / 2.0,
            Align::SpaceBetween if n > 1.0 && free > 0.0 => between += free / (n - 1.0),
            Align::SpaceBetween => {}
            Align::SpaceAround if free > 0.0 => {
                start = free / n / 2.0;
                between += free / n;
            }
            Align::SpaceAround => start = free / 2.0,
            Align::SpaceEvenly if free > 0.0 => {
                start = free / (n + 1.0);
                between += free / (n + 1.0);
            }
            Align::SpaceEvenly => start = free / 2.0,
            _ => {}
        }
        let mut pos = start;
        let order: Vec<usize> = if reverse_main { ln.iter().rev().copied().collect() } else { ln.clone() };
        for (oi, &i) in order.iter().enumerate() {
            let k = items[i].b;
            let ks = l.boxes[k].style.clone();
            if oi > 0 {
                pos += between;
            }
            let (ma, mb) = items[i].auto_margins_main;
            if ma {
                pos += auto_each;
            }
            let outer_main = items[i].target + items[i].main_edges;
            // main-axis offset of the margin box
            let main_off = if reverse_main { container_main - pos - outer_main } else { pos };
            pos += outer_main;
            if mb {
                pos += auto_each;
            }
            // cross
            let align = if ks.align_self == Align::Auto { s.align_items } else { ks.align_self };
            let lc = line_cross[li];
            let mut cross = items[i].cross;
            let cross_spec_auto = if row { ks.height.is_none() } else { ks.width.is_none() };
            let (ca, cb_) = items[i].auto_margins_cross;
            if matches!(align, Align::Stretch | Align::Normal) && cross_spec_auto && !ca && !cb_ {
                cross = (lc - items[i].cross_edges).max(0.0);
                cross = if row { l.clamp_height(k, cross, None) } else { l.clamp_width(k, cross, cw) };
            }
            let free_c = lc - cross - items[i].cross_edges;
            let mut cross_off = match align {
                Align::FlexEnd => free_c,
                Align::Center => free_c / 2.0,
                Align::Baseline if row => line_base[li] - items[i].baseline,
                _ => 0.0,
            };
            if ca || cb_ {
                cross_off = match (ca, cb_) {
                    (true, true) => (free_c / 2.0).max(0.0),
                    (true, false) => free_c.max(0.0),
                    _ => 0.0,
                };
            }
            if s.wrap_reverse && !matches!(align, Align::Baseline) && !(ca || cb_) {
                cross_off = match align {
                    Align::FlexStart => free_c,
                    Align::FlexEnd => 0.0,
                    _ => cross_off,
                };
            }
            let line_off = line_offsets[li];
            // final layout at the resolved size
            let bx = &l.boxes[k];
            let (ml, mt) = (bx.m[3], bx.m[0]);
            let (x, y, w, hforce) = if row {
                (cx + main_off + ml, cy + line_off + cross_off + mt, items[i].target, Some(cross))
            } else {
                (cx + line_off + cross_off + ml, cy + main_off + mt, cross, Some(items[i].target))
            };
            // lay out with the definite sizes
            let saved = l.boxes[k].style.clone();
            let mut st = (*saved).clone();
            let edges_v = l.boxes[k].b[0] + l.boxes[k].b[2] + l.boxes[k].p[0] + l.boxes[k].p[2];
            if let Some(h) = hforce {
                let stretched_or_main = !row || (matches!(align, Align::Stretch | Align::Normal) && cross_spec_auto && !ca && !cb_);
                if stretched_or_main {
                    st.height = Some(Len::px(if st.box_sizing == BoxSizing::BorderBox { h + edges_v } else { h }));
                    st.min_height = None;
                    st.max_height = None;
                }
            }
            l.boxes[k].style = Rc::new(st);
            block::layout_root_at(l, k, x, y, w, if row { Some(lc) } else { own_h });
            l.boxes[k].style = saved;
        }
    }
    // the container's height
    let h = if row {
        container_cross
    } else {
        match own_h {
            Some(h) => h,
            None => container_main,
        }
    };
    l.boxes[b].h = h;
    l.boxes[b].laid_out = true;
    // the first item's baseline is the container's
    let mut bl = None;
    for it in &items {
        if let Some(x) = super::inline_baseline(l, it.b) {
            bl = Some(x + {
                let (_, my, _, _) = l.boxes[it.b].margin_box();
                my
            });
            break;
        }
    }
    l.boxes[b].baseline = bl;
    l.boxes[b].first_baseline = bl;
}

fn resolve_flexible(items: &mut [Item], ln: &[usize], container: f32, gap: f32) {
    let hypo_sum: f32 = ln.iter().map(|&i| items[i].hypo + items[i].main_edges).sum::<f32>() + gap * (ln.len().saturating_sub(1)) as f32;
    let growing = hypo_sum < container;
    for &i in ln {
        let it = &mut items[i];
        it.target = it.hypo;
        it.frozen = false;
        let factor = if growing { it.grow } else { it.shrink };
        if factor == 0.0 || (growing && it.base > it.hypo) || (!growing && it.base < it.hypo) {
            it.frozen = true;
        }
    }
    for _round in 0..20 {
        if ln.iter().all(|&i| items[i].frozen) {
            break;
        }
        let used: f32 = ln.iter().map(|&i| (if items[i].frozen { items[i].target } else { items[i].base }) + items[i].main_edges).sum::<f32>()
            + gap * (ln.len().saturating_sub(1)) as f32;
        let mut free = container - used;
        let sum_factors: f32 = ln.iter().filter(|&&i| !items[i].frozen).map(|&i| if growing { items[i].grow } else { items[i].shrink }).sum();
        if sum_factors < 1.0 {
            let initial_free = container - hypo_sum;
            let scaled = initial_free * sum_factors;
            if scaled.abs() < free.abs() {
                free = scaled;
            }
        }
        // distribute
        if growing {
            for &i in ln {
                if !items[i].frozen {
                    items[i].target = items[i].base + if sum_factors > 0.0 { free * items[i].grow / sum_factors } else { 0.0 };
                }
            }
        } else {
            let scaled_sum: f32 = ln.iter().filter(|&&i| !items[i].frozen).map(|&i| items[i].shrink * items[i].base).sum();
            for &i in ln {
                if !items[i].frozen {
                    let r = if scaled_sum > 0.0 { items[i].shrink * items[i].base / scaled_sum } else { 0.0 };
                    items[i].target = items[i].base + free * r;
                }
            }
        }
        // fix violations
        let mut total_violation = 0.0;
        let mut viol = vec![0i8; items.len()];
        for &i in ln {
            if items[i].frozen {
                continue;
            }
            let t = items[i].target;
            let c = t.max(items[i].min).min(items[i].max).max(0.0);
            total_violation += c - t;
            viol[i] = if c > t { 1 } else if c < t { -1 } else { 0 };
            items[i].target = c;
        }
        let mut any = false;
        for &i in ln {
            if items[i].frozen {
                continue;
            }
            let freeze = if total_violation == 0.0 { true } else if total_violation > 0.0 { viol[i] > 0 } else { viol[i] < 0 };
            if freeze {
                items[i].frozen = true;
                any = true;
            }
        }
        if !any {
            break;
        }
    }
}
