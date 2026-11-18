//! Tree construction: the insertion modes of the HTML standard (13.2.6), as of
//! the 2025 revision (customizable select: no "in select" modes; processing
//! instructions). Scripts do not run during parsing.
use super::dom::*;
use super::tokenizer::{Attr, State, Tag, Token, Tokenizer};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Initial,
    BeforeHtml,
    BeforeHead,
    InHead,
    InHeadNoscript,
    AfterHead,
    InBody,
    Text,
    InTable,
    InTableText,
    InCaption,
    InColumnGroup,
    InTableBody,
    InRow,
    InCell,
    InTemplate,
    AfterBody,
    InFrameset,
    AfterFrameset,
    AfterAfterBody,
    AfterAfterFrameset,
}

#[derive(Clone, Debug)]
enum Fe {
    Marker,
    El(NodeId, Tag),
}

enum Step {
    Done,
    Again(Token),
}
use Step::*;

#[derive(Clone, Copy, PartialEq)]
enum Scope {
    Default,
    ListItem,
    Button,
    Table,
}

pub struct TreeBuilder<'h> {
    hook: Option<&'h mut dyn FnMut(&mut Document, NodeId)>,
    tok: Tokenizer,
    pub doc: Document,
    open: Vec<NodeId>,
    afe: Vec<Fe>,
    head: Option<NodeId>,
    form: Option<NodeId>,
    mode: Mode,
    original: Mode,
    template_modes: Vec<Mode>,
    frameset_ok: bool,
    scripting: bool,
    foster: bool,
    pending_table: Vec<char>,
    context: Option<NodeId>,
    ignore_lf: bool,
    stopped: bool,
    pub errors: usize,
}

fn is_ws(c: char) -> bool {
    matches!(c, '\t' | '\n' | '\x0C' | '\r' | ' ')
}
fn start(t: &Token, names: &[&str]) -> bool {
    matches!(t, Token::StartTag(g) if names.contains(&g.name.as_str()))
}
fn end(t: &Token, names: &[&str]) -> bool {
    matches!(t, Token::EndTag(g) if names.contains(&g.name.as_str()))
}
fn tag_name(t: &Token) -> &str {
    match t {
        Token::StartTag(g) | Token::EndTag(g) => &g.name,
        _ => "",
    }
}
fn bare(name: &str) -> Tag {
    Tag { name: name.into(), attrs: vec![], self_closing: false }
}

const SPECIAL_HTML: &[&str] = &[
    "address", "applet", "area", "article", "aside", "base", "basefont", "bgsound", "blockquote", "body", "br", "button",
    "caption", "center", "col", "colgroup", "dd", "details", "dir", "div", "dl", "dt", "embed", "fieldset", "figcaption",
    "figure", "footer", "form", "frame", "frameset", "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hgroup", "hr",
    "html", "iframe", "img", "input", "keygen", "li", "link", "listing", "main", "marquee", "menu", "meta", "nav",
    "noembed", "noframes", "noscript", "object", "ol", "p", "param", "plaintext", "pre", "script", "search", "section",
    "select", "source", "style", "summary", "table", "tbody", "td", "template", "textarea", "tfoot", "th", "thead",
    "title", "tr", "track", "ul", "wbr", "xmp",
];
const FORMATTING: &[&str] = &["a", "b", "big", "code", "em", "font", "i", "nobr", "s", "small", "strike", "strong", "tt", "u"];
const IMPLIED_END: &[&str] = &["dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc"];
const IMPLIED_END_THOROUGH: &[&str] = &[
    "caption", "colgroup", "dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc", "tbody", "td", "tfoot",
    "th", "thead", "tr",
];
const EOF_OK: &[&str] = &[
    "dd", "dt", "li", "optgroup", "option", "p", "rb", "rp", "rt", "rtc", "tbody", "td", "tfoot", "th", "thead", "tr",
    "body", "html",
];

const SVG_TAGS: &[(&str, &str)] = &[
    ("altglyph", "altGlyph"), ("altglyphdef", "altGlyphDef"), ("altglyphitem", "altGlyphItem"),
    ("animatecolor", "animateColor"), ("animatemotion", "animateMotion"), ("animatetransform", "animateTransform"),
    ("clippath", "clipPath"), ("feblend", "feBlend"), ("fecolormatrix", "feColorMatrix"),
    ("fecomponenttransfer", "feComponentTransfer"), ("fecomposite", "feComposite"),
    ("feconvolvematrix", "feConvolveMatrix"), ("fediffuselighting", "feDiffuseLighting"),
    ("fedisplacementmap", "feDisplacementMap"), ("fedistantlight", "feDistantLight"), ("fedropshadow", "feDropShadow"),
    ("feflood", "feFlood"), ("fefunca", "feFuncA"), ("fefuncb", "feFuncB"), ("fefuncg", "feFuncG"),
    ("fefuncr", "feFuncR"), ("fegaussianblur", "feGaussianBlur"), ("feimage", "feImage"), ("femerge", "feMerge"),
    ("femergenode", "feMergeNode"), ("femorphology", "feMorphology"), ("feoffset", "feOffset"),
    ("fepointlight", "fePointLight"), ("fespecularlighting", "feSpecularLighting"), ("fespotlight", "feSpotLight"),
    ("fetile", "feTile"), ("feturbulence", "feTurbulence"), ("foreignobject", "foreignObject"),
    ("glyphref", "glyphRef"), ("lineargradient", "linearGradient"), ("radialgradient", "radialGradient"),
    ("textpath", "textPath"),
];
const SVG_ATTRS: &[(&str, &str)] = &[
    ("attributename", "attributeName"), ("attributetype", "attributeType"), ("basefrequency", "baseFrequency"),
    ("baseprofile", "baseProfile"), ("calcmode", "calcMode"), ("clippathunits", "clipPathUnits"),
    ("diffuseconstant", "diffuseConstant"), ("edgemode", "edgeMode"), ("filterunits", "filterUnits"),
    ("glyphref", "glyphRef"), ("gradienttransform", "gradientTransform"), ("gradientunits", "gradientUnits"),
    ("kernelmatrix", "kernelMatrix"), ("kernelunitlength", "kernelUnitLength"), ("keypoints", "keyPoints"),
    ("keysplines", "keySplines"), ("keytimes", "keyTimes"), ("lengthadjust", "lengthAdjust"),
    ("limitingconeangle", "limitingConeAngle"), ("markerheight", "markerHeight"), ("markerunits", "markerUnits"),
    ("markerwidth", "markerWidth"), ("maskcontentunits", "maskContentUnits"), ("maskunits", "maskUnits"),
    ("numoctaves", "numOctaves"), ("pathlength", "pathLength"), ("patterncontentunits", "patternContentUnits"),
    ("patterntransform", "patternTransform"), ("patternunits", "patternUnits"), ("pointsatx", "pointsAtX"),
    ("pointsaty", "pointsAtY"), ("pointsatz", "pointsAtZ"), ("preservealpha", "preserveAlpha"),
    ("preserveaspectratio", "preserveAspectRatio"), ("primitiveunits", "primitiveUnits"), ("refx", "refX"),
    ("refy", "refY"), ("repeatcount", "repeatCount"), ("repeatdur", "repeatDur"),
    ("requiredextensions", "requiredExtensions"), ("requiredfeatures", "requiredFeatures"),
    ("specularconstant", "specularConstant"), ("specularexponent", "specularExponent"),
    ("spreadmethod", "spreadMethod"), ("startoffset", "startOffset"), ("stddeviation", "stdDeviation"),
    ("stitchtiles", "stitchTiles"), ("surfacescale", "surfaceScale"), ("systemlanguage", "systemLanguage"),
    ("tablevalues", "tableValues"), ("targetx", "targetX"), ("targety", "targetY"), ("textlength", "textLength"),
    ("viewbox", "viewBox"), ("viewtarget", "viewTarget"), ("xchannelselector", "xChannelSelector"),
    ("ychannelselector", "yChannelSelector"), ("zoomandpan", "zoomAndPan"),
];

fn adjust_svg_attrs(t: &mut Tag) {
    for a in &mut t.attrs {
        if let Some((_, to)) = SVG_ATTRS.iter().find(|(f, _)| *f == a.name) {
            a.name = to.to_string();
        }
    }
}
fn adjust_mathml_attrs(t: &mut Tag) {
    for a in &mut t.attrs {
        if a.name == "definitionurl" {
            a.name = "definitionURL".into();
        }
    }
}
fn adjust_foreign_attrs(t: &mut Tag) {
    for a in &mut t.attrs {
        let n = match a.name.as_str() {
            "xlink:actuate" | "xlink:arcrole" | "xlink:href" | "xlink:role" | "xlink:show" | "xlink:title"
            | "xlink:type" | "xml:lang" | "xml:space" | "xmlns:xlink" => a.name.replacen(':', " ", 1),
            _ => continue,
        };
        a.name = n;
    }
}

const QUIRKY_PUBLIC_PREFIXES: &[&str] = &[
    "+//silmaril//dtd html pro v0r11 19970101//", "-//as//dtd html 3.0 aswedit + extensions//",
    "-//advasoft ltd//dtd html 3.0 aswedit + extensions//", "-//ietf//dtd html 2.0 level 1//",
    "-//ietf//dtd html 2.0 level 2//", "-//ietf//dtd html 2.0 strict level 1//", "-//ietf//dtd html 2.0 strict level 2//",
    "-//ietf//dtd html 2.0 strict//", "-//ietf//dtd html 2.0//", "-//ietf//dtd html 2.1e//", "-//ietf//dtd html 3.0//",
    "-//ietf//dtd html 3.2 final//", "-//ietf//dtd html 3.2//", "-//ietf//dtd html 3//", "-//ietf//dtd html level 0//",
    "-//ietf//dtd html level 1//", "-//ietf//dtd html level 2//", "-//ietf//dtd html level 3//",
    "-//ietf//dtd html strict level 0//", "-//ietf//dtd html strict level 1//", "-//ietf//dtd html strict level 2//",
    "-//ietf//dtd html strict level 3//", "-//ietf//dtd html strict//", "-//ietf//dtd html//",
    "-//metrius//dtd metrius presentational//", "-//microsoft//dtd internet explorer 2.0 html strict//",
    "-//microsoft//dtd internet explorer 2.0 html//", "-//microsoft//dtd internet explorer 2.0 tables//",
    "-//microsoft//dtd internet explorer 3.0 html strict//", "-//microsoft//dtd internet explorer 3.0 html//",
    "-//microsoft//dtd internet explorer 3.0 tables//", "-//netscape comm. corp.//dtd html//",
    "-//netscape comm. corp.//dtd strict html//", "-//o'reilly and associates//dtd html 2.0//",
    "-//o'reilly and associates//dtd html extended 1.0//", "-//o'reilly and associates//dtd html extended relaxed 1.0//",
    "-//sq//dtd html 2.0 hotmetal + extensions//",
    "-//softquad software//dtd hotmetal pro 6.0::19990601::extensions to html 4.0//",
    "-//softquad//dtd hotmetal pro 4.0::19971010::extensions to html 4.0//", "-//spyglass//dtd html 2.0 extended//",
    "-//sun microsystems corp.//dtd hotjava html//", "-//sun microsystems corp.//dtd hotjava strict html//",
    "-//w3c//dtd html 3 1995-03-24//", "-//w3c//dtd html 3.2 draft//", "-//w3c//dtd html 3.2 final//",
    "-//w3c//dtd html 3.2//", "-//w3c//dtd html 3.2s draft//", "-//w3c//dtd html 4.0 frameset//",
    "-//w3c//dtd html 4.0 transitional//", "-//w3c//dtd html experimental 19960712//",
    "-//w3c//dtd html experimental 970421//", "-//w3c//dtd w3 html//", "-//w3o//dtd w3 html 3.0//",
    "-//webtechs//dtd mozilla html 2.0//", "-//webtechs//dtd mozilla html//",
];

/// parse a whole document
pub fn parse(html: &str, scripting: bool) -> Document {
    let mut tb = TreeBuilder::new(html, scripting);
    tb.run();
    tb.doc
}

/// parse a whole document, calling `hook` as each script element ends (scripts run during parsing)
pub fn parse_with_scripts(html: &str, hook: &mut dyn FnMut(&mut Document, NodeId)) -> Document {
    let mut tb = TreeBuilder::new(html, true);
    tb.hook = Some(hook);
    tb.run();
    tb.doc
}

/// parse a fragment in the context of an element (`ns`, `name`); returns the
/// document and the root whose children are the fragment
pub fn parse_fragment(html: &str, ns: Ns, name: &str, scripting: bool) -> (Document, NodeId) {
    let mut tb = TreeBuilder::new(html, scripting);
    let ctx = tb.doc.create_element(ns, name, vec![]);
    tb.context = Some(ctx);
    if ns == Ns::Html {
        tb.tok.state = match name {
            "title" | "textarea" => State::Rcdata,
            "style" | "xmp" | "iframe" | "noembed" | "noframes" => State::Rawtext,
            "script" => State::ScriptData,
            "noscript" if scripting => State::Rawtext,
            "plaintext" => State::Plaintext,
            _ => State::Data,
        };
    }
    let root = tb.doc.create_element(Ns::Html, "html", vec![]);
    tb.doc.append(DOCUMENT, root);
    tb.open.push(root);
    if ns == Ns::Html && name == "template" {
        tb.template_modes.push(Mode::InTemplate);
    }
    tb.reset_mode();
    if ns == Ns::Html && name == "form" {
        tb.form = Some(ctx);
    }
    tb.run();
    (tb.doc, root)
}

impl<'h> TreeBuilder<'h> {
    fn new(html: &str, scripting: bool) -> Self {
        TreeBuilder {
            hook: None,
            tok: Tokenizer::new(html),
            doc: Document::new(),
            open: vec![],
            afe: vec![],
            head: None,
            form: None,
            mode: Mode::Initial,
            original: Mode::Initial,
            template_modes: vec![],
            frameset_ok: true,
            scripting,
            foster: false,
            pending_table: vec![],
            context: None,
            ignore_lf: false,
            stopped: false,
            errors: 0,
        }
    }

    fn run(&mut self) {
        while !self.stopped {
            let acn = self.adjusted_current();
            self.tok.allow_cdata = acn.is_some_and(|n| self.doc.element(n).is_some_and(|e| e.ns != Ns::Html));
            let t = self.tok.next_token();
            if self.ignore_lf {
                self.ignore_lf = false;
                if t == Token::Char('\n') {
                    continue;
                }
            }
            let eof = t == Token::Eof;
            self.dispatch(t);
            if eof {
                break;
            }
        }
        self.errors += self.tok.errors;
        self.update_selectedcontent();
    }

    /// the DOM's selectedcontent mirroring (HTML 4.10.x "update a select's
    /// descendant selectedcontent elements"), applied once the tree is built:
    /// each selectedcontent of a single-select gets a clone of the selected
    /// option's children (scripts, frames and plugins removed)
    fn update_selectedcontent(&mut self) {
        let all: Vec<NodeId> = (0..self.doc.nodes.len()).collect();
        for sel in all {
            if !self.doc.is(sel, "select") || self.el(sel).attr("multiple").is_some() || self.doc.parent(sel).is_none() {
                continue;
            }
            let desc = self.doc.descendants(sel);
            let options: Vec<NodeId> = desc.iter().copied().filter(|&n| self.doc.is(n, "option")).collect();
            let targets: Vec<NodeId> = desc.iter().copied().filter(|&n| self.doc.is(n, "selectedcontent")).collect();
            if targets.is_empty() {
                continue;
            }
            let chosen = options
                .iter()
                .rev()
                .find(|&&o| self.el(o).attr("selected").is_some())
                .or_else(|| options.iter().find(|&&o| self.el(o).attr("disabled").is_none()))
                .copied();
            for t in targets {
                let old: Vec<NodeId> = self.doc.children(t).to_vec();
                for c in old {
                    self.doc.detach(c);
                }
                if let Some(o) = chosen {
                    for c in self.doc.children(o).to_vec() {
                        if let Some(k) = self.clone_subtree(c) {
                            self.doc.append(t, k);
                        }
                    }
                }
            }
        }
    }

    fn clone_subtree(&mut self, id: NodeId) -> Option<NodeId> {
        if let Some(e) = self.doc.element(id) {
            let dropped = match e.ns {
                Ns::Html => matches!(e.name.as_str(), "script" | "frame" | "iframe" | "embed" | "object"),
                Ns::Svg => e.name == "script",
                Ns::MathMl => false,
            };
            if dropped {
                return None;
            }
        }
        let data = self.doc.nodes[id].data.clone();
        let n = self.doc.create(data);
        for c in self.doc.children(id).to_vec() {
            if let Some(k) = self.clone_subtree(c) {
                self.doc.append(n, k);
            }
        }
        Some(n)
    }

    fn err(&mut self) {
        self.errors += 1;
    }

    // ---- the stack of open elements ----
    fn current(&self) -> NodeId {
        *self.open.last().unwrap()
    }
    fn adjusted_current(&self) -> Option<NodeId> {
        if self.open.len() == 1 && self.context.is_some() {
            self.context
        } else {
            self.open.last().copied()
        }
    }
    fn el(&self, id: NodeId) -> &Element {
        self.doc.element(id).unwrap()
    }
    fn is(&self, id: NodeId, name: &str) -> bool {
        self.doc.is(id, name)
    }
    fn is_one(&self, id: NodeId, names: &[&str]) -> bool {
        let e = self.el(id);
        e.ns == Ns::Html && names.contains(&e.name.as_str())
    }
    fn cur_is(&self, name: &str) -> bool {
        self.is(self.current(), name)
    }
    fn pop(&mut self) -> NodeId {
        self.open.pop().unwrap()
    }
    fn pop_until(&mut self, names: &[&str]) {
        while let Some(n) = self.open.pop() {
            if self.is_one(n, names) {
                break;
            }
        }
    }
    fn pop_until_node(&mut self, id: NodeId) {
        while let Some(n) = self.open.pop() {
            if n == id {
                break;
            }
        }
    }
    fn remove_open(&mut self, id: NodeId) {
        self.open.retain(|&n| n != id);
    }
    fn in_open(&self, id: NodeId) -> bool {
        self.open.contains(&id)
    }
    fn is_special(&self, id: NodeId) -> bool {
        let e = self.el(id);
        match e.ns {
            Ns::Html => SPECIAL_HTML.contains(&e.name.as_str()),
            Ns::MathMl => matches!(e.name.as_str(), "mi" | "mo" | "mn" | "ms" | "mtext" | "annotation-xml"),
            Ns::Svg => matches!(e.name.as_str(), "foreignObject" | "desc" | "title"),
        }
    }
    fn is_boundary(&self, id: NodeId, scope: Scope) -> bool {
        let e = self.el(id);
        let n = e.name.as_str();
        if scope == Scope::Table {
            return e.ns == Ns::Html && matches!(n, "html" | "table" | "template");
        }
        let base = match e.ns {
            Ns::Html => matches!(
                n,
                "applet" | "caption" | "html" | "table" | "td" | "th" | "marquee" | "object" | "select" | "template"
            ),
            Ns::MathMl => matches!(n, "mi" | "mo" | "mn" | "ms" | "mtext" | "annotation-xml"),
            Ns::Svg => matches!(n, "foreignObject" | "desc" | "title"),
        };
        base || (e.ns == Ns::Html
            && match scope {
                Scope::ListItem => matches!(n, "ol" | "ul"),
                Scope::Button => n == "button",
                _ => false,
            })
    }
    fn in_scope_where(&self, f: impl Fn(&Element) -> bool, scope: Scope) -> bool {
        for &n in self.open.iter().rev() {
            if f(self.el(n)) {
                return true;
            }
            if self.is_boundary(n, scope) {
                return false;
            }
        }
        false
    }
    fn in_scope(&self, name: &str, scope: Scope) -> bool {
        self.in_scope_where(|e| e.is(name), scope)
    }
    fn node_in_scope(&self, id: NodeId) -> bool {
        for &n in self.open.iter().rev() {
            if n == id {
                return true;
            }
            if self.is_boundary(n, Scope::Default) {
                return false;
            }
        }
        false
    }
    fn generate_implied(&mut self, except: Option<&str>) {
        while let Some(&n) = self.open.last() {
            let e = self.el(n);
            if e.ns == Ns::Html && IMPLIED_END.contains(&e.name.as_str()) && Some(e.name.as_str()) != except {
                self.open.pop();
            } else {
                break;
            }
        }
    }
    fn generate_implied_thorough(&mut self) {
        while let Some(&n) = self.open.last() {
            if self.is_one(n, IMPLIED_END_THOROUGH) {
                self.open.pop();
            } else {
                break;
            }
        }
    }
    fn close_p(&mut self) {
        self.generate_implied(Some("p"));
        if !self.cur_is("p") {
            self.err();
        }
        self.pop_until(&["p"]);
    }
    fn close_p_if_in_button_scope(&mut self) {
        if self.in_scope("p", Scope::Button) {
            self.close_p();
        }
    }

    // ---- insertion ----
    fn appropriate_place(&self, target: Option<NodeId>) -> (NodeId, Option<NodeId>) {
        let target = target.unwrap_or_else(|| self.current());
        let (parent, before) = if self.foster && self.is_one(target, &["table", "tbody", "tfoot", "thead", "tr"]) {
            let last_template = self.open.iter().rposition(|&n| self.is(n, "template"));
            let last_table = self.open.iter().rposition(|&n| self.is(n, "table"));
            match (last_template, last_table) {
                (Some(t), tb) if tb.is_none_or(|tb| t > tb) => (self.open[t], None),
                (_, None) => (self.open[0], None),
                (_, Some(tb)) => {
                    let table = self.open[tb];
                    match self.doc.parent(table) {
                        Some(p) => (p, Some(table)),
                        None => (self.open[tb - 1], None),
                    }
                }
            }
        } else {
            (target, None)
        };
        if let Some(e) = self.doc.element(parent) {
            if let (true, Some(c)) = (e.is("template"), e.template_contents) {
                return (c, None);
            }
        }
        (parent, before)
    }
    fn insert_foreign(&mut self, tag: &Tag, ns: Ns) -> NodeId {
        let (parent, before) = self.appropriate_place(None);
        let id = self.doc.create_element(ns, &tag.name, tag.attrs.clone());
        self.doc.insert_before(parent, id, before);
        self.open.push(id);
        id
    }
    fn insert_html(&mut self, tag: &Tag) -> NodeId {
        self.insert_foreign(tag, Ns::Html)
    }
    fn insert_char(&mut self, c: char) {
        let (parent, before) = self.appropriate_place(None);
        if parent == DOCUMENT {
            return;
        }
        let mut b = [0u8; 4];
        self.doc.insert_text(parent, before, c.encode_utf8(&mut b));
    }
    fn insert_comment(&mut self, data: &str, at: Option<NodeId>) {
        let (parent, before) = match at {
            Some(p) => (p, None),
            None => self.appropriate_place(None),
        };
        let id = self.doc.create(NodeData::Comment(data.to_string()));
        self.doc.insert_before(parent, id, before);
    }
    fn insert_pi(&mut self, target: &str, data: &str, at: Option<NodeId>) {
        let (parent, before) = match at {
            Some(p) => (p, None),
            None => self.appropriate_place(None),
        };
        let id = self.doc.create(NodeData::Pi { target: target.into(), data: data.into() });
        self.doc.insert_before(parent, id, before);
    }

    // ---- active formatting elements ----
    fn push_afe(&mut self, id: NodeId, tag: &Tag) {
        let mut same = vec![];
        for (i, f) in self.afe.iter().enumerate().rev() {
            match f {
                Fe::Marker => break,
                Fe::El(n, t) => {
                    let e = self.el(*n);
                    let mut a1: Vec<_> = t.attrs.iter().map(|a| (&a.name, &a.value)).collect();
                    let mut a2: Vec<_> = tag.attrs.iter().map(|a| (&a.name, &a.value)).collect();
                    a1.sort();
                    a2.sort();
                    if e.ns == Ns::Html && t.name == tag.name && a1 == a2 {
                        same.push(i);
                    }
                }
            }
        }
        if same.len() >= 3 {
            self.afe.remove(*same.last().unwrap());
        }
        self.afe.push(Fe::El(id, tag.clone()));
    }
    fn afe_index(&self, id: NodeId) -> Option<usize> {
        self.afe.iter().position(|f| matches!(f, Fe::El(n, _) if *n == id))
    }
    fn reconstruct(&mut self) {
        let Some(last) = self.afe.last() else { return };
        match last {
            Fe::Marker => return,
            Fe::El(n, _) if self.in_open(*n) => return,
            _ => {}
        }
        let mut i = self.afe.len() - 1;
        while i > 0 {
            match &self.afe[i - 1] {
                Fe::Marker => break,
                Fe::El(n, _) if self.in_open(*n) => break,
                _ => i -= 1,
            }
        }
        for j in i..self.afe.len() {
            let Fe::El(_, tag) = self.afe[j].clone() else { continue };
            let id = self.insert_html(&tag);
            self.afe[j] = Fe::El(id, tag);
        }
    }
    fn clear_afe_to_marker(&mut self) {
        while let Some(f) = self.afe.pop() {
            if matches!(f, Fe::Marker) {
                break;
            }
        }
    }

    /// the adoption agency algorithm; returns false if "any other end tag" applies
    fn adoption_agency(&mut self, subject: &str) -> bool {
        let cur = self.current();
        if self.is(cur, subject) && self.afe_index(cur).is_none() {
            self.pop();
            return true;
        }
        for _ in 0..8 {
            // the formatting element: the last one with this name after the last marker
            let mut fi = None;
            for (i, f) in self.afe.iter().enumerate().rev() {
                match f {
                    Fe::Marker => break,
                    Fe::El(n, _) if self.is(*n, subject) => {
                        fi = Some(i);
                        break;
                    }
                    _ => {}
                }
            }
            let Some(fi) = fi else { return false };
            let Fe::El(fe, fe_tag) = self.afe[fi].clone() else { unreachable!() };
            if !self.in_open(fe) {
                self.err();
                self.afe.remove(fi);
                return true;
            }
            if !self.node_in_scope(fe) {
                self.err();
                return true;
            }
            if fe != self.current() {
                self.err();
            }
            let fe_pos = self.open.iter().position(|&n| n == fe).unwrap();
            let furthest = self.open[fe_pos + 1..].iter().copied().find(|&n| self.is_special(n));
            let Some(furthest) = furthest else {
                self.pop_until_node(fe);
                self.afe.remove(fi);
                return true;
            };
            let common = self.open[fe_pos - 1];
            let mut bookmark = fi;
            let mut node = furthest;
            let mut last = furthest;
            let mut node_pos = self.open.iter().position(|&n| n == furthest).unwrap();
            let mut inner = 0;
            loop {
                inner += 1;
                node_pos -= 1;
                node = self.open[node_pos];
                if node == fe {
                    break;
                }
                if inner > 3 {
                    if let Some(ai) = self.afe_index(node) {
                        self.afe.remove(ai);
                        if ai < bookmark {
                            bookmark -= 1;
                        }
                    }
                }
                let Some(ai) = self.afe_index(node) else {
                    self.open.remove(node_pos);
                    continue;
                };
                let Fe::El(_, tag) = self.afe[ai].clone() else { unreachable!() };
                let new = self.doc.create_element(Ns::Html, &tag.name, tag.attrs.clone());
                self.afe[ai] = Fe::El(new, tag);
                self.open[node_pos] = new;
                node = new;
                if last == furthest {
                    bookmark = ai + 1;
                }
                self.doc.append(node, last);
                last = node;
            }
            let _ = node;
            let (parent, before) = self.appropriate_place(Some(common));
            self.doc.insert_before(parent, last, before);
            let new = self.doc.create_element(Ns::Html, &fe_tag.name, fe_tag.attrs.clone());
            let kids: Vec<NodeId> = self.doc.children(furthest).to_vec();
            for k in kids {
                self.doc.append(new, k);
            }
            self.doc.append(furthest, new);
            let fi_now = self.afe_index(fe).unwrap();
            self.afe.insert(bookmark, Fe::El(new, fe_tag.clone()));
            let fi_now = if bookmark <= fi_now { fi_now + 1 } else { fi_now };
            self.afe.remove(fi_now);
            self.remove_open(fe);
            let fp = self.open.iter().position(|&n| n == furthest).unwrap();
            self.open.insert(fp + 1, new);
        }
        true
    }

    fn reset_mode(&mut self) {
        for i in (0..self.open.len()).rev() {
            let last = i == 0;
            let node = if last && self.context.is_some() { self.context.unwrap() } else { self.open[i] };
            let e = self.el(node);
            if e.ns != Ns::Html {
                if last {
                    self.mode = Mode::InBody;
                    return;
                }
                continue;
            }
            self.mode = match e.name.as_str() {
                "td" | "th" if !last => Mode::InCell,
                "tr" => Mode::InRow,
                "tbody" | "thead" | "tfoot" => Mode::InTableBody,
                "caption" => Mode::InCaption,
                "colgroup" => Mode::InColumnGroup,
                "table" => Mode::InTable,
                "template" => *self.template_modes.last().unwrap(),
                "head" if !last => Mode::InHead,
                "body" => Mode::InBody,
                "frameset" => Mode::InFrameset,
                "html" => {
                    if self.head.is_none() {
                        Mode::BeforeHead
                    } else {
                        Mode::AfterHead
                    }
                }
                _ if last => Mode::InBody,
                _ => continue,
            };
            return;
        }
    }

    // ---- dispatch ----
    fn dispatch(&mut self, t: Token) {
        let html_rules = match self.adjusted_current() {
            None => true,
            Some(acn) => {
                let e = self.el(acn);
                let mathml_text = e.ns == Ns::MathMl && matches!(e.name.as_str(), "mi" | "mo" | "mn" | "ms" | "mtext");
                let html_ip = (e.ns == Ns::MathMl && e.html_integration)
                    || (e.ns == Ns::Svg && matches!(e.name.as_str(), "foreignObject" | "desc" | "title"));
                e.ns == Ns::Html
                    || (mathml_text && matches!(&t, Token::StartTag(g) if g.name != "mglyph" && g.name != "malignmark"))
                    || (mathml_text && matches!(t, Token::Char(_)))
                    || (e.ns == Ns::MathMl && e.name == "annotation-xml" && start(&t, &["svg"]))
                    || (html_ip && matches!(t, Token::StartTag(_) | Token::Char(_)))
                    || t == Token::Eof
            }
        };
        if html_rules {
            let mut t = t;
            loop {
                match self.step(self.mode, t) {
                    Done => break,
                    Again(x) => t = x,
                }
            }
        } else {
            self.foreign(t);
        }
    }

    fn foreign(&mut self, t: Token) {
        match t {
            Token::Char('\0') => {
                self.err();
                self.insert_char('\u{FFFD}');
            }
            Token::Char(c) => {
                self.insert_char(c);
                if !is_ws(c) {
                    self.frameset_ok = false;
                }
            }
            Token::Comment(c) => self.insert_comment(&c, None),
            Token::Pi(a, b) => self.insert_pi(&a, &b, None),
            Token::Doctype(_) => self.err(),
            Token::StartTag(ref g)
                if [
                    "b", "big", "blockquote", "body", "br", "center", "code", "dd", "div", "dl", "dt", "em", "embed",
                    "h1", "h2", "h3", "h4", "h5", "h6", "head", "hr", "i", "img", "li", "listing", "menu", "meta", "nobr",
                    "ol", "p", "pre", "ruby", "s", "small", "span", "strong", "strike", "sub", "sup", "table", "tt",
                    "u", "ul", "var",
                ]
                .contains(&g.name.as_str())
                    || (g.name == "font" && g.attrs.iter().any(|a| matches!(a.name.as_str(), "color" | "face" | "size"))) =>
            {
                self.break_out_of_foreign(t)
            }
            Token::EndTag(ref g) if g.name == "br" || g.name == "p" => self.break_out_of_foreign(t),
            Token::StartTag(mut g) => {
                let acn = self.adjusted_current().unwrap();
                let ns = self.el(acn).ns;
                if ns == Ns::MathMl {
                    adjust_mathml_attrs(&mut g);
                }
                if ns == Ns::Svg {
                    if let Some((_, to)) = SVG_TAGS.iter().find(|(f, _)| *f == g.name) {
                        g.name = to.to_string();
                    }
                    adjust_svg_attrs(&mut g);
                }
                adjust_foreign_attrs(&mut g);
                self.insert_foreign(&g, ns);
                if g.self_closing {
                    self.pop();
                }
            }
            Token::EndTag(g) => {
                let mut i = self.open.len() - 1;
                if !self.el(self.open[i]).name.eq_ignore_ascii_case(&g.name) {
                    self.err();
                }
                loop {
                    if i == 0 {
                        return;
                    }
                    let node = self.open[i];
                    if self.el(node).name.to_ascii_lowercase() == g.name {
                        self.open.truncate(i);
                        return;
                    }
                    i -= 1;
                    if self.el(self.open[i]).ns == Ns::Html {
                        let mut t = Token::EndTag(g);
                        loop {
                            match self.step(self.mode, t) {
                                Done => return,
                                Again(x) => t = x,
                            }
                        }
                    }
                }
            }
            Token::Eof => {}
        }
    }

    fn break_out_of_foreign(&mut self, t: Token) {
        self.err();
        loop {
            let n = self.current();
            let e = self.el(n);
            let mathml_text = e.ns == Ns::MathMl && matches!(e.name.as_str(), "mi" | "mo" | "mn" | "ms" | "mtext");
            let html_ip = (e.ns == Ns::MathMl && e.html_integration)
                || (e.ns == Ns::Svg && matches!(e.name.as_str(), "foreignObject" | "desc" | "title"));
            if e.ns == Ns::Html || mathml_text || html_ip {
                break;
            }
            self.pop();
        }
        let mut t = t;
        loop {
            match self.step(self.mode, t) {
                Done => break,
                Again(x) => t = x,
            }
        }
    }

    fn step(&mut self, mode: Mode, t: Token) -> Step {
        match mode {
            Mode::Initial => self.initial(t),
            Mode::BeforeHtml => self.before_html(t),
            Mode::BeforeHead => self.before_head(t),
            Mode::InHead => self.in_head(t),
            Mode::InHeadNoscript => self.in_head_noscript(t),
            Mode::AfterHead => self.after_head(t),
            Mode::InBody => self.in_body(t),
            Mode::Text => self.text(t),
            Mode::InTable => self.in_table(t),
            Mode::InTableText => self.in_table_text(t),
            Mode::InCaption => self.in_caption(t),
            Mode::InColumnGroup => self.in_column_group(t),
            Mode::InTableBody => self.in_table_body(t),
            Mode::InRow => self.in_row(t),
            Mode::InCell => self.in_cell(t),
            Mode::InTemplate => self.in_template(t),
            Mode::AfterBody => self.after_body(t),
            Mode::InFrameset => self.in_frameset(t),
            Mode::AfterFrameset => self.after_frameset(t),
            Mode::AfterAfterBody => self.after_after_body(t),
            Mode::AfterAfterFrameset => self.after_after_frameset(t),
        }
    }

    fn initial(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => Done,
            Token::Comment(c) => {
                self.insert_comment(&c, Some(DOCUMENT));
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, Some(DOCUMENT));
                Done
            }
            Token::Doctype(d) => {
                let name = d.name.clone().unwrap_or_default();
                if name != "html" || d.public_id.is_some() || d.system_id.as_deref().is_some_and(|s| s != "about:legacy-compat") {
                    self.err();
                }
                let has = self.doc.children(DOCUMENT).iter().any(|&c| {
                    matches!(self.doc.nodes[c].data, NodeData::Doctype { .. } | NodeData::Element(_))
                });
                if !has {
                    let id = self.doc.create(NodeData::Doctype {
                        name: name.clone(),
                        public_id: d.public_id.clone().unwrap_or_default(),
                        system_id: d.system_id.clone().unwrap_or_default(),
                    });
                    self.doc.append(DOCUMENT, id);
                }
                let p = d.public_id.as_deref().map(|s| s.to_ascii_lowercase());
                let s = d.system_id.as_deref().map(|s| s.to_ascii_lowercase());
                let pp = p.as_deref().unwrap_or("");
                let starts = |x: &str| p.as_deref().is_some_and(|p| p.starts_with(x));
                let quirks = d.force_quirks
                    || name != "html"
                    || pp == "-//w3o//dtd w3 html strict 3.0//en//"
                    || pp == "-/w3c/dtd html 4.0 transitional/en"
                    || pp == "html"
                    || s.as_deref() == Some("http://www.ibm.com/data/dtd/v11/ibmxhtml1-transitional.dtd")
                    || QUIRKY_PUBLIC_PREFIXES.iter().any(|x| starts(x))
                    || (s.is_none()
                        && (starts("-//w3c//dtd html 4.01 frameset//") || starts("-//w3c//dtd html 4.01 transitional//")));
                let limited = starts("-//w3c//dtd xhtml 1.0 frameset//")
                    || starts("-//w3c//dtd xhtml 1.0 transitional//")
                    || (s.is_some()
                        && (starts("-//w3c//dtd html 4.01 frameset//") || starts("-//w3c//dtd html 4.01 transitional//")));
                if quirks {
                    self.doc.quirks = Quirks::Quirks;
                } else if limited {
                    self.doc.quirks = Quirks::Limited;
                }
                self.mode = Mode::BeforeHtml;
                Done
            }
            t => {
                self.err();
                self.doc.quirks = Quirks::Quirks;
                self.mode = Mode::BeforeHtml;
                Again(t)
            }
        }
    }

    fn before_html(&mut self, t: Token) -> Step {
        match t {
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::Comment(c) => {
                self.insert_comment(&c, Some(DOCUMENT));
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, Some(DOCUMENT));
                Done
            }
            Token::Char(c) if is_ws(c) => Done,
            Token::StartTag(ref g) if g.name == "html" => {
                let id = self.doc.create_element(Ns::Html, "html", g.attrs.clone());
                self.doc.append(DOCUMENT, id);
                self.open.push(id);
                self.mode = Mode::BeforeHead;
                Done
            }
            Token::EndTag(ref g) if !matches!(g.name.as_str(), "head" | "body" | "html" | "br") => {
                self.err();
                Done
            }
            t => {
                let id = self.doc.create_element(Ns::Html, "html", vec![]);
                self.doc.append(DOCUMENT, id);
                self.open.push(id);
                self.mode = Mode::BeforeHead;
                Again(t)
            }
        }
    }

    fn before_head(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => Done,
            Token::Comment(c) => {
                self.insert_comment(&c, None);
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, None);
                Done
            }
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::StartTag(ref g) if g.name == "head" => {
                let h = self.insert_html(g);
                self.head = Some(h);
                self.mode = Mode::InHead;
                Done
            }
            Token::EndTag(ref g) if !matches!(g.name.as_str(), "head" | "body" | "html" | "br") => {
                self.err();
                Done
            }
            t => {
                let h = self.insert_html(&bare("head"));
                self.head = Some(h);
                self.mode = Mode::InHead;
                Again(t)
            }
        }
    }

    fn generic_text(&mut self, g: &Tag, state: State) {
        self.insert_html(g);
        self.tok.state = state;
        self.original = self.mode;
        self.mode = Mode::Text;
    }

    fn in_head(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => {
                self.insert_char(c);
                Done
            }
            Token::Comment(c) => {
                self.insert_comment(&c, None);
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, None);
                Done
            }
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::StartTag(ref g) if matches!(g.name.as_str(), "base" | "basefont" | "bgsound" | "link" | "meta") => {
                self.insert_html(g);
                self.pop();
                Done
            }
            Token::StartTag(ref g) if g.name == "title" => {
                self.generic_text(g, State::Rcdata);
                Done
            }
            Token::StartTag(ref g)
                if (g.name == "noscript" && self.scripting) || g.name == "noframes" || g.name == "style" =>
            {
                self.generic_text(g, State::Rawtext);
                Done
            }
            Token::StartTag(ref g) if g.name == "noscript" => {
                self.insert_html(g);
                self.mode = Mode::InHeadNoscript;
                Done
            }
            Token::StartTag(ref g) if g.name == "script" => {
                self.generic_text(g, State::ScriptData);
                Done
            }
            Token::EndTag(ref g) if g.name == "head" => {
                self.pop();
                self.mode = Mode::AfterHead;
                Done
            }
            Token::StartTag(ref g) if g.name == "template" => {
                self.afe.push(Fe::Marker);
                self.frameset_ok = false;
                self.mode = Mode::InTemplate;
                self.template_modes.push(Mode::InTemplate);
                self.insert_html(g);
                Done
            }
            Token::EndTag(ref g) if g.name == "template" => {
                if !self.open.iter().any(|&n| self.is(n, "template")) {
                    self.err();
                    return Done;
                }
                self.generate_implied_thorough();
                if !self.cur_is("template") {
                    self.err();
                }
                self.pop_until(&["template"]);
                self.clear_afe_to_marker();
                self.template_modes.pop();
                self.reset_mode();
                Done
            }
            Token::StartTag(ref g) if g.name == "head" => {
                self.err();
                Done
            }
            Token::EndTag(ref g) if !matches!(g.name.as_str(), "body" | "html" | "br") => {
                self.err();
                Done
            }
            t => {
                self.pop();
                self.mode = Mode::AfterHead;
                Again(t)
            }
        }
    }

    fn in_head_noscript(&mut self, t: Token) -> Step {
        match t {
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::EndTag(ref g) if g.name == "noscript" => {
                self.pop();
                self.mode = Mode::InHead;
                Done
            }
            Token::Char(c) if is_ws(c) => self.in_head(t),
            Token::Comment(_) | Token::Pi(..) => self.in_head(t),
            Token::StartTag(ref g)
                if matches!(g.name.as_str(), "basefont" | "bgsound" | "link" | "meta" | "noframes" | "style") =>
            {
                self.in_head(t)
            }
            Token::StartTag(ref g) if g.name == "head" || g.name == "noscript" => {
                self.err();
                Done
            }
            Token::EndTag(ref g) if g.name != "br" => {
                self.err();
                Done
            }
            t => {
                self.err();
                self.pop();
                self.mode = Mode::InHead;
                Again(t)
            }
        }
    }

    fn after_head(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => {
                self.insert_char(c);
                Done
            }
            Token::Comment(c) => {
                self.insert_comment(&c, None);
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, None);
                Done
            }
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::StartTag(ref g) if g.name == "body" => {
                self.insert_html(g);
                self.frameset_ok = false;
                self.mode = Mode::InBody;
                Done
            }
            Token::StartTag(ref g) if g.name == "frameset" => {
                self.insert_html(g);
                self.mode = Mode::InFrameset;
                Done
            }
            Token::StartTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "base" | "basefont" | "bgsound" | "link" | "meta" | "noframes" | "script" | "style" | "template" | "title"
                ) =>
            {
                self.err();
                let head = self.head.unwrap();
                self.open.push(head);
                let r = self.in_head(t);
                self.remove_open(head);
                r
            }
            Token::EndTag(ref g) if g.name == "template" => self.in_head(t),
            Token::StartTag(ref g) if g.name == "head" => {
                self.err();
                Done
            }
            Token::EndTag(ref g) if !matches!(g.name.as_str(), "body" | "html" | "br") => {
                self.err();
                Done
            }
            t => {
                self.insert_html(&bare("body"));
                self.frameset_ok = true;
                self.mode = Mode::InBody;
                Again(t)
            }
        }
    }

    fn any_other_end_tag(&mut self, name: &str) {
        let mut i = self.open.len();
        while i > 0 {
            i -= 1;
            let node = self.open[i];
            if self.is(node, name) {
                self.generate_implied(Some(name));
                if node != self.current() {
                    self.err();
                }
                self.pop_until_node(node);
                return;
            }
            if self.is_special(node) {
                self.err();
                return;
            }
        }
    }

    /// a template element is open, or the fragment context is a template
    fn parsing_template_contents(&self) -> bool {
        self.open.iter().any(|&n| self.is(n, "template")) || self.context.is_some_and(|c| self.is(c, "template"))
    }

    fn eof_check(&mut self) {
        if self.open.iter().any(|&n| !self.is_one(n, EOF_OK)) {
            self.err();
        }
    }

    fn in_body(&mut self, t: Token) -> Step {
        match t {
            Token::Char('\0') => {
                self.err();
            }
            Token::Char(c) => {
                self.reconstruct();
                self.insert_char(c);
                if !is_ws(c) {
                    self.frameset_ok = false;
                }
            }
            Token::Comment(c) => self.insert_comment(&c, None),
            Token::Pi(a, b) => self.insert_pi(&a, &b, None),
            Token::Doctype(_) => self.err(),
            Token::StartTag(ref g) if g.name == "html" => {
                self.err();
                if !self.open.iter().any(|&n| self.is(n, "template")) {
                    let html = self.open[0];
                    for a in &g.attrs {
                        let e = self.doc.element_mut(html).unwrap();
                        if e.attr(&a.name).is_none() {
                            e.attrs.push(a.clone());
                        }
                    }
                }
            }
            Token::StartTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "base" | "basefont" | "bgsound" | "link" | "meta" | "noframes" | "script" | "style" | "template" | "title"
                ) =>
            {
                return self.in_head(t)
            }
            Token::EndTag(ref g) if g.name == "template" => return self.in_head(t),
            Token::StartTag(ref g) if g.name == "body" => {
                self.err();
                if self.open.len() == 1 || !self.is(self.open[1], "body") || self.open.iter().any(|&n| self.is(n, "template")) {
                    return Done;
                }
                self.frameset_ok = false;
                let body = self.open[1];
                for a in &g.attrs {
                    let e = self.doc.element_mut(body).unwrap();
                    if e.attr(&a.name).is_none() {
                        e.attrs.push(a.clone());
                    }
                }
            }
            Token::StartTag(ref g) if g.name == "frameset" => {
                self.err();
                if self.open.len() == 1 || !self.is(self.open[1], "body") || !self.frameset_ok {
                    return Done;
                }
                let body = self.open[1];
                self.doc.detach(body);
                self.open.truncate(1);
                self.insert_html(g);
                self.mode = Mode::InFrameset;
            }
            Token::Eof => {
                if !self.template_modes.is_empty() {
                    return self.in_template(t);
                }
                self.eof_check();
                self.stopped = true;
            }
            Token::EndTag(ref g) if g.name == "body" || g.name == "html" => {
                if !self.in_scope("body", Scope::Default) {
                    self.err();
                    return Done;
                }
                self.eof_check();
                self.mode = Mode::AfterBody;
                if g.name == "html" {
                    return Again(t);
                }
            }
            Token::StartTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "address" | "article" | "aside" | "blockquote" | "center" | "details" | "dialog" | "dir" | "div"
                        | "dl" | "fieldset" | "figcaption" | "figure" | "footer" | "header" | "hgroup" | "main"
                        | "menu" | "nav" | "ol" | "p" | "search" | "section" | "summary" | "ul"
                ) =>
            {
                self.close_p_if_in_button_scope();
                self.insert_html(g);
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "h1" | "h2" | "h3" | "h4" | "h5" | "h6") => {
                self.close_p_if_in_button_scope();
                if self.is_one(self.current(), &["h1", "h2", "h3", "h4", "h5", "h6"]) {
                    self.err();
                    self.pop();
                }
                self.insert_html(g);
            }
            Token::StartTag(ref g) if g.name == "pre" || g.name == "listing" => {
                self.close_p_if_in_button_scope();
                self.insert_html(g);
                self.ignore_lf = true;
                self.frameset_ok = false;
            }
            Token::StartTag(ref g) if g.name == "form" => {
                let in_template = self.parsing_template_contents();
                if self.form.is_some() && !in_template {
                    self.err();
                    return Done;
                }
                self.close_p_if_in_button_scope();
                let f = self.insert_html(g);
                if !in_template {
                    self.form = Some(f);
                }
            }
            Token::StartTag(ref g) if g.name == "li" || g.name == "dd" || g.name == "dt" => {
                self.frameset_ok = false;
                let names: &[&str] = if g.name == "li" { &["li"] } else { &["dd", "dt"] };
                for i in (0..self.open.len()).rev() {
                    let node = self.open[i];
                    if self.is_one(node, names) {
                        let n = self.el(node).name.clone();
                        self.generate_implied(Some(&n));
                        if !self.cur_is(&n) {
                            self.err();
                        }
                        self.pop_until(&[&n]);
                        break;
                    }
                    if self.is_special(node) && !self.is_one(node, &["address", "div", "p"]) {
                        break;
                    }
                }
                self.close_p_if_in_button_scope();
                self.insert_html(g);
            }
            Token::StartTag(ref g) if g.name == "plaintext" => {
                self.close_p_if_in_button_scope();
                self.insert_html(g);
                self.tok.state = State::Plaintext;
            }
            Token::StartTag(ref g) if g.name == "button" => {
                if self.in_scope("button", Scope::Default) {
                    self.err();
                    self.generate_implied(None);
                    self.pop_until(&["button"]);
                }
                self.reconstruct();
                self.insert_html(g);
                self.frameset_ok = false;
            }
            Token::EndTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "address" | "article" | "aside" | "blockquote" | "button" | "center" | "details" | "dialog"
                        | "dir" | "div" | "dl" | "fieldset" | "figcaption" | "figure" | "footer" | "header" | "hgroup"
                        | "listing" | "main" | "menu" | "nav" | "ol" | "pre" | "search" | "section" | "select"
                        | "summary" | "ul"
                ) =>
            {
                if !self.in_scope(&g.name, Scope::Default) {
                    self.err();
                    return Done;
                }
                self.generate_implied(None);
                if !self.cur_is(&g.name) {
                    self.err();
                }
                self.pop_until(&[&g.name]);
            }
            Token::EndTag(ref g) if g.name == "form" => {
                if !self.parsing_template_contents() {
                    let node = self.form.take();
                    match node {
                        Some(n) if self.node_in_scope(n) => {
                            self.generate_implied(None);
                            if self.current() != n {
                                self.err();
                            }
                            self.remove_open(n);
                        }
                        _ => self.err(),
                    }
                } else {
                    if !self.in_scope("form", Scope::Default) {
                        self.err();
                        return Done;
                    }
                    self.generate_implied(None);
                    if !self.cur_is("form") {
                        self.err();
                    }
                    self.pop_until(&["form"]);
                }
            }
            Token::EndTag(ref g) if g.name == "p" => {
                if !self.in_scope("p", Scope::Button) {
                    self.err();
                    self.insert_html(&bare("p"));
                }
                self.close_p();
            }
            Token::EndTag(ref g) if g.name == "li" => {
                if !self.in_scope("li", Scope::ListItem) {
                    self.err();
                    return Done;
                }
                self.generate_implied(Some("li"));
                if !self.cur_is("li") {
                    self.err();
                }
                self.pop_until(&["li"]);
            }
            Token::EndTag(ref g) if g.name == "dd" || g.name == "dt" => {
                if !self.in_scope(&g.name, Scope::Default) {
                    self.err();
                    return Done;
                }
                self.generate_implied(Some(&g.name));
                if !self.cur_is(&g.name) {
                    self.err();
                }
                self.pop_until(&[&g.name]);
            }
            Token::EndTag(ref g) if matches!(g.name.as_str(), "h1" | "h2" | "h3" | "h4" | "h5" | "h6") => {
                let hs = ["h1", "h2", "h3", "h4", "h5", "h6"];
                if !self.in_scope_where(|e| e.ns == Ns::Html && hs.contains(&e.name.as_str()), Scope::Default) {
                    self.err();
                    return Done;
                }
                self.generate_implied(None);
                if !self.cur_is(&g.name) {
                    self.err();
                }
                self.pop_until(&hs);
            }
            Token::StartTag(ref g) if g.name == "a" => {
                let mut existing = None;
                for f in self.afe.iter().rev() {
                    match f {
                        Fe::Marker => break,
                        Fe::El(n, _) if self.is(*n, "a") => {
                            existing = Some(*n);
                            break;
                        }
                        _ => {}
                    }
                }
                if let Some(a) = existing {
                    self.err();
                    if !self.adoption_agency("a") {
                        self.any_other_end_tag("a");
                    }
                    if let Some(i) = self.afe_index(a) {
                        self.afe.remove(i);
                    }
                    self.remove_open(a);
                }
                self.reconstruct();
                let id = self.insert_html(g);
                self.push_afe(id, g);
            }
            Token::StartTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "b" | "big" | "code" | "em" | "font" | "i" | "s" | "small" | "strike" | "strong" | "tt" | "u"
                ) =>
            {
                self.reconstruct();
                let id = self.insert_html(g);
                self.push_afe(id, g);
            }
            Token::StartTag(ref g) if g.name == "nobr" => {
                self.reconstruct();
                if self.in_scope("nobr", Scope::Default) {
                    self.err();
                    if !self.adoption_agency("nobr") {
                        self.any_other_end_tag("nobr");
                    }
                    self.reconstruct();
                }
                let id = self.insert_html(g);
                self.push_afe(id, g);
            }
            Token::EndTag(ref g) if FORMATTING.contains(&g.name.as_str()) => {
                if !self.adoption_agency(&g.name) {
                    self.any_other_end_tag(&g.name);
                }
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "applet" | "marquee" | "object") => {
                self.reconstruct();
                self.insert_html(g);
                self.afe.push(Fe::Marker);
                self.frameset_ok = false;
            }
            Token::EndTag(ref g) if matches!(g.name.as_str(), "applet" | "marquee" | "object") => {
                if !self.in_scope(&g.name, Scope::Default) {
                    self.err();
                    return Done;
                }
                self.generate_implied(None);
                if !self.cur_is(&g.name) {
                    self.err();
                }
                self.pop_until(&[&g.name]);
                self.clear_afe_to_marker();
            }
            Token::StartTag(ref g) if g.name == "table" => {
                if self.doc.quirks != Quirks::Quirks {
                    self.close_p_if_in_button_scope();
                }
                self.insert_html(g);
                self.frameset_ok = false;
                self.mode = Mode::InTable;
            }
            Token::EndTag(ref g) if g.name == "br" => {
                self.err();
                return Again(Token::StartTag(bare("br")));
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "area" | "br" | "embed" | "img" | "keygen" | "wbr") => {
                self.reconstruct();
                self.insert_html(g);
                self.pop();
                self.frameset_ok = false;
            }
            Token::StartTag(ref g) if g.name == "input" => {
                if self.context.is_some_and(|c| self.is(c, "select")) {
                    self.err();
                    return Done;
                }
                if self.in_scope("select", Scope::Default) {
                    self.err();
                    self.pop_until(&["select"]);
                }
                self.reconstruct();
                self.insert_html(g);
                self.pop();
                if !g.attr("type").is_some_and(|v| v.eq_ignore_ascii_case("hidden")) {
                    self.frameset_ok = false;
                }
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "param" | "source" | "track") => {
                self.insert_html(g);
                self.pop();
            }
            Token::StartTag(ref g) if g.name == "hr" => {
                self.close_p_if_in_button_scope();
                if self.in_scope("select", Scope::Default) {
                    self.generate_implied(None);
                    if self.in_scope("option", Scope::Default) || self.in_scope("optgroup", Scope::Default) {
                        self.err();
                    }
                }
                self.insert_html(g);
                self.pop();
                self.frameset_ok = false;
            }
            Token::StartTag(ref g) if g.name == "image" => {
                self.err();
                let mut g2 = g.clone();
                g2.name = "img".into();
                return Again(Token::StartTag(g2));
            }
            Token::StartTag(ref g) if g.name == "textarea" => {
                self.insert_html(g);
                self.ignore_lf = true;
                self.tok.state = State::Rcdata;
                self.original = self.mode;
                self.frameset_ok = false;
                self.mode = Mode::Text;
            }
            Token::StartTag(ref g) if g.name == "xmp" => {
                self.close_p_if_in_button_scope();
                self.reconstruct();
                self.frameset_ok = false;
                self.generic_text(g, State::Rawtext);
            }
            Token::StartTag(ref g) if g.name == "iframe" => {
                self.frameset_ok = false;
                self.generic_text(g, State::Rawtext);
            }
            Token::StartTag(ref g) if g.name == "noembed" || (g.name == "noscript" && self.scripting) => {
                self.generic_text(g, State::Rawtext);
            }
            Token::StartTag(ref g) if g.name == "select" => {
                if self.context.is_some_and(|c| self.is(c, "select")) {
                    self.err();
                } else if self.in_scope("select", Scope::Default) {
                    self.err();
                    self.pop_until(&["select"]);
                } else {
                    self.reconstruct();
                    self.insert_html(g);
                    self.frameset_ok = false;
                }
            }
            Token::StartTag(ref g) if g.name == "option" => {
                if self.in_scope("select", Scope::Default) {
                    self.generate_implied(Some("optgroup"));
                    if self.in_scope("option", Scope::Default) {
                        self.err();
                    }
                } else if self.cur_is("option") {
                    self.pop();
                }
                self.reconstruct();
                self.insert_html(g);
            }
            Token::StartTag(ref g) if g.name == "optgroup" => {
                if self.in_scope("select", Scope::Default) {
                    self.generate_implied(None);
                    if self.in_scope("option", Scope::Default) || self.in_scope("optgroup", Scope::Default) {
                        self.err();
                    }
                } else if self.cur_is("option") {
                    self.pop();
                }
                self.reconstruct();
                self.insert_html(g);
            }
            Token::StartTag(ref g) if g.name == "rb" || g.name == "rtc" => {
                if self.in_scope("ruby", Scope::Default) {
                    self.generate_implied(None);
                    if !self.cur_is("ruby") {
                        self.err();
                    }
                }
                self.insert_html(g);
            }
            Token::StartTag(ref g) if g.name == "rp" || g.name == "rt" => {
                if self.in_scope("ruby", Scope::Default) {
                    self.generate_implied(Some("rtc"));
                    if !self.cur_is("rtc") && !self.cur_is("ruby") {
                        self.err();
                    }
                }
                self.insert_html(g);
            }
            Token::StartTag(ref g) if g.name == "math" || g.name == "svg" => {
                self.reconstruct();
                let mut g = g.clone();
                let ns = if g.name == "math" {
                    adjust_mathml_attrs(&mut g);
                    Ns::MathMl
                } else {
                    adjust_svg_attrs(&mut g);
                    Ns::Svg
                };
                adjust_foreign_attrs(&mut g);
                self.insert_foreign(&g, ns);
                if g.self_closing {
                    self.pop();
                }
            }
            Token::StartTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "caption" | "col" | "colgroup" | "frame" | "head" | "tbody" | "td" | "tfoot" | "th" | "thead" | "tr"
                ) =>
            {
                self.err();
            }
            Token::StartTag(ref g) => {
                self.reconstruct();
                self.insert_html(g);
            }
            Token::EndTag(ref g) => {
                let n = g.name.clone();
                self.any_other_end_tag(&n);
            }
        }
        Done
    }

    fn text(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) => {
                self.insert_char(c);
                Done
            }
            Token::Eof => {
                self.err();
                self.pop();
                self.mode = self.original;
                Again(t)
            }
            Token::EndTag(ref g) if g.name == "script" => {
                let script = self.pop();
                self.mode = self.original;
                if let Some(h) = self.hook.as_mut() {
                    h(&mut self.doc, script);
                }
                Done
            }
            _ => {
                self.pop();
                self.mode = self.original;
                Done
            }
        }
    }

    fn clear_to_table_context(&mut self, names: &[&str]) {
        while !self.is_one(self.current(), names) {
            self.pop();
        }
    }

    fn in_table(&mut self, t: Token) -> Step {
        match t {
            Token::Char(_) if self.is_one(self.current(), &["table", "tbody", "template", "tfoot", "thead", "tr"]) => {
                self.pending_table.clear();
                self.original = self.mode;
                self.mode = Mode::InTableText;
                Again(t)
            }
            Token::Comment(c) => {
                self.insert_comment(&c, None);
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, None);
                Done
            }
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "caption" => {
                self.clear_to_table_context(&["table", "template", "html"]);
                self.afe.push(Fe::Marker);
                self.insert_html(g);
                self.mode = Mode::InCaption;
                Done
            }
            Token::StartTag(ref g) if g.name == "colgroup" => {
                self.clear_to_table_context(&["table", "template", "html"]);
                self.insert_html(g);
                self.mode = Mode::InColumnGroup;
                Done
            }
            Token::StartTag(ref g) if g.name == "col" => {
                self.clear_to_table_context(&["table", "template", "html"]);
                self.insert_html(&bare("colgroup"));
                self.mode = Mode::InColumnGroup;
                Again(t)
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "tbody" | "tfoot" | "thead") => {
                self.clear_to_table_context(&["table", "template", "html"]);
                self.insert_html(g);
                self.mode = Mode::InTableBody;
                Done
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "td" | "th" | "tr") => {
                self.clear_to_table_context(&["table", "template", "html"]);
                self.insert_html(&bare("tbody"));
                self.mode = Mode::InTableBody;
                Again(t)
            }
            Token::StartTag(ref g) if g.name == "table" => {
                self.err();
                if !self.in_scope("table", Scope::Table) {
                    return Done;
                }
                self.pop_until(&["table"]);
                self.reset_mode();
                Again(t)
            }
            Token::EndTag(ref g) if g.name == "table" => {
                if !self.in_scope("table", Scope::Table) {
                    self.err();
                    return Done;
                }
                self.pop_until(&["table"]);
                self.reset_mode();
                Done
            }
            Token::EndTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "body" | "caption" | "col" | "colgroup" | "html" | "tbody" | "td" | "tfoot" | "th" | "thead" | "tr"
                ) =>
            {
                self.err();
                Done
            }
            Token::StartTag(ref g) if matches!(g.name.as_str(), "style" | "script" | "template") => self.in_head(t),
            Token::EndTag(ref g) if g.name == "template" => self.in_head(t),
            Token::StartTag(ref g) if g.name == "input" && g.attr("type").is_some_and(|v| v.eq_ignore_ascii_case("hidden")) => {
                self.err();
                self.insert_html(g);
                self.pop();
                Done
            }
            Token::StartTag(ref g) if g.name == "form" => {
                self.err();
                let in_template = self.parsing_template_contents();
                if self.form.is_some() && !in_template {
                    return Done;
                }
                let f = self.insert_html(g);
                if !in_template {
                    self.form = Some(f);
                }
                self.pop();
                Done
            }
            Token::Eof => self.in_body(t),
            t => {
                self.err();
                self.foster = true;
                let r = self.in_body(t);
                self.foster = false;
                r
            }
        }
    }

    fn in_table_text(&mut self, t: Token) -> Step {
        match t {
            Token::Char('\0') => {
                self.err();
                Done
            }
            Token::Char(c) => {
                self.pending_table.push(c);
                Done
            }
            t => {
                let chars = std::mem::take(&mut self.pending_table);
                if chars.iter().any(|&c| !is_ws(c)) {
                    self.err();
                    for c in chars {
                        self.foster = true;
                        let mut tok = Token::Char(c);
                        // "anything else" in table: in body with foster parenting
                        loop {
                            match self.in_body(tok) {
                                Done => break,
                                Again(x) => tok = x,
                            }
                        }
                        self.foster = false;
                    }
                } else {
                    for c in chars {
                        self.insert_char(c);
                    }
                }
                self.mode = self.original;
                Again(t)
            }
        }
    }

    fn close_caption(&mut self) -> bool {
        if !self.in_scope("caption", Scope::Table) {
            self.err();
            return false;
        }
        self.generate_implied(None);
        if !self.cur_is("caption") {
            self.err();
        }
        self.pop_until(&["caption"]);
        self.clear_afe_to_marker();
        self.mode = Mode::InTable;
        true
    }

    fn in_caption(&mut self, t: Token) -> Step {
        if end(&t, &["caption"]) {
            self.close_caption();
            return Done;
        }
        if start(&t, &["caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"]) || end(&t, &["table"]) {
            return if self.close_caption() { Again(t) } else { Done };
        }
        if end(&t, &["body", "col", "colgroup", "html", "tbody", "td", "tfoot", "th", "thead", "tr"]) {
            self.err();
            return Done;
        }
        self.in_body(t)
    }

    fn in_column_group(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => {
                self.insert_char(c);
                Done
            }
            Token::Comment(c) => {
                self.insert_comment(&c, None);
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, None);
                Done
            }
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::StartTag(ref g) if g.name == "col" => {
                self.insert_html(g);
                self.pop();
                Done
            }
            Token::EndTag(ref g) if g.name == "colgroup" => {
                if !self.cur_is("colgroup") {
                    self.err();
                } else {
                    self.pop();
                    self.mode = Mode::InTable;
                }
                Done
            }
            Token::EndTag(ref g) if g.name == "col" => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "template" => self.in_head(t),
            Token::EndTag(ref g) if g.name == "template" => self.in_head(t),
            Token::Eof => self.in_body(t),
            t => {
                if !self.cur_is("colgroup") {
                    self.err();
                    return Done;
                }
                self.pop();
                self.mode = Mode::InTable;
                Again(t)
            }
        }
    }

    fn in_table_body(&mut self, t: Token) -> Step {
        let body_ctx = ["tbody", "tfoot", "thead", "template", "html"];
        if start(&t, &["tr"]) {
            self.clear_to_table_context(&body_ctx);
            if let Token::StartTag(g) = &t {
                self.insert_html(g);
            }
            self.mode = Mode::InRow;
            return Done;
        }
        if start(&t, &["th", "td"]) {
            self.err();
            self.clear_to_table_context(&body_ctx);
            self.insert_html(&bare("tr"));
            self.mode = Mode::InRow;
            return Again(t);
        }
        if end(&t, &["tbody", "tfoot", "thead"]) {
            if !self.in_scope(tag_name(&t), Scope::Table) {
                self.err();
                return Done;
            }
            self.clear_to_table_context(&body_ctx);
            self.pop();
            self.mode = Mode::InTable;
            return Done;
        }
        if start(&t, &["caption", "col", "colgroup", "tbody", "tfoot", "thead"]) || end(&t, &["table"]) {
            if !self.in_scope_where(|e| e.ns == Ns::Html && matches!(e.name.as_str(), "tbody" | "thead" | "tfoot"), Scope::Table) {
                self.err();
                return Done;
            }
            self.clear_to_table_context(&body_ctx);
            self.pop();
            self.mode = Mode::InTable;
            return Again(t);
        }
        if end(&t, &["body", "caption", "col", "colgroup", "html", "td", "th", "tr"]) {
            self.err();
            return Done;
        }
        self.in_table(t)
    }

    fn close_row(&mut self) -> bool {
        if !self.in_scope("tr", Scope::Table) {
            self.err();
            return false;
        }
        self.clear_to_table_context(&["tr", "template", "html"]);
        self.pop();
        self.mode = Mode::InTableBody;
        true
    }

    fn in_row(&mut self, t: Token) -> Step {
        if start(&t, &["th", "td"]) {
            self.clear_to_table_context(&["tr", "template", "html"]);
            if let Token::StartTag(g) = &t {
                self.insert_html(g);
            }
            self.mode = Mode::InCell;
            self.afe.push(Fe::Marker);
            return Done;
        }
        if end(&t, &["tr"]) {
            self.close_row();
            return Done;
        }
        if start(&t, &["caption", "col", "colgroup", "tbody", "tfoot", "thead", "tr"]) || end(&t, &["table"]) {
            return if self.close_row() { Again(t) } else { Done };
        }
        if end(&t, &["tbody", "tfoot", "thead"]) {
            if !self.in_scope(tag_name(&t), Scope::Table) {
                self.err();
                return Done;
            }
            if !self.in_scope("tr", Scope::Table) {
                return Done;
            }
            self.close_row();
            return Again(t);
        }
        if end(&t, &["body", "caption", "col", "colgroup", "html", "td", "th"]) {
            self.err();
            return Done;
        }
        self.in_table(t)
    }

    fn close_cell(&mut self) {
        self.generate_implied(None);
        if !self.cur_is("td") && !self.cur_is("th") {
            self.err();
        }
        self.pop_until(&["td", "th"]);
        self.clear_afe_to_marker();
        self.mode = Mode::InRow;
    }

    fn in_cell(&mut self, t: Token) -> Step {
        if end(&t, &["td", "th"]) {
            let n = tag_name(&t).to_string();
            if !self.in_scope(&n, Scope::Table) {
                self.err();
                return Done;
            }
            self.generate_implied(None);
            if !self.cur_is(&n) {
                self.err();
            }
            self.pop_until(&[&n]);
            self.clear_afe_to_marker();
            self.mode = Mode::InRow;
            return Done;
        }
        if start(&t, &["caption", "col", "colgroup", "tbody", "td", "tfoot", "th", "thead", "tr"]) {
            if !self.in_scope("td", Scope::Table) && !self.in_scope("th", Scope::Table) {
                self.err();
                return Done;
            }
            self.close_cell();
            return Again(t);
        }
        if end(&t, &["body", "caption", "col", "colgroup", "html"]) {
            self.err();
            return Done;
        }
        if end(&t, &["table", "tbody", "tfoot", "thead", "tr"]) {
            if !self.in_scope(tag_name(&t), Scope::Table) {
                self.err();
                return Done;
            }
            self.close_cell();
            return Again(t);
        }
        self.in_body(t)
    }

    fn in_template(&mut self, t: Token) -> Step {
        match t {
            Token::Char(_) | Token::Comment(_) | Token::Pi(..) | Token::Doctype(_) => self.in_body(t),
            Token::StartTag(ref g)
                if matches!(
                    g.name.as_str(),
                    "base" | "basefont" | "bgsound" | "link" | "meta" | "noframes" | "script" | "style" | "template" | "title"
                ) =>
            {
                self.in_head(t)
            }
            Token::EndTag(ref g) if g.name == "template" => self.in_head(t),
            Token::StartTag(ref g) => {
                let m = match g.name.as_str() {
                    "caption" | "colgroup" | "tbody" | "tfoot" | "thead" => Mode::InTable,
                    "col" => Mode::InColumnGroup,
                    "tr" => Mode::InTableBody,
                    "td" | "th" => Mode::InRow,
                    _ => Mode::InBody,
                };
                self.template_modes.pop();
                self.template_modes.push(m);
                self.mode = m;
                Again(t)
            }
            Token::EndTag(_) => {
                self.err();
                Done
            }
            Token::Eof => {
                if !self.open.iter().any(|&n| self.is(n, "template")) {
                    self.stopped = true;
                    return Done;
                }
                self.err();
                self.pop_until(&["template"]);
                self.clear_afe_to_marker();
                self.template_modes.pop();
                self.reset_mode();
                Again(t)
            }
        }
    }

    fn after_body(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => self.in_body(t),
            Token::Comment(c) => {
                let html = self.open[0];
                self.insert_comment(&c, Some(html));
                Done
            }
            Token::Pi(a, b) => {
                let html = self.open[0];
                self.insert_pi(&a, &b, Some(html));
                Done
            }
            Token::Doctype(_) => {
                self.err();
                Done
            }
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::EndTag(ref g) if g.name == "html" => {
                if self.context.is_some() {
                    self.err();
                } else {
                    self.mode = Mode::AfterAfterBody;
                }
                Done
            }
            Token::Eof => {
                self.stopped = true;
                Done
            }
            t => {
                self.err();
                self.mode = Mode::InBody;
                Again(t)
            }
        }
    }

    fn in_frameset(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => self.insert_char(c),
            Token::Comment(c) => self.insert_comment(&c, None),
            Token::Pi(a, b) => self.insert_pi(&a, &b, None),
            Token::StartTag(ref g) if g.name == "html" => return self.in_body(t),
            Token::StartTag(ref g) if g.name == "frameset" => {
                self.insert_html(g);
            }
            Token::EndTag(ref g) if g.name == "frameset" => {
                if self.current() == self.open[0] {
                    self.err();
                } else {
                    self.pop();
                    if self.context.is_none() && !self.cur_is("frameset") {
                        self.mode = Mode::AfterFrameset;
                    }
                }
            }
            Token::StartTag(ref g) if g.name == "frame" => {
                self.insert_html(g);
                self.pop();
            }
            Token::StartTag(ref g) if g.name == "noframes" => return self.in_head(t),
            Token::Eof => {
                if self.current() != self.open[0] {
                    self.err();
                }
                self.stopped = true;
            }
            _ => self.err(),
        }
        Done
    }

    fn after_frameset(&mut self, t: Token) -> Step {
        match t {
            Token::Char(c) if is_ws(c) => self.insert_char(c),
            Token::Comment(c) => self.insert_comment(&c, None),
            Token::Pi(a, b) => self.insert_pi(&a, &b, None),
            Token::StartTag(ref g) if g.name == "html" => return self.in_body(t),
            Token::EndTag(ref g) if g.name == "html" => self.mode = Mode::AfterAfterFrameset,
            Token::StartTag(ref g) if g.name == "noframes" => return self.in_head(t),
            Token::Eof => self.stopped = true,
            _ => self.err(),
        }
        Done
    }

    fn after_after_body(&mut self, t: Token) -> Step {
        match t {
            Token::Comment(c) => {
                self.insert_comment(&c, Some(DOCUMENT));
                Done
            }
            Token::Pi(a, b) => {
                self.insert_pi(&a, &b, Some(DOCUMENT));
                Done
            }
            Token::Doctype(_) => self.in_body(t),
            Token::Char(c) if is_ws(c) => self.in_body(t),
            Token::StartTag(ref g) if g.name == "html" => self.in_body(t),
            Token::Eof => {
                self.stopped = true;
                Done
            }
            t => {
                self.err();
                self.mode = Mode::InBody;
                Again(t)
            }
        }
    }

    fn after_after_frameset(&mut self, t: Token) -> Step {
        match t {
            Token::Comment(c) => self.insert_comment(&c, Some(DOCUMENT)),
            Token::Pi(a, b) => self.insert_pi(&a, &b, Some(DOCUMENT)),
            Token::Doctype(_) => return self.in_body(t),
            Token::Char(c) if is_ws(c) => return self.in_body(t),
            Token::StartTag(ref g) if g.name == "html" => return self.in_body(t),
            Token::Eof => self.stopped = true,
            Token::StartTag(ref g) if g.name == "noframes" => return self.in_head(t),
            _ => self.err(),
        }
        Done
    }
}

/// unused helper kept for API symmetry
#[allow(dead_code)]
fn attr(name: &str, value: &str) -> Attr {
    Attr { name: name.into(), value: value.into() }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn simple_document() {
        let d = parse("<!DOCTYPE html><p>a<b>b</p>c", false);
        assert_eq!(
            d.dump(DOCUMENT),
            "| <!DOCTYPE html>\n| <html>\n|   <head>\n|   <body>\n|     <p>\n|       \"a\"\n|       <b>\n|         \"b\"\n|     <b>\n|       \"c\"\n"
        );
    }

    #[test]
    fn foster_parenting() {
        let d = parse("<table>x<tr><td>y</table>", false);
        assert!(d.dump(DOCUMENT).contains("|     \"x\"\n|     <table>"));
    }
}
