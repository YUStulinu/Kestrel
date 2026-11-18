// The DOM for scripts, built on __host(op, ...args) (implemented in Rust,
// engine/src/js.rs). Nodes are numbers on the host side; wrappers are cached
// so identity holds. Op numbers must match js.rs.
(function () {
  "use strict";
  const H = globalThis.__host;
  delete globalThis.__host;
  const OP = {
    DOC_ELEMENT: 0, BODY: 1, HEAD: 2, BY_ID: 3, TYPE: 4, NAME: 5, PARENT: 6, CHILDREN: 7, GET_ATTR: 8, SET_ATTR: 9,
    REMOVE_ATTR: 10, ATTR_NAMES: 11, TEXT: 12, SET_TEXT: 13, CREATE_ELEMENT: 14, CREATE_TEXT: 15, APPEND: 16,
    INSERT_BEFORE: 17, REMOVE_CHILD: 18, QUERY: 19, MATCHES: 20, INNER_HTML: 21, SET_INNER_HTML: 22, DATA: 23,
    SET_DATA: 24, LOG: 25, CREATE_COMMENT: 26, CLONE: 27, OUTER_HTML: 28, CREATE_FRAGMENT: 29, NEXT: 30, PREV: 31,
    TITLE: 32, SET_TITLE: 33, DOCUMENT: 34, ADJACENT_HTML: 35, NOW: 36, LAYOUT_RECT: 37,
  };
  const cache = new Map();
  const listeners = new Map();
  function wrap(id) {
    if (id === null || id === undefined || id < 0) return null;
    let w = cache.get(id);
    if (w) return w;
    const t = H(OP.TYPE, id);
    const proto = t === 1 ? (H(OP.NAME, id) === "TEMPLATE" ? HTMLTemplateElement.prototype : HTMLElement.prototype)
      : t === 3 ? Text.prototype : t === 8 ? Comment.prototype : t === 9 ? Document.prototype
      : t === 11 ? DocumentFragment.prototype : Node.prototype;
    w = Object.create(proto);
    Object.defineProperty(w, "__id", { value: id });
    cache.set(id, w);
    return w;
  }
  function ids(s) {
    return s === "" ? [] : s.split(",").map((x) => wrap(+x));
  }
  function id(n) {
    if (n === null || n === undefined) return -1;
    if (typeof n !== "object" || n.__id === undefined) throw new TypeError("parameter is not of type 'Node'");
    return n.__id;
  }
  function toNode(x) {
    return typeof x === "object" && x !== null && x.__id !== undefined ? x : document.createTextNode(String(x));
  }

  class EventTarget {
    addEventListener(type, fn) {
      if (typeof fn !== "function" && !(fn && typeof fn.handleEvent === "function")) return;
      const key = this === globalThis ? "window" : this.__id;
      let m = listeners.get(key);
      if (!m) listeners.set(key, (m = {}));
      (m[type] = m[type] || []).push(fn);
    }
    removeEventListener(type, fn) {
      const m = listeners.get(this === globalThis ? "window" : this.__id);
      if (m && m[type]) m[type] = m[type].filter((f) => f !== fn);
    }
    dispatchEvent(ev) {
      let node = this;
      if (!ev.target) ev.target = this;
      while (node) {
        const m = listeners.get(node === globalThis ? "window" : node.__id);
        ev.currentTarget = node;
        if (m && m[ev.type]) for (const f of m[ev.type].slice()) typeof f === "function" ? f.call(node, ev) : f.handleEvent(ev);
        if (!ev.bubbles || ev._stop) break;
        node = node === globalThis ? null : node.parentNode || (node.nodeType === 9 ? globalThis : null);
      }
      return !ev.defaultPrevented;
    }
  }
  class Event {
    constructor(type, init) {
      this.type = type;
      this.bubbles = !!(init && init.bubbles);
      this.cancelable = !!(init && init.cancelable);
      this.defaultPrevented = false;
      this.target = null;
      this.currentTarget = null;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = true; }
  }
  class CustomEvent extends Event {
    constructor(type, init) {
      super(type, init);
      this.detail = init && init.detail !== undefined ? init.detail : null;
    }
  }

  class Node extends EventTarget {
    get nodeType() { return H(OP.TYPE, this.__id); }
    get nodeName() { return H(OP.NAME, this.__id); }
    get parentNode() { return wrap(H(OP.PARENT, this.__id)); }
    get parentElement() {
      const p = this.parentNode;
      return p && p.nodeType === 1 ? p : null;
    }
    get childNodes() { return ids(H(OP.CHILDREN, this.__id)); }
    get firstChild() { return this.childNodes[0] || null; }
    get lastChild() { const c = this.childNodes; return c[c.length - 1] || null; }
    get nextSibling() { return wrap(H(OP.NEXT, this.__id)); }
    get previousSibling() { return wrap(H(OP.PREV, this.__id)); }
    get ownerDocument() { return this.nodeType === 9 ? null : document; }
    get isConnected() {
      let n = this;
      while (n) { if (n.nodeType === 9) return true; n = n.parentNode; }
      return false;
    }
    hasChildNodes() { return this.childNodes.length > 0; }
    get textContent() { return this.nodeType === 9 ? null : H(OP.TEXT, this.__id); }
    set textContent(v) { H(OP.SET_TEXT, this.__id, v === null ? "" : String(v)); }
    get nodeValue() { return this.nodeType === 3 || this.nodeType === 8 ? H(OP.DATA, this.__id) : null; }
    set nodeValue(v) { if (this.nodeType === 3 || this.nodeType === 8) H(OP.SET_DATA, this.__id, String(v)); }
    appendChild(c) {
      if (c.nodeType === 11) { for (const k of c.childNodes) this.appendChild(k); return c; }
      H(OP.APPEND, this.__id, id(c));
      return c;
    }
    insertBefore(c, ref) {
      if (c.nodeType === 11) { for (const k of c.childNodes) this.insertBefore(k, ref); return c; }
      H(OP.INSERT_BEFORE, this.__id, id(c), ref ? id(ref) : -1);
      return c;
    }
    removeChild(c) {
      if (c.parentNode !== this) throw new DOMException("The node to be removed is not a child of this node.", "NotFoundError");
      H(OP.REMOVE_CHILD, this.__id, id(c));
      return c;
    }
    replaceChild(n, old) {
      this.insertBefore(n, old);
      return this.removeChild(old);
    }
    remove() { const p = this.parentNode; if (p) p.removeChild(this); }
    contains(n) {
      while (n) { if (n === this) return true; n = n.parentNode; }
      return false;
    }
    cloneNode(deep) { return wrap(H(OP.CLONE, this.__id, !!deep)); }
    append(...nodes) { for (const n of nodes) this.appendChild(toNode(n)); }
    prepend(...nodes) { const f = this.firstChild; for (const n of nodes) this.insertBefore(toNode(n), f); }
    before(...nodes) { const p = this.parentNode; if (p) for (const n of nodes) p.insertBefore(toNode(n), this); }
    after(...nodes) {
      const p = this.parentNode;
      if (!p) return;
      let ref = this.nextSibling;
      for (const n of nodes) p.insertBefore(toNode(n), ref);
    }
    replaceWith(...nodes) { this.before(...nodes); this.remove(); }
    get children() { return this.childNodes.filter((n) => n.nodeType === 1); }
    get firstElementChild() { return this.children[0] || null; }
    get lastElementChild() { const c = this.children; return c[c.length - 1] || null; }
    get childElementCount() { return this.children.length; }
    querySelector(s) { const r = H(OP.QUERY, this.__id, String(s), 1); if (r === null) throw new DOMException("not a valid selector", "SyntaxError"); return r === "" ? null : wrap(+r); }
    querySelectorAll(s) { const r = H(OP.QUERY, this.__id, String(s), 0); if (r === null) throw new DOMException("not a valid selector", "SyntaxError"); return ids(r); }
    getElementsByTagName(t) { return this.querySelectorAll(t === "*" ? "*" : String(t)); }
    getElementsByClassName(c) { return this.querySelectorAll(String(c).trim().split(/\s+/).filter(Boolean).map((x) => "." + x).join("")); }
  }
  Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;

  function camelToKebab(p) { return p.replace(/[A-Z]/g, (c) => "-" + c.toLowerCase()); }
  function parseStyle(s) {
    const m = new Map();
    for (const decl of (s || "").split(";")) {
      const i = decl.indexOf(":");
      if (i > 0) m.set(decl.slice(0, i).trim().toLowerCase(), decl.slice(i + 1).trim());
    }
    return m;
  }
  function writeStyle(el, m) {
    el.setAttribute("style", [...m].map(([k, v]) => k + ": " + v).join("; "));
  }
  class CSSStyleDeclaration {
    constructor(el) { Object.defineProperty(this, "_el", { value: el }); }
    getPropertyValue(p) { return parseStyle(this._el.getAttribute("style")).get(p.toLowerCase()) || ""; }
    setProperty(p, v) {
      const m = parseStyle(this._el.getAttribute("style"));
      if (v === null || v === "") m.delete(p.toLowerCase());
      else m.set(p.toLowerCase(), String(v));
      writeStyle(this._el, m);
    }
    removeProperty(p) { const old = this.getPropertyValue(p); this.setProperty(p, ""); return old; }
    get cssText() { return this._el.getAttribute("style") || ""; }
    set cssText(v) { this._el.setAttribute("style", String(v)); }
  }
  for (const p of ["color", "background", "backgroundColor", "width", "height", "display", "margin", "marginTop",
    "marginRight", "marginBottom", "marginLeft", "padding", "paddingTop", "paddingRight", "paddingBottom", "paddingLeft",
    "border", "borderColor", "borderWidth", "borderStyle", "position", "top", "left", "right", "bottom", "float",
    "fontSize", "fontWeight", "fontFamily", "fontStyle", "lineHeight", "textAlign", "visibility", "opacity", "zIndex",
    "flex", "flexDirection", "flexWrap", "justifyContent", "alignItems", "order", "overflow", "minWidth", "maxWidth",
    "minHeight", "maxHeight", "textDecoration", "verticalAlign", "whiteSpace", "boxSizing", "clear", "content",
    "transform", "transition", "cursor", "gap"]) {
    const css = camelToKebab(p);
    Object.defineProperty(CSSStyleDeclaration.prototype, p, {
      get() { return this.getPropertyValue(css); },
      set(v) { this.setProperty(css, v); },
    });
  }
  class DOMTokenList {
    constructor(el) { Object.defineProperty(this, "_el", { value: el }); }
    _list() { return (this._el.getAttribute("class") || "").split(/\s+/).filter(Boolean); }
    _set(l) { this._el.setAttribute("class", l.join(" ")); }
    get length() { return this._list().length; }
    item(i) { return this._list()[i] || null; }
    contains(c) { return this._list().includes(c); }
    add(...cs) { const l = this._list(); for (const c of cs) if (!l.includes(c)) l.push(c); this._set(l); }
    remove(...cs) { this._set(this._list().filter((c) => !cs.includes(c))); }
    toggle(c, force) {
      const has = this.contains(c);
      const want = force === undefined ? !has : !!force;
      if (want && !has) this.add(c);
      if (!want && has) this.remove(c);
      return want;
    }
    replace(a, b) { if (!this.contains(a)) return false; this._set(this._list().map((c) => (c === a ? b : c))); return true; }
    toString() { return this._el.getAttribute("class") || ""; }
    [Symbol.iterator]() { return this._list()[Symbol.iterator](); }
  }
  class Element extends Node {
    get tagName() { return this.nodeName; }
    get localName() { return this.nodeName.toLowerCase(); }
    get id() { return this.getAttribute("id") || ""; }
    set id(v) { this.setAttribute("id", v); }
    get className() { return this.getAttribute("class") || ""; }
    set className(v) { this.setAttribute("class", v); }
    get classList() { return new DOMTokenList(this); }
    get style() { return new CSSStyleDeclaration(this); }
    getAttribute(n) { return H(OP.GET_ATTR, this.__id, String(n).toLowerCase()); }
    setAttribute(n, v) { H(OP.SET_ATTR, this.__id, String(n).toLowerCase(), String(v)); }
    removeAttribute(n) { H(OP.REMOVE_ATTR, this.__id, String(n).toLowerCase()); }
    hasAttribute(n) { return this.getAttribute(n) !== null; }
    toggleAttribute(n, f) {
      const has = this.hasAttribute(n);
      const want = f === undefined ? !has : !!f;
      if (want && !has) this.setAttribute(n, "");
      if (!want && has) this.removeAttribute(n);
      return want;
    }
    getAttributeNames() { const s = H(OP.ATTR_NAMES, this.__id); return s === "" ? [] : s.split("\u0001"); }
    get attributes() { return this.getAttributeNames().map((name) => ({ name, value: this.getAttribute(name) })); }
    get dataset() {
      const el = this;
      const o = {};
      for (const n of this.getAttributeNames()) {
        if (!n.startsWith("data-")) continue;
        const key = n.slice(5).replace(/-([a-z])/g, (_, c) => c.toUpperCase());
        Object.defineProperty(o, key, { enumerable: true, get: () => el.getAttribute(n), set: (v) => el.setAttribute(n, v) });
      }
      return o;
    }
    get nextElementSibling() { let n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; }
    get previousElementSibling() { let n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; }
    matches(s) { const r = H(OP.MATCHES, this.__id, String(s)); if (r === null) throw new DOMException("not a valid selector", "SyntaxError"); return r; }
    closest(s) { let n = this; while (n && n.nodeType === 1) { if (n.matches(s)) return n; n = n.parentNode; } return null; }
    get innerHTML() { return H(OP.INNER_HTML, this.__id); }
    set innerHTML(v) { H(OP.SET_INNER_HTML, this.__id, String(v)); }
    get outerHTML() { return H(OP.OUTER_HTML, this.__id); }
    insertAdjacentHTML(pos, html) { H(OP.ADJACENT_HTML, this.__id, String(pos).toLowerCase(), String(html)); }
    insertAdjacentElement(pos, el) {
      switch (String(pos).toLowerCase()) {
        case "beforebegin": this.before(el); break;
        case "afterbegin": this.prepend(el); break;
        case "beforeend": this.append(el); break;
        case "afterend": this.after(el); break;
      }
      return el;
    }
    insertAdjacentText(pos, t) { this.insertAdjacentElement(pos, document.createTextNode(t)); }
    getBoundingClientRect() {
      const r = H(OP.LAYOUT_RECT, this.__id).split(",").map(Number);
      return { x: r[0], y: r[1], left: r[0], top: r[1], width: r[2], height: r[3], right: r[0] + r[2], bottom: r[1] + r[3] };
    }
    get offsetWidth() { return this.getBoundingClientRect().width; }
    get offsetHeight() { return this.getBoundingClientRect().height; }
    get clientWidth() { return this.getBoundingClientRect().width; }
    get clientHeight() { return this.getBoundingClientRect().height; }
    click() { this.dispatchEvent(new Event("click", { bubbles: true })); }
    focus() {}
    blur() {}
  }
  class HTMLElement extends Element {
    get hidden() { return this.hasAttribute("hidden"); }
    set hidden(v) { this.toggleAttribute("hidden", !!v); }
    get title() { return this.getAttribute("title") || ""; }
    set title(v) { this.setAttribute("title", v); }
    get value() { return this.getAttribute("value") || ""; }
    set value(v) { this.setAttribute("value", v); }
    get href() { return this.getAttribute("href") || ""; }
    set href(v) { this.setAttribute("href", v); }
    get src() { return this.getAttribute("src") || ""; }
    set src(v) { this.setAttribute("src", v); }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
  }
  class HTMLTemplateElement extends HTMLElement {}
  class CharacterData extends Node {
    get data() { return H(OP.DATA, this.__id); }
    set data(v) { H(OP.SET_DATA, this.__id, String(v)); }
    get length() { return this.data.length; }
    appendData(s) { this.data += s; }
  }
  class Text extends CharacterData {
    get wholeText() { return this.data; }
  }
  class Comment extends CharacterData {}
  class DocumentFragment extends Node {}
  class Document extends Node {
    get documentElement() { return wrap(H(OP.DOC_ELEMENT)); }
    get body() { return wrap(H(OP.BODY)); }
    get head() { return wrap(H(OP.HEAD)); }
    get title() { return H(OP.TITLE); }
    set title(v) { H(OP.SET_TITLE, String(v)); }
    get readyState() { return "complete"; }
    get compatMode() { return "CSS1Compat"; }
    get defaultView() { return globalThis; }
    getElementById(i) { return wrap(H(OP.BY_ID, String(i))); }
    createElement(n) { return wrap(H(OP.CREATE_ELEMENT, String(n).toLowerCase())); }
    createElementNS(ns, n) { return this.createElement(n); }
    createTextNode(t) { return wrap(H(OP.CREATE_TEXT, String(t))); }
    createComment(t) { return wrap(H(OP.CREATE_COMMENT, String(t))); }
    createDocumentFragment() { return wrap(H(OP.CREATE_FRAGMENT)); }
    createEvent() { return new Event(""); }
    write() { throw new DOMException("document.write is not supported", "NotSupportedError"); }
    writeln() { this.write(); }
  }
  class DOMException extends Error {
    constructor(message, name) {
      super(message);
      this.name = name || "Error";
    }
  }

  const timers = [];
  let timerSeq = 0;
  const g = globalThis;
  Object.assign(g, {
    window: g, self: g, Node, Element, HTMLElement, Text, Comment, Document, DocumentFragment, CharacterData,
    EventTarget, Event, CustomEvent, DOMException, CSSStyleDeclaration, DOMTokenList, HTMLTemplateElement,
    document: wrap(H(OP.DOCUMENT)),
    console: {
      log: (...a) => H(OP.LOG, "log", a.map(String).join(" ")),
      info: (...a) => H(OP.LOG, "info", a.map(String).join(" ")),
      warn: (...a) => H(OP.LOG, "warn", a.map(String).join(" ")),
      error: (...a) => H(OP.LOG, "error", a.map(String).join(" ")),
      debug: () => {},
    },
    setTimeout: (fn, ms, ...args) => { const id = ++timerSeq; timers.push({ id, fn, at: +ms || 0, args, seq: id }); return id; },
    setInterval: (fn, ms, ...args) => { const id = ++timerSeq; timers.push({ id, fn, at: +ms || 0, args, seq: id }); return id; },
    clearTimeout: (id) => { const i = timers.findIndex((t) => t.id === id); if (i >= 0) timers.splice(i, 1); },
    clearInterval: (id) => g.clearTimeout(id),
    requestAnimationFrame: (fn) => g.setTimeout(() => fn(H(OP.NOW)), 16),
    cancelAnimationFrame: (id) => g.clearTimeout(id),
    queueMicrotask: (fn) => Promise.resolve().then(fn),
    getComputedStyle: (el) => el.style,
    performance: { now: () => H(OP.NOW) },
    navigator: { userAgent: "Kestrel/0.1", language: "en-US" },
    location: { href: "about:blank", protocol: "about:", host: "", pathname: "blank", search: "", hash: "" },
    innerWidth: 800, innerHeight: 600, devicePixelRatio: 1,
    // the browser calls this after the scripts: fires load events, then runs timers by due time
    __kestrelAfterScripts() {
      document.dispatchEvent(new Event("DOMContentLoaded", { bubbles: true }));
      g.dispatchEvent(new Event("load"));
      for (let guard = 0; timers.length && guard < 10000; guard++) {
        timers.sort((a, b) => a.at - b.at || a.seq - b.seq);
        const t = timers.shift();
        try { t.fn(...t.args); } catch (e) { g.console.error(String(e)); }
      }
    },
  });
  g.addEventListener = EventTarget.prototype.addEventListener;
  g.removeEventListener = EventTarget.prototype.removeEventListener;
  g.dispatchEvent = EventTarget.prototype.dispatchEvent;
})();
