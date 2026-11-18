//! Scripts: the kjs engine (C++, linked in) and the host side of the DOM. The
//! DOM API itself is dom.js, which calls `__host(op, ...)` implemented here.
use crate::css::{selector, syntax};
use crate::html::dom::{Document, NodeData, NodeId, Ns, DOCUMENT};
use crate::html::tokenizer::Attr;
use crate::html::treebuilder;

const DOM_JS: &str = include_str!("dom.js");

#[repr(C)]
#[derive(Clone, Copy)]
pub struct KjsVal {
    tag: i32,
    num: f64,
    str_: *const u16,
    len: usize,
}

#[cfg(kjs)]
extern "C" {
    fn kjs_new(f: extern "C" fn(*mut std::ffi::c_void, i32, *const KjsVal, i32, *mut KjsVal), ctx: *mut std::ffi::c_void) -> *mut std::ffi::c_void;
    fn kjs_free(rt: *mut std::ffi::c_void);
    fn kjs_eval(rt: *mut std::ffi::c_void, src: *const u16, len: usize, name: *const u8, err: *mut u8, errlen: usize) -> i32;
    fn kjs_run_jobs(rt: *mut std::ffi::c_void, err: *mut u8, errlen: usize) -> i32;
}

pub fn available() -> bool {
    cfg!(kjs)
}

struct HostCtx {
    doc: *mut Document,
    ret: Vec<u16>,
    console: Vec<String>,
    start: std::time::Instant,
}

fn val_str(v: &KjsVal) -> String {
    if v.tag != 4 || v.str_.is_null() {
        return match v.tag {
            2 => (v.num != 0.0).to_string(),
            3 => format!("{}", v.num),
            1 => "null".into(),
            _ => "undefined".into(),
        };
    }
    let s = unsafe { std::slice::from_raw_parts(v.str_, v.len) };
    String::from_utf16_lossy(s)
}
fn val_num(v: &KjsVal) -> f64 {
    if v.tag == 3 || v.tag == 2 {
        v.num
    } else {
        -1.0
    }
}

fn node_type(doc: &Document, n: NodeId) -> f64 {
    match &doc.nodes[n].data {
        NodeData::Element(_) => 1.0,
        NodeData::Text(_) => 3.0,
        NodeData::Comment(_) | NodeData::Pi { .. } => 8.0,
        NodeData::Document => 9.0,
        NodeData::Doctype { .. } => 10.0,
        NodeData::Fragment => 11.0,
    }
}
fn node_name(doc: &Document, n: NodeId) -> String {
    match &doc.nodes[n].data {
        NodeData::Element(e) => {
            if e.ns == Ns::Html {
                e.name.to_ascii_uppercase()
            } else {
                e.name.clone()
            }
        }
        NodeData::Text(_) => "#text".into(),
        NodeData::Comment(_) => "#comment".into(),
        NodeData::Document => "#document".into(),
        NodeData::Fragment => "#document-fragment".into(),
        NodeData::Doctype { name, .. } => name.clone(),
        NodeData::Pi { target, .. } => target.clone(),
    }
}

/// the HTML fragment serialization algorithm (children of `n`)
pub fn serialize_children(doc: &Document, n: NodeId) -> String {
    let mut out = String::new();
    let kids: Vec<NodeId> = match doc.element(n).and_then(|e| e.template_contents) {
        Some(t) => doc.children(t).to_vec(),
        None => doc.children(n).to_vec(),
    };
    let raw_parent = doc.element(n).is_some_and(|e| {
        e.ns == Ns::Html && matches!(e.name.as_str(), "style" | "script" | "xmp" | "iframe" | "noembed" | "noframes" | "plaintext" | "noscript")
    });
    for c in kids {
        serialize_node(doc, c, raw_parent, &mut out);
    }
    out
}
fn esc(s: &str, attr: bool) -> String {
    let mut o = String::with_capacity(s.len());
    for ch in s.chars() {
        match ch {
            '&' => o.push_str("&amp;"),
            '\u{A0}' => o.push_str("&nbsp;"),
            '"' if attr => o.push_str("&quot;"),
            '<' if !attr => o.push_str("&lt;"),
            '>' if !attr => o.push_str("&gt;"),
            c => o.push(c),
        }
    }
    o
}
fn serialize_node(doc: &Document, n: NodeId, raw: bool, out: &mut String) {
    match &doc.nodes[n].data {
        NodeData::Element(e) => {
            out.push('<');
            out.push_str(&e.name);
            for a in &e.attrs {
                out.push(' ');
                out.push_str(&a.name.replace(' ', ":"));
                out.push_str("=\"");
                out.push_str(&esc(&a.value, true));
                out.push('"');
            }
            out.push('>');
            let void = e.ns == Ns::Html
                && matches!(e.name.as_str(), "area" | "base" | "br" | "col" | "embed" | "hr" | "img" | "input" | "link" | "meta" | "param" | "source" | "track" | "wbr" | "basefont" | "bgsound" | "frame" | "keygen");
            if void {
                return;
            }
            out.push_str(&serialize_children(doc, n));
            out.push_str("</");
            out.push_str(&e.name);
            out.push('>');
        }
        NodeData::Text(t) => out.push_str(&if raw { t.clone() } else { esc(t, false) }),
        NodeData::Comment(c) => {
            out.push_str("<!--");
            out.push_str(c);
            out.push_str("-->");
        }
        NodeData::Pi { target, data } => {
            out.push_str(&format!("<?{target} {data}>"));
        }
        NodeData::Doctype { name, .. } => out.push_str(&format!("<!DOCTYPE {name}>")),
        _ => {}
    }
}

/// copies a subtree from another document
fn import(src: &Document, n: NodeId, dst: &mut Document) -> NodeId {
    let id = match &src.nodes[n].data {
        NodeData::Element(e) => {
            let id = dst.create_element(e.ns, &e.name, e.attrs.clone());
            if let (Some(st), Some(dt)) = (e.template_contents, dst.element(id).and_then(|x| x.template_contents)) {
                for &c in src.children(st) {
                    let k = import(src, c, dst);
                    dst.append(dt, k);
                }
            }
            id
        }
        d => dst.create(d.clone()),
    };
    for &c in src.children(n) {
        let k = import(src, c, dst);
        dst.append(id, k);
    }
    id
}
fn clone_node(doc: &mut Document, n: NodeId, deep: bool) -> NodeId {
    let data = doc.nodes[n].data.clone();
    let id = match &data {
        NodeData::Element(e) => doc.create_element(e.ns, &e.name, e.attrs.clone()),
        d => doc.create(d.clone()),
    };
    if deep {
        for c in doc.children(n).to_vec() {
            let k = clone_node(doc, c, true);
            doc.append(id, k);
        }
    }
    id
}

fn parse_into(doc: &mut Document, context: NodeId, html: &str) -> Vec<NodeId> {
    let (ns, name) = match doc.element(context) {
        Some(e) => (e.ns, e.name.clone()),
        None => (Ns::Html, "body".to_string()),
    };
    let (frag, root) = treebuilder::parse_fragment(html, ns, &name, true);
    let kids: Vec<NodeId> = frag.children(root).to_vec();
    kids.into_iter().map(|k| import(&frag, k, doc)).collect()
}

fn ids_str(v: &[NodeId]) -> String {
    v.iter().map(|x| x.to_string()).collect::<Vec<_>>().join(",")
}

fn find_tag(doc: &Document, tag: &str) -> Option<NodeId> {
    doc.descendants(DOCUMENT).into_iter().find(|&n| doc.is(n, tag))
}

enum Ret {
    Undef,
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
}

fn host_op(ctx: &mut HostCtx, op: i32, a: &[KjsVal]) -> Ret {
    let doc = unsafe { &mut *ctx.doc };
    let n = |i: usize| -> NodeId { a.get(i).map(val_num).unwrap_or(-1.0).max(0.0) as NodeId };
    let s = |i: usize| -> String { a.get(i).map(val_str).unwrap_or_default() };
    let valid = |doc: &Document, i: usize| -> bool { a.get(i).map(val_num).is_some_and(|v| v >= 0.0 && (v as usize) < doc.nodes.len()) };
    if matches!(op, 4..=13 | 16..=24 | 27 | 28 | 30 | 31 | 35 | 37) && !valid(doc, 0) {
        return Ret::Null;
    }
    match op {
        0 => doc.document_element().map(|x| Ret::Num(x as f64)).unwrap_or(Ret::Null),
        1 => find_tag(doc, "body").or_else(|| find_tag(doc, "frameset")).map(|x| Ret::Num(x as f64)).unwrap_or(Ret::Null),
        2 => find_tag(doc, "head").map(|x| Ret::Num(x as f64)).unwrap_or(Ret::Null),
        3 => {
            let id = s(0);
            doc.descendants(DOCUMENT).into_iter().find(|&x| doc.element(x).is_some_and(|e| e.attr("id") == Some(id.as_str()))).map(|x| Ret::Num(x as f64)).unwrap_or(Ret::Null)
        }
        4 => Ret::Num(node_type(doc, n(0))),
        5 => Ret::Str(node_name(doc, n(0))),
        6 => doc.parent(n(0)).map(|p| Ret::Num(p as f64)).unwrap_or(Ret::Null),
        7 => Ret::Str(ids_str(doc.children(n(0)))),
        8 => doc.element(n(0)).and_then(|e| e.attr(&s(1)).map(|v| Ret::Str(v.to_string()))).unwrap_or(Ret::Null),
        9 => {
            let (name, value) = (s(1), s(2));
            if let Some(e) = doc.element_mut(n(0)) {
                match e.attrs.iter_mut().find(|x| x.name == name) {
                    Some(x) => x.value = value,
                    None => e.attrs.push(Attr { name, value }),
                }
            }
            Ret::Undef
        }
        10 => {
            let name = s(1);
            if let Some(e) = doc.element_mut(n(0)) {
                e.attrs.retain(|x| x.name != name);
            }
            Ret::Undef
        }
        11 => Ret::Str(doc.element(n(0)).map(|e| e.attrs.iter().map(|x| x.name.clone()).collect::<Vec<_>>().join("\u{1}")).unwrap_or_default()),
        12 => Ret::Str(doc.text_content(n(0))),
        13 => {
            let x = n(0);
            for c in doc.children(x).to_vec() {
                doc.detach(c);
            }
            let t = s(1);
            if !t.is_empty() {
                let k = doc.create(NodeData::Text(t));
                doc.append(x, k);
            }
            Ret::Undef
        }
        14 => Ret::Num(doc.create_element(Ns::Html, &s(0), vec![]) as f64),
        15 => Ret::Num(doc.create(NodeData::Text(s(0))) as f64),
        16 => {
            let (p, c) = (n(0), n(1));
            if valid(doc, 1) && !is_ancestor(doc, c, p) {
                let p = doc.element(p).and_then(|e| e.template_contents).unwrap_or(p);
                doc.append(p, c);
            }
            Ret::Undef
        }
        17 => {
            let (p, c) = (n(0), n(1));
            let r = a.get(2).map(val_num).unwrap_or(-1.0);
            if valid(doc, 1) && !is_ancestor(doc, c, p) {
                let refn = if r >= 0.0 && doc.parent(r as usize) == Some(p) { Some(r as usize) } else { None };
                if r >= 0.0 && refn.is_none() {
                    return Ret::Undef;
                }
                if refn == Some(c) {
                    return Ret::Undef;
                }
                doc.insert_before(p, c, refn);
            }
            Ret::Undef
        }
        18 => {
            let (p, c) = (n(0), n(1));
            if valid(doc, 1) && doc.parent(c) == Some(p) {
                doc.detach(c);
            }
            Ret::Undef
        }
        19 => {
            let root = n(0);
            let Some(sels) = selector::parse_list(&syntax::tokenize(&s(1))) else { return Ret::Null };
            let first = a.get(2).map(val_num).unwrap_or(0.0) == 1.0;
            let mut out = vec![];
            for d in doc.descendants(root) {
                if doc.element(d).is_some() && sels.iter().any(|x| selector::matches(doc, d, x)) {
                    out.push(d);
                    if first {
                        break;
                    }
                }
            }
            Ret::Str(ids_str(&out))
        }
        20 => match selector::parse_list(&syntax::tokenize(&s(1))) {
            Some(sels) => Ret::Bool(sels.iter().any(|x| selector::matches(doc, n(0), x))),
            None => Ret::Null,
        },
        21 => Ret::Str(serialize_children(doc, n(0))),
        22 => {
            let x = n(0);
            let target = doc.element(x).and_then(|e| e.template_contents).unwrap_or(x);
            for c in doc.children(target).to_vec() {
                doc.detach(c);
            }
            for k in parse_into(doc, x, &s(1)) {
                doc.append(target, k);
            }
            Ret::Undef
        }
        23 => match &doc.nodes[n(0)].data {
            NodeData::Text(t) | NodeData::Comment(t) => Ret::Str(t.clone()),
            _ => Ret::Null,
        },
        24 => {
            let v = s(1);
            match &mut doc.nodes[n(0)].data {
                NodeData::Text(t) | NodeData::Comment(t) => *t = v,
                _ => {}
            }
            Ret::Undef
        }
        25 => {
            ctx.console.push(format!("[{}] {}", s(0), s(1)));
            Ret::Undef
        }
        26 => Ret::Num(doc.create(NodeData::Comment(s(0))) as f64),
        27 => {
            let deep = a.get(1).is_some_and(|v| v.num != 0.0);
            Ret::Num(clone_node(doc, n(0), deep) as f64)
        }
        28 => {
            let mut out = String::new();
            serialize_node(doc, n(0), false, &mut out);
            Ret::Str(out)
        }
        29 => Ret::Num(doc.create(NodeData::Fragment) as f64),
        30 | 31 => {
            let x = n(0);
            let Some(p) = doc.parent(x) else { return Ret::Null };
            let kids = doc.children(p);
            let i = kids.iter().position(|&k| k == x).unwrap();
            let j = if op == 30 { i.checked_add(1) } else { i.checked_sub(1) };
            j.and_then(|j| kids.get(j)).map(|&k| Ret::Num(k as f64)).unwrap_or(Ret::Null)
        }
        32 => Ret::Str(find_tag(doc, "title").map(|t| doc.text_content(t).split_whitespace().collect::<Vec<_>>().join(" ")).unwrap_or_default()),
        33 => {
            let t = match find_tag(doc, "title") {
                Some(t) => t,
                None => {
                    let t = doc.create_element(Ns::Html, "title", vec![]);
                    if let Some(h) = find_tag(doc, "head") {
                        doc.append(h, t);
                    }
                    t
                }
            };
            for c in doc.children(t).to_vec() {
                doc.detach(c);
            }
            let k = doc.create(NodeData::Text(s(0)));
            doc.append(t, k);
            Ret::Undef
        }
        34 => Ret::Num(DOCUMENT as f64),
        35 => {
            let x = n(0);
            let pos = s(1);
            let ctxn = if pos == "beforebegin" || pos == "afterend" { doc.parent(x).unwrap_or(x) } else { x };
            let nodes = parse_into(doc, ctxn, &s(2));
            match pos.as_str() {
                "beforebegin" => {
                    if let Some(p) = doc.parent(x) {
                        for k in nodes {
                            doc.insert_before(p, k, Some(x));
                        }
                    }
                }
                "afterbegin" => {
                    let first = doc.children(x).first().copied();
                    for k in nodes {
                        doc.insert_before(x, k, first);
                    }
                }
                "beforeend" => {
                    for k in nodes {
                        doc.append(x, k);
                    }
                }
                "afterend" => {
                    if let Some(p) = doc.parent(x) {
                        let kids = doc.children(p);
                        let next = kids.iter().position(|&k| k == x).and_then(|i| kids.get(i + 1)).copied();
                        for k in nodes {
                            doc.insert_before(p, k, next);
                        }
                    }
                }
                _ => {}
            }
            Ret::Undef
        }
        36 => Ret::Num(ctx.start.elapsed().as_secs_f64() * 1000.0),
        37 => Ret::Str("0,0,0,0".into()),
        _ => Ret::Undef,
    }
}

fn is_ancestor(doc: &Document, a: NodeId, b: NodeId) -> bool {
    // is a an inclusive ancestor of b?
    let mut p = Some(b);
    while let Some(x) = p {
        if x == a {
            return true;
        }
        p = doc.parent(x);
    }
    false
}

extern "C" fn host_cb(ctx: *mut std::ffi::c_void, op: i32, args: *const KjsVal, argc: i32, ret: *mut KjsVal) {
    let ctx = unsafe { &mut *(ctx as *mut HostCtx) };
    let a = if argc > 0 { unsafe { std::slice::from_raw_parts(args, argc as usize) } } else { &[] };
    let r = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| host_op(ctx, op, a))).unwrap_or(Ret::Undef);
    let out = unsafe { &mut *ret };
    match r {
        Ret::Undef => out.tag = 0,
        Ret::Null => out.tag = 1,
        Ret::Bool(b) => {
            out.tag = 2;
            out.num = b as i32 as f64;
        }
        Ret::Num(v) => {
            out.tag = 3;
            out.num = v;
        }
        Ret::Str(s) => {
            ctx.ret = s.encode_utf16().collect();
            out.tag = 4;
            out.str_ = ctx.ret.as_ptr();
            out.len = ctx.ret.len();
        }
    }
}

/// a script context bound to a document
pub struct ScriptContext {
    #[allow(dead_code)]
    rt: *mut std::ffi::c_void,
    ctx: Box<HostCtx>,
    pub errors: Vec<String>,
}

impl ScriptContext {
    pub fn new(doc: &mut Document) -> Option<Self> {
        #[cfg(kjs)]
        {
            let mut ctx = Box::new(HostCtx { doc: doc as *mut Document, ret: vec![], console: vec![], start: std::time::Instant::now() });
            let rt = unsafe { kjs_new(host_cb, &mut *ctx as *mut HostCtx as *mut std::ffi::c_void) };
            let mut sc = ScriptContext { rt, ctx, errors: vec![] };
            sc.eval(DOM_JS, "dom.js");
            return Some(sc);
        }
        #[cfg(not(kjs))]
        {
            let _ = doc;
            let _ = DOM_JS;
            None
        }
    }
    /// the document may have moved (a new borrow): point the context at it
    pub fn rebind(&mut self, doc: &mut Document) {
        self.ctx.doc = doc as *mut Document;
    }
    pub fn eval(&mut self, src: &str, name: &str) -> bool {
        #[cfg(kjs)]
        {
            let u: Vec<u16> = src.encode_utf16().collect();
            let mut err = vec![0u8; 1024];
            let cname = format!("{name}\0");
            let rc = unsafe { kjs_eval(self.rt, u.as_ptr(), u.len(), cname.as_ptr(), err.as_mut_ptr(), err.len()) };
            let mut e2 = vec![0u8; 1024];
            unsafe { kjs_run_jobs(self.rt, e2.as_mut_ptr(), e2.len()) };
            if rc != 0 {
                let n = err.iter().position(|&b| b == 0).unwrap_or(err.len());
                self.errors.push(format!("{name}: {}", String::from_utf8_lossy(&err[..n])));
                return false;
            }
            return true;
        }
        #[cfg(not(kjs))]
        {
            let _ = (src, name);
            false
        }
    }
    pub fn after_scripts(&mut self) {
        self.eval("__kestrelAfterScripts()", "events");
    }
    pub fn console(&self) -> &[String] {
        &self.ctx.console
    }
}

impl Drop for ScriptContext {
    fn drop(&mut self) {
        #[cfg(kjs)]
        unsafe {
            kjs_free(self.rt)
        };
    }
}
