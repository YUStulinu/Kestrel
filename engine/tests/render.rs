//! End-to-end checks: HTML in, boxes, pixels and DOM out.
use kestrel::render::{render, BoxInfo, Loader, Rendered, RenderOptions};
use std::path::Path;

fn run(html: &str) -> Rendered {
    let loader = Loader { root: None, net: None };
    let fonts = Path::new(env!("CARGO_MANIFEST_DIR")).join("../fonts");
    render(html, "file:///test.html", &loader, &fonts, RenderOptions { width: 400, height: 300, decoder: None, scripts: true, _p: Default::default() })
}

fn by_id<'a>(r: &'a Rendered, id: &str) -> &'a BoxInfo {
    let node = (0..r.doc.nodes.len())
        .find(|&n| r.doc.element(n).and_then(|e| e.attr("id")) == Some(id))
        .unwrap_or_else(|| panic!("no element #{id}"));
    r.boxes.iter().find(|b| b.node == node).unwrap_or_else(|| panic!("no box for #{id}"))
}

fn pixel(r: &Rendered, x: usize, y: usize) -> [u8; 3] {
    let i = (y * r.canvas.w + x) * 4;
    [r.canvas.px[i], r.canvas.px[i + 1], r.canvas.px[i + 2]]
}

#[test]
fn sibling_margins_collapse() {
    let r = run(r#"<body style="margin:0"><div id=a style="height:10px;margin-bottom:30px"></div><div id=b style="height:10px;margin-top:20px"></div>"#);
    assert_eq!(by_id(&r, "a").rect.1, 0.0);
    assert_eq!(by_id(&r, "b").rect.1, 40.0); // 10 + max(30, 20)
}

#[test]
fn flex_grow_shares_free_space() {
    let r = run(r#"<body style="margin:0"><div style="display:flex;width:300px"><div id=a style="flex:1"></div><div id=b style="flex:2"></div></div>"#);
    assert_eq!(by_id(&r, "a").rect.2, 100.0);
    assert_eq!(by_id(&r, "b").rect.0, 100.0);
    assert_eq!(by_id(&r, "b").rect.2, 200.0);
}

#[test]
fn floats_and_absolute_positioning() {
    let r = run(r#"<body style="margin:0"><div style="position:relative;width:200px;height:100px">
        <div id=f style="float:right;width:50px;height:20px"></div>
        <div id=p style="position:absolute;left:10px;bottom:5px;width:30px;height:30px"></div></div>"#);
    assert_eq!(by_id(&r, "f").rect.0, 150.0);
    assert_eq!((by_id(&r, "p").rect.0, by_id(&r, "p").rect.1), (10.0, 65.0));
}

#[test]
fn backgrounds_and_borders_are_painted() {
    let r = run(r#"<body style="margin:0"><div style="width:40px;height:40px;background:#00ff00;border:5px solid #0000ff"></div>"#);
    assert_eq!(pixel(&r, 2, 2), [0, 0, 255]);
    assert_eq!(pixel(&r, 20, 20), [0, 255, 0]);
    assert_eq!(pixel(&r, 100, 100), [255, 255, 255]);
}

#[test]
fn text_is_drawn_with_the_font() {
    let r = run(r#"<body style="margin:0;font:40px DejaVu Sans"><span style="color:#000">Hello</span>"#);
    let ink = (0..60).flat_map(|y| (0..120).map(move |x| (x, y))).filter(|&(x, y)| pixel(&r, x, y)[0] < 100).count();
    assert!(ink > 200, "only {ink} dark pixels");
}

#[test]
fn scripts_mutate_the_dom_before_layout() {
    if !kestrel::js::available() {
        return; // built without a C++ compiler
    }
    let r = run(r#"<body style="margin:0"><div id=a></div><script>
        const d = document.getElementById('a');
        d.style.height = '25px';
        const n = document.createElement('div'); n.id = 'b'; n.style.height = '5px';
        document.body.appendChild(n);
        console.log('kids', document.body.children.length);
        </script>"#);
    assert_eq!(by_id(&r, "a").rect.3, 25.0);
    assert_eq!(by_id(&r, "b").rect.1, 25.0);
    assert!(r.console.iter().any(|c| c.contains("kids")), "{:?} {:?}", r.console, r.script_errors);
}
