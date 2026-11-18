//! The kestrel command line: render pages, and the batch modes the Python
//! harness uses to run the official test suites.
use kestrel::html::dom::{Ns, DOCUMENT};
use kestrel::html::tokenizer::{State, Token, Tokenizer};
use kestrel::html::treebuilder;
use kestrel::{paint, render};

fn fonts_dir() -> std::path::PathBuf {
    // next to the executable's repository: KESTREL_FONTS, or ./fonts, or ../fonts
    if let Ok(d) = std::env::var("KESTREL_FONTS") {
        return d.into();
    }
    for c in ["fonts", "../fonts", "../../fonts"] {
        if std::path::Path::new(c).join("DejaVuSans.ttf").exists() {
            return c.into();
        }
    }
    let exe = std::env::current_exe().unwrap_or_default();
    let mut p = exe.parent().map(|p| p.to_path_buf()).unwrap_or_default();
    for _ in 0..5 {
        if p.join("fonts").join("DejaVuSans.ttf").exists() {
            return p.join("fonts");
        }
        if !p.pop() {
            break;
        }
    }
    "fonts".into()
}
use std::io::{Read, Write};

fn json_str(s: &str) -> String {
    let mut o = String::from("\"");
    for c in s.chars() {
        match c {
            '"' => o.push_str("\\\""),
            '\\' => o.push_str("\\\\"),
            '\n' => o.push_str("\\n"),
            '\r' => o.push_str("\\r"),
            '\t' => o.push_str("\\t"),
            c if (c as u32) < 0x20 || c == '\u{7f}' => o.push_str(&format!("\\u{:04x}", c as u32)),
            c => o.push(c),
        }
    }
    o.push('"');
    o
}

fn opt(s: &Option<String>) -> String {
    s.as_ref().map(|x| json_str(x)).unwrap_or_else(|| "null".into())
}

/// records: field  field  byte-length  input-bytes (the input may contain anything)
fn records(input: &[u8]) -> Vec<(String, String, String)> {
    let mut out = vec![];
    let mut i = 0;
    while i < input.len() {
        let mut fields = vec![];
        for _ in 0..3 {
            let j = i + input[i..].iter().position(|&b| b == 0x1f).unwrap();
            fields.push(String::from_utf8_lossy(&input[i..j]).into_owned());
            i = j + 1;
        }
        let n: usize = fields[2].parse().unwrap();
        let body = String::from_utf8_lossy(&input[i..i + n]).into_owned();
        i += n;
        out.push((fields[0].clone(), fields[1].clone(), body));
    }
    out
}

fn state_by_name(n: &str) -> State {
    match n {
        "RCDATA state" => State::Rcdata,
        "RAWTEXT state" => State::Rawtext,
        "Script data state" => State::ScriptData,
        "PLAINTEXT state" => State::Plaintext,
        "CDATA section state" => State::CdataSection,
        _ => State::Data,
    }
}

/// records: state, last start tag, input; one JSON array of tokens per line out
fn tokenize_batch(input: &[u8]) -> String {
    let mut out = String::new();
    for (state, last, src) in records(input) {
        let mut t = Tokenizer::new(&src);
        t.state = state_by_name(&state);
        t.last_start_tag = last;
        let mut toks: Vec<String> = vec![];
        let mut chars = String::new();
        loop {
            let tok = t.next_token();
            if let Token::Char(c) = tok {
                chars.push(c);
                continue;
            }
            if !chars.is_empty() {
                toks.push(format!("[\"Character\",{}]", json_str(&chars)));
                chars.clear();
            }
            match tok {
                Token::Eof => break,
                Token::Doctype(d) => toks.push(format!(
                    "[\"DOCTYPE\",{},{},{},{}]",
                    opt(&d.name),
                    opt(&d.public_id),
                    opt(&d.system_id),
                    !d.force_quirks
                )),
                Token::StartTag(g) => {
                    let attrs: Vec<String> = g.attrs.iter().map(|a| format!("{}:{}", json_str(&a.name), json_str(&a.value))).collect();
                    if g.self_closing {
                        toks.push(format!("[\"StartTag\",{},{{{}}},true]", json_str(&g.name), attrs.join(",")));
                    } else {
                        toks.push(format!("[\"StartTag\",{},{{{}}}]", json_str(&g.name), attrs.join(",")));
                    }
                }
                Token::EndTag(g) => toks.push(format!("[\"EndTag\",{}]", json_str(&g.name))),
                Token::Comment(c) => toks.push(format!("[\"Comment\",{}]", json_str(&c))),
                Token::Pi(a, b) => toks.push(format!("[\"ProcessingInstruction\",{},{}]", json_str(&a), json_str(&b))),
                Token::Char(_) => unreachable!(),
            }
        }
        out.push('[');
        out.push_str(&toks.join(","));
        out.push_str("]\n");
    }
    out
}

/// records: context ("" = document, "svg path", "td"), scripting (0/1), input;
/// the html5lib tree dumps out, each followed by a "\x01" line
fn parse_batch(input: &[u8]) -> String {
    let mut out = String::new();
    for (ctx, scr, src) in records(input) {
        let parts = [ctx.as_str(), scr.as_str(), src.as_str()];
        let scripting = parts[1] == "1";
        let dump = std::panic::catch_unwind(|| {
            if parts[0].is_empty() {
                if parts[1] == "2" && kestrel::js::available() {
                    // scripted tests: scripts run as their end tags are parsed
                    let mut dummy = kestrel::html::dom::Document::new();
                    let mut sc = kestrel::js::ScriptContext::new(&mut dummy).unwrap();
                    let mut hook = |d: &mut kestrel::html::dom::Document, s: usize| {
                        let code = d.text_content(s);
                        sc.rebind(d);
                        sc.eval(&code, "script");
                    };
                    treebuilder::parse_with_scripts(parts[2], &mut hook).dump(DOCUMENT)
                } else {
                    treebuilder::parse(parts[2], scripting).dump(DOCUMENT)
                }
            } else {
                let (ns, name) = match parts[0].split_once(' ') {
                    Some(("svg", n)) => (Ns::Svg, n),
                    Some(("math", n)) => (Ns::MathMl, n),
                    _ => (Ns::Html, parts[0]),
                };
                let (doc, root) = treebuilder::parse_fragment(parts[2], ns, name, scripting);
                doc.dump(root)
            }
        });
        out.push_str(&dump.unwrap_or_else(|_| "PANIC\n".into()));
        out.push_str("\x01\n");
    }
    out
}

/// the devtools data: DOM tree, boxes, console, timings, network log (JSON)
fn inspect_json(r: &render::Rendered, url: &str, net: &[(String, u16, usize, String)]) -> String {
    use kestrel::html::dom::{Document, NodeData};
    fn node(doc: &Document, n: usize, out: &mut String, depth: usize) {
        if depth > 200 {
            return;
        }
        let kids: Vec<usize> = doc.children(n).to_vec();
        match &doc.nodes[n].data {
            NodeData::Element(e) => {
                let attrs: Vec<String> = e.attrs.iter().map(|a| format!("[{},{}]", json_str(&a.name), json_str(&a.value))).collect();
                out.push_str(&format!(r#"{{"id":{n},"type":"element","name":{},"attrs":[{}],"children":["#, json_str(&e.name), attrs.join(",")));
            }
            NodeData::Text(t) => {
                if t.trim().is_empty() {
                    out.push_str("null");
                    return;
                }
                out.push_str(&format!(r#"{{"id":{n},"type":"text","text":{}}}"#, json_str(t)));
                return;
            }
            NodeData::Comment(c) => {
                out.push_str(&format!(r#"{{"id":{n},"type":"comment","text":{}}}"#, json_str(c)));
                return;
            }
            NodeData::Doctype { name, .. } => {
                out.push_str(&format!(r#"{{"id":{n},"type":"doctype","name":{}}}"#, json_str(name)));
                return;
            }
            _ => out.push_str(&format!(r#"{{"id":{n},"type":"document","children":["#)),
        }
        let mut first = true;
        for k in kids {
            let mut s = String::new();
            node(doc, k, &mut s, depth + 1);
            if s == "null" || s.is_empty() {
                continue;
            }
            if !first {
                out.push(',');
            }
            first = false;
            out.push_str(&s);
        }
        out.push_str("]}");
    }
    let mut dom = String::new();
    node(&r.doc, 0, &mut dom, 0);
    let f4 = |a: [f32; 4]| format!("[{},{},{},{}]", a[0], a[1], a[2], a[3]);
    let boxes: Vec<String> = r
        .boxes
        .iter()
        .map(|b| {
            format!(
                r#"{{"id":{},"x":{},"y":{},"w":{},"h":{},"margin":{},"border":{},"padding":{},"display":{},"position":{},"color":{},"background":{},"font":{}}}"#,
                b.node,
                b.rect.0,
                b.rect.1,
                b.rect.2,
                b.rect.3,
                f4(b.margin),
                f4(b.border),
                f4(b.padding),
                json_str(&b.display),
                json_str(&b.position),
                json_str(&b.color),
                json_str(&b.background),
                json_str(&b.font)
            )
        })
        .collect();
    let timings: Vec<String> = r.timings.iter().map(|(k, v)| format!("[{},{v}]", json_str(k))).collect();
    let console: Vec<String> = r.console.iter().map(|c| json_str(c)).collect();
    let errors: Vec<String> = r.script_errors.iter().map(|c| json_str(c)).collect();
    let network: Vec<String> = net
        .iter()
        .map(|(u, s, n, src)| format!(r#"{{"url":{},"status":{s},"bytes":{n},"source":{}}}"#, json_str(u), json_str(src)))
        .collect();
    format!(
        r#"{{"url":{},"dom":{dom},"boxes":[{}],"timings":[{}],"console":[{}],"errors":[{}],"network":[{}]}}"#,
        json_str(url),
        boxes.join(","),
        timings.join(","),
        console.join(","),
        errors.join(","),
        network.join(",")
    )
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(|s| s.as_str()).unwrap_or("help");
    let mut stdin = vec![];
    match cmd {
        "tokenize-batch" => {
            std::io::stdin().read_to_end(&mut stdin).unwrap();
            std::io::stdout().write_all(tokenize_batch(&stdin).as_bytes()).unwrap();
        }
        "parse-batch" => {
            std::panic::set_hook(Box::new(|_| {}));
            std::io::stdin().read_to_end(&mut stdin).unwrap();
            std::io::stdout().write_all(parse_batch(&stdin).as_bytes()).unwrap();
        }
        "render" => {
            // kestrel render <file.html> [-o out.png] [--width W] [--height H] [--root DIR]
            let mut file = None;
            let mut out = "out.png".to_string();
            let (mut w, mut h) = (800usize, 600usize);
            let mut root = None;
            let mut i = 2;
            while i < args.len() {
                match args[i].as_str() {
                    "-o" => {
                        out = args[i + 1].clone();
                        i += 1;
                    }
                    "--width" => {
                        w = args[i + 1].parse().unwrap();
                        i += 1;
                    }
                    "--height" => {
                        h = args[i + 1].parse().unwrap();
                        i += 1;
                    }
                    "--root" => {
                        root = Some(std::path::PathBuf::from(&args[i + 1]));
                        i += 1;
                    }
                    f => file = Some(f.to_string()),
                }
                i += 1;
            }
            let file = file.expect("a file to render");
            let html = String::from_utf8_lossy(&std::fs::read(&file).expect("read")).to_string();
            let loader = render::Loader { root, net: None };
            let r = render::render(&html, &file.replace('\\', "/"), &loader, &fonts_dir(), render::RenderOptions { width: w, height: h, decoder: Some(render::decode_image), scripts: true, _p: Default::default() });
            std::fs::write(&out, paint::encode_png(&r.canvas)).expect("write png");
            for (k, v) in r.timings {
                eprintln!("{k}: {v:.1} ms");
            }
        }
        "inspect" => {
            // kestrel inspect <file|url> [-o out.png] [--json out.json] [--width W] [--height H]
            let mut input = None;
            let mut out = "out.png".to_string();
            let mut json_out = None;
            let (mut w, mut h) = (800usize, 600usize);
            let mut i = 2;
            while i < args.len() {
                match args[i].as_str() {
                    "-o" => {
                        out = args[i + 1].clone();
                        i += 1;
                    }
                    "--json" => {
                        json_out = Some(args[i + 1].clone());
                        i += 1;
                    }
                    "--width" => {
                        w = args[i + 1].parse().unwrap();
                        i += 1;
                    }
                    "--height" => {
                        h = args[i + 1].parse().unwrap();
                        i += 1;
                    }
                    f => input = Some(f.to_string()),
                }
                i += 1;
            }
            let input = input.expect("a file or URL");
            let netlog: render::NetLog = Default::default();
            let net = render::net_hook(netlog.clone());
            let loader = render::Loader { root: None, net };
            let base = if input.contains("://") { input.clone() } else { input.replace('\\', "/") };
            let html = match loader.fetch(&base) {
                Some(b) => String::from_utf8_lossy(&b).to_string(),
                None => {
                    eprintln!("cannot load {input}");
                    std::process::exit(1);
                }
            };
            let opts = render::RenderOptions { width: w, height: h, decoder: Some(render::decode_image), scripts: true, _p: Default::default() };
            let r = render::render(&html, &base, &loader, &fonts_dir(), opts);
            std::fs::write(&out, paint::encode_png(&r.canvas)).expect("write png");
            if let Some(j) = json_out {
                std::fs::write(j, inspect_json(&r, &base, &netlog.borrow())).expect("write json");
            }
        }
        "reftest-batch" => {
            // stdin lines: test-path<TAB>ref-path<TAB>match|mismatch<TAB>maxdiff<TAB>maxpixels ; root = argv[2]
            let root = args.get(2).map(std::path::PathBuf::from);
            std::io::stdin().read_to_end(&mut stdin).unwrap();
            let text = String::from_utf8_lossy(&stdin).to_string();
            let fonts = fonts_dir();
            std::panic::set_hook(Box::new(|_| {}));
            for line in text.lines() {
                let parts: Vec<&str> = line.split('\t').collect();
                if parts.len() < 5 {
                    continue;
                }
                let (t, rf, kind) = (parts[0], parts[1], parts[2]);
                let maxdiff: u8 = parts[3].parse().unwrap_or(0);
                let maxpix: usize = parts[4].parse().unwrap_or(0);
                let res = std::panic::catch_unwind(|| {
                    let loader = render::Loader { root: root.clone(), net: None };
                    let draw = |p: &str| {
                        let html = String::from_utf8_lossy(&std::fs::read(p).unwrap_or_default()).to_string();
                        render::render(&html, p, &loader, &fonts, render::RenderOptions { width: 800, height: 600, decoder: Some(render::decode_image), scripts: true, _p: Default::default() }).canvas
                    };
                    let a = draw(t);
                    let b = draw(rf);
                    render::compare(&a, &b)
                });
                match res {
                    Ok((n, mx)) => {
                        let same = n == 0 || (mx <= maxdiff && n <= maxpix);
                        let pass = if kind == "mismatch" { !same } else { same };
                        println!("{} {} {} {}", if pass { "PASS" } else { "FAIL" }, t, n, mx);
                    }
                    Err(_) => println!("FAIL {} panic 0", t),
                }
                std::io::stdout().flush().unwrap();
            }
        }
        "parse" => {
            let src = std::fs::read_to_string(&args[2]).expect("read file");
            print!("{}", treebuilder::parse(&src, false).dump(DOCUMENT));
        }
        _ => {
            eprintln!("usage: kestrel parse <file.html> | tokenize-batch | parse-batch");
            std::process::exit(2);
        }
    }
}
