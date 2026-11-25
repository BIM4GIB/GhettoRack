/* GhettoRack at 480x320: the frame and the parts every page shares on the 3.5" touch
   screen. A page loads this (/small.js) and, when opened with ?small, builds itself
   with Small.frame() instead of its TV / phone layout. Styles: small.css.
   The rules, from the ApcBox page's small layout: a 24 px top bar, a 40 px rail on
   the left with HOME last, content that never scrolls (page it with Small.pager),
   a touch acts when the finger lands, no mouse pointer unless a mouse moves. */
"use strict";
const Small = (() => {
  /* ?small=1 once and this browser stays small on every page of the box, whatever link brought it
     there (the kiosk has a browser of its own). ?small=0 ends it. */
  const q = new URLSearchParams(location.search);
  let on = q.has("small") ? q.get("small") !== "0" : null;
  try { if (on === null) on = localStorage.getItem("gr-small") === "1"; else localStorage.setItem("gr-small", on ? "1" : "0"); } catch (e) { on = !!on; }
  const W = 480, H = 320;
  const el = (tag, cls, text) => { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; };
  /* another page of the box, in the same layout */
  const url = path => path + (path.includes("?") ? "&" : "?") + "small=1";

  function fit(app) {
    const vv = window.visualViewport, w = vv ? vv.width : innerWidth, h = vv ? vv.height : innerHeight;
    let s = Math.min(w / W, h / H); if (s > 0.98 && s < 1.1) s = 1;   /* Chromium's window is never under 500 px wide */
    app.style.transform = "scale(" + s + ")";
    return s;
  }

  /* opts: title, tabs [[id, label]...] (none: no rail), home (false: no HOME on the rail), onTab(id).
     Returns { view, top, status(text, cls), tab(id), scale }. */
  function frame(opts) {
    const css = document.createElement("link"); css.rel = "stylesheet"; css.href = "/small.css"; document.head.appendChild(css);
    document.querySelectorAll("style").forEach(x => x.remove());      /* the page's TV / phone styles */
    document.body.textContent = ""; document.body.className = "g-small";
    const tabs = opts.tabs || [], rail = tabs.length > 0;
    const app = el("div", "g-app" + (rail ? "" : " norail")), top = el("div", "g-top"), view = el("div", "g-view"), leds = el("span");
    top.append(el("span", "g-title", opts.title || ""), el("span", "g-sp"), leds);
    app.appendChild(top);
    const btn = {};
    if (rail) {
      const r = el("div", "g-rail");
      for (const [id, label] of tabs) { const b = el("button", "g-tab", label); b.onclick = () => api.tab(id); r.appendChild(b); btn[id] = b; }
      if (opts.home !== false) { const h = el("button", "g-tab home", "HOME"); h.onclick = () => { location.href = url("/"); }; r.appendChild(h); }
      app.appendChild(r);
    }
    app.appendChild(view);
    document.body.appendChild(app);
    const api = { view, top, scale: fit(app),
      status(text, cls) { leds.textContent = ""; if (text) leds.appendChild(el("span", "g-led " + (cls || ""), text)); },
      tab(id) { for (const k in btn) btn[k].classList.toggle("on", k === id); view.textContent = ""; view.className = "g-view"; if (opts.onTab) opts.onTab(id); } };
    const refit = () => { api.scale = fit(app); };
    addEventListener("resize", refit); if (window.visualViewport) visualViewport.addEventListener("resize", refit);
    return api;
  }

  /* A button. cls: on, go, warn, rec */
  function btn(text, onTap, cls) { const b = el("button", "g-b " + (cls || ""), text); b.onclick = onTap; return b; }

  /* A list that never scrolls: `per` items a page, drawn by row(item, index) -> element.
     Returns { el (the rows), nav (‹ 1 / 3 ›), set(items), page }. */
  function pager(per, row) {
    const list = el("div"), nav = el("div", "g-nav"), pv = btn("‹", () => { p.page--; draw(); }), nx = btn("›", () => { p.page++; draw(); }), n = el("span");
    nav.append(pv, n, nx);
    let items = [];
    const p = { el: list, nav, page: 0, set(a) { items = a || []; draw(); }, redraw() { draw(); } };
    function draw() {
      const pages = Math.max(1, Math.ceil(items.length / per));
      p.page = Math.max(0, Math.min(pages - 1, p.page));
      list.textContent = "";
      items.slice(p.page * per, (p.page + 1) * per).forEach((it, k) => list.appendChild(row(it, p.page * per + k)));
      n.textContent = (p.page + 1) + " / " + pages; pv.disabled = p.page === 0; nx.disabled = p.page >= pages - 1;
    }
    return p;
  }

  /* Typing on a screen with no keyboard. keyboard({ title, value, max, secret }) and
     numpad({ title, max }) cover the frame and resolve with the text, or null on CANCEL.
     secret: shown as dots until SHOW is tapped (a password is easier typed in sight). */
  const ROWS = { lower: ["1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm._@"], upper: ["1234567890", "QWERTYUIOP", "ASDFGHJKL-", "ZXCVBNM,!?"],
                 sym: ["!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|", "}~"] };
  function sheet(cls) { const o = el("div", "g-sheet " + cls); document.querySelector(".g-app").appendChild(o); return o; }
  function keyboard(opts) {
    return new Promise(done => {
      const o = sheet("g-kb"), max = opts.max || 63;
      let val = opts.value || "", mode = "lower", show = !opts.secret;
      const head = el("div", "g-khead"), title = el("span", "g-dim", opts.title || ""), out = el("span", "g-kval"), eye = btn("SHOW", () => { show = !show; draw(); });
      head.append(title, out); if (opts.secret) head.appendChild(eye);
      const keys = el("div", "g-keys"), foot = el("div", "g-kfoot");
      const end = v => { o.remove(); done(v); };
      const shift = btn("SHIFT", () => { mode = mode === "upper" ? "lower" : "upper"; draw(); }), sym = btn("#+=", () => { mode = mode === "sym" ? "lower" : "sym"; draw(); });
      foot.append(shift, sym, btn("SPACE", () => { if (val.length < max) val += " "; draw(); }), btn("DEL", () => { val = val.slice(0, -1); draw(); }),
        btn("CANCEL", () => end(null)), btn("OK", () => end(val), "go"));
      o.append(head, keys, foot);
      function draw() {
        const t = show ? val : "•".repeat(val.length);       /* a long text: its end stays in sight */
        out.textContent = (t.length > 40 ? "…" + t.slice(-39) : t) + "▏"; eye.textContent = show ? "HIDE" : "SHOW";
        shift.classList.toggle("on", mode === "upper"); sym.classList.toggle("on", mode === "sym");
        if (keys.dataset.m === mode) return; keys.dataset.m = mode; keys.textContent = "";
        for (const row of ROWS[mode]) for (let i = 0; i < 10; i++) { const c = row[i];
          if (c === undefined) { keys.appendChild(el("span")); continue; }
          keys.appendChild(btn(c, () => { if (val.length < max) val += c; if (mode === "upper") mode = "lower"; draw(); })); }
      }
      draw();
    });
  }
  function numpad(opts) {
    return new Promise(done => {
      const o = sheet("g-np"), max = opts.max || 8; let val = "";
      const head = el("div", "g-khead"), out = el("span", "g-kval"); head.append(el("span", "g-dim", opts.title || "PIN"), out);
      const keys = el("div", "g-nkeys"), end = v => { o.remove(); done(v); }, draw = () => { out.textContent = "•".repeat(val.length) + "▏"; };
      for (const k of ["1", "2", "3", "4", "5", "6", "7", "8", "9", "DEL", "0", "OK"])
        keys.appendChild(btn(k, () => { if (k === "OK") return end(val); if (k === "DEL") val = val.slice(0, -1); else if (val.length < max) val += k; draw(); }, k === "OK" ? "go" : ""));
      const foot = el("div", "g-kfoot"); foot.style.gridTemplateColumns = "1fr"; foot.appendChild(btn("CANCEL", () => end(null)));
      o.append(head, keys, foot); draw();
    });
  }

  if (on) {
    /* A touch acts when the finger lands, not when it lifts: a press on this resistive panel
       lasts 0.2 to 0.3 s and a click only comes at its end. The click that follows is swallowed. */
    let fired = null;
    document.addEventListener("pointerdown", e => { const t = e.target.closest ? e.target.closest("button") : null; fired = null;
      if (!t || !t.onclick || t.disabled || e.pointerType === "mouse") return; fired = t; t.onclick(e); }, true);
    document.addEventListener("click", e => { if (fired && e.isTrusted) { e.stopPropagation(); e.preventDefault(); } fired = null; }, true);
    /* the mouse pointer goes away after 5 s without a mouse moving */
    let idle = 0;
    const wake = () => { document.body.classList.remove("nocursor"); clearTimeout(idle); idle = setTimeout(() => document.body.classList.add("nocursor"), 5000); };
    const mouse = e => { if (e.pointerType === "mouse") wake(); };
    document.addEventListener("pointermove", mouse); document.addEventListener("pointerdown", mouse);
    document.addEventListener("DOMContentLoaded", wake);
  }
  return { on, W, H, el, url, frame, btn, pager, keyboard, numpad };
})();
