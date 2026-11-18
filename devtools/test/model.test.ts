import { test } from "node:test";
import assert from "node:assert/strict";
import { boxModel, hitTest, indexTree, label, pathTo, timeline } from "../src/model.ts";
import type { BoxInfo, DomNode } from "../src/model.ts";

const dom: DomNode = {
  id: 0,
  type: "document",
  children: [
    {
      id: 2, type: "element", name: "body", attrs: [],
      children: [
        { id: 5, type: "element", name: "div", attrs: [["id", "a"], ["class", "x  y"]], children: [{ id: 6, type: "text", text: "hello" }] },
      ],
    },
  ],
};

const box = (id: number, x: number, y: number, w: number, h: number): BoxInfo => ({
  id, x, y, w, h, margin: [1, 2, 3, 4], border: [1, 1, 1, 1], padding: [2, 2, 2, 2],
  display: "block", position: "static", color: "", background: "", font: "",
});

test("tree index and paths", () => {
  const ix = indexTree(dom);
  assert.equal(ix.get(6)!.depth, 3);
  assert.deepEqual(pathTo(ix, 6), [0, 2, 5, 6]);
});

test("hit testing picks the deepest box", () => {
  const ix = indexTree(dom);
  const boxes = [box(2, 0, 0, 800, 600), box(5, 10, 10, 100, 50)];
  assert.equal(hitTest(boxes, ix, 20, 20)!.id, 5);
  assert.equal(hitTest(boxes, ix, 500, 500)!.id, 2);
  assert.equal(hitTest(boxes, ix, 900, 900), null);
});

test("the box model from a border box", () => {
  const m = boxModel(box(5, 10, 10, 100, 50));
  assert.deepEqual(m.margin, { x: 6, y: 9, w: 106, h: 54 });
  assert.deepEqual(m.content, { x: 13, y: 13, w: 94, h: 44 });
});

test("labels and the timeline", () => {
  assert.equal(label(dom.children![0].children![0]), "<div#a.x.y>");
  assert.equal(label({ id: 1, type: "text", text: "hi" }), '"hi"');
  assert.deepEqual(timeline([["parse", 2], ["layout", 3]]), [["parse", 2, 0], ["layout", 3, 2]]);
});
