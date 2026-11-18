// The devtools data model: the JSON from `kestrel inspect`, and pure helpers
// (tested in test/model.test.ts) for the tree, hit testing and the box model.

export interface DomNode {
  id: number;
  type: "document" | "element" | "text" | "comment" | "doctype";
  name?: string;
  attrs?: [string, string][];
  text?: string;
  children?: DomNode[];
}

export interface BoxInfo {
  id: number;
  x: number;
  y: number;
  w: number;
  h: number;
  margin: [number, number, number, number];
  border: [number, number, number, number];
  padding: [number, number, number, number];
  display: string;
  position: string;
  color: string;
  background: string;
  font: string;
}

export interface NetEntry {
  url: string;
  status: number;
  bytes: number;
  source: string;
}

export interface Inspection {
  url: string;
  dom: DomNode;
  boxes: BoxInfo[];
  timings: [string, number][];
  console: string[];
  errors: string[];
  network: NetEntry[];
  png?: string;
}

/** parent links and depth for every node */
export function indexTree(root: DomNode): Map<number, { node: DomNode; parent: number | null; depth: number }> {
  const out = new Map<number, { node: DomNode; parent: number | null; depth: number }>();
  const walk = (n: DomNode, parent: number | null, depth: number) => {
    out.set(n.id, { node: n, parent, depth });
    for (const c of n.children ?? []) walk(c, n.id, depth + 1);
  };
  walk(root, null, 0);
  return out;
}

/** the path of ancestors from the root to a node (inclusive) */
export function pathTo(index: ReturnType<typeof indexTree>, id: number): number[] {
  const path: number[] = [];
  let cur: number | null = id;
  while (cur !== null && index.has(cur)) {
    path.push(cur);
    cur = index.get(cur)!.parent;
  }
  return path.reverse();
}

/** the deepest box containing the point: the box with the greatest depth, then the smallest area */
export function hitTest(boxes: BoxInfo[], index: ReturnType<typeof indexTree>, x: number, y: number): BoxInfo | null {
  let best: BoxInfo | null = null;
  let bestDepth = -1;
  for (const b of boxes) {
    if (x < b.x || y < b.y || x >= b.x + b.w || y >= b.y + b.h) continue;
    const d = index.get(b.id)?.depth ?? 0;
    if (d > bestDepth || (d === bestDepth && best && b.w * b.h < best.w * best.h)) {
      best = b;
      bestDepth = d;
    }
  }
  return best;
}

export interface Rect {
  x: number;
  y: number;
  w: number;
  h: number;
}

/** the four boxes of the CSS box model from a border box and its edges */
export function boxModel(b: BoxInfo): { margin: Rect; border: Rect; padding: Rect; content: Rect } {
  const [mt, mr, mb, ml] = b.margin;
  const [bt, br, bb, bl] = b.border;
  const [pt, pr, pb, pl] = b.padding;
  const border = { x: b.x, y: b.y, w: b.w, h: b.h };
  const margin = { x: b.x - ml, y: b.y - mt, w: b.w + ml + mr, h: b.h + mt + mb };
  const padding = { x: b.x + bl, y: b.y + bt, w: b.w - bl - br, h: b.h - bt - bb };
  const content = { x: padding.x + pl, y: padding.y + pt, w: padding.w - pl - pr, h: padding.h - pt - pb };
  return { margin, border, padding, content };
}

/** a short label for a node in the tree: <div id=a class=b> */
export function label(n: DomNode): string {
  switch (n.type) {
    case "element": {
      const id = n.attrs?.find((a) => a[0] === "id")?.[1];
      const cls = n.attrs?.find((a) => a[0] === "class")?.[1];
      return `<${n.name}${id ? "#" + id : ""}${cls ? "." + cls.trim().split(/\s+/).join(".") : ""}>`;
    }
    case "text":
      return JSON.stringify(n.text!.length > 40 ? n.text!.slice(0, 40) + "…" : n.text);
    case "comment":
      return `<!-- ${n.text} -->`;
    case "doctype":
      return `<!DOCTYPE ${n.name}>`;
    default:
      return "#document";
  }
}

/** timing bars: [name, ms, offset] laid end to end */
export function timeline(t: [string, number][]): [string, number, number][] {
  let at = 0;
  return t.map(([k, v]) => {
    const r: [string, number, number] = [k, v, at];
    at += v;
    return r;
  });
}
