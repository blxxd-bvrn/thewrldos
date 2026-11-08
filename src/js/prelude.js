// THE WRLD OS - browser environment for XplorerLite.
// Runs once per page before the page's own scripts. Builds window, document,
// elements, events, timers, storage and fetch on top of the native __host
// primitives, which are captured here and removed from the global object.
(function (H) {
"use strict";
const G = globalThis;
delete G.__host;

/* ---------------- node wrappers ---------------- */
const cache = new Map();
function wrap(n) {
    if (!n) return null;
    let w = cache.get(n);
    if (w) return w;
    const t = H.type(n);
    w = t === 3 ? new Text(n) : new Element(n);
    cache.set(n, w);
    return w;
}
const unwrap = (w) => (w && w._n) || 0;
const list = (arr) => { const a = arr.map(wrap); a.item = (i) => a[i] || null; return a; };
const str = (v) => (v === null || v === undefined) ? "" : String(v);

/* ---------------- events ---------------- */
class Event {
    constructor(type, init) {
        init = init || {};
        this.type = String(type); this.bubbles = !!init.bubbles; this.cancelable = init.cancelable !== false;
        this.defaultPrevented = false; this.target = null; this.currentTarget = null;
        this.timeStamp = H.now(); this.isTrusted = false; this._stop = false; this._stopNow = false;
        this.detail = init.detail;
    }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopNow = true; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get srcElement() { return this.target; }
    composedPath() { return this._path || []; }
}
class CustomEvent extends Event {}
class UIEvent extends Event {}
class MouseEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.button = i.button || 0; this.clientX = i.clientX || 0; this.clientY = i.clientY || 0; this.pageX = this.clientX; this.pageY = this.clientY; this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = false; this.which = this.button + 1; } }
class KeyboardEvent extends UIEvent { constructor(t, i) { super(t, i); i = i || {}; this.key = i.key || ""; this.code = i.code || ""; this.keyCode = i.keyCode || 0; this.which = this.keyCode; this.charCode = i.charCode || 0; this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = false; this.repeat = false; } }
class FocusEvent extends UIEvent {}
class InputEvent extends UIEvent {}
class SubmitEvent extends Event {}

class EventTarget {
    addEventListener(type, fn, opts) {
        if (!fn) return;
        const l = this._l || (this._l = {});
        const a = l[type] || (l[type] = []);
        if (!a.some((e) => e.fn === fn)) a.push({ fn, once: !!(opts && typeof opts === "object" && opts.once) });
    }
    removeEventListener(type, fn) { const a = this._l && this._l[type]; if (a) { const i = a.findIndex((e) => e.fn === fn); if (i >= 0) a.splice(i, 1); } }
    dispatchEvent(ev) {
        ev.target = this;
        const path = [];
        for (let t = this; t; t = t._parentTarget()) path.push(t);
        ev._path = path;
        for (const t of path) {
            fireOn(t, ev);
            if (ev._stop || !ev.bubbles) break;
        }
        return !ev.defaultPrevented;
    }
    _parentTarget() { return null; }
}
function callHandler(t, fn, ev) {
    try {
        const r = typeof fn === "function" ? fn.call(t, ev) : (fn && typeof fn.handleEvent === "function" ? fn.handleEvent(ev) : undefined);
        if (r === false) ev.preventDefault();
    } catch (e) { reportError(e); }
}
function fireOn(t, ev) {
    ev.currentTarget = t;
    const a = t._l && t._l[ev.type];
    if (a) for (const e of a.slice()) {
        if (e.once) t.removeEventListener(ev.type, e.fn);
        callHandler(t, e.fn, ev);
        if (ev._stopNow) return;
    }
    const prop = t["on" + ev.type];
    if (typeof prop === "function") callHandler(t, prop, ev);
    else if (t instanceof Element) {
        const code = t.getAttribute("on" + ev.type);
        if (code) {
            let f = t._ah && t._ah[ev.type];
            if (!f || f.code !== code) {
                try { f = new Function("event", code); f.code = code; } catch (e) { reportError(e); f = null; }
                (t._ah || (t._ah = {}))[ev.type] = f;
            }
            if (f) callHandler(t, f, ev);
        }
    }
}

/* ---------------- nodes ---------------- */
class Node extends EventTarget {
    constructor(n) { super(); this._n = n; }
    get nodeType() { return H.type(this._n); }
    get nodeName() { const t = H.type(this._n); return t === 3 ? "#text" : t === 9 ? "#document" : H.tag(this._n).toUpperCase(); }
    get parentNode() { if (this._n === H.root()) return document; return wrap(H.parent(this._n)); }
    get parentElement() { const p = H.parent(this._n); return p ? wrap(p) : null; }
    get firstChild() { return wrap(H.first(this._n)); }
    get lastChild() { return wrap(H.last(this._n)); }
    get nextSibling() { return wrap(H.next(this._n)); }
    get previousSibling() { return wrap(H.prev(this._n)); }
    get childNodes() { const a = []; for (let c = H.first(this._n); c; c = H.next(c)) a.push(c); return list(a); }
    get ownerDocument() { return document; }
    get isConnected() { let n = this._n; while (n) { if (n === H.root()) return true; n = H.parent(n); } return false; }
    hasChildNodes() { return !!H.first(this._n); }
    contains(o) { if (!o) return false; for (let n = unwrap(o); n; n = H.parent(n)) if (n === this._n) return true; return false; }
    appendChild(c) { return this.insertBefore(c, null); }
    insertBefore(c, ref) {
        if (c instanceof DocumentFragment) { for (const k of c._kids.splice(0)) this.insertBefore(k, ref); return c; }
        if (typeof c === "string") c = document.createTextNode(c);
        H.insert(this._n, unwrap(c), unwrap(ref));
        return c;
    }
    removeChild(c) { H.remove(unwrap(c)); return c; }
    replaceChild(nc, old) { this.insertBefore(nc, old); this.removeChild(old); return old; }
    remove() { if (H.parent(this._n)) H.remove(this._n); }
    cloneNode(deep) { return wrap(H.clone(this._n, deep ? 1 : 0)); }
    get textContent() { return H.text(this._n); }
    set textContent(v) { H.setText(this._n, str(v)); }
    get nodeValue() { return H.type(this._n) === 3 ? H.text(this._n) : null; }
    set nodeValue(v) { if (H.type(this._n) === 3) H.setText(this._n, str(v)); }
    _parentTarget() { const p = H.parent(this._n); return p ? wrap(p) : (this._n === H.root() ? document : null); }
    // ChildNode / ParentNode helpers
    append(...ns) { for (const n of ns) this.appendChild(typeof n === "string" ? document.createTextNode(n) : n); }
    prepend(...ns) { const f = this.firstChild; for (const n of ns) this.insertBefore(typeof n === "string" ? document.createTextNode(n) : n, f); }
    before(...ns) { const p = this.parentNode; if (p) for (const n of ns) p.insertBefore(typeof n === "string" ? document.createTextNode(n) : n, this); }
    after(...ns) { const p = this.parentNode, nx = this.nextSibling; if (p) for (const n of ns) p.insertBefore(typeof n === "string" ? document.createTextNode(n) : n, nx); }
    replaceWith(...ns) { this.before(...ns); this.remove(); }
    replaceChildren(...ns) { this.textContent = ""; this.append(...ns); }
}
Node.ELEMENT_NODE = 1; Node.TEXT_NODE = 3; Node.DOCUMENT_NODE = 9; Node.DOCUMENT_FRAGMENT_NODE = 11;
class Text extends Node {
    get data() { return H.text(this._n); }
    set data(v) { H.setText(this._n, str(v)); }
    get length() { return this.data.length; }
    get wholeText() { return this.data; }
}

const camelToCss = (p) => p.replace(/[A-Z]/g, (m) => "-" + m.toLowerCase());
function styleProxy(el) {
    const target = {
        setProperty: (p, v) => H.setStyle(el._n, p, str(v)),
        getPropertyValue: (p) => H.getStyle(el._n, p),
        removeProperty: (p) => { const v = H.getStyle(el._n, p); H.setStyle(el._n, p, ""); return v; },
    };
    return new Proxy(target, {
        get(t, p) {
            if (p in t) return t[p];
            if (p === "cssText") return el.getAttribute("style") || "";
            return typeof p === "string" ? H.getStyle(el._n, camelToCss(p)) : undefined;
        },
        set(t, p, v) {
            if (p === "cssText") { el.setAttribute("style", str(v)); return true; }
            if (typeof p === "string") H.setStyle(el._n, camelToCss(p), str(v));
            return true;
        },
    });
}
class DOMTokenList {
    constructor(el) { this._el = el; }
    _get() { return (this._el.getAttribute("class") || "").split(/\s+/).filter(Boolean); }
    _set(a) { this._el.setAttribute("class", a.join(" ")); }
    get length() { return this._get().length; }
    get value() { return this._el.getAttribute("class") || ""; }
    item(i) { return this._get()[i] || null; }
    contains(c) { return this._get().includes(c); }
    add(...cs) { const a = this._get(); for (const c of cs) if (!a.includes(c)) a.push(c); this._set(a); }
    remove(...cs) { this._set(this._get().filter((c) => !cs.includes(c))); }
    toggle(c, force) { const has = this.contains(c), want = force === undefined ? !has : !!force; if (want && !has) this.add(c); else if (!want && has) this.remove(c); return want; }
    replace(a, b) { const l = this._get(), i = l.indexOf(a); if (i < 0) return false; l[i] = b; this._set(l); return true; }
    forEach(f, t) { this._get().forEach(f, t); }
    toString() { return this.value; }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
}
const boolAttr = (name) => ({ get() { return this.hasAttribute(name); }, set(v) { if (v) this.setAttribute(name, ""); else this.removeAttribute(name); } });
const strAttr = (name) => ({ get() { return this.getAttribute(name) || ""; }, set(v) { this.setAttribute(name, str(v)); } });
const urlAttr = (name) => ({ get() { const v = this.getAttribute(name); return v === null ? "" : H.resolve(v); }, set(v) { this.setAttribute(name, str(v)); } });

class Element extends Node {
    get tagName() { return H.tag(this._n).toUpperCase(); }
    get localName() { return H.tag(this._n); }
    getAttribute(n) { return H.getAttr(this._n, String(n).toLowerCase()); }
    setAttribute(n, v) { H.setAttr(this._n, String(n).toLowerCase(), str(v)); }
    removeAttribute(n) { H.removeAttr(this._n, String(n).toLowerCase()); }
    hasAttribute(n) { return H.getAttr(this._n, String(n).toLowerCase()) !== null; }
    toggleAttribute(n, f) { const h = this.hasAttribute(n), w = f === undefined ? !h : !!f; if (w) this.setAttribute(n, ""); else this.removeAttribute(n); return w; }
    getAttributeNames() { return H.attrNames(this._n); }
    get attributes() { const a = H.attrNames(this._n).map((name) => ({ name, value: this.getAttribute(name) })); a.item = (i) => a[i] || null; a.getNamedItem = (n) => a.find((x) => x.name === n) || null; return a; }
    get id() { return this.getAttribute("id") || ""; }
    set id(v) { this.setAttribute("id", v); }
    get className() { return this.getAttribute("class") || ""; }
    set className(v) { this.setAttribute("class", v); }
    get classList() { return this._cl || (this._cl = new DOMTokenList(this)); }
    get style() { return this._st || (this._st = styleProxy(this)); }
    get dataset() {
        const el = this;
        return new Proxy({}, {
            get: (t, p) => typeof p === "string" ? (el.getAttribute("data-" + camelToCss(p)) ?? undefined) : undefined,
            set: (t, p, v) => { el.setAttribute("data-" + camelToCss(p), v); return true; },
            has: (t, p) => el.hasAttribute("data-" + camelToCss(p)),
            deleteProperty: (t, p) => { el.removeAttribute("data-" + camelToCss(p)); return true; },
        });
    }
    get children() { const a = []; for (let c = H.first(this._n); c; c = H.next(c)) if (H.type(c) === 1) a.push(c); return list(a); }
    get childElementCount() { return this.children.length; }
    get firstElementChild() { for (let c = H.first(this._n); c; c = H.next(c)) if (H.type(c) === 1) return wrap(c); return null; }
    get lastElementChild() { for (let c = H.last(this._n); c; c = H.prev(c)) if (H.type(c) === 1) return wrap(c); return null; }
    get nextElementSibling() { for (let c = H.next(this._n); c; c = H.next(c)) if (H.type(c) === 1) return wrap(c); return null; }
    get previousElementSibling() { for (let c = H.prev(this._n); c; c = H.prev(c)) if (H.type(c) === 1) return wrap(c); return null; }
    get innerHTML() { return H.html(this._n, 0); }
    set innerHTML(v) { H.setHtml(this._n, str(v)); }
    get outerHTML() { return H.html(this._n, 1); }
    set outerHTML(v) { H.insertHtml(this._n, "beforebegin", str(v)); this.remove(); }
    get innerText() { return H.text(this._n); }
    set innerText(v) { H.setText(this._n, str(v)); }
    get outerText() { return this.innerText; }
    insertAdjacentHTML(where, html) { H.insertHtml(this._n, String(where).toLowerCase(), str(html)); }
    insertAdjacentText(where, text) { const t = document.createTextNode(text); this.insertAdjacentElement(where, t); }
    insertAdjacentElement(where, el) {
        switch (String(where).toLowerCase()) {
        case "beforebegin": this.before(el); break;
        case "afterbegin": this.prepend(el); break;
        case "beforeend": this.append(el); break;
        case "afterend": this.after(el); break;
        }
        return el;
    }
    querySelector(s) { const r = H.query(this._n, String(s), 1); return r.length ? wrap(r[0]) : null; }
    querySelectorAll(s) { return list(H.query(this._n, String(s), 0)); }
    getElementsByTagName(t) { return this.querySelectorAll(t === "*" ? "*" : String(t)); }
    getElementsByClassName(c) { return this.querySelectorAll(String(c).trim().split(/\s+/).map((x) => "." + x).join("")); }
    matches(s) { return !!H.matches(this._n, String(s)); }
    webkitMatchesSelector(s) { return this.matches(s); }
    closest(s) { for (let n = this._n; n && H.type(n) === 1 && n !== H.root(); n = H.parent(n)) if (H.matches(n, String(s))) return wrap(n); return null; }
    getBoundingClientRect() {
        const r = H.rect(this._n) || [0, 0, 0, 0], sy = H.viewport(2);
        const x = r[0], y = r[1] - sy;
        return { x, y, left: x, top: y, width: r[2], height: r[3], right: x + r[2], bottom: y + r[3], toJSON() { return this; } };
    }
    getClientRects() { return [this.getBoundingClientRect()]; }
    get offsetWidth() { return (H.rect(this._n) || [0, 0, 0])[2]; }
    get offsetHeight() { return (H.rect(this._n) || [0, 0, 0, 0])[3]; }
    get offsetTop() { return (H.rect(this._n) || [0, 0])[1]; }
    get offsetLeft() { return (H.rect(this._n) || [0])[0]; }
    get clientWidth() { return this._n === H.root() ? H.viewport(0) : this.offsetWidth; }
    get clientHeight() { return this._n === H.root() ? H.viewport(1) : this.offsetHeight; }
    get scrollHeight() { return this._n === H.root() ? H.viewport(3) : this.offsetHeight; }
    get scrollWidth() { return this.clientWidth; }
    get offsetParent() { return document.body; }
    get scrollTop() { return this._n === H.root() ? H.viewport(2) : 0; }
    set scrollTop(v) { if (this._n === H.root()) H.scrollTo(0, +v || 0); }
    scrollIntoView() { const r = H.rect(this._n); if (r) H.scrollTo(0, r[1]); }
    focus() { H.focus(this._n); document._active = this; }
    blur() { if (document._active === this) document._active = null; }
    click() { const ev = new MouseEvent("click", { bubbles: true }); ev.target = this; if (this.dispatchEvent(ev)) H.defaultAction(this._n, "click"); }
    // form controls
    get value() { const t = H.tag(this._n); return (t === "input" || t === "textarea" || t === "select" || t === "button" || t === "option") ? H.getValue(this._n) : (this.getAttribute("value") || ""); }
    set value(v) { H.setValue(this._n, str(v)); }
    get defaultValue() { return this.getAttribute("value") || ""; }
    get checked() { return !!H.getChecked(this._n); }
    set checked(v) { H.setChecked(this._n, v ? 1 : 0); }
    get selectedIndex() { return H.getSel(this._n); }
    set selectedIndex(v) { H.setSel(this._n, v | 0); }
    get options() { return this.querySelectorAll("option"); }
    get type() { return (this.getAttribute("type") || (H.tag(this._n) === "button" ? "submit" : H.tag(this._n) === "select" ? "select-one" : "text")).toLowerCase(); }
    set type(v) { this.setAttribute("type", v); }
    get form() { return this.closest("form"); }
    get elements() { return this.querySelectorAll("input,select,textarea,button"); }
    get length() { return H.tag(this._n) === "select" ? this.options.length : this.elements.length; }
    submit() { H.submit(this._n); }
    requestSubmit() { const ev = new SubmitEvent("submit", { bubbles: true }); if (this.dispatchEvent(ev)) H.submit(this._n); }
    reset() {}
    select() {}
    setSelectionRange() {}
    checkValidity() { return true; }
    reportValidity() { return true; }
    setCustomValidity() {}
    get validity() { return { valid: true }; }
    get text() { return H.text(this._n); }
    set text(v) { H.setText(this._n, str(v)); }
    get complete() { return true; }
    get naturalWidth() { return this.offsetWidth; }
    get naturalHeight() { return this.offsetHeight; }
    get shadowRoot() { return null; }
    attachShadow() { return this; }
    animate() { return { finished: Promise.resolve(), cancel() {}, play() {}, pause() {} }; }
    toString() { return "[object HTML" + (this.tagName.charAt(0) + this.tagName.slice(1).toLowerCase()) + "Element]"; }
}
for (const [prop, attr] of [["title", "title"], ["name", "name"], ["alt", "alt"], ["placeholder", "placeholder"], ["rel", "rel"], ["target", "target"], ["lang", "lang"], ["htmlFor", "for"], ["method", "method"], ["role", "role"], ["width", "width"], ["height", "height"], ["label", "label"], ["content", "content"], ["charset", "charset"], ["dir", "dir"]])
    Object.defineProperty(Element.prototype, prop, strAttr(attr));
for (const a of ["href", "src", "action"]) Object.defineProperty(Element.prototype, a, urlAttr(a));
for (const a of ["disabled", "hidden", "readonly", "required", "multiple", "selected", "autofocus", "defer", "async", "open", "novalidate"])
    Object.defineProperty(Element.prototype, a === "readonly" ? "readOnly" : a === "novalidate" ? "noValidate" : a, boolAttr(a));
// link parts (a.hostname etc.)
for (const part of ["protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin"])
    Object.defineProperty(Element.prototype, part, { get() { return parseURL(this.href)[part]; } });

class DocumentFragment extends EventTarget {
    constructor() { super(); this._kids = []; }
    get nodeType() { return 11; }
    appendChild(c) { if (c instanceof DocumentFragment) this._kids.push(...c._kids.splice(0)); else { if (c.parentNode) c.remove(); this._kids.push(c); } return c; }
    append(...ns) { for (const n of ns) this.appendChild(typeof n === "string" ? document.createTextNode(n) : n); }
    get childNodes() { return this._kids; }
    get children() { return this._kids.filter((k) => k.nodeType === 1); }
    get firstChild() { return this._kids[0] || null; }
    querySelector(s) { for (const k of this._kids) { if (k.nodeType !== 1) continue; if (k.matches(s)) return k; const r = k.querySelector(s); if (r) return r; } return null; }
    querySelectorAll(s) { const out = []; for (const k of this._kids) if (k.nodeType === 1) { if (k.matches(s)) out.push(k); out.push(...k.querySelectorAll(s)); } return out; }
    get textContent() { return this._kids.map((k) => k.textContent).join(""); }
}

/* ---------------- URLs ---------------- */
function parseURL(u) {
    u = str(u);
    const m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)(?:\/\/([^/?#]*))?([^?#]*)(\?[^#]*)?(#.*)?$/.exec(u) || [];
    const protocol = m[1] || "", host = m[2] || "", hp = host.split(":");
    return { href: u, protocol, host, hostname: hp[0] || "", port: hp[1] || "", pathname: m[3] || (host ? "/" : ""), search: m[4] || "", hash: m[5] || "",
             origin: host ? protocol + "//" + host : "null" };
}
class URLSearchParams {
    constructor(init) {
        this._p = [];
        if (typeof init === "string") { for (const kv of init.replace(/^\?/, "").split("&")) if (kv) { const i = kv.indexOf("="); const d = (s) => decodeURIComponent(s.replace(/\+/g, " ")); this._p.push(i < 0 ? [d(kv), ""] : [d(kv.slice(0, i)), d(kv.slice(i + 1))]); } }
        else if (init && typeof init === "object") for (const k of Object.keys(init)) this._p.push([k, str(init[k])]);
    }
    get(k) { const e = this._p.find((x) => x[0] === k); return e ? e[1] : null; }
    getAll(k) { return this._p.filter((x) => x[0] === k).map((x) => x[1]); }
    has(k) { return this._p.some((x) => x[0] === k); }
    set(k, v) { this.delete(k); this._p.push([k, str(v)]); }
    append(k, v) { this._p.push([k, str(v)]); }
    delete(k) { this._p = this._p.filter((x) => x[0] !== k); }
    forEach(f) { for (const [k, v] of this._p) f(v, k, this); }
    keys() { return this._p.map((x) => x[0])[Symbol.iterator](); }
    values() { return this._p.map((x) => x[1])[Symbol.iterator](); }
    entries() { return this._p.map((x) => [x[0], x[1]])[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this._p.map(([k, v]) => encodeURIComponent(k) + "=" + encodeURIComponent(v)).join("&"); }
}
class URL {
    constructor(u, base) { const p = parseURL(base !== undefined ? H.resolveAgainst(str(base), str(u)) : H.resolveAgainst(str(u), str(u))); Object.assign(this, p); this.searchParams = new URLSearchParams(p.search); this.username = ""; this.password = ""; }
    toString() { return this.href; }
    toJSON() { return this.href; }
    static createObjectURL() { return "about:blank"; }
    static revokeObjectURL() {}
}

/* ---------------- console, dialogs ---------------- */
function fmt(args) {
    return args.map((a) => {
        if (typeof a === "string") return a;
        if (a instanceof Error) return a.name + ": " + a.message;
        if (a instanceof Node) return "<" + a.nodeName.toLowerCase() + ">";
        try { const s = JSON.stringify(a); return s === undefined ? String(a) : s; } catch (e) { return String(a); }
    }).join(" ");
}
function reportError(e) {
    const msg = e && e.stack ? (e.name + ": " + e.message + "\n" + e.stack) : String(e);
    H.console(3, msg);
    try { const ev = new Event("error"); ev.message = e && e.message; ev.error = e; fireOn(window, ev); } catch (x) {}
}
const console = {
    log: (...a) => H.console(0, fmt(a)), info: (...a) => H.console(1, fmt(a)), debug: (...a) => H.console(0, fmt(a)),
    warn: (...a) => H.console(2, fmt(a)), error: (...a) => H.console(3, fmt(a)), trace: (...a) => H.console(0, fmt(a)),
    dir: (...a) => H.console(0, fmt(a)), table: (...a) => H.console(0, fmt(a)), group() {}, groupCollapsed() {}, groupEnd() {},
    time() {}, timeEnd() {}, count() {}, assert: (c, ...a) => { if (!c) H.console(3, "Assertion failed: " + fmt(a)); },
};

/* ---------------- timers ---------------- */
let timerSeq = 1;
const timers = new Map();
function addTimer(fn, ms, args, repeat) {
    const id = timerSeq++;
    ms = Math.max(repeat ? 10 : 0, +ms || 0);
    timers.set(id, { fn, args, ms, repeat, due: H.now() + ms });
    return id;
}
G.__wrld_timers = function () {
    let n = 0, now = H.now();
    const due = [...timers.entries()].filter(([, t]) => t.due <= now).sort((a, b) => a[1].due - b[1].due);
    for (const [id, t] of due) {
        if (!timers.has(id)) continue;
        if (t.repeat) t.due = Math.max(t.due + t.ms, now + 1); else timers.delete(id);
        try { if (typeof t.fn === "function") t.fn(...t.args); else (0, eval)(String(t.fn)); } catch (e) { reportError(e); }
        if (++n > 64) break;
    }
    let next = -1; now = H.now();
    for (const t of timers.values()) { const d = Math.max(0, t.due - now); if (next < 0 || d < next) next = d; }
    return next;
};

/* ---------------- storage ---------------- */
class Storage {
    constructor(persist) { this._persist = persist; this._m = new Map(); }
    get length() { return this._persist ? H.storageKeys().length : this._m.size; }
    key(i) { return (this._persist ? H.storageKeys() : [...this._m.keys()])[i] ?? null; }
    getItem(k) { k = String(k); return this._persist ? H.sget(k) : (this._m.has(k) ? this._m.get(k) : null); }
    setItem(k, v) { k = String(k); v = String(v); if (this._persist) H.sset(k, v); else this._m.set(k, v); }
    removeItem(k) { k = String(k); if (this._persist) H.sset(k, null); else this._m.delete(k); }
    clear() { if (this._persist) H.sclear(); else this._m.clear(); }
}
const storageProxy = (s) => new Proxy(s, {
    get: (t, p) => (p in t || typeof p !== "string") ? (typeof t[p] === "function" ? t[p].bind(t) : t[p]) : t.getItem(p) ?? undefined,
    set: (t, p, v) => { if (p in t) t[p] = v; else t.setItem(p, v); return true; },
});

/* ---------------- network ---------------- */
class Headers {
    constructor(h) { this._h = {}; if (h) for (const k of Object.keys(h)) this._h[k.toLowerCase()] = String(h[k]); }
    get(k) { return this._h[String(k).toLowerCase()] ?? null; }
    set(k, v) { this._h[String(k).toLowerCase()] = String(v); }
    has(k) { return String(k).toLowerCase() in this._h; }
    append(k, v) { this.set(k, v); }
    forEach(f) { for (const k of Object.keys(this._h)) f(this._h[k], k); }
}
class Response {
    constructor(body, init) { init = init || {}; this._b = str(body); this.status = init.status ?? 200; this.ok = this.status >= 200 && this.status < 300; this.statusText = init.statusText || ""; this.headers = new Headers(init.headers); this.url = init.url || ""; this.redirected = false; this.type = "basic"; }
    text() { return Promise.resolve(this._b); }
    json() { return new Promise((res) => res(JSON.parse(this._b))); }
    blob() { return Promise.resolve({ size: this._b.length, type: this.headers.get("content-type") || "", text: () => Promise.resolve(this._b) }); }
    arrayBuffer() { const a = new Uint8Array(this._b.length); for (let i = 0; i < a.length; i++) a[i] = this._b.charCodeAt(i) & 255; return Promise.resolve(a.buffer); }
    clone() { return new Response(this._b, { status: this.status, headers: this.headers._h, url: this.url }); }
}
function doFetch(url, method, body) {
    const r = H.fetch(H.resolve(str(url)), method || "GET", body === undefined || body === null ? null : str(body));
    return r;   // { status, text, type } or null
}
function fetch(input, init) {
    init = init || {};
    const url = typeof input === "string" ? input : (input && (input.url || input.href)) || str(input);
    return new Promise((resolve, reject) => {
        setTimeout(() => {
            const r = doFetch(url, (init.method || "GET").toUpperCase(), init.body);
            if (!r) reject(new TypeError("Failed to fetch"));
            else resolve(new Response(r.text, { status: r.status, headers: { "content-type": r.type }, url: H.resolve(url) }));
        }, 0);
    });
}
class XMLHttpRequest extends EventTarget {
    constructor() { super(); this.readyState = 0; this.status = 0; this.responseText = ""; this.response = ""; this.responseType = ""; this._h = {}; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget(); }
    open(method, url, async) { this._m = String(method).toUpperCase(); this._u = String(url); this._async = async !== false; this.readyState = 1; }
    setRequestHeader(k, v) { this._h[k] = v; }
    getResponseHeader(k) { return String(k).toLowerCase() === "content-type" ? this._type || null : null; }
    getAllResponseHeaders() { return this._type ? "content-type: " + this._type + "\r\n" : ""; }
    overrideMimeType() {}
    abort() { this._aborted = true; }
    send(body) {
        const run = () => {
            if (this._aborted) return;
            const r = doFetch(this._u, this._m, body);
            this.readyState = 4;
            if (r) { this.status = r.status; this.responseText = r.text; this._type = r.type; this.response = this.responseType === "json" ? (() => { try { return JSON.parse(r.text); } catch (e) { return null; } })() : r.text; }
            const ev = (t) => { const e = new Event(t); e.target = this; fireOn(this, e); };
            ev("readystatechange");
            ev(r ? "load" : "error");
            ev("loadend");
        };
        if (this._async) setTimeout(run, 0); else run();
    }
}
XMLHttpRequest.UNSENT = 0; XMLHttpRequest.OPENED = 1; XMLHttpRequest.DONE = 4;

/* ---------------- document ---------------- */
class Document extends Node {
    get nodeType() { return 9; }
    get nodeName() { return "#document"; }
    get documentElement() { return wrap(H.root()); }
    get body() { return wrap(H.root()); }
    get head() { return wrap(H.root()); }
    get title() { return H.title(); }
    set title(v) { H.setTitle(str(v)); }
    get URL() { return H.url(); }
    get documentURI() { return H.url(); }
    get baseURI() { return H.url(); }
    get domain() { return parseURL(H.url()).hostname; }
    get referrer() { return H.referrer(); }
    get cookie() { return H.cookie(); }
    set cookie(v) { H.setCookie(str(v)); }
    get location() { return location; }
    set location(v) { location.href = v; }
    get readyState() { return readyState; }
    get compatMode() { return "CSS1Compat"; }
    get characterSet() { return "UTF-8"; }
    get contentType() { return "text/html"; }
    get visibilityState() { return "visible"; }
    get hidden() { return false; }
    get defaultView() { return window; }
    get activeElement() { return this._active || this.body; }
    get forms() { return this.querySelectorAll("form"); }
    get images() { return this.querySelectorAll("img"); }
    get links() { return this.querySelectorAll("a[href]"); }
    get scripts() { return this.querySelectorAll("script"); }
    get scrollingElement() { return this.body; }
    get parentNode() { return null; }
    get firstChild() { return this.body; }
    get childNodes() { return [this.body]; }
    get children() { return [this.body]; }
    get firstElementChild() { return this.body; }
    get textContent() { return null; }
    getElementById(id) { return wrap(H.byId(String(id))); }
    getElementsByName(n) { return this.querySelectorAll('[name="' + String(n) + '"]'); }
    querySelector(s) { const r = H.query(H.root(), String(s), 1); return r.length ? wrap(r[0]) : null; }
    querySelectorAll(s) { return list(H.query(H.root(), String(s), 0)); }
    getElementsByTagName(t) { return this.querySelectorAll(String(t)); }
    getElementsByClassName(c) { return this.body.getElementsByClassName(c); }
    createElement(t) { return wrap(H.create(String(t).toLowerCase())); }
    createElementNS(ns, t) { return this.createElement(t); }
    createTextNode(s) { return wrap(H.createText(str(s))); }
    createComment() { return new DocumentFragment(); }
    createDocumentFragment() { return new DocumentFragment(); }
    createEvent(k) { const e = new Event(""); e.initEvent = (t, b, c) => { e.type = t; e.bubbles = !!b; e.cancelable = !!c; }; e.initCustomEvent = (t, b, c, d) => { e.initEvent(t, b, c); e.detail = d; }; return e; }
    createRange() { return { selectNodeContents() {}, setStart() {}, setEnd() {}, collapse() {}, getBoundingClientRect: () => ({ top: 0, left: 0, width: 0, height: 0 }), createContextualFragment: (h) => { const d = document.createElement("div"); d.innerHTML = h; const f = new DocumentFragment(); for (const c of [...d.childNodes]) f.appendChild(c); return f; } }; }
    write(...s) { written += s.join(""); }
    writeln(...s) { written += s.join("") + "\n"; }
    open() { return this; }
    close() {}
    hasFocus() { return true; }
    execCommand() { return false; }
    elementFromPoint() { return null; }
    getSelection() { return { toString: () => "", rangeCount: 0, removeAllRanges() {}, addRange() {} }; }
    appendChild(c) { return this.body.appendChild(c); }
    _parentTarget() { return window; }
}
let written = "", readyState = "loading";
G.__wrld_take_written = function () { const w = written; written = ""; return w; };

/* ---------------- window ---------------- */
const window = G;
const document = new Document(H.root());
const location = {
    get href() { return H.url(); }, set href(v) { H.navigate(H.resolve(str(v)), 0); },
    assign(u) { H.navigate(H.resolve(str(u)), 0); }, replace(u) { H.navigate(H.resolve(str(u)), 1); }, reload() { H.reload(); },
    toString() { return H.url(); },
};
for (const part of ["protocol", "host", "hostname", "port", "pathname", "search", "origin"])
    Object.defineProperty(location, part, { get() { return parseURL(H.url())[part]; }, enumerable: true });
Object.defineProperty(location, "hash", { get() { return parseURL(H.url()).hash; }, set(v) { v = str(v); if (v && v[0] !== "#") v = "#" + v; H.navigate(H.url().replace(/#.*$/, "") + v, 1); }, enumerable: true });
const history = { get length() { return 1; }, state: null, back() { H.history(-1); }, forward() { H.history(1); }, go(d) { if (d) H.history(d | 0); else H.reload(); },
                  pushState(s) { this.state = s; }, replaceState(s) { this.state = s; }, scrollRestoration: "auto" };
const navigator = { userAgent: "Mozilla/5.0 (THE WRLD OS; i686) XplorerLite/1.1", appName: "Netscape", appVersion: "5.0 (THE WRLD OS)", appCodeName: "Mozilla",
                    product: "Gecko", vendor: "", platform: "WRLD i686", language: "en-US", languages: ["en-US", "en"], onLine: true, cookieEnabled: true,
                    hardwareConcurrency: 1, maxTouchPoints: 0, doNotTrack: "1", webdriver: false, javaEnabled: () => false,
                    sendBeacon: () => true, clipboard: { writeText: () => Promise.resolve(), readText: () => Promise.resolve("") },
                    serviceWorker: undefined, geolocation: undefined, mediaDevices: undefined };
const screen = { get width() { return H.viewport(4); }, get height() { return H.viewport(5); }, get availWidth() { return H.viewport(4); }, get availHeight() { return H.viewport(5); }, colorDepth: 32, pixelDepth: 32 };
const performance = { now: () => H.now() - startTime, timeOrigin: H.epoch(), mark() {}, measure() {}, getEntriesByType: () => [], getEntriesByName: () => [] };
const startTime = H.now();

const props = {
    window, self: window, top: window, parent: window, frames: window, opener: null, closed: false, name: "", length: 0, frameElement: null,
    document, location, history, navigator, screen, performance, console,
    localStorage: storageProxy(new Storage(true)), sessionStorage: storageProxy(new Storage(false)),
    Event, CustomEvent, UIEvent, MouseEvent, KeyboardEvent, FocusEvent, InputEvent, SubmitEvent, EventTarget,
    Node, Element, HTMLElement: Element, HTMLDivElement: Element, HTMLInputElement: Element, HTMLAnchorElement: Element, HTMLImageElement: Element,
    HTMLFormElement: Element, HTMLButtonElement: Element, HTMLSelectElement: Element, HTMLTextAreaElement: Element, HTMLScriptElement: Element,
    HTMLDocument: Document, Document, DocumentFragment, Text, SVGElement: Element, DOMTokenList,
    URL, URLSearchParams, Headers, Response, Request: class { constructor(u, i) { this.url = String(u); Object.assign(this, i || {}); } },
    XMLHttpRequest, fetch,
    setTimeout: (f, ms, ...a) => addTimer(f, ms, a, false), setInterval: (f, ms, ...a) => addTimer(f, ms, a, true),
    clearTimeout: (id) => { timers.delete(id); }, clearInterval: (id) => { timers.delete(id); },
    requestAnimationFrame: (f) => addTimer(() => f(H.now() - startTime), 16, [], false), cancelAnimationFrame: (id) => { timers.delete(id); },
    requestIdleCallback: (f) => addTimer(() => f({ didTimeout: false, timeRemaining: () => 10 }), 1, [], false), cancelIdleCallback: (id) => { timers.delete(id); },
    queueMicrotask: (f) => { Promise.resolve().then(f); },
    alert: (m) => { H.alert("alert", str(m)); }, confirm: (m) => { H.alert("confirm", str(m)); return true; }, prompt: (m, d) => { H.alert("prompt", str(m)); return d === undefined ? null : str(d); },
    print() {}, focus() {}, blur() {}, close() {}, stop() {}, open: (u) => { if (u) H.navigate(H.resolve(str(u)), 0); return null; },
    scrollTo: (x, y) => { if (typeof x === "object" && x) { y = x.top; } H.scrollTo(0, +y || 0); },
    scroll: (x, y) => window.scrollTo(x, y), scrollBy: (x, y) => { if (typeof x === "object" && x) y = x.top; H.scrollTo(0, H.viewport(2) + (+y || 0)); },
    getComputedStyle: (el) => el && el.style ? new Proxy({}, { get: (t, p) => p === "getPropertyValue" ? (q) => el.style.getPropertyValue(q) : (typeof p === "string" ? el.style[p] || "" : undefined) }) : {},
    matchMedia: (q) => ({ matches: /prefers-color-scheme:\s*light|min-width:\s*([0-9]+)px/.test(q) && (!/min-width/.test(q) || H.viewport(0) >= +(/min-width:\s*([0-9]+)/.exec(q) || [0, 0])[1]), media: q, addListener() {}, removeListener() {}, addEventListener() {}, removeEventListener() {} }),
    MutationObserver: class { observe() {} disconnect() {} takeRecords() { return []; } },
    IntersectionObserver: class { constructor(cb) { this._cb = cb; } observe(el) { setTimeout(() => this._cb([{ target: el, isIntersecting: true, intersectionRatio: 1 }], this), 0); } unobserve() {} disconnect() {} },
    ResizeObserver: class { observe() {} unobserve() {} disconnect() {} },
    Image: function (w, h) { const i = document.createElement("img"); if (w) i.setAttribute("width", w); if (h) i.setAttribute("height", h); return i; },
    Option: function (text, value, def, sel) { const o = document.createElement("option"); o.textContent = str(text); if (value !== undefined) o.setAttribute("value", value); if (sel) o.setAttribute("selected", ""); return o; },
    btoa: (s) => { const c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; s = String(s); let o = ""; for (let i = 0; i < s.length; i += 3) { const a = s.charCodeAt(i), b = s.charCodeAt(i + 1), d = s.charCodeAt(i + 2); o += c[a >> 2] + c[((a & 3) << 4) | (b >> 4 || 0)] + (isNaN(b) ? "=" : c[((b & 15) << 2) | (d >> 6 || 0)]) + (isNaN(d) ? "=" : c[d & 63]); } return o; },
    atob: (s) => { const c = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; s = String(s).replace(/[^A-Za-z0-9+/]/g, ""); let o = "", acc = 0, bits = 0; for (const ch of s) { acc = (acc << 6) | c.indexOf(ch); bits += 6; if (bits >= 8) { bits -= 8; o += String.fromCharCode((acc >> bits) & 255); } } return o; },
    get innerWidth() { return H.viewport(0); }, get innerHeight() { return H.viewport(1); },
    get outerWidth() { return H.viewport(0); }, get outerHeight() { return H.viewport(1); },
    get scrollX() { return 0; }, get scrollY() { return H.viewport(2); }, get pageXOffset() { return 0; }, get pageYOffset() { return H.viewport(2); },
    devicePixelRatio: 1, isSecureContext: true, origin: parseURL(H.url()).origin,
    get status() { return ""; }, set status(v) { H.status(str(v)); },
};
for (const k of Object.keys(props)) {
    const d = Object.getOwnPropertyDescriptor(props, k);
    Object.defineProperty(G, k, d.get || d.set ? { get: d.get, set: d.set, configurable: true } : { value: d.value, writable: true, configurable: true });
}
Object.setPrototypeOf(G, Object.create(EventTarget.prototype));   // window.addEventListener etc.

/* ---------------- entry points for the browser ---------------- */
G.__wrld_dispatch = function (n, type, key) {
    const target = wrap(n) || document;
    let ev;
    if (type === "click" || type === "mousedown" || type === "mouseup" || type === "dblclick" || type === "mouseover" || type === "mouseout")
        ev = new MouseEvent(type, { bubbles: true, cancelable: true });
    else if (type === "keydown" || type === "keyup" || type === "keypress") {
        const names = { 8: "Backspace", 9: "Tab", 13: "Enter", 27: "Escape", 37: "ArrowLeft", 38: "ArrowUp", 39: "ArrowRight", 40: "ArrowDown", 46: "Delete", 32: " " };
        ev = new KeyboardEvent(type, { bubbles: true, cancelable: true, key: names[key] || String.fromCharCode(key), keyCode: key >= 97 && key <= 122 ? key - 32 : key, charCode: key });
    } else ev = new Event(type, { bubbles: type !== "focus" && type !== "blur" && type !== "load", cancelable: true });
    ev.isTrusted = true;
    if (type === "focus") document._active = target;
    target.dispatchEvent(ev);
    return ev.defaultPrevented;
};
G.__wrld_fire = function (type) {
    if (type === "DOMContentLoaded") { readyState = "interactive"; document.dispatchEvent(new Event("readystatechange")); document.dispatchEvent(new Event("DOMContentLoaded", { bubbles: true })); }
    else if (type === "load") { readyState = "complete"; document.dispatchEvent(new Event("readystatechange")); const ev = new Event("load"); ev.target = document; fireOn(window, ev); }
};
G.__wrld_report = reportError;
})(globalThis.__host);
