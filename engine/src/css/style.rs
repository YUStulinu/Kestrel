//! The cascade and computed values: matching rules to elements, shorthand
//! expansion, inheritance, and the computed style of each element.
use super::selector::{self, PseudoElement, Selector};
use super::syntax::{self, Declaration, Rule, Tok};
use super::values::*;
use crate::html::dom::{Document, NodeData, NodeId, Ns};
use std::collections::HashMap;

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Display {
    None,
    Block,
    Inline,
    InlineBlock,
    ListItem,
    Flex,
    InlineFlex,
    Table,
    InlineTable,
    TableRowGroup,
    TableHeaderGroup,
    TableFooterGroup,
    TableRow,
    TableCell,
    TableColumn,
    TableColumnGroup,
    TableCaption,
    Contents,
    FlowRoot,
}

impl Display {
    pub fn is_inline_level(self) -> bool {
        matches!(self, Display::Inline | Display::InlineBlock | Display::InlineFlex | Display::InlineTable)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Position {
    Static,
    Relative,
    Absolute,
    Fixed,
    Sticky,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Float {
    None,
    Left,
    Right,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Clear {
    None,
    Left,
    Right,
    Both,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BorderStyle {
    None,
    Hidden,
    Solid,
    Dashed,
    Dotted,
    Double,
    Groove,
    Ridge,
    Inset,
    Outset,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum TextAlign {
    Left,
    Right,
    Center,
    Justify,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum VAlign {
    Baseline,
    Top,
    Middle,
    Bottom,
    TextTop,
    TextBottom,
    Sub,
    Super,
    Len(f32),
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum WhiteSpace {
    Normal,
    Pre,
    NoWrap,
    PreWrap,
    PreLine,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum LineHeight {
    Normal,
    Number(f32),
    Px(f32),
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Overflow {
    Visible,
    Hidden,
    Scroll,
    Auto,
    Clip,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum FlexDirection {
    Row,
    RowReverse,
    Column,
    ColumnReverse,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Align {
    Auto,
    Normal,
    Stretch,
    FlexStart,
    FlexEnd,
    Center,
    Baseline,
    SpaceBetween,
    SpaceAround,
    SpaceEvenly,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum BoxSizing {
    ContentBox,
    BorderBox,
}
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum ListStyle {
    None,
    Disc,
    Circle,
    Square,
    Decimal,
    LowerAlpha,
    UpperAlpha,
    LowerRoman,
    UpperRoman,
}

#[derive(Clone, Debug, PartialEq)]
pub enum ContentItem {
    Text(String),
    Attr(String),
    Counter(String),
    OpenQuote,
    CloseQuote,
}

/// a computed style; lengths are px except percentages kept in Len
#[derive(Clone, Debug)]
pub struct Style {
    pub display: Display,
    pub position: Position,
    pub float: Float,
    pub clear: Clear,
    pub inset: [Option<Len>; 4], // top right bottom left; None = auto
    pub width: Option<Len>,
    pub height: Option<Len>,
    pub min_width: Option<Len>, // None = auto
    pub min_height: Option<Len>,
    pub max_width: Option<Len>, // None = none
    pub max_height: Option<Len>,
    pub margin: [Option<Len>; 4],
    pub padding: [Len; 4],
    pub border_width: [f32; 4],
    pub border_style: [BorderStyle; 4],
    pub border_color: [Color; 4],
    pub box_sizing: BoxSizing,
    pub color: Color,
    pub background_color: Color,
    pub background_image: Option<String>,
    pub background_repeat: (bool, bool),
    pub background_position: (Len, Len),
    pub font_family: Vec<String>,
    pub font_size: f32,
    pub font_weight: u16,
    pub italic: bool,
    pub line_height: LineHeight,
    pub text_align: TextAlign,
    pub vertical_align: VAlign,
    pub white_space: WhiteSpace,
    pub underline: bool,
    pub line_through: bool,
    pub overline: bool,
    pub text_indent: Len,
    pub letter_spacing: f32,
    pub word_spacing: f32,
    pub text_transform: u8, // 0 none 1 upper 2 lower 3 capitalize
    pub overflow: Overflow,
    pub visible: bool,
    pub z_index: Option<i32>,
    pub opacity: f32,
    pub flex_direction: FlexDirection,
    pub flex_wrap: bool,
    pub wrap_reverse: bool,
    pub justify_content: Align,
    pub align_items: Align,
    pub align_self: Align,
    pub align_content: Align,
    pub flex_grow: f32,
    pub flex_shrink: f32,
    pub flex_basis: Option<Len>, // None = auto
    pub flex_basis_content: bool,
    pub order: i32,
    pub row_gap: Len,
    pub column_gap: Len,
    pub list_style: ListStyle,
    pub list_inside: bool,
    pub content: Option<Vec<ContentItem>>,
    pub border_collapse: bool,
    pub border_spacing: (f32, f32),
    pub table_layout_fixed: bool,
    pub outline_width: f32,
    pub outline_style: BorderStyle,
    pub outline_color: Color,
    pub direction_rtl: bool,
}

impl Default for Style {
    fn default() -> Self {
        Style {
            display: Display::Inline,
            position: Position::Static,
            float: Float::None,
            clear: Clear::None,
            inset: [None; 4],
            width: None,
            height: None,
            min_width: None,
            min_height: None,
            max_width: None,
            max_height: None,
            margin: [Some(Len::ZERO); 4],
            padding: [Len::ZERO; 4],
            border_width: [3.0; 4],
            border_style: [BorderStyle::None; 4],
            border_color: [Color::BLACK; 4],
            box_sizing: BoxSizing::ContentBox,
            color: Color::BLACK,
            background_color: Color::TRANSPARENT,
            background_image: None,
            background_repeat: (true, true),
            background_position: (Len::ZERO, Len::ZERO),
            font_family: vec!["serif".into()],
            font_size: 16.0,
            font_weight: 400,
            italic: false,
            line_height: LineHeight::Normal,
            text_align: TextAlign::Left,
            vertical_align: VAlign::Baseline,
            white_space: WhiteSpace::Normal,
            underline: false,
            line_through: false,
            overline: false,
            text_indent: Len::ZERO,
            letter_spacing: 0.0,
            word_spacing: 0.0,
            text_transform: 0,
            overflow: Overflow::Visible,
            visible: true,
            z_index: None,
            opacity: 1.0,
            flex_direction: FlexDirection::Row,
            flex_wrap: false,
            wrap_reverse: false,
            justify_content: Align::Normal,
            align_items: Align::Normal,
            align_self: Align::Auto,
            align_content: Align::Normal,
            flex_grow: 0.0,
            flex_shrink: 1.0,
            flex_basis: None,
            flex_basis_content: false,
            order: 0,
            row_gap: Len::ZERO,
            column_gap: Len::ZERO,
            list_style: ListStyle::Disc,
            list_inside: false,
            content: None,
            border_collapse: false,
            border_spacing: (0.0, 0.0),
            table_layout_fixed: false,
            outline_width: 3.0,
            outline_style: BorderStyle::None,
            outline_color: Color::BLACK,
            direction_rtl: false,
        }
    }
}

impl Style {
    /// the used border width (zero when the style is none or hidden)
    pub fn border(&self, side: usize) -> f32 {
        if matches!(self.border_style[side], BorderStyle::None | BorderStyle::Hidden) {
            0.0
        } else {
            self.border_width[side]
        }
    }
    pub fn is_positioned(&self) -> bool {
        self.position != Position::Static
    }
    pub fn is_out_of_flow(&self) -> bool {
        matches!(self.position, Position::Absolute | Position::Fixed) || self.float != Float::None
    }
}

// ------------------------------------------------------------------ the UA sheet
pub const UA_CSS: &str = include_str!("ua.css");

#[derive(Clone)]
pub struct StyleRule {
    pub selector: Selector,
    pub decls: std::rc::Rc<Vec<Declaration>>,
    pub origin: u8, // 0 UA, 1 author
    pub order: usize,
}

/// a parsed author or UA style sheet, flattened (media queries evaluated)
pub struct Sheet {
    pub rules: Vec<StyleRule>,
    pub font_faces: Vec<(String, String, u16, bool)>, // family, url, weight, italic
    pub imports: Vec<String>,
}

/// the UA sheet, parsed once per thread (it has no media queries)
pub fn ua_sheet(vp: &Viewport) -> Sheet {
    thread_local! {
        static UA: std::cell::RefCell<Option<Vec<StyleRule>>> = const { std::cell::RefCell::new(None) };
    }
    let rules = UA.with(|u| {
        if u.borrow().is_none() {
            *u.borrow_mut() = Some(parse_sheet(UA_CSS, 0, 0, vp).rules);
        }
        u.borrow().clone().unwrap()
    });
    Sheet { rules, font_faces: vec![], imports: vec![] }
}

pub struct Viewport {
    pub width: f32,
    pub height: f32,
}

fn media_matches(prelude: &[Tok], vp: &Viewport) -> bool {
    // comma-separated queries; supports all/screen/print and (min|max)-(width|height)
    let s: String = prelude
        .iter()
        .map(|t| match t {
            Tok::Ident(i) => i.to_ascii_lowercase(),
            Tok::Dimension(v, u) => format!("{v}{u}"),
            Tok::Number(v, _) => format!("{v}"),
            Tok::Colon => ":".into(),
            Tok::LParen => "(".into(),
            Tok::RParen => ")".into(),
            Tok::Comma => ",".into(),
            Tok::Ws => " ".into(),
            Tok::Function(f) => format!("{f}("),
            _ => "?".into(),
        })
        .collect();
    if s.trim().is_empty() {
        return true;
    }
    s.split(',').any(|q| {
        let q = q.trim();
        let mut ok = true;
        let words: Vec<&str> = q.split(" and ").map(|w| w.trim()).collect();
        for w in words {
            let w = w.trim_start_matches("only ").trim();
            if w == "all" || w == "screen" {
                continue;
            }
            if w == "print" || w.starts_with("not ") || w == "?" {
                ok = false;
                continue;
            }
            if let Some(inner) = w.strip_prefix('(').and_then(|x| x.strip_suffix(')')) {
                let mut parts = inner.splitn(2, ':');
                let name = parts.next().unwrap_or("").trim();
                let val: f32 = parts.next().unwrap_or("").trim().trim_end_matches("px").parse().unwrap_or(f32::NAN);
                ok &= match name {
                    "min-width" => vp.width >= val,
                    "max-width" => vp.width <= val,
                    "min-height" => vp.height >= val,
                    "max-height" => vp.height <= val,
                    "color" | "orientation" => true,
                    _ => false,
                };
            }
        }
        ok
    })
}

pub fn parse_sheet(src: &str, origin: u8, order_base: usize, vp: &Viewport) -> Sheet {
    let mut sheet = Sheet { rules: vec![], font_faces: vec![], imports: vec![] };
    let rules = syntax::parse_stylesheet(src);
    let mut order = order_base;
    flatten(&rules, origin, &mut order, vp, &mut sheet);
    sheet
}

fn flatten(rules: &[Rule], origin: u8, order: &mut usize, vp: &Viewport, sheet: &mut Sheet) {
    for r in rules {
        match r {
            Rule::Style { prelude, decls } => {
                let Some(sels) = selector::parse_list(prelude) else { continue };
                let decls = std::rc::Rc::new(expand(decls));
                for s in sels {
                    sheet.rules.push(StyleRule { selector: s, decls: decls.clone(), origin, order: *order });
                    *order += 1;
                }
            }
            Rule::At { name, prelude, block } => match name.as_str() {
                "media" => {
                    if media_matches(prelude, vp) {
                        if let Some(b) = block {
                            let inner = syntax::Parser::new(b).rules(false);
                            flatten(&inner, origin, order, vp, sheet);
                        }
                    }
                }
                "supports" => {
                    if let Some(b) = block {
                        let inner = syntax::Parser::new(b).rules(false);
                        flatten(&inner, origin, order, vp, sheet);
                    }
                }
                "import" => {
                    for t in prelude {
                        match t {
                            Tok::Url(u) | Tok::Str(u) => {
                                sheet.imports.push(u.clone());
                                break;
                            }
                            _ => {}
                        }
                    }
                }
                "font-face" => {
                    if let Some(b) = block {
                        let d = syntax::declarations(b);
                        let mut fam = String::new();
                        let mut url = String::new();
                        let mut weight = 400;
                        let mut italic = false;
                        for x in d {
                            match x.name.as_str() {
                                "font-family" => {
                                    for t in &x.value {
                                        match t {
                                            Tok::Str(s) | Tok::Ident(s) => fam = s.clone(),
                                            _ => {}
                                        }
                                    }
                                }
                                "src" => {
                                    for t in &x.value {
                                        if let Tok::Url(u) = t {
                                            if url.is_empty() {
                                                url = u.clone();
                                            }
                                        }
                                    }
                                    // url("...") tokenizes as a function with a string argument
                                    let mut k = 0;
                                    while k + 1 < x.value.len() {
                                        if let (Tok::Function(f), Tok::Str(s)) = (&x.value[k], &x.value[k + 1]) {
                                            if f.eq_ignore_ascii_case("url") && url.is_empty() {
                                                url = s.clone();
                                            }
                                        }
                                        k += 1;
                                    }
                                }
                                "font-weight" => {
                                    if let Some(Tok::Number(v, _)) = x.value.first() {
                                        weight = *v as u16;
                                    } else if let Some(Tok::Ident(v)) = x.value.first() {
                                        if v == "bold" {
                                            weight = 700;
                                        }
                                    }
                                }
                                "font-style" => italic = matches!(x.value.first(), Some(Tok::Ident(v)) if v == "italic" || v == "oblique"),
                                _ => {}
                            }
                        }
                        if !fam.is_empty() && !url.is_empty() {
                            sheet.font_faces.push((fam, url, weight, italic));
                        }
                    }
                }
                _ => {}
            },
        }
    }
}

// ------------------------------------------------------------------ shorthands
fn split_ws(v: &[Tok]) -> Vec<Vec<Tok>> {
    let mut out: Vec<Vec<Tok>> = vec![];
    let mut cur = vec![];
    let mut depth = 0;
    for t in v {
        match t {
            Tok::Ws if depth == 0 => {
                if !cur.is_empty() {
                    out.push(std::mem::take(&mut cur));
                }
                continue;
            }
            Tok::Function(_) | Tok::LParen => depth += 1,
            Tok::RParen => depth -= 1,
            _ => {}
        }
        cur.push(t.clone());
    }
    if !cur.is_empty() {
        out.push(cur);
    }
    out
}
fn split_slash(v: &[Tok]) -> (Vec<Tok>, Option<Vec<Tok>>) {
    let mut depth = 0;
    for (i, t) in v.iter().enumerate() {
        match t {
            Tok::Function(_) | Tok::LParen => depth += 1,
            Tok::RParen => depth -= 1,
            Tok::Delim('/') if depth == 0 => return (v[..i].to_vec(), Some(v[i + 1..].to_vec())),
            _ => {}
        }
    }
    (v.to_vec(), None)
}
fn d(name: &str, value: Vec<Tok>, important: bool) -> Declaration {
    Declaration { name: name.to_string(), value, important }
}
fn is_global(v: &[Tok]) -> bool {
    matches!(v, [Tok::Ident(s)] if matches!(s.to_ascii_lowercase().as_str(), "inherit" | "initial" | "unset" | "revert" | "revert-layer"))
}
const SIDES: [&str; 4] = ["top", "right", "bottom", "left"];

fn box4(name_of: impl Fn(&str) -> String, v: &[Tok], imp: bool, out: &mut Vec<Declaration>) {
    if is_global(v) {
        for s in SIDES {
            out.push(d(&name_of(s), v.to_vec(), imp));
        }
        return;
    }
    let p = split_ws(v);
    let (t, r, b, l) = match p.len() {
        1 => (&p[0], &p[0], &p[0], &p[0]),
        2 => (&p[0], &p[1], &p[0], &p[1]),
        3 => (&p[0], &p[1], &p[2], &p[1]),
        4 => (&p[0], &p[1], &p[2], &p[3]),
        _ => return,
    };
    for (s, x) in SIDES.iter().zip([t, r, b, l]) {
        out.push(d(&name_of(s), x.clone(), imp));
    }
}

fn is_border_style(t: &Tok) -> bool {
    matches!(t, Tok::Ident(s) if matches!(s.to_ascii_lowercase().as_str(),
        "none" | "hidden" | "solid" | "dashed" | "dotted" | "double" | "groove" | "ridge" | "inset" | "outset"))
}
fn is_width_keyword(t: &Tok) -> bool {
    matches!(t, Tok::Ident(s) if matches!(s.to_ascii_lowercase().as_str(), "thin" | "medium" | "thick"))
}

/// border-* shorthand parts: (width, style, color), defaults when missing
fn border_parts(v: &[Tok]) -> Option<(Vec<Tok>, Vec<Tok>, Vec<Tok>)> {
    let mut w = None;
    let mut s = None;
    let mut c = None;
    for p in split_ws(v) {
        if p.len() == 1 && is_border_style(&p[0]) && s.is_none() {
            s = Some(p);
        } else if p.len() == 1 && (is_width_keyword(&p[0]) || matches!(p[0], Tok::Dimension(..)) || p[0] == Tok::Number(0.0, true))
            && w.is_none()
        {
            w = Some(p);
        } else if matches!(p.first(), Some(Tok::Function(f)) if f.eq_ignore_ascii_case("calc")) && w.is_none() {
            w = Some(p);
        } else if c.is_none() {
            let mut i = 0;
            parse_color(&p, &mut i)?;
            if i != p.len() {
                return None;
            }
            c = Some(p);
        } else {
            return None;
        }
    }
    Some((
        w.unwrap_or_else(|| vec![Tok::Ident("medium".into())]),
        s.unwrap_or_else(|| vec![Tok::Ident("none".into())]),
        c.unwrap_or_else(|| vec![Tok::Ident("currentcolor".into())]),
    ))
}

/// expands shorthand declarations into longhands
pub fn expand(decls: &[Declaration]) -> Vec<Declaration> {
    let mut out = vec![];
    for x in decls {
        let v = &x.value;
        let imp = x.important;
        match x.name.as_str() {
            "margin" => box4(|s| format!("margin-{s}"), v, imp, &mut out),
            "padding" => box4(|s| format!("padding-{s}"), v, imp, &mut out),
            "inset" => box4(|s| s.to_string(), v, imp, &mut out),
            "border-width" => box4(|s| format!("border-{s}-width"), v, imp, &mut out),
            "border-style" => box4(|s| format!("border-{s}-style"), v, imp, &mut out),
            "border-color" => box4(|s| format!("border-{s}-color"), v, imp, &mut out),
            "border" | "border-top" | "border-right" | "border-bottom" | "border-left" => {
                let sides: Vec<&str> = if x.name == "border" { SIDES.to_vec() } else { vec![&x.name[7..]] };
                if is_global(v) {
                    for s in &sides {
                        for p in ["width", "style", "color"] {
                            out.push(d(&format!("border-{s}-{p}"), v.clone(), imp));
                        }
                    }
                    continue;
                }
                if let Some((w, s_, c)) = border_parts(v) {
                    for s in &sides {
                        out.push(d(&format!("border-{s}-width"), w.clone(), imp));
                        out.push(d(&format!("border-{s}-style"), s_.clone(), imp));
                        out.push(d(&format!("border-{s}-color"), c.clone(), imp));
                    }
                }
            }
            "outline" => {
                if let Some((w, s_, c)) = border_parts(v) {
                    out.push(d("outline-width", w, imp));
                    out.push(d("outline-style", s_, imp));
                    out.push(d("outline-color", c, imp));
                }
            }
            "background" => {
                if is_global(v) {
                    for p in ["background-color", "background-image", "background-repeat", "background-position"] {
                        out.push(d(p, v.clone(), imp));
                    }
                    continue;
                }
                // the color, an image url, repeat keywords; the rest is ignored
                let mut color = vec![Tok::Ident("transparent".into())];
                let mut image = vec![Tok::Ident("none".into())];
                let mut repeat = vec![Tok::Ident("repeat".into())];
                let mut pos: Vec<Tok> = vec![];
                for p in split_ws(&split_slash(v).0) {
                    match p.first() {
                        Some(Tok::Url(_)) => image = p,
                        Some(Tok::Function(f)) if f.eq_ignore_ascii_case("url") => image = p,
                        Some(Tok::Ident(i)) if matches!(i.to_ascii_lowercase().as_str(), "repeat" | "no-repeat" | "repeat-x" | "repeat-y") => repeat = p,
                        Some(Tok::Ident(i)) if matches!(i.to_ascii_lowercase().as_str(), "none") => image = p,
                        Some(Tok::Ident(i)) if matches!(i.to_ascii_lowercase().as_str(), "left" | "right" | "top" | "bottom" | "center") => {
                            if !pos.is_empty() {
                                pos.push(Tok::Ws);
                            }
                            pos.extend(p);
                        }
                        Some(Tok::Ident(i)) if matches!(i.to_ascii_lowercase().as_str(), "scroll" | "fixed" | "local" | "border-box" | "padding-box" | "content-box") => {}
                        Some(Tok::Dimension(..)) | Some(Tok::Percentage(_)) | Some(Tok::Number(..)) => {
                            if !pos.is_empty() {
                                pos.push(Tok::Ws);
                            }
                            pos.extend(p);
                        }
                        _ => {
                            let mut i = 0;
                            if parse_color(&p, &mut i).is_some() && i == p.len() {
                                color = p;
                            }
                        }
                    }
                }
                out.push(d("background-color", color, imp));
                out.push(d("background-image", image, imp));
                out.push(d("background-repeat", repeat, imp));
                if pos.is_empty() {
                    pos = vec![Tok::Percentage(0.0), Tok::Ws, Tok::Percentage(0.0)];
                }
                out.push(d("background-position", pos, imp));
            }
            "font" => {
                if is_global(v) {
                    for p in ["font-style", "font-weight", "font-size", "line-height", "font-family"] {
                        out.push(d(p, v.clone(), imp));
                    }
                    continue;
                }
                // [style || weight || variant]? size [/ line-height]? family
                let parts = split_ws(v);
                let mut style = vec![Tok::Ident("normal".into())];
                let mut weight = vec![Tok::Ident("normal".into())];
                let mut i = 0;
                while i < parts.len() {
                    match parts[i].first() {
                        Some(Tok::Ident(s)) if matches!(s.to_ascii_lowercase().as_str(), "italic" | "oblique") => style = parts[i].clone(),
                        Some(Tok::Ident(s)) if matches!(s.to_ascii_lowercase().as_str(), "bold" | "bolder" | "lighter") => weight = parts[i].clone(),
                        Some(Tok::Ident(s)) if matches!(s.to_ascii_lowercase().as_str(), "normal" | "small-caps") => {}
                        Some(Tok::Number(n, _)) if *n >= 1.0 && *n <= 1000.0 && parts[i].len() == 1 => weight = parts[i].clone(),
                        _ => break,
                    }
                    i += 1;
                }
                if i >= parts.len() {
                    continue;
                }
                // size (possibly "size/line-height" in one part, or "/" separate)
                let rest: Vec<Tok> = {
                    let mut r = vec![];
                    for (k, p) in parts[i..].iter().enumerate() {
                        if k > 0 {
                            r.push(Tok::Ws);
                        }
                        r.extend(p.clone());
                    }
                    r
                };
                let (size_lh, family) = {
                    // the family starts after the size (and line-height)
                    let toks: Vec<Tok> = rest.clone();
                    let mut k = 0;
                    let mut size = vec![];
                    while k < toks.len() && toks[k] != Tok::Ws && toks[k] != Tok::Delim('/') {
                        size.push(toks[k].clone());
                        k += 1;
                    }
                    let mut lh = None;
                    let mut kk = k;
                    while kk < toks.len() && toks[kk] == Tok::Ws {
                        kk += 1;
                    }
                    if kk < toks.len() && toks[kk] == Tok::Delim('/') {
                        kk += 1;
                        while kk < toks.len() && toks[kk] == Tok::Ws {
                            kk += 1;
                        }
                        let mut l = vec![];
                        while kk < toks.len() && toks[kk] != Tok::Ws {
                            l.push(toks[kk].clone());
                            kk += 1;
                        }
                        lh = Some(l);
                        k = kk;
                    }
                    while k < toks.len() && toks[k] == Tok::Ws {
                        k += 1;
                    }
                    ((size, lh), toks[k..].to_vec())
                };
                if family.is_empty() {
                    continue;
                }
                out.push(d("font-style", style, imp));
                out.push(d("font-weight", weight, imp));
                out.push(d("font-size", size_lh.0, imp));
                out.push(d("line-height", size_lh.1.unwrap_or_else(|| vec![Tok::Ident("normal".into())]), imp));
                out.push(d("font-family", family, imp));
            }
            "flex" => {
                if is_global(v) {
                    for p in ["flex-grow", "flex-shrink", "flex-basis"] {
                        out.push(d(p, v.clone(), imp));
                    }
                    continue;
                }
                let parts = split_ws(v);
                let one = |n: f64| vec![Tok::Number(n, true)];
                let (g, s, b) = match parts.as_slice() {
                    [p] if matches!(&p[..], [Tok::Ident(i)] if i == "none") => (one(0.0), one(0.0), vec![Tok::Ident("auto".into())]),
                    [p] if matches!(&p[..], [Tok::Ident(i)] if i == "auto") => (one(1.0), one(1.0), vec![Tok::Ident("auto".into())]),
                    [p] if matches!(&p[..], [Tok::Number(..)]) => (p.clone(), one(1.0), vec![Tok::Percentage(0.0)]),
                    [p] => (one(1.0), one(1.0), p.clone()),
                    [a, b] if matches!(&b[..], [Tok::Number(..)]) => (a.clone(), b.clone(), vec![Tok::Percentage(0.0)]),
                    [a, b] => (a.clone(), one(1.0), b.clone()),
                    [a, b, c] => (a.clone(), b.clone(), c.clone()),
                    _ => continue,
                };
                out.push(d("flex-grow", g, imp));
                out.push(d("flex-shrink", s, imp));
                out.push(d("flex-basis", b, imp));
            }
            "flex-flow" => {
                for p in split_ws(v) {
                    match p.first() {
                        Some(Tok::Ident(i)) if i.contains("wrap") => out.push(d("flex-wrap", p, imp)),
                        _ => out.push(d("flex-direction", p, imp)),
                    }
                }
            }
            "gap" | "grid-gap" => {
                let p = split_ws(v);
                if p.is_empty() {
                    continue;
                }
                out.push(d("row-gap", p[0].clone(), imp));
                out.push(d("column-gap", p.get(1).unwrap_or(&p[0]).clone(), imp));
            }
            "list-style" => {
                for p in split_ws(v) {
                    match p.first() {
                        Some(Tok::Ident(i)) if i == "inside" || i == "outside" => out.push(d("list-style-position", p, imp)),
                        Some(Tok::Url(_)) => {}
                        _ => out.push(d("list-style-type", p, imp)),
                    }
                }
            }
            "overflow" => {
                let p = split_ws(v);
                if let Some(first) = p.first() {
                    out.push(d("overflow", first.clone(), imp));
                }
            }
            "text-decoration" => out.push(d("text-decoration-line", v.clone(), imp)),
            "place-items" => {
                let p = split_ws(v);
                if let Some(first) = p.first() {
                    out.push(d("align-items", first.clone(), imp));
                }
            }
            "place-content" => {
                let p = split_ws(v);
                if let Some(first) = p.first() {
                    out.push(d("align-content", first.clone(), imp));
                    out.push(d("justify-content", p.get(1).unwrap_or(first).clone(), imp));
                }
            }
            "margin-block" | "margin-inline" | "padding-block" | "padding-inline" => {
                let p = split_ws(v);
                if p.is_empty() {
                    continue;
                }
                let base = if x.name.starts_with("margin") { "margin" } else { "padding" };
                let (a, b) = if x.name.ends_with("block") { ("top", "bottom") } else { ("left", "right") };
                out.push(d(&format!("{base}-{a}"), p[0].clone(), imp));
                out.push(d(&format!("{base}-{b}"), p.get(1).unwrap_or(&p[0]).clone(), imp));
            }
            "margin-block-start" => out.push(d("margin-top", v.clone(), imp)),
            "margin-block-end" => out.push(d("margin-bottom", v.clone(), imp)),
            "margin-inline-start" => out.push(d("margin-left", v.clone(), imp)),
            "margin-inline-end" => out.push(d("margin-right", v.clone(), imp)),
            "padding-block-start" => out.push(d("padding-top", v.clone(), imp)),
            "padding-block-end" => out.push(d("padding-bottom", v.clone(), imp)),
            "padding-inline-start" => out.push(d("padding-left", v.clone(), imp)),
            "padding-inline-end" => out.push(d("padding-right", v.clone(), imp)),
            "inline-size" => out.push(d("width", v.clone(), imp)),
            "block-size" => out.push(d("height", v.clone(), imp)),
            "min-inline-size" => out.push(d("min-width", v.clone(), imp)),
            "min-block-size" => out.push(d("min-height", v.clone(), imp)),
            "max-inline-size" => out.push(d("max-width", v.clone(), imp)),
            "max-block-size" => out.push(d("max-height", v.clone(), imp)),
            _ => out.push(x.clone()),
        }
    }
    out
}

// ------------------------------------------------------------------ computing
const INHERITED: &[&str] = &[
    "color", "font-family", "font-size", "font-weight", "font-style", "line-height", "text-align", "white-space",
    "visibility", "list-style-type", "list-style-position", "letter-spacing", "word-spacing", "text-indent",
    "border-collapse", "border-spacing", "text-transform", "direction", "cursor", "quotes",
];

pub struct ComputeCtx<'a> {
    pub parent: &'a Style,
    pub root_font_size: f32,
    pub vw: f32,
    pub vh: f32,
}

fn ident(v: &[Tok]) -> Option<String> {
    match v {
        [Tok::Ident(s)] => Some(s.to_ascii_lowercase()),
        _ => None,
    }
}

fn len_of(v: &[Tok], u: &Units, unitless_zero: bool) -> Option<Len> {
    let mut i = 0;
    let l = parse_len(v, &mut i, u, unitless_zero)?;
    if i != v.len() {
        return None;
    }
    Some(l)
}
fn non_negative(l: Len) -> Option<Len> {
    if l.px < 0.0 || l.pct < 0.0 {
        None
    } else {
        Some(l)
    }
}

fn color_of(v: &[Tok], current: Color) -> Option<Color> {
    let mut i = 0;
    let c = parse_color(v, &mut i)?;
    if i != v.len() {
        return None;
    }
    Some(match c {
        ColorValue::Color(c) => c,
        ColorValue::Current => current,
    })
}

fn border_style_of(s: &str) -> Option<BorderStyle> {
    Some(match s {
        "none" => BorderStyle::None,
        "hidden" => BorderStyle::Hidden,
        "solid" => BorderStyle::Solid,
        "dashed" => BorderStyle::Dashed,
        "dotted" => BorderStyle::Dotted,
        "double" => BorderStyle::Double,
        "groove" => BorderStyle::Groove,
        "ridge" => BorderStyle::Ridge,
        "inset" => BorderStyle::Inset,
        "outset" => BorderStyle::Outset,
        _ => return None,
    })
}

fn align_of(s: &str) -> Option<Align> {
    Some(match s {
        "auto" => Align::Auto,
        "normal" => Align::Normal,
        "stretch" => Align::Stretch,
        "flex-start" | "start" | "self-start" | "left" => Align::FlexStart,
        "flex-end" | "end" | "self-end" | "right" => Align::FlexEnd,
        "center" => Align::Center,
        "baseline" | "first" | "last" => Align::Baseline,
        "space-between" => Align::SpaceBetween,
        "space-around" => Align::SpaceAround,
        "space-evenly" => Align::SpaceEvenly,
        _ => return None,
    })
}

/// applies one declaration (already cascaded) to the style being computed
fn apply(s: &mut Style, name: &str, v: &[Tok], ctx: &ComputeCtx, u: &Units) {
    let id = ident(v);
    let ids = id.as_deref().unwrap_or("");
    macro_rules! set {
        ($field:expr, $val:expr) => {
            if let Some(x) = $val {
                $field = x;
            }
        };
    }
    match name {
        "display" => {
            // "block flow", "inline flow-root" etc.: take the meaningful words
            let words: Vec<String> = v.iter().filter_map(|t| if let Tok::Ident(i) = t { Some(i.to_ascii_lowercase()) } else { None }).collect();
            let w: Vec<&str> = words.iter().map(|x| x.as_str()).collect();
            let dsp = match w.as_slice() {
                ["none"] => Some(Display::None),
                ["block"] | ["block", "flow"] => Some(Display::Block),
                ["inline"] | ["inline", "flow"] => Some(Display::Inline),
                ["inline-block"] | ["inline", "flow-root"] => Some(Display::InlineBlock),
                ["list-item"] | ["block", "list-item"] | ["list-item", "block"] => Some(Display::ListItem),
                ["flex"] | ["block", "flex"] => Some(Display::Flex),
                ["inline-flex"] | ["inline", "flex"] => Some(Display::InlineFlex),
                ["grid"] => Some(Display::Block),
                ["inline-grid"] => Some(Display::InlineBlock),
                ["flow-root"] | ["block", "flow-root"] => Some(Display::FlowRoot),
                ["table"] => Some(Display::Table),
                ["inline-table"] => Some(Display::InlineTable),
                ["table-row-group"] => Some(Display::TableRowGroup),
                ["table-header-group"] => Some(Display::TableHeaderGroup),
                ["table-footer-group"] => Some(Display::TableFooterGroup),
                ["table-row"] => Some(Display::TableRow),
                ["table-cell"] => Some(Display::TableCell),
                ["table-column"] => Some(Display::TableColumn),
                ["table-column-group"] => Some(Display::TableColumnGroup),
                ["table-caption"] => Some(Display::TableCaption),
                ["contents"] => Some(Display::Contents),
                ["run-in"] => Some(Display::Block),
                _ => None,
            };
            set!(s.display, dsp);
        }
        "position" => set!(s.position, match ids {
            "static" => Some(Position::Static),
            "relative" => Some(Position::Relative),
            "absolute" => Some(Position::Absolute),
            "fixed" => Some(Position::Fixed),
            "sticky" => Some(Position::Sticky),
            _ => None,
        }),
        "float" => set!(s.float, match ids {
            "none" => Some(Float::None),
            "left" | "inline-start" => Some(Float::Left),
            "right" | "inline-end" => Some(Float::Right),
            _ => None,
        }),
        "clear" => set!(s.clear, match ids {
            "none" => Some(Clear::None),
            "left" | "inline-start" => Some(Clear::Left),
            "right" | "inline-end" => Some(Clear::Right),
            "both" => Some(Clear::Both),
            _ => None,
        }),
        "top" | "right" | "bottom" | "left" => {
            let k = ["top", "right", "bottom", "left"].iter().position(|x| *x == name).unwrap();
            if ids == "auto" {
                s.inset[k] = None;
            } else if let Some(l) = len_of(v, u, true) {
                s.inset[k] = Some(l);
            }
        }
        "width" | "height" => {
            let f = if name == "width" { &mut s.width } else { &mut s.height };
            if ids == "auto" {
                *f = None;
            } else if let Some(l) = len_of(v, u, true).and_then(non_negative) {
                *f = Some(l);
            } else if matches!(ids, "min-content" | "max-content" | "fit-content" | "-moz-fit-content" | "-webkit-fit-content") {
                *f = None;
            }
        }
        "min-width" | "min-height" => {
            let f = if name == "min-width" { &mut s.min_width } else { &mut s.min_height };
            if ids == "auto" {
                *f = None;
            } else if let Some(l) = len_of(v, u, true).and_then(non_negative) {
                *f = Some(l);
            }
        }
        "max-width" | "max-height" => {
            let f = if name == "max-width" { &mut s.max_width } else { &mut s.max_height };
            if ids == "none" {
                *f = None;
            } else if let Some(l) = len_of(v, u, true).and_then(non_negative) {
                *f = Some(l);
            }
        }
        "margin-top" | "margin-right" | "margin-bottom" | "margin-left" => {
            let k = SIDES.iter().position(|x| *x == &name[7..]).unwrap();
            if ids == "auto" {
                s.margin[k] = None;
            } else if let Some(l) = len_of(v, u, true) {
                s.margin[k] = Some(l);
            }
        }
        "padding-top" | "padding-right" | "padding-bottom" | "padding-left" => {
            let k = SIDES.iter().position(|x| *x == &name[8..]).unwrap();
            set!(s.padding[k], len_of(v, u, true).and_then(non_negative));
        }
        n if n.starts_with("border-") && n.ends_with("-width") && n.len() > 13 => {
            let side = &n[7..n.len() - 6];
            let Some(k) = SIDES.iter().position(|x| *x == side) else { return };
            let w = match ids {
                "thin" => Some(1.0),
                "medium" => Some(3.0),
                "thick" => Some(5.0),
                _ => len_of(v, u, true).filter(|l| !l.has_pct() && l.px >= 0.0).map(|l| l.px),
            };
            // widths snap to whole device pixels (at least 1px when non-zero)
            set!(s.border_width[k], w.map(|w| if w > 0.0 && w < 1.0 { 1.0 } else { w.floor() }));
        }
        n if n.starts_with("border-") && n.ends_with("-style") => {
            let side = &n[7..n.len() - 6];
            let Some(k) = SIDES.iter().position(|x| *x == side) else { return };
            set!(s.border_style[k], border_style_of(ids));
        }
        n if n.starts_with("border-") && n.ends_with("-color") => {
            let side = &n[7..n.len() - 6];
            let Some(k) = SIDES.iter().position(|x| *x == side) else { return };
            set!(s.border_color[k], color_of(v, s.color));
        }
        "outline-width" => {
            let w = match ids {
                "thin" => Some(1.0),
                "medium" => Some(3.0),
                "thick" => Some(5.0),
                _ => len_of(v, u, true).map(|l| l.px),
            };
            set!(s.outline_width, w);
        }
        "outline-style" => set!(s.outline_style, border_style_of(ids)),
        "outline-color" => set!(s.outline_color, color_of(v, s.color)),
        "box-sizing" => set!(s.box_sizing, match ids {
            "content-box" => Some(BoxSizing::ContentBox),
            "border-box" => Some(BoxSizing::BorderBox),
            _ => None,
        }),
        "color" => set!(s.color, color_of(v, ctx.parent.color)),
        "background-color" => set!(s.background_color, color_of(v, s.color)),
        "background-image" => {
            if ids == "none" {
                s.background_image = None;
            } else if let Some(Tok::Url(u)) = v.first() {
                s.background_image = Some(u.clone());
            } else if let (Some(Tok::Function(f)), Some(Tok::Str(u))) = (v.first(), v.get(1)) {
                if f.eq_ignore_ascii_case("url") {
                    s.background_image = Some(u.clone());
                }
            }
        }
        "background-repeat" => set!(s.background_repeat, match ids {
            "repeat" => Some((true, true)),
            "no-repeat" => Some((false, false)),
            "repeat-x" => Some((true, false)),
            "repeat-y" => Some((false, true)),
            _ => None,
        }),
        "background-position" => {
            let parts = split_ws(v);
            let kw = |p: &[Tok]| -> Option<(Option<bool>, Len)> {
                // (is horizontal?, value)
                match ident(p).as_deref() {
                    Some("left") => Some((Some(true), Len { px: 0.0, pct: 0.0 })),
                    Some("right") => Some((Some(true), Len { px: 0.0, pct: 100.0 })),
                    Some("top") => Some((Some(false), Len { px: 0.0, pct: 0.0 })),
                    Some("bottom") => Some((Some(false), Len { px: 0.0, pct: 100.0 })),
                    Some("center") => Some((None, Len { px: 0.0, pct: 50.0 })),
                    _ => len_of(p, u, true).map(|l| (None, l)),
                }
            };
            let vals: Vec<(Option<bool>, Len)> = parts.iter().filter_map(|p| kw(p)).collect();
            match vals.as_slice() {
                [(h, a)] => {
                    if *h == Some(false) {
                        s.background_position = (Len { px: 0.0, pct: 50.0 }, *a);
                    } else {
                        s.background_position = (*a, Len { px: 0.0, pct: 50.0 });
                    }
                }
                [(h1, a), (_, b)] => {
                    if *h1 == Some(false) {
                        s.background_position = (*b, *a);
                    } else {
                        s.background_position = (*a, *b);
                    }
                }
                _ => {}
            }
        }
        "font-family" => {
            let mut fams = vec![];
            let mut cur = String::new();
            for t in v {
                match t {
                    Tok::Ident(i) => {
                        if !cur.is_empty() {
                            cur.push(' ');
                        }
                        cur.push_str(i);
                    }
                    Tok::Str(st) => cur = st.clone(),
                    Tok::Comma => {
                        if !cur.is_empty() {
                            fams.push(std::mem::take(&mut cur));
                        }
                    }
                    _ => {}
                }
            }
            if !cur.is_empty() {
                fams.push(cur);
            }
            if !fams.is_empty() {
                s.font_family = fams;
            }
        }
        "font-size" => {
            let p = ctx.parent.font_size;
            let sz = match ids {
                "xx-small" => Some(9.0),
                "x-small" => Some(10.0),
                "small" => Some(13.0),
                "medium" => Some(16.0),
                "large" => Some(18.0),
                "x-large" => Some(24.0),
                "xx-large" => Some(32.0),
                "xxx-large" => Some(48.0),
                "smaller" => Some(p / 1.2),
                "larger" => Some(p * 1.2),
                _ => {
                    // em and % are relative to the parent's size
                    let pu = Units { em: p, ex: p * 0.5, ch: p * 0.5, ..*u };
                    len_of(v, &pu, true).filter(|l| l.px >= 0.0 && l.pct >= 0.0).map(|l| l.resolve(p))
                }
            };
            set!(s.font_size, sz);
        }
        "font-weight" => {
            let p = ctx.parent.font_weight;
            let w = match ids {
                "normal" => Some(400),
                "bold" => Some(700),
                "bolder" => Some(if p < 350 { 400 } else if p < 550 { 700 } else { 900 }),
                "lighter" => Some(if p < 550 { 100 } else if p < 750 { 400 } else { 700 }),
                _ => match v {
                    [Tok::Number(n, _)] if *n >= 1.0 && *n <= 1000.0 => Some(*n as u16),
                    _ => None,
                },
            };
            set!(s.font_weight, w);
        }
        "font-style" => set!(s.italic, match ids {
            "normal" => Some(false),
            "italic" | "oblique" => Some(true),
            _ => None,
        }),
        "line-height" => {
            let lh = match v {
                [Tok::Ident(i)] if i.eq_ignore_ascii_case("normal") => Some(LineHeight::Normal),
                [Tok::Number(n, _)] if *n >= 0.0 => Some(LineHeight::Number(*n as f32)),
                _ => len_of(v, u, false).filter(|l| l.px >= 0.0 && l.pct >= 0.0).map(|l| LineHeight::Px(l.resolve(s.font_size))),
            };
            set!(s.line_height, lh);
        }
        "text-align" => set!(s.text_align, match ids {
            "left" | "start" | "-webkit-left" => Some(TextAlign::Left),
            "right" | "end" | "-webkit-right" => Some(TextAlign::Right),
            "center" | "-webkit-center" => Some(TextAlign::Center),
            "justify" => Some(TextAlign::Justify),
            _ => None,
        }),
        "vertical-align" => {
            let va = match ids {
                "baseline" => Some(VAlign::Baseline),
                "top" => Some(VAlign::Top),
                "middle" => Some(VAlign::Middle),
                "bottom" => Some(VAlign::Bottom),
                "text-top" => Some(VAlign::TextTop),
                "text-bottom" => Some(VAlign::TextBottom),
                "sub" => Some(VAlign::Sub),
                "super" => Some(VAlign::Super),
                _ => len_of(v, u, true).map(|l| {
                    let lh = match s.line_height {
                        LineHeight::Px(p) => p,
                        LineHeight::Number(n) => n * s.font_size,
                        LineHeight::Normal => s.font_size * 1.2,
                    };
                    VAlign::Len(l.resolve(lh))
                }),
            };
            set!(s.vertical_align, va);
        }
        "white-space" | "white-space-collapse" => set!(s.white_space, match ids {
            "normal" => Some(WhiteSpace::Normal),
            "pre" | "preserve" => Some(WhiteSpace::Pre),
            "nowrap" => Some(WhiteSpace::NoWrap),
            "pre-wrap" | "break-spaces" => Some(WhiteSpace::PreWrap),
            "pre-line" | "preserve-breaks" => Some(WhiteSpace::PreLine),
            _ => None,
        }),
        "text-decoration-line" => {
            let words: Vec<String> = v.iter().filter_map(|t| if let Tok::Ident(i) = t { Some(i.to_ascii_lowercase()) } else { None }).collect();
            if words.iter().any(|w| w == "none") {
                s.underline = false;
                s.line_through = false;
                s.overline = false;
            }
            for w in words {
                match w.as_str() {
                    "underline" => s.underline = true,
                    "line-through" => s.line_through = true,
                    "overline" => s.overline = true,
                    _ => {}
                }
            }
        }
        "text-indent" => set!(s.text_indent, len_of(v, u, true)),
        "letter-spacing" => set!(s.letter_spacing, if ids == "normal" { Some(0.0) } else { len_of(v, u, true).map(|l| l.px) }),
        "word-spacing" => set!(s.word_spacing, if ids == "normal" { Some(0.0) } else { len_of(v, u, true).map(|l| l.px) }),
        "text-transform" => set!(s.text_transform, match ids {
            "none" => Some(0),
            "uppercase" => Some(1),
            "lowercase" => Some(2),
            "capitalize" => Some(3),
            _ => None,
        }),
        "overflow" | "overflow-x" | "overflow-y" => set!(s.overflow, match ids {
            "visible" => Some(Overflow::Visible),
            "hidden" => Some(Overflow::Hidden),
            "scroll" => Some(Overflow::Scroll),
            "auto" => Some(Overflow::Auto),
            "clip" => Some(Overflow::Clip),
            _ => None,
        }),
        "visibility" => set!(s.visible, match ids {
            "visible" => Some(true),
            "hidden" | "collapse" => Some(false),
            _ => None,
        }),
        "z-index" => match v {
            [Tok::Number(n, true)] => s.z_index = Some(*n as i32),
            [Tok::Ident(i)] if i == "auto" => s.z_index = None,
            _ => {}
        },
        "opacity" => match v {
            [Tok::Number(n, _)] => s.opacity = (*n as f32).clamp(0.0, 1.0),
            [Tok::Percentage(p)] => s.opacity = (*p as f32 / 100.0).clamp(0.0, 1.0),
            _ => {}
        },
        "flex-direction" => set!(s.flex_direction, match ids {
            "row" => Some(FlexDirection::Row),
            "row-reverse" => Some(FlexDirection::RowReverse),
            "column" => Some(FlexDirection::Column),
            "column-reverse" => Some(FlexDirection::ColumnReverse),
            _ => None,
        }),
        "flex-wrap" => match ids {
            "nowrap" => {
                s.flex_wrap = false;
                s.wrap_reverse = false;
            }
            "wrap" => {
                s.flex_wrap = true;
                s.wrap_reverse = false;
            }
            "wrap-reverse" => {
                s.flex_wrap = true;
                s.wrap_reverse = true;
            }
            _ => {}
        },
        "justify-content" => set!(s.justify_content, last_ident(v).and_then(|x| align_of(&x))),
        "align-items" => set!(s.align_items, last_ident(v).and_then(|x| align_of(&x))),
        "align-self" => set!(s.align_self, last_ident(v).and_then(|x| align_of(&x))),
        "align-content" => set!(s.align_content, last_ident(v).and_then(|x| align_of(&x))),
        "flex-grow" => {
            if let [Tok::Number(n, _)] = v {
                if *n >= 0.0 {
                    s.flex_grow = *n as f32;
                }
            }
        }
        "flex-shrink" => {
            if let [Tok::Number(n, _)] = v {
                if *n >= 0.0 {
                    s.flex_shrink = *n as f32;
                }
            }
        }
        "flex-basis" => match ids {
            "auto" => {
                s.flex_basis = None;
                s.flex_basis_content = false;
            }
            "content" => {
                s.flex_basis = None;
                s.flex_basis_content = true;
            }
            _ => {
                if let Some(l) = len_of(v, u, true).and_then(non_negative) {
                    s.flex_basis = Some(l);
                    s.flex_basis_content = false;
                }
            }
        },
        "order" => {
            if let [Tok::Number(n, true)] = v {
                s.order = *n as i32;
            }
        }
        "row-gap" => set!(s.row_gap, if ids == "normal" { Some(Len::ZERO) } else { len_of(v, u, true) }),
        "column-gap" => set!(s.column_gap, if ids == "normal" { Some(Len::ZERO) } else { len_of(v, u, true) }),
        "list-style-type" => set!(s.list_style, match ids {
            "none" => Some(ListStyle::None),
            "disc" => Some(ListStyle::Disc),
            "circle" => Some(ListStyle::Circle),
            "square" => Some(ListStyle::Square),
            "decimal" | "decimal-leading-zero" => Some(ListStyle::Decimal),
            "lower-alpha" | "lower-latin" => Some(ListStyle::LowerAlpha),
            "upper-alpha" | "upper-latin" => Some(ListStyle::UpperAlpha),
            "lower-roman" => Some(ListStyle::LowerRoman),
            "upper-roman" => Some(ListStyle::UpperRoman),
            _ => None,
        }),
        "list-style-position" => set!(s.list_inside, match ids {
            "inside" => Some(true),
            "outside" => Some(false),
            _ => None,
        }),
        "content" => {
            if ids == "none" || ids == "normal" {
                s.content = None;
            } else {
                let mut items = vec![];
                let mut i = 0;
                while i < v.len() {
                    match &v[i] {
                        Tok::Str(st) => items.push(ContentItem::Text(st.clone())),
                        Tok::Function(f) if f.eq_ignore_ascii_case("attr") => {
                            if let Some(Tok::Ident(a)) = v.get(i + 1) {
                                items.push(ContentItem::Attr(a.to_ascii_lowercase()));
                            }
                            while i < v.len() && v[i] != Tok::RParen {
                                i += 1;
                            }
                        }
                        Tok::Function(f) if f.eq_ignore_ascii_case("counter") => {
                            if let Some(Tok::Ident(a)) = v.get(i + 1) {
                                items.push(ContentItem::Counter(a.clone()));
                            }
                            while i < v.len() && v[i] != Tok::RParen {
                                i += 1;
                            }
                        }
                        Tok::Ident(q) if q == "open-quote" => items.push(ContentItem::OpenQuote),
                        Tok::Ident(q) if q == "close-quote" => items.push(ContentItem::CloseQuote),
                        _ => {}
                    }
                    i += 1;
                }
                s.content = Some(items);
            }
        }
        "border-collapse" => set!(s.border_collapse, match ids {
            "collapse" => Some(true),
            "separate" => Some(false),
            _ => None,
        }),
        "border-spacing" => {
            let p: Vec<f32> = split_ws(v).iter().filter_map(|x| len_of(x, u, true).map(|l| l.px)).collect();
            match p.as_slice() {
                [a] => s.border_spacing = (*a, *a),
                [a, b] => s.border_spacing = (*a, *b),
                _ => {}
            }
        }
        "table-layout" => set!(s.table_layout_fixed, match ids {
            "fixed" => Some(true),
            "auto" => Some(false),
            _ => None,
        }),
        "direction" => set!(s.direction_rtl, match ids {
            "rtl" => Some(true),
            "ltr" => Some(false),
            _ => None,
        }),
        _ => {}
    }
}
fn last_ident(v: &[Tok]) -> Option<String> {
    v.iter().rev().find_map(|t| if let Tok::Ident(i) = t { Some(i.to_ascii_lowercase()) } else { None })
}

/// the cascaded declarations for an element (or one of its pseudo-elements), computed
pub fn compute(winners: &[(&str, &[Tok])], ctx: &ComputeCtx) -> Style {
    let mut s = Style::default();
    // inherited properties start from the parent
    let p = ctx.parent;
    s.color = p.color;
    s.font_family = p.font_family.clone();
    s.font_size = p.font_size;
    s.font_weight = p.font_weight;
    s.italic = p.italic;
    s.line_height = p.line_height;
    s.text_align = p.text_align;
    s.white_space = p.white_space;
    s.visible = p.visible;
    s.list_style = p.list_style;
    s.list_inside = p.list_inside;
    s.letter_spacing = p.letter_spacing;
    s.word_spacing = p.word_spacing;
    s.text_indent = p.text_indent;
    s.border_collapse = p.border_collapse;
    s.border_spacing = p.border_spacing;
    s.text_transform = p.text_transform;
    s.direction_rtl = p.direction_rtl;
    // two passes: font-size and color first (other values depend on them)
    let units = |s: &Style| Units {
        em: s.font_size,
        rem: ctx.root_font_size,
        vw: ctx.vw,
        vh: ctx.vh,
        ex: s.font_size * 0.5,
        ch: s.font_size * 0.55,
    };
    let mut border_colors_set = [false; 4];
    for pass in 0..2 {
        for (name, v) in winners {
            let early = matches!(*name, "font-size" | "color" | "font-family" | "font-weight" | "font-style" | "line-height");
            if (pass == 0) != early {
                continue;
            }
            // global keywords
            if let Some(g) = ident(v) {
                let inherited = INHERITED.contains(name);
                let action = match g.as_str() {
                    "inherit" => Some(true),
                    "initial" => Some(false),
                    "unset" | "revert" | "revert-layer" => Some(inherited),
                    _ => None,
                };
                if let Some(inherit) = action {
                    let initial = Style::default();
                    copy_prop(&mut s, name, if inherit { p } else { &initial }, inherit);
                    if name.starts_with("border-") && name.ends_with("-color") {
                        if let Some(k) = SIDES.iter().position(|x| name[7..].starts_with(x)) {
                            border_colors_set[k] = inherit;
                        }
                    }
                    continue;
                }
            }
            let u = units(&s);
            apply(&mut s, name, v, ctx, &u);
            if name.starts_with("border-") && name.ends_with("-color") {
                if let Some(k) = SIDES.iter().position(|x| name[7..].starts_with(x)) {
                    border_colors_set[k] = true;
                }
            }
        }
    }
    // border colors default to currentcolor
    for k in 0..4 {
        if !border_colors_set[k] {
            s.border_color[k] = s.color;
        }
    }
    s
}

fn copy_prop(s: &mut Style, name: &str, from: &Style, inherit: bool) {
    let _ = inherit;
    match name {
        "display" => s.display = from.display,
        "position" => s.position = from.position,
        "float" => s.float = from.float,
        "clear" => s.clear = from.clear,
        "top" => s.inset[0] = from.inset[0],
        "right" => s.inset[1] = from.inset[1],
        "bottom" => s.inset[2] = from.inset[2],
        "left" => s.inset[3] = from.inset[3],
        "width" => s.width = from.width,
        "height" => s.height = from.height,
        "min-width" => s.min_width = from.min_width,
        "min-height" => s.min_height = from.min_height,
        "max-width" => s.max_width = from.max_width,
        "max-height" => s.max_height = from.max_height,
        "color" => s.color = from.color,
        "background-color" => s.background_color = from.background_color,
        "background-image" => s.background_image = from.background_image.clone(),
        "font-family" => s.font_family = from.font_family.clone(),
        "font-size" => s.font_size = from.font_size,
        "font-weight" => s.font_weight = from.font_weight,
        "font-style" => s.italic = from.italic,
        "line-height" => s.line_height = from.line_height,
        "text-align" => s.text_align = from.text_align,
        "vertical-align" => s.vertical_align = from.vertical_align,
        "white-space" => s.white_space = from.white_space,
        "visibility" => s.visible = from.visible,
        "overflow" => s.overflow = from.overflow,
        "box-sizing" => s.box_sizing = from.box_sizing,
        "text-indent" => s.text_indent = from.text_indent,
        "letter-spacing" => s.letter_spacing = from.letter_spacing,
        "word-spacing" => s.word_spacing = from.word_spacing,
        "list-style-type" => s.list_style = from.list_style,
        "list-style-position" => s.list_inside = from.list_inside,
        "z-index" => s.z_index = from.z_index,
        "opacity" => s.opacity = from.opacity,
        "border-collapse" => s.border_collapse = from.border_collapse,
        "border-spacing" => s.border_spacing = from.border_spacing,
        "text-transform" => s.text_transform = from.text_transform,
        "flex-grow" => s.flex_grow = from.flex_grow,
        "flex-shrink" => s.flex_shrink = from.flex_shrink,
        "flex-basis" => s.flex_basis = from.flex_basis,
        n => {
            for (k, side) in SIDES.iter().enumerate() {
                if n == format!("margin-{side}") {
                    s.margin[k] = from.margin[k];
                } else if n == format!("padding-{side}") {
                    s.padding[k] = from.padding[k];
                } else if n == format!("border-{side}-width") {
                    s.border_width[k] = from.border_width[k];
                } else if n == format!("border-{side}-style") {
                    s.border_style[k] = from.border_style[k];
                } else if n == format!("border-{side}-color") {
                    s.border_color[k] = from.border_color[k];
                }
            }
        }
    }
}

// ------------------------------------------------------------------ the cascade over a document
pub struct Styles {
    pub elements: HashMap<NodeId, Style>,
    pub before: HashMap<NodeId, Style>,
    pub after: HashMap<NodeId, Style>,
}

/// cascades every element of the document; `sheets` are in document order (UA first)
pub fn cascade(doc: &Document, sheets: &[Sheet], vp: &Viewport) -> Styles {
    let mut out = Styles { elements: HashMap::new(), before: HashMap::new(), after: HashMap::new() };
    let rules: Vec<&StyleRule> = sheets.iter().flat_map(|s| s.rules.iter()).collect();
    // an index by the subject's id / class / tag for speed
    let mut by_key: HashMap<String, Vec<usize>> = HashMap::new();
    let mut universal = vec![];
    for (i, r) in rules.iter().enumerate() {
        let c = &r.selector.compounds[0];
        if let Some(id) = c.ids.first() {
            by_key.entry(format!("#{id}")).or_default().push(i);
        } else if let Some(cl) = c.classes.first() {
            by_key.entry(format!(".{cl}")).or_default().push(i);
        } else if let Some(t) = &c.tag {
            by_key.entry(t.clone()).or_default().push(i);
        } else {
            universal.push(i);
        }
    }
    let root = Style::default();
    let mut root_font = 16.0;
    // depth-first with the parent style
    fn walk(
        doc: &Document,
        n: NodeId,
        parent: &Style,
        rules: &[&StyleRule],
        by_key: &HashMap<String, Vec<usize>>,
        universal: &[usize],
        vp: &Viewport,
        root_font: &mut f32,
        out: &mut Styles,
    ) {
        let Some(e) = doc.element(n) else {
            for &c in doc.children(n) {
                walk(doc, c, parent, rules, by_key, universal, vp, root_font, out);
            }
            return;
        };
        let mut cand: Vec<usize> = universal.to_vec();
        if let Some(id) = e.attr("id") {
            if let Some(v) = by_key.get(&format!("#{id}")) {
                cand.extend(v);
            }
        }
        if let Some(cl) = e.attr("class") {
            for c in cl.split_ascii_whitespace() {
                if let Some(v) = by_key.get(&format!(".{c}")) {
                    cand.extend(v);
                }
            }
        }
        let lname = e.name.to_ascii_lowercase();
        if let Some(v) = by_key.get(&lname) {
            cand.extend(v);
        }
        cand.sort_unstable();
        cand.dedup();
        let mut matched: [Vec<(u8, bool, (u32, u32, u32), usize, &Declaration)>; 3] = [vec![], vec![], vec![]];
        for i in cand {
            let r = rules[i];
            if !selector::matches(doc, n, &r.selector) {
                continue;
            }
            let slot = match r.selector.pseudo_element {
                None => 0,
                Some(PseudoElement::Before) => 1,
                Some(PseudoElement::After) => 2,
                _ => continue,
            };
            for d in r.decls.iter() {
                matched[slot].push((r.origin, d.important, r.selector.specificity, r.order, d));
            }
        }
        // the style attribute (and presentational hints)
        let inline: Vec<Declaration> = e.attr("style").map(|s| expand(&syntax::declarations(&syntax::tokenize(s)))).unwrap_or_default();
        let hints = presentational_hints(doc, n);
        for d in &hints {
            matched[0].push((1, false, (0, 0, 0), 0, d));
        }
        for d in &inline {
            matched[0].push((2, d.important, (1, 0, 0), usize::MAX, d));
        }
        let cx = ComputeCtx { parent, root_font_size: *root_font, vw: vp.width, vh: vp.height };
        let mut styles = vec![];
        for slot in 0..3 {
            let m = &mut matched[slot];
            if slot > 0 && m.is_empty() {
                styles.push(None);
                continue;
            }
            // precedence: origin+importance, then specificity, then order
            m.sort_by_key(|(origin, important, spec, order, _)| {
                let layer = match (origin, important) {
                    (0, false) => 0,
                    (1, false) => 1,
                    (2, false) => 2,
                    (1, true) => 3,
                    (2, true) => 4,
                    (0, true) => 5,
                    _ => 1,
                };
                (layer, *spec, *order)
            });
            let winners: Vec<(&str, &[Tok])> = m.iter().map(|(_, _, _, _, d)| (d.name.as_str(), d.value.as_slice())).collect();
            let st = if slot == 0 {
                compute(&winners, &cx)
            } else {
                let el = styles[0].as_ref().unwrap();
                let pcx = ComputeCtx { parent: el, root_font_size: *root_font, vw: vp.width, vh: vp.height };
                compute(&winners, &pcx)
            };
            styles.push(Some(st));
        }
        let mut el = styles[0].take().unwrap();
        // the root element's font size is the rem base
        if doc.parent(n) == Some(crate::html::dom::DOCUMENT) {
            *root_font = el.font_size;
            // float/position adjustments of the root are not needed for rendering
            if el.display == Display::Inline {
                el.display = Display::Block;
            }
        }
        // blockification of floats and absolutely positioned boxes
        if el.float != Float::None || matches!(el.position, Position::Absolute | Position::Fixed) {
            el.display = match el.display {
                Display::Inline | Display::InlineBlock | Display::TableRow | Display::TableRowGroup | Display::TableCell
                | Display::TableCaption | Display::TableHeaderGroup | Display::TableFooterGroup | Display::TableColumn
                | Display::TableColumnGroup => Display::Block,
                Display::InlineFlex => Display::Flex,
                Display::InlineTable => Display::Table,
                d => d,
            };
            if matches!(el.position, Position::Absolute | Position::Fixed) {
                el.float = Float::None;
            }
        }
        // flex items are blockified too
        if matches!(parent.display, Display::Flex | Display::InlineFlex) && doc.parent(n).is_some() {
            el.display = match el.display {
                Display::Inline | Display::InlineBlock => Display::Block,
                Display::InlineFlex => Display::Flex,
                Display::InlineTable => Display::Table,
                d => d,
            };
        }
        if let Some(b) = styles[1].take() {
            if b.content.is_some() {
                out.before.insert(n, b);
            }
        }
        if let Some(a) = styles[2].take() {
            if a.content.is_some() {
                out.after.insert(n, a);
            }
        }
        let el_clone = el.clone();
        out.elements.insert(n, el);
        // template contents are not rendered
        if e.ns == Ns::Html && e.name == "template" {
            return;
        }
        for &c in doc.children(n) {
            walk(doc, c, &el_clone, rules, by_key, universal, vp, root_font, out);
        }
    }
    for &c in doc.children(crate::html::dom::DOCUMENT) {
        if matches!(doc.nodes[c].data, NodeData::Element(_)) {
            walk(doc, c, &root, &rules, &by_key, &universal, vp, &mut root_font, &mut out);
        }
    }
    out
}

/// HTML presentational attributes mapped to CSS (a common subset)
fn presentational_hints(doc: &Document, n: NodeId) -> Vec<Declaration> {
    let e = doc.element(n).unwrap();
    if e.ns != Ns::Html {
        return vec![];
    }
    let mut css = String::new();
    let dim = |v: &str| -> Option<String> {
        let v = v.trim();
        if v.is_empty() {
            return None;
        }
        if let Some(p) = v.strip_suffix('%') {
            p.parse::<f32>().ok().map(|x| format!("{x}%"))
        } else {
            let num: String = v.chars().take_while(|c| c.is_ascii_digit() || *c == '.').collect();
            num.parse::<f32>().ok().map(|x| format!("{x}px"))
        }
    };
    if let Some(w) = e.attr("width").and_then(dim) {
        if matches!(e.name.as_str(), "img" | "table" | "td" | "th" | "col" | "hr" | "iframe" | "canvas" | "video" | "object" | "embed") {
            css += &format!("width:{w};");
        }
    }
    if let Some(h) = e.attr("height").and_then(dim) {
        if matches!(e.name.as_str(), "img" | "table" | "td" | "th" | "tr" | "iframe" | "canvas" | "video" | "object" | "embed") {
            css += &format!("height:{h};");
        }
    }
    if let Some(c) = e.attr("bgcolor") {
        css += &format!("background-color:{c};");
    }
    if e.name == "font" {
        if let Some(c) = e.attr("color") {
            css += &format!("color:{c};");
        }
        if let Some(f) = e.attr("face") {
            css += &format!("font-family:{f};");
        }
        if let Some(s) = e.attr("size") {
            let sizes = ["x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large"];
            if let Ok(k) = s.trim().parse::<i32>() {
                let k = if s.trim().starts_with(['+', '-']) { 3 + k } else { k };
                css += &format!("font-size:{};", sizes[(k.clamp(1, 7) - 1) as usize]);
            }
        }
    }
    if e.name == "body" {
        if let Some(c) = e.attr("text") {
            css += &format!("color:{c};");
        }
    }
    if let Some(a) = e.attr("align") {
        let a = a.to_ascii_lowercase();
        if matches!(e.name.as_str(), "div" | "p" | "h1" | "h2" | "h3" | "h4" | "h5" | "h6" | "td" | "th" | "tr") {
            css += &format!("text-align:{a};");
        } else if matches!(e.name.as_str(), "img" | "table") && (a == "left" || a == "right") {
            css += &format!("float:{a};");
        }
    }
    if e.name == "table" {
        if let Some(b) = e.attr("border") {
            let w = b.trim().parse::<f32>().unwrap_or(1.0);
            css += &format!("border:{w}px outset gray;");
        }
        if let Some(cs) = e.attr("cellspacing").and_then(dim) {
            css += &format!("border-spacing:{cs};");
        }
    }
    if e.attr("hidden").is_some() {
        css += "display:none;";
    }
    if css.is_empty() {
        return vec![];
    }
    expand(&syntax::declarations(&syntax::tokenize(&css)))
}
