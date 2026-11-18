//! Selectors: parsing (from rule preludes), specificity and matching.
use super::syntax::Tok;
use crate::html::dom::{Document, NodeData, NodeId, Ns};

#[derive(Clone, Debug, PartialEq)]
pub enum Combinator {
    Descendant,
    Child,
    Next,
    Subsequent,
}

#[derive(Clone, Debug)]
pub enum AttrOp {
    Exists,
    Eq,
    Includes, // ~=
    Dash,     // |=
    Prefix,   // ^=
    Suffix,   // $=
    Substr,   // *=
}

#[derive(Clone, Debug)]
pub struct AttrSel {
    pub name: String,
    pub op: AttrOp,
    pub value: String,
    pub icase: bool,
}

#[derive(Clone, Debug)]
pub enum Pseudo {
    Nth { a: i32, b: i32, last: bool, of_type: bool },
    Root,
    Empty,
    Not(Vec<Selector>),
    Is(Vec<Selector>),    // also :where (zero specificity) and :matches
    Where(Vec<Selector>),
    Link,
    Never, // :hover, :visited, :active, :focus ... (a static render)
    Checked,
    Disabled,
    Enabled,
    Has(Vec<Selector>),
}

#[derive(Clone, Debug, Default)]
pub struct Compound {
    pub tag: Option<String>,
    pub ids: Vec<String>,
    pub classes: Vec<String>,
    pub attrs: Vec<AttrSel>,
    pub pseudos: Vec<Pseudo>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PseudoElement {
    Before,
    After,
    FirstLine,
    FirstLetter,
    Marker,
}

#[derive(Clone, Debug)]
pub struct Selector {
    /// subject first; each compound is joined to the next (to its left in source) by the combinator
    pub compounds: Vec<Compound>,
    pub combinators: Vec<Combinator>,
    pub pseudo_element: Option<PseudoElement>,
    pub specificity: (u32, u32, u32),
}

/// parses a selector list; None if any selector is invalid (the whole rule is dropped)
pub fn parse_list(t: &[Tok]) -> Option<Vec<Selector>> {
    let mut out = vec![];
    for part in split_commas(t) {
        out.push(parse_complex(&part)?);
    }
    if out.is_empty() {
        None
    } else {
        Some(out)
    }
}

fn split_commas(t: &[Tok]) -> Vec<Vec<Tok>> {
    let mut parts = vec![vec![]];
    let mut depth = 0;
    for x in t {
        match x {
            Tok::Function(_) | Tok::LParen | Tok::LBracket => depth += 1,
            Tok::RParen | Tok::RBracket => depth -= 1,
            Tok::Comma if depth == 0 => {
                parts.push(vec![]);
                continue;
            }
            _ => {}
        }
        parts.last_mut().unwrap().push(x.clone());
    }
    parts
}

fn parse_complex(t: &[Tok]) -> Option<Selector> {
    let mut i = 0;
    let t: Vec<Tok> = {
        // trim whitespace
        let mut v = t.to_vec();
        while v.first() == Some(&Tok::Ws) {
            v.remove(0);
        }
        while v.last() == Some(&Tok::Ws) {
            v.pop();
        }
        v
    };
    if t.is_empty() {
        return None;
    }
    let mut compounds = vec![];
    let mut combinators = vec![];
    let mut pseudo_element = None;
    loop {
        let (c, pe) = parse_compound(&t, &mut i)?;
        if pseudo_element.is_some() {
            return None; // nothing may follow a pseudo-element
        }
        pseudo_element = pe;
        compounds.push(c);
        // combinator
        let mut saw_ws = false;
        while i < t.len() && t[i] == Tok::Ws {
            i += 1;
            saw_ws = true;
        }
        if i >= t.len() {
            break;
        }
        let comb = match &t[i] {
            Tok::Delim('>') => {
                i += 1;
                Combinator::Child
            }
            Tok::Delim('+') => {
                i += 1;
                Combinator::Next
            }
            Tok::Delim('~') => {
                i += 1;
                Combinator::Subsequent
            }
            _ if saw_ws => Combinator::Descendant,
            _ => return None,
        };
        while i < t.len() && t[i] == Tok::Ws {
            i += 1;
        }
        if i >= t.len() {
            return None;
        }
        combinators.push(comb);
    }
    compounds.reverse();
    combinators.reverse();
    let mut spec = (0, 0, 0);
    for c in &compounds {
        let s = compound_specificity(c);
        spec.0 += s.0;
        spec.1 += s.1;
        spec.2 += s.2;
    }
    if pseudo_element.is_some() {
        spec.2 += 1;
    }
    Some(Selector { compounds, combinators, pseudo_element, specificity: spec })
}

fn max_spec(list: &[Selector]) -> (u32, u32, u32) {
    list.iter().map(|s| s.specificity).max().unwrap_or((0, 0, 0))
}
fn compound_specificity(c: &Compound) -> (u32, u32, u32) {
    let mut s = (c.ids.len() as u32, (c.classes.len() + c.attrs.len()) as u32, c.tag.is_some() as u32);
    for p in &c.pseudos {
        match p {
            Pseudo::Not(l) | Pseudo::Is(l) | Pseudo::Has(l) => {
                let m = max_spec(l);
                s.0 += m.0;
                s.1 += m.1;
                s.2 += m.2;
            }
            Pseudo::Where(_) => {}
            _ => s.1 += 1,
        }
    }
    s
}

fn parse_compound(t: &[Tok], i: &mut usize) -> Option<(Compound, Option<PseudoElement>)> {
    let mut c = Compound::default();
    let start = *i;
    let mut pe = None;
    // type or universal (with an optional namespace prefix)
    match t.get(*i) {
        Some(Tok::Ident(n)) => {
            if t.get(*i + 1) == Some(&Tok::Delim('|')) {
                *i += 2;
                if let Some(Tok::Ident(n2)) = t.get(*i) {
                    c.tag = Some(n2.to_ascii_lowercase());
                    *i += 1;
                } else if t.get(*i) == Some(&Tok::Delim('*')) {
                    *i += 1;
                }
            } else {
                c.tag = Some(n.to_ascii_lowercase());
                *i += 1;
            }
        }
        Some(Tok::Delim('*')) => {
            *i += 1;
            if t.get(*i) == Some(&Tok::Delim('|')) {
                *i += 1;
                if let Some(Tok::Ident(n2)) = t.get(*i) {
                    c.tag = Some(n2.to_ascii_lowercase());
                    *i += 1;
                } else if t.get(*i) == Some(&Tok::Delim('*')) {
                    *i += 1;
                }
            }
        }
        Some(Tok::Delim('|')) => {
            *i += 1;
            match t.get(*i) {
                Some(Tok::Ident(n2)) => {
                    c.tag = Some(n2.to_ascii_lowercase());
                    *i += 1;
                }
                Some(Tok::Delim('*')) => *i += 1,
                _ => return None,
            }
        }
        _ => {}
    }
    loop {
        match t.get(*i) {
            Some(Tok::Hash(h, true)) => {
                c.ids.push(h.clone());
                *i += 1;
            }
            Some(Tok::Hash(_, false)) => return None,
            Some(Tok::Delim('.')) => {
                if let Some(Tok::Ident(n)) = t.get(*i + 1) {
                    c.classes.push(n.clone());
                    *i += 2;
                } else {
                    return None;
                }
            }
            Some(Tok::LBracket) => {
                *i += 1;
                c.attrs.push(parse_attr(t, i)?);
            }
            Some(Tok::Colon) => {
                *i += 1;
                if t.get(*i) == Some(&Tok::Colon) {
                    *i += 1;
                    let Some(Tok::Ident(n)) = t.get(*i) else { return None };
                    pe = Some(match n.to_ascii_lowercase().as_str() {
                        "before" => PseudoElement::Before,
                        "after" => PseudoElement::After,
                        "first-line" => PseudoElement::FirstLine,
                        "first-letter" => PseudoElement::FirstLetter,
                        "marker" => PseudoElement::Marker,
                        _ => return None,
                    });
                    *i += 1;
                    break;
                }
                match t.get(*i) {
                    Some(Tok::Ident(n)) => {
                        let n = n.to_ascii_lowercase();
                        *i += 1;
                        // legacy single-colon pseudo-elements
                        let legacy = match n.as_str() {
                            "before" => Some(PseudoElement::Before),
                            "after" => Some(PseudoElement::After),
                            "first-line" => Some(PseudoElement::FirstLine),
                            "first-letter" => Some(PseudoElement::FirstLetter),
                            _ => None,
                        };
                        if let Some(l) = legacy {
                            pe = Some(l);
                            break;
                        }
                        let ps = match n.as_str() {
                            "first-child" => Pseudo::Nth { a: 0, b: 1, last: false, of_type: false },
                            "last-child" => Pseudo::Nth { a: 0, b: 1, last: true, of_type: false },
                            "only-child" => {
                                c.pseudos.push(Pseudo::Nth { a: 0, b: 1, last: false, of_type: false });
                                Pseudo::Nth { a: 0, b: 1, last: true, of_type: false }
                            }
                            "first-of-type" => Pseudo::Nth { a: 0, b: 1, last: false, of_type: true },
                            "last-of-type" => Pseudo::Nth { a: 0, b: 1, last: true, of_type: true },
                            "only-of-type" => {
                                c.pseudos.push(Pseudo::Nth { a: 0, b: 1, last: false, of_type: true });
                                Pseudo::Nth { a: 0, b: 1, last: true, of_type: true }
                            }
                            "root" => Pseudo::Root,
                            "empty" => Pseudo::Empty,
                            "link" | "any-link" => Pseudo::Link,
                            "visited" | "hover" | "active" | "focus" | "focus-within" | "focus-visible" | "target" => Pseudo::Never,
                            "checked" => Pseudo::Checked,
                            "disabled" => Pseudo::Disabled,
                            "enabled" => Pseudo::Enabled,
                            _ => return None,
                        };
                        c.pseudos.push(ps);
                    }
                    Some(Tok::Function(f)) => {
                        let f = f.to_ascii_lowercase();
                        *i += 1;
                        // the arguments up to the matching parenthesis
                        let mut depth = 1;
                        let mut args = vec![];
                        while *i < t.len() {
                            match &t[*i] {
                                Tok::Function(_) | Tok::LParen => depth += 1,
                                Tok::RParen => {
                                    depth -= 1;
                                    if depth == 0 {
                                        *i += 1;
                                        break;
                                    }
                                }
                                _ => {}
                            }
                            args.push(t[*i].clone());
                            *i += 1;
                        }
                        c.pseudos.push(match f.as_str() {
                            "not" => Pseudo::Not(parse_list(&args)?),
                            "is" | "matches" | "-webkit-any" => Pseudo::Is(parse_forgiving(&args)),
                            "where" => Pseudo::Where(parse_forgiving(&args)),
                            "has" => Pseudo::Has(parse_list(&args)?),
                            "nth-child" | "nth-last-child" | "nth-of-type" | "nth-last-of-type" => {
                                let (a, b) = parse_nth(&args)?;
                                Pseudo::Nth { a, b, last: f.contains("last"), of_type: f.contains("type") }
                            }
                            _ => return None,
                        });
                    }
                    _ => return None,
                }
            }
            _ => break,
        }
    }
    if *i == start && pe.is_none() {
        return None;
    }
    Some((c, pe))
}

fn parse_forgiving(t: &[Tok]) -> Vec<Selector> {
    split_commas(t).iter().filter_map(|p| parse_complex(p)).collect()
}

fn parse_attr(t: &[Tok], i: &mut usize) -> Option<AttrSel> {
    let skip_ws = |i: &mut usize| {
        while t.get(*i) == Some(&Tok::Ws) {
            *i += 1;
        }
    };
    skip_ws(i);
    let Some(Tok::Ident(name)) = t.get(*i) else { return None };
    let name = name.to_ascii_lowercase();
    *i += 1;
    skip_ws(i);
    let op = match t.get(*i) {
        Some(Tok::RBracket) => {
            *i += 1;
            return Some(AttrSel { name, op: AttrOp::Exists, value: String::new(), icase: false });
        }
        Some(Tok::Delim('=')) => {
            *i += 1;
            AttrOp::Eq
        }
        Some(Tok::Delim(d)) if t.get(*i + 1) == Some(&Tok::Delim('=')) => {
            let op = match d {
                '~' => AttrOp::Includes,
                '|' => AttrOp::Dash,
                '^' => AttrOp::Prefix,
                '$' => AttrOp::Suffix,
                '*' => AttrOp::Substr,
                _ => return None,
            };
            *i += 2;
            op
        }
        _ => return None,
    };
    skip_ws(i);
    let value = match t.get(*i) {
        Some(Tok::Ident(v)) | Some(Tok::Str(v)) => v.clone(),
        _ => return None,
    };
    *i += 1;
    skip_ws(i);
    let mut icase = false;
    if let Some(Tok::Ident(f)) = t.get(*i) {
        if f.eq_ignore_ascii_case("i") {
            icase = true;
        }
        *i += 1;
        skip_ws(i);
    }
    if t.get(*i) != Some(&Tok::RBracket) {
        return None;
    }
    *i += 1;
    Some(AttrSel { name, op, value, icase })
}

/// an+b
fn parse_nth(t: &[Tok]) -> Option<(i32, i32)> {
    let mut s = String::new();
    for x in t {
        match x {
            Tok::Ws => {}
            Tok::Ident(v) => s.push_str(v),
            Tok::Number(v, _) => {
                if *v >= 0.0 && !s.is_empty() && !s.ends_with('+') && !s.ends_with('-') {
                    s.push('+');
                }
                s.push_str(&format!("{}", *v as i64));
            }
            Tok::Dimension(v, u) => {
                s.push_str(&format!("{}", *v as i64));
                s.push_str(u);
            }
            Tok::Delim(c) => s.push(*c),
            _ => return None,
        }
    }
    let s = s.to_ascii_lowercase();
    if s == "odd" {
        return Some((2, 1));
    }
    if s == "even" {
        return Some((2, 0));
    }
    if let Some(p) = s.find('n') {
        let a_str = &s[..p];
        let a = match a_str {
            "" | "+" => 1,
            "-" => -1,
            x => x.parse().ok()?,
        };
        let rest = &s[p + 1..];
        let b = if rest.is_empty() { 0 } else { rest.trim_start_matches('+').parse().ok()? };
        Some((a, b))
    } else {
        Some((0, s.trim_start_matches('+').parse().ok()?))
    }
}

// ------------------------------------------------------------------ matching
fn element_parent(doc: &Document, n: NodeId) -> Option<NodeId> {
    let p = doc.parent(n)?;
    doc.element(p).map(|_| p)
}
fn element_siblings(doc: &Document, n: NodeId) -> Vec<NodeId> {
    match doc.parent(n) {
        Some(p) => doc.children(p).iter().copied().filter(|&c| doc.element(c).is_some()).collect(),
        None => vec![n],
    }
}

fn attr_matches(doc: &Document, n: NodeId, a: &AttrSel) -> bool {
    let e = doc.element(n).unwrap();
    let Some(v) = e.attrs.iter().find(|x| x.name.eq_ignore_ascii_case(&a.name)).map(|x| x.value.as_str()) else {
        return false;
    };
    // HTML attribute values that are case-insensitive by default (a short list)
    let icase = a.icase || (e.ns == Ns::Html && matches!(a.name.as_str(), "type" | "align" | "dir" | "lang" | "valign" | "nowrap" | "method"));
    let (v, want) = if icase { (v.to_ascii_lowercase(), a.value.to_ascii_lowercase()) } else { (v.to_string(), a.value.clone()) };
    match a.op {
        AttrOp::Exists => true,
        AttrOp::Eq => v == want,
        AttrOp::Includes => !want.is_empty() && !want.contains(char::is_whitespace) && v.split_ascii_whitespace().any(|w| w == want),
        AttrOp::Dash => v == want || v.starts_with(&(want.clone() + "-")),
        AttrOp::Prefix => !want.is_empty() && v.starts_with(&want),
        AttrOp::Suffix => !want.is_empty() && v.ends_with(&want),
        AttrOp::Substr => !want.is_empty() && v.contains(&want),
    }
}

fn nth_ok(a: i32, b: i32, pos: i32) -> bool {
    if a == 0 {
        return pos == b;
    }
    let d = pos - b;
    d % a == 0 && d / a >= 0
}

pub fn compound_matches(doc: &Document, n: NodeId, c: &Compound) -> bool {
    let Some(e) = doc.element(n) else { return false };
    if let Some(t) = &c.tag {
        let ok = if e.ns == Ns::Html { e.name.eq_ignore_ascii_case(t) } else { e.name == *t || e.name.eq_ignore_ascii_case(t) };
        if !ok {
            return false;
        }
    }
    for id in &c.ids {
        if e.attr("id") != Some(id.as_str()) {
            return false;
        }
    }
    if !c.classes.is_empty() {
        let cls = e.attr("class").unwrap_or("");
        for k in &c.classes {
            if !cls.split_ascii_whitespace().any(|w| w == k) {
                return false;
            }
        }
    }
    for a in &c.attrs {
        if !attr_matches(doc, n, a) {
            return false;
        }
    }
    for p in &c.pseudos {
        let ok = match p {
            Pseudo::Nth { a, b, last, of_type } => {
                let sibs: Vec<NodeId> = element_siblings(doc, n)
                    .into_iter()
                    .filter(|&s| !*of_type || doc.element(s).unwrap().name == e.name)
                    .collect();
                let idx = sibs.iter().position(|&s| s == n).unwrap() as i32;
                let pos = if *last { sibs.len() as i32 - idx } else { idx + 1 };
                nth_ok(*a, *b, pos)
            }
            Pseudo::Root => doc.parent(n).is_some_and(|p| matches!(doc.nodes[p].data, NodeData::Document)),
            Pseudo::Empty => doc.children(n).iter().all(|&k| match &doc.nodes[k].data {
                NodeData::Element(_) => false,
                NodeData::Text(t) => t.is_empty(),
                _ => true,
            }),
            Pseudo::Not(l) => !l.iter().any(|s| matches(doc, n, s)),
            Pseudo::Is(l) | Pseudo::Where(l) => l.iter().any(|s| matches(doc, n, s)),
            Pseudo::Has(l) => doc.descendants(n).iter().any(|&d| doc.element(d).is_some() && l.iter().any(|s| matches(doc, d, s))),
            Pseudo::Link => e.ns == Ns::Html && (e.name == "a" || e.name == "area") && e.attr("href").is_some(),
            Pseudo::Never => false,
            Pseudo::Checked => e.attr("checked").is_some() || e.attr("selected").is_some(),
            Pseudo::Disabled => e.attr("disabled").is_some(),
            Pseudo::Enabled => {
                matches!(e.name.as_str(), "input" | "button" | "select" | "textarea" | "option") && e.attr("disabled").is_none()
            }
        };
        if !ok {
            return false;
        }
    }
    true
}

/// whether element `n` matches the selector (ignoring its pseudo-element)
pub fn matches(doc: &Document, n: NodeId, s: &Selector) -> bool {
    match_from(doc, n, s, 0)
}

fn match_from(doc: &Document, n: NodeId, s: &Selector, k: usize) -> bool {
    if !compound_matches(doc, n, &s.compounds[k]) {
        return false;
    }
    if k + 1 == s.compounds.len() {
        return true;
    }
    match s.combinators[k] {
        Combinator::Child => element_parent(doc, n).is_some_and(|p| match_from(doc, p, s, k + 1)),
        Combinator::Descendant => {
            let mut p = element_parent(doc, n);
            while let Some(x) = p {
                if match_from(doc, x, s, k + 1) {
                    return true;
                }
                p = element_parent(doc, x);
            }
            false
        }
        Combinator::Next => {
            let sibs = element_siblings(doc, n);
            let i = sibs.iter().position(|&x| x == n).unwrap();
            i > 0 && match_from(doc, sibs[i - 1], s, k + 1)
        }
        Combinator::Subsequent => {
            let sibs = element_siblings(doc, n);
            let i = sibs.iter().position(|&x| x == n).unwrap();
            sibs[..i].iter().any(|&x| match_from(doc, x, s, k + 1))
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::syntax::tokenize;
    use crate::html::treebuilder::parse;

    fn sel(s: &str) -> Vec<Selector> {
        parse_list(&tokenize(s)).unwrap()
    }

    #[test]
    fn specificity_and_matching() {
        let doc = parse("<div id=a class='x y'><p>1</p><p class=z>2</p><span>3</span></div>", false);
        let ps: Vec<NodeId> = (0..doc.nodes.len()).filter(|&n| doc.is(n, "p")).collect();
        assert_eq!(sel("#a .z")[0].specificity, (1, 1, 0));
        assert!(matches(&doc, ps[1], &sel("div.x > p:last-of-type")[0]));
        assert!(!matches(&doc, ps[0], &sel("p + p")[0]));
        assert!(matches(&doc, ps[1], &sel("p + p")[0]));
        assert!(matches(&doc, ps[0], &sel("p:nth-child(2n+1)")[0]));
        assert!(matches(&doc, ps[0], &sel(":not(.z)")[0]));
        assert!(parse_list(&tokenize("p::before:hover")).is_none());
    }
}
