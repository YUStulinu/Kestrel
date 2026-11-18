//! The document tree: nodes in an arena, addressed by index.
use super::tokenizer::Attr;

pub type NodeId = usize;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum Ns {
    Html,
    Svg,
    MathMl,
}

#[derive(Clone, Debug)]
pub struct Element {
    pub ns: Ns,
    pub name: String,
    /// foreign attributes keep their prefix as "xlink href" (the html5lib-tests form)
    pub attrs: Vec<Attr>,
    /// a template element's contents (a document fragment node)
    pub template_contents: Option<NodeId>,
    /// annotation-xml with encoding text/html or application/xhtml+xml
    pub html_integration: bool,
}

impl Element {
    pub fn attr(&self, name: &str) -> Option<&str> {
        self.attrs.iter().find(|a| a.name == name).map(|a| a.value.as_str())
    }
    pub fn is(&self, name: &str) -> bool {
        self.ns == Ns::Html && self.name == name
    }
}

#[derive(Clone, Debug)]
pub enum NodeData {
    Document,
    Fragment,
    Doctype { name: String, public_id: String, system_id: String },
    Element(Element),
    Text(String),
    Comment(String),
    Pi { target: String, data: String },
}

#[derive(Clone, Debug)]
pub struct Node {
    pub data: NodeData,
    pub parent: Option<NodeId>,
    pub children: Vec<NodeId>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Quirks {
    No,
    Limited,
    Quirks,
}

#[derive(Clone, Debug)]
pub struct Document {
    pub nodes: Vec<Node>,
    pub quirks: Quirks,
}

pub const DOCUMENT: NodeId = 0;

impl Default for Document {
    fn default() -> Self {
        Self::new()
    }
}

impl Document {
    pub fn new() -> Self {
        Document { nodes: vec![Node { data: NodeData::Document, parent: None, children: vec![] }], quirks: Quirks::No }
    }

    pub fn create(&mut self, data: NodeData) -> NodeId {
        self.nodes.push(Node { data, parent: None, children: vec![] });
        self.nodes.len() - 1
    }

    pub fn create_element(&mut self, ns: Ns, name: &str, attrs: Vec<Attr>) -> NodeId {
        let html_integration = ns == Ns::MathMl
            && name == "annotation-xml"
            && attrs.iter().any(|a| {
                a.name == "encoding"
                    && (a.value.eq_ignore_ascii_case("text/html") || a.value.eq_ignore_ascii_case("application/xhtml+xml"))
            });
        let id = self.create(NodeData::Element(Element {
            ns,
            name: name.to_string(),
            attrs,
            template_contents: None,
            html_integration,
        }));
        if ns == Ns::Html && name == "template" {
            let frag = self.create(NodeData::Fragment);
            if let NodeData::Element(e) = &mut self.nodes[id].data {
                e.template_contents = Some(frag);
            }
        }
        id
    }

    pub fn element(&self, id: NodeId) -> Option<&Element> {
        match &self.nodes[id].data {
            NodeData::Element(e) => Some(e),
            _ => None,
        }
    }
    pub fn element_mut(&mut self, id: NodeId) -> Option<&mut Element> {
        match &mut self.nodes[id].data {
            NodeData::Element(e) => Some(e),
            _ => None,
        }
    }
    /// true if `id` is the HTML element `name`
    pub fn is(&self, id: NodeId, name: &str) -> bool {
        self.element(id).is_some_and(|e| e.is(name))
    }
    pub fn parent(&self, id: NodeId) -> Option<NodeId> {
        self.nodes[id].parent
    }
    pub fn children(&self, id: NodeId) -> &[NodeId] {
        &self.nodes[id].children
    }

    pub fn detach(&mut self, id: NodeId) {
        if let Some(p) = self.nodes[id].parent.take() {
            self.nodes[p].children.retain(|&c| c != id);
        }
    }
    pub fn append(&mut self, parent: NodeId, child: NodeId) {
        self.detach(child);
        self.nodes[child].parent = Some(parent);
        self.nodes[parent].children.push(child);
    }
    pub fn insert_before(&mut self, parent: NodeId, child: NodeId, before: Option<NodeId>) {
        match before {
            None => self.append(parent, child),
            Some(b) => {
                self.detach(child);
                let i = self.nodes[parent].children.iter().position(|&c| c == b).unwrap();
                self.nodes[child].parent = Some(parent);
                self.nodes[parent].children.insert(i, child);
            }
        }
    }

    /// insert text, merging with a Text node right before the insertion point
    pub fn insert_text(&mut self, parent: NodeId, before: Option<NodeId>, s: &str) {
        let kids = &self.nodes[parent].children;
        let prev = match before {
            None => kids.last().copied(),
            Some(b) => {
                let i = kids.iter().position(|&c| c == b).unwrap();
                if i > 0 {
                    Some(kids[i - 1])
                } else {
                    None
                }
            }
        };
        if let Some(p) = prev {
            if let NodeData::Text(t) = &mut self.nodes[p].data {
                t.push_str(s);
                return;
            }
        }
        let t = self.create(NodeData::Text(s.to_string()));
        self.insert_before(parent, t, before);
    }

    /// the concatenated text of a node and its descendants
    pub fn text_content(&self, id: NodeId) -> String {
        let mut s = String::new();
        self.collect_text(id, &mut s);
        s
    }
    fn collect_text(&self, id: NodeId, out: &mut String) {
        match &self.nodes[id].data {
            NodeData::Text(t) => out.push_str(t),
            _ => {
                for &c in &self.nodes[id].children {
                    self.collect_text(c, out);
                }
            }
        }
    }

    /// the document element (the first element child of the document)
    pub fn document_element(&self) -> Option<NodeId> {
        self.nodes[DOCUMENT].children.iter().copied().find(|&c| self.element(c).is_some())
    }

    /// depth-first descendants of `id` (not including it)
    pub fn descendants(&self, id: NodeId) -> Vec<NodeId> {
        let mut out = vec![];
        let mut stack: Vec<NodeId> = self.nodes[id].children.iter().rev().copied().collect();
        while let Some(n) = stack.pop() {
            out.push(n);
            stack.extend(self.nodes[n].children.iter().rev());
        }
        out
    }

    /// the html5lib-tests tree format ("| <html>" lines) of the children of `root`
    pub fn dump(&self, root: NodeId) -> String {
        let mut s = String::new();
        for &c in &self.nodes[root].children {
            self.dump_node(c, 1, &mut s);
        }
        s
    }
    fn dump_node(&self, id: NodeId, depth: usize, s: &mut String) {
        let pad = "  ".repeat(depth - 1);
        match &self.nodes[id].data {
            NodeData::Doctype { name, public_id, system_id } => {
                if public_id.is_empty() && system_id.is_empty() {
                    s.push_str(&format!("| {pad}<!DOCTYPE {name}>\n"));
                } else {
                    s.push_str(&format!("| {pad}<!DOCTYPE {name} \"{public_id}\" \"{system_id}\">\n"));
                }
            }
            NodeData::Text(t) => s.push_str(&format!("| {pad}\"{t}\"\n")),
            NodeData::Comment(c) => s.push_str(&format!("| {pad}<!-- {c} -->\n")),
            NodeData::Pi { target, data } => s.push_str(&format!("| {pad}<?{target} {data}?>\n")),
            NodeData::Element(e) => {
                let prefix = match e.ns {
                    Ns::Html => "",
                    Ns::Svg => "svg ",
                    Ns::MathMl => "math ",
                };
                s.push_str(&format!("| {pad}<{prefix}{}>\n", e.name));
                let mut attrs: Vec<&Attr> = e.attrs.iter().collect();
                attrs.sort_by(|a, b| a.name.cmp(&b.name));
                for a in attrs {
                    s.push_str(&format!("| {pad}  {}=\"{}\"\n", a.name, a.value));
                }
                if let Some(t) = e.template_contents {
                    s.push_str(&format!("| {pad}  content\n"));
                    for &c in &self.nodes[t].children {
                        self.dump_node(c, depth + 2, s);
                    }
                }
            }
            NodeData::Document | NodeData::Fragment => {}
        }
        for &c in &self.nodes[id].children {
            self.dump_node(c, depth + 1, s);
        }
    }
}
