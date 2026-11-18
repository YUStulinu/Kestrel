// The Kestrel devtools: render a page with the engine (through the Go
// server), then inspect its DOM, boxes, console, network and timings.
import { boxModel, hitTest, indexTree, label, pathTo, timeline } from "./model.js";
import type { BoxInfo, DomNode, Inspection } from "./model.js";

const $ = <T extends HTMLElement>(sel: string) => document.querySelector(sel) as T;
let data: Inspection | null = null;
let index: ReturnType<typeof indexTree> = new Map();
let selected: number | null = null;
let hovered: BoxInfo | null = null;
const expanded = new Set<number>();

async function loadPages() {
  const names: string[] = await (await fetch("/api/pages")).json();
  const sel = $<HTMLSelectElement>("#page");
  sel.innerHTML = names.map((n) => `<option>${n}</option>`).join("");
}

async function render(body: object) {
  $("#status").textContent = "rendering…";
  const t0 = performance.now();
  const r = await fetch("/api/render", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
  if (!r.ok) {
    $("#status").textContent = "error: " + (await r.text());
    return;
  }
  data = await r.json();
  index = indexTree(data!.dom);
  selected = null;
  expanded.clear();
  for (const [id, e] of index) if (e.depth <= 2) expanded.add(id);
  $<HTMLImageElement>("#shot").src = "data:image/png;base64," + data!.png;
  $("#status").textContent = `${data!.url} — ${data!.boxes.length} boxes, ${Math.round(performance.now() - t0)} ms round trip`;
  drawTree();
  drawPanels();
  drawOverlay();
}

function drawTree() {
  const out: string[] = [];
  const walk = (n: DomNode) => {
    const e = index.get(n.id)!;
    const kids = n.children ?? [];
    const open = expanded.has(n.id);
    const twisty = kids.length ? (open ? "▾" : "▸") : " ";
    const cls = n.id === selected ? "node sel" : "node";
    out.push(`<div class="${cls}" data-id="${n.id}" style="padding-left:${e.depth * 12}px"><span class="tw">${twisty}</span>${escapeHtml(label(n))}</div>`);
    if (open) kids.forEach(walk);
  };
  if (data) walk(data.dom);
  $("#tree").innerHTML = out.join("");
}

function escapeHtml(s: string) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

function drawPanels() {
  if (!data) return;
  const b = selected !== null ? data.boxes.find((x) => x.id === selected) : null;
  $("#computed").innerHTML = b
    ? `<div class="bm">${boxModelHtml(b)}</div>
       <table>${[["display", b.display], ["position", b.position], ["color", b.color], ["background", b.background], ["font", b.font],
         ["size", `${b.w.toFixed(1)} × ${b.h.toFixed(1)}`], ["at", `${b.x.toFixed(1)}, ${b.y.toFixed(1)}`]]
         .map(([k, v]) => `<tr><th>${k}</th><td>${escapeHtml(String(v))}</td></tr>`).join("")}</table>`
    : `<p class="muted">Select an element (in the tree or on the page).</p>`;
  const msgs = [...data.console.map((c) => `<div class="log">${escapeHtml(c)}</div>`), ...data.errors.map((e) => `<div class="err">${escapeHtml(e)}</div>`)];
  $("#console").innerHTML = msgs.join("") || `<p class="muted">No messages.</p>`;
  $("#network").innerHTML = data.network.length
    ? `<table><tr><th>status</th><th>source</th><th>bytes</th><th>url</th></tr>${data.network
        .map((n) => `<tr><td>${n.status}</td><td>${n.source}</td><td>${n.bytes}</td><td>${escapeHtml(n.url)}</td></tr>`)
        .join("")}</table>`
    : `<p class="muted">Local files only.</p>`;
  const tl = timeline(data.timings);
  const total = tl.reduce((s, [, ms]) => s + ms, 0) || 1;
  $("#timings").innerHTML = tl
    .map(([k, ms, at]) => `<div class="bar"><span class="lbl">${k}</span><span class="track"><span class="fill" style="left:${(at / total) * 100}%;width:${Math.max(0.5, (ms / total) * 100)}%"></span></span><span class="ms">${ms.toFixed(1)} ms</span></div>`)
    .join("");
}

function boxModelHtml(b: BoxInfo) {
  const f = (v: number) => (v ? v.toFixed(0) : "–");
  const [mt, mr, mb, ml] = b.margin;
  const [bt, br, bb, bl] = b.border;
  const [pt, pr, pb, pl] = b.padding;
  const m = boxModel(b);
  return `<div class="m">margin<span class="t">${f(mt)}</span><span class="r">${f(mr)}</span><span class="b">${f(mb)}</span><span class="l">${f(ml)}</span>
    <div class="bo">border<span class="t">${f(bt)}</span><span class="r">${f(br)}</span><span class="b">${f(bb)}</span><span class="l">${f(bl)}</span>
    <div class="pa">padding<span class="t">${f(pt)}</span><span class="r">${f(pr)}</span><span class="b">${f(pb)}</span><span class="l">${f(pl)}</span>
    <div class="co">${m.content.w.toFixed(0)} × ${m.content.h.toFixed(0)}</div></div></div></div>`;
}

function drawOverlay() {
  const c = $<HTMLCanvasElement>("#overlay");
  const img = $<HTMLImageElement>("#shot");
  c.width = img.naturalWidth || 800;
  c.height = img.naturalHeight || 600;
  const g = c.getContext("2d")!;
  g.clearRect(0, 0, c.width, c.height);
  const show = (b: BoxInfo | null | undefined, strong: boolean) => {
    if (!b) return;
    const m = boxModel(b);
    const fill = (r: { x: number; y: number; w: number; h: number }, col: string) => {
      g.fillStyle = col;
      g.fillRect(r.x, r.y, r.w, r.h);
    };
    fill(m.margin, strong ? "rgba(246,178,107,0.55)" : "rgba(246,178,107,0.35)");
    g.clearRect(m.border.x, m.border.y, m.border.w, m.border.h);
    fill(m.border, "rgba(255,229,153,0.6)");
    g.clearRect(m.padding.x, m.padding.y, m.padding.w, m.padding.h);
    fill(m.padding, "rgba(147,196,125,0.55)");
    g.clearRect(m.content.x, m.content.y, m.content.w, m.content.h);
    fill(m.content, "rgba(111,168,220,0.55)");
  };
  if (data) {
    show(hovered, false);
    show(data.boxes.find((b) => b.id === selected), true);
  }
}

function select(id: number) {
  selected = id;
  for (const p of pathTo(index, id)) expanded.add(p);
  drawTree();
  drawPanels();
  drawOverlay();
  document.querySelector(`.node[data-id="${id}"]`)?.scrollIntoView({ block: "nearest" });
}

function setup() {
  loadPages().catch(() => ($("#status").textContent = "server not reachable"));
  $("#go-page").addEventListener("click", () => render({ page: $<HTMLSelectElement>("#page").value }));
  $("#go-url").addEventListener("click", () => render({ url: $<HTMLInputElement>("#url").value }));
  $("#go-html").addEventListener("click", () => render({ html: $<HTMLTextAreaElement>("#html").value }));
  $("#tree").addEventListener("click", (e) => {
    const row = (e.target as HTMLElement).closest(".node") as HTMLElement | null;
    if (!row) return;
    const id = +row.dataset.id!;
    if ((e.target as HTMLElement).classList.contains("tw")) {
      expanded.has(id) ? expanded.delete(id) : expanded.add(id);
      drawTree();
      return;
    }
    select(id);
  });
  $("#tree").addEventListener("mousemove", (e) => {
    const row = (e.target as HTMLElement).closest(".node") as HTMLElement | null;
    hovered = row && data ? data.boxes.find((b) => b.id === +row.dataset.id!) ?? null : null;
    drawOverlay();
  });
  const pageXY = (e: MouseEvent) => {
    const c = $<HTMLCanvasElement>("#overlay");
    const r = c.getBoundingClientRect();
    return [((e.clientX - r.left) * c.width) / r.width, ((e.clientY - r.top) * c.height) / r.height];
  };
  $("#overlay").addEventListener("mousemove", (e) => {
    if (!data) return;
    const [x, y] = pageXY(e as MouseEvent);
    hovered = hitTest(data.boxes, index, x, y);
    drawOverlay();
  });
  $("#overlay").addEventListener("click", (e) => {
    if (!data) return;
    const [x, y] = pageXY(e as MouseEvent);
    const b = hitTest(data.boxes, index, x, y);
    if (b) select(b.id);
  });
  document.querySelectorAll<HTMLButtonElement>(".tabs button").forEach((b) =>
    b.addEventListener("click", () => {
      document.querySelectorAll(".tabs button").forEach((x) => x.classList.toggle("on", x === b));
      document.querySelectorAll(".pane").forEach((p) => p.classList.toggle("on", p.id === b.dataset.pane));
    }),
  );
}

setup();
