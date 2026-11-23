/* ApcBox on the T95: full-screen views driven by the Daisy's report lines
   (SSE /events) and POST /do (control protocol 4, firmware v13.9+). Works
   with no APC40. The look and the view list: t95/ui/VIEWS.md. This file is
   inlined into apcbox.html by t95/ui/build.py. */
"use strict";
const AM = "#ff9a1f", GR = "#35d05a", BL = "#4aa3ff";
const FXN = ["rpt 1/4", "rpt 1/8", "rpt 1/16", "rpt 1/32", "tape stop", "reverse", "crush", "gate"];
const DIV = ["1/16", "1/8", "1/8 dot", "1/4", "1/4 dot", "1/2"];
const NOTE = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"];
const Q = new URLSearchParams(location.search);
const tv = Q.has("tv");
/* One layout drawn for a 1920 px wide screen (TV, Mac, iPad on its side) and one
   for a 390 px wide phone held upright (also an iPad held upright). Whichever
   fits is scaled to the window, so nothing is ever cut off and the view bar is
   always on screen. The height comes from the visible area, which on iOS is
   smaller than "100vh" while Safari's bars show. */
const WIDE_W = 1920, WIDE_MIN_H = 900, TALL_W = 390, TALL_MIN_H = 700, SHORT_W = 844, SHORT_MIN_H = 330;
let PHONE = false, SHORT = false, SCALE = 1;   /* PHONE: upright; SHORT: a phone on its side; neither: wide */
const sz = (wide, tall, short) => SHORT ? short : PHONE ? tall : wide;
function fit() {
  const vv = window.visualViewport, w = Math.round(vv ? vv.width : innerWidth), h = Math.round(vv ? vv.height : innerHeight);
  const tall = h > w * 1.1, short = !tall && h < 520 && !tv;
  const dw = tall ? TALL_W : short ? SHORT_W : WIDE_W, dh = tall ? TALL_MIN_H : short ? SHORT_MIN_H : WIDE_MIN_H;
  let s = w / dw;
  if (h / s < dh) s = h / dh;
  const app = document.querySelector(".app");
  app.style.width = (w / s) + "px"; app.style.height = (h / s) + "px"; app.style.transform = "scale(" + s + ")";
  SCALE = s;
  const was = (PHONE ? 1 : 0) + (SHORT ? 2 : 0);
  PHONE = tall; SHORT = short;
  document.body.classList.toggle("phone", PHONE);
  document.body.classList.toggle("short", SHORT);
  return was !== (PHONE ? 1 : 0) + (SHORT ? 2 : 0);
}
fit();
document.body.classList.toggle("tv", tv);

/* ---------- state, from the Daisy ---------- */
const st = {
  S: null, playing: 0, step: 0, pat: 0, qpat: -1, rec: 0, bpm: 1200, metro: 0, sel: 0, mute: 0, solo: 0, synth: 0,
  loading: 0, fx: 0, lrec: 0, lrecT: 0, lrecBars: 0, inPeak: 0, mon: 0, cpu: 0, apc: 0, sd: 0, set: -1, dirty: 0,
  grid: "0".repeat(256), gridPat: -1, patmask: 0n, meters: new Uint8Array(18), glob: new Uint8Array(10),
  tr: Array.from({ length: 16 }, () => ({ v: new Uint8Array(13), name: "", acid: new Uint8Array(8), notes: null, notesPat: -1, wave: null })),
  log: [], daisy: false, ctl: 0, bridge: null, ver: 0,
  lp: { len: 0, count: 0, qrec: 0, qpat: 0, qmute: 0, gain: 64, mode: 0, undo: 0, redo: 0, qfx: 1 },
  sets: Array.from({ length: 8 }, () => ({ exists: false })), trans: 0, transName: "", setOp: 0, kits: [], kitPg: [-1, -1], xfer: null, caps: {},
};
const patName = p => "ABCDEFGH"[p >> 3] + (1 + (p & 7));
const typeOf = t => (st.synth >> t & 1) ? "303" : st.tr[t].name.startsWith("[loop") ? "loop" : "sample";
function trackName(t) {
  let n = st.tr[t].name;
  if (!n) return "-";
  if (n === "[303]") return "303";
  if (n.startsWith("[loop")) return "loop";
  if (n[0] === "[") { n = n.slice(1, -1); const i = n.indexOf(" "); return i > 0 ? n.slice(i + 1) : n; }
  return n.replace(/\.[a-z0-9]+$/i, "");
}

/* ---------- sending: coalesced, 25 times a second ---------- */
const pend = new Map(), busy = new Map();
function act(cmd, key) { pend.set(key || cmd, cmd); }
function hold(key, ms) { busy.set(key, performance.now() + (ms || 500)); }
const isBusy = key => (busy.get(key) || 0) > performance.now();
let sendErr = "";
setInterval(async () => {
  if (!pend.size) return;
  const a = [...pend.values()].slice(0, 32);
  for (const k of [...pend.keys()].slice(0, 32)) pend.delete(k);
  try {
    const r = await fetch("/do", { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify({ a }) });
    const j = await r.json();
    sendErr = j.error || "";
  } catch (e) { sendErr = "T95 bridge not answering"; }
  topStatus();
}, 40);

/* ---------- parts ---------- */
function el(tag, cls, text) { const e = document.createElement(tag); if (cls) e.className = cls; if (text !== undefined) e.textContent = text; return e; }
const fmt = {
  pct: v => Math.round(v / 1.27) + "%",
  pan: v => v === 63 || v === 64 ? "C" : v < 64 ? "L" + Math.round((64 - v) / 0.64) : "R" + Math.round((v - 63) / 0.64),
  pitch: v => { const s = Math.round((v / 127 - 0.5) * 24); return (s > 0 ? "+" : "") + s + " st"; },
  decay: v => v >= 127 ? "full" : Math.round(10 * Math.pow(400, v / 127)) + " ms",
  filter: v => { const p = v / 127; return Math.abs(p - 0.5) < 0.02 ? "off" : p < 0.5 ? "LP " + Math.round((0.5 - p) * 200) : "HP " + Math.round((p - 0.5) * 200); },
  rev: v => v >= 64 ? "rev" : "fwd",
  adecay: v => Math.round(30 * Math.pow(80, v / 127)) + " ms",
  tune: v => { const s = (v === 63 || v === 64) ? 0 : (v / 127 - 0.5) * 24; return (s > 0 ? "+" : "") + s.toFixed(1) + " st"; },
  slide: v => Math.round(12 * Math.pow(25, v / 127)) + " ms",
  wave: v => v >= 64 ? "square" : "saw",
  div: v => DIV[Math.min(5, Math.floor(v / 127 * 6))],
  swing: v => Math.round(50 + v / 127 * 25) + "%",
  xf: v => { const x = (v === 63 || v === 64) ? 0.5 : v / 127; return x === 0.5 ? "A+B" : x < 0.5 ? "A" : "B"; },
};

/* A pot with a ring of 15 LEDs. mode "pan": lit from the centre. Drag up/down, wheel, arrow keys when focused. */
function Pot(o) {
  const size = o.size || 54, n = 15, r = size / 2 - 4, c = size / 2, kr = r - size / 7;
  const wrap = el("div", o.cell ? "cell" : "kn");
  const ns = "http://www.w3.org/2000/svg";
  const svg = document.createElementNS(ns, "svg");
  svg.setAttribute("width", size); svg.setAttribute("height", size); svg.setAttribute("viewBox", `0 0 ${size} ${size}`);
  svg.setAttribute("tabindex", "0"); svg.classList.add("pot");
  const leds = [];
  for (let i = 0; i < n; i++) {
    const a = (135 + 270 * i / (n - 1)) * Math.PI / 180, d = document.createElementNS(ns, "circle");
    d.setAttribute("cx", (c + r * Math.cos(a)).toFixed(1)); d.setAttribute("cy", (c + r * Math.sin(a)).toFixed(1)); d.setAttribute("r", size / 26);
    d.setAttribute("fill", "#23262a"); svg.appendChild(d); leds.push(d);
  }
  const knob = document.createElementNS(ns, "circle");
  knob.setAttribute("cx", c); knob.setAttribute("cy", c); knob.setAttribute("r", kr); knob.setAttribute("fill", "#2b2f34"); knob.setAttribute("stroke", "#000"); knob.setAttribute("stroke-width", 1.5);
  const line = document.createElementNS(ns, "line");
  line.setAttribute("x1", c); line.setAttribute("y1", c - kr * 0.35); line.setAttribute("x2", c); line.setAttribute("y2", c - kr * 0.92);
  line.setAttribute("stroke", "#fff"); line.setAttribute("stroke-width", Math.max(2, size / 30)); line.setAttribute("stroke-linecap", "round");
  svg.appendChild(knob); svg.appendChild(line);
  wrap.appendChild(svg);
  let valEl = null;
  if (o.cell) { valEl = el("span", "val"); wrap.appendChild(valEl); wrap.appendChild(el("span", "n", o.label)); }
  else if (o.label) wrap.appendChild(document.createTextNode(o.label));
  let val = -1, lit = new Array(n).fill(false), txt = "";
  function set(v) {
    if (v === val) return;
    val = v;
    const p = v / 127;
    for (let i = 0; i < n; i++) {
      const q = i / (n - 1);
      const on = o.mode === "pan" ? (p >= 0.5 ? q >= 0.499 && q <= p + 0.001 : q <= 0.501 && q >= p - 0.001) : (v > 0 || o.mode !== "vol0") && q <= p + 0.001;
      if (on !== lit[i]) { lit[i] = on; leds[i].setAttribute("fill", on ? o.color : "#23262a"); }
    }
    line.setAttribute("transform", `rotate(${-135 + 270 * p} ${c} ${c})`);
    if (valEl && o.fmt) { const t = o.fmt(v); if (t !== txt) valEl.textContent = txt = t; }
  }
  function change(v) { v = Math.max(0, Math.min(127, Math.round(v))); if (v === val) return; set(v); o.onChange(v); }
  let drag = null;
  svg.addEventListener("pointerdown", e => { drag = { y: e.clientY, v: val }; svg.setPointerCapture(e.pointerId); svg.focus(); e.preventDefault(); });
  svg.addEventListener("pointermove", e => { if (drag) change(drag.v + (drag.y - e.clientY) / Math.max(0.5, SCALE) * (e.shiftKey ? 0.2 : 0.8)); });
  const end = () => { drag = null; };
  svg.addEventListener("pointerup", end); svg.addEventListener("pointercancel", end);
  svg.addEventListener("dblclick", () => { if (o.reset !== undefined) change(o.reset); });
  svg.addEventListener("wheel", e => { change(val + (e.deltaY < 0 ? 2 : -2)); e.preventDefault(); }, { passive: false });
  svg.addEventListener("keydown", e => {
    const d = { ArrowUp: 2, ArrowDown: -2, PageUp: 10, PageDown: -10 }[e.key];
    if (d) { change(val + d); e.preventDefault(); e.stopPropagation(); }
  });
  svg.style.touchAction = "none";
  return { el: wrap, set, step: d => change(val + d), get dragging() { return !!drag; } };
}

/* LED meter: n segments, level 0-255 linear peak shown on a 48 dB scale */
function Meter(n) {
  const m = el("div", "mt"), seg = [];
  for (let i = 0; i < (n || 14); i++) { const s = el("i"); m.appendChild(s); seg.push(s); }
  let cur = 0;
  return { el: m, set(p) {
    const db = p > 0 ? 20 * Math.log10(p / 255) : -99, k = Math.max(0, Math.min(seg.length, Math.round((db + 48) / 48 * seg.length)));
    if (k === cur) return;
    for (let i = Math.min(k, cur); i < Math.max(k, cur); i++) seg[i].className = i < k ? (i >= seg.length - 2 ? "r" : i >= seg.length - 5 ? "y" : "g") : "";
    cur = k;
  } };
}

/* Fader with a meter each side */
function Fader(onChange) {
  const f = el("div", "fd"), m1 = Meter(), m2 = Meter(), slot = el("div", "slot"), cap = el("div", "cap");
  f.appendChild(m1.el); f.appendChild(slot); f.appendChild(m2.el); f.appendChild(cap);
  let val = -1, drag = false;
  function set(v) { if (v === val) return; val = v; cap.style.top = ((1 - v / 127) * 88) + "%"; }
  function at(e) { const r = f.getBoundingClientRect(); const v = Math.max(0, Math.min(127, Math.round((1 - (e.clientY - r.top - 9 * SCALE) / (r.height - 18 * SCALE)) * 127))); if (v !== val) { set(v); onChange(v); } }
  f.addEventListener("pointerdown", e => { drag = true; f.setPointerCapture(e.pointerId); at(e); e.preventDefault(); });
  f.addEventListener("pointermove", e => { if (drag) at(e); });
  const end = () => { drag = false; };
  f.addEventListener("pointerup", end); f.addEventListener("pointercancel", end);
  f.style.touchAction = "none";
  return { el: f, set, meter(p) { m1.set(p); m2.set(p); }, meters(a, b) { m1.set(a); m2.set(b); }, get dragging() { return drag; } };
}

function Btn(cls, text, onTap) {
  const b = el("button", "b " + cls, text);
  b.onclick = onTap;
  let on = null;
  return { el: b, set(v, blink) { const k = (v ? 1 : 0) + (blink ? 2 : 0); if (k === on) return; on = k; b.classList.toggle("on", !!v); b.classList.toggle("blink", !!blink); } };
}

/* One of a few: [[value, label]...]. get() says which is on, pick(value) on a tap */
function Seg(opts, get, pick) {
  const d = el("div", "seg"), sp = opts.map(([v, n]) => { const x = el("span", "", n); x.onclick = () => { pick(v); upd(); }; d.appendChild(x); return [v, x]; });
  function upd() { const c = get(); for (const [v, x] of sp) x.classList.toggle("on", v === c); }
  upd();
  return { el: d, upd };
}
/* A looper / quantise setting: shown at once, sent, and not overwritten by the Daisy's answer for a moment */
const lpAct = (k, v, cmd) => { st.lp[k] = v; hold("lp." + k); act(cmd, "lp." + k); };
const LEN_OPTS = [[0, "free"], [1, "1"], [2, "2"], [4, "4"], [8, "8"]];
/* What the looper is doing on track t, in words ("" = nothing) */
function lrecText(t) {
  if (!st.lrec || st.lrecT !== t) return "";
  const on = st.lp.qrec ? "beat" : "bar", m = st.lp.mode;
  if (st.lrec === 1) return m === 2 ? "sample: waiting for sound" : (m === 1 ? "overdub" : "loop") + " armed: starts on the next " + on + (st.lp.count && !m ? " (count-in)" : "");
  if (m === 2) return "● recording a sample: STOP when done";
  return "● " + (m === 1 ? "overdubbing" : "recording") + ": bar " + (st.lrecBars + 1) + (st.lp.len ? " of " + st.lp.len : "");
}
/* A button that's on while it's held (roll) */
function HoldBtn(cls, text, down, up) {
  const b = el("button", "b " + cls, text); let held = false;
  const d = e => { held = true; b.classList.add("on"); down(); try { b.setPointerCapture(e.pointerId); } catch (x) { } e.preventDefault(); }, u = () => { if (!held) return; held = false; b.classList.remove("on"); up(); };
  b.addEventListener("pointerdown", d); b.addEventListener("pointerup", u); b.addEventListener("pointercancel", u);
  b.style.touchAction = "none";
  return b;
}

/* Waveform from a Z line's 96 peaks, with optional markers [pos 0..1, colour] */
function Wave() {
  const w = el("div", "wave"), cv = el("canvas"), tag = el("span", "tag");
  tag.style.left = "10px"; tag.style.color = "var(--dim)";
  w.appendChild(cv); w.appendChild(tag);
  let last = "";
  function draw(wave, marks, color, text) {
    const key = (wave ? wave.key : "-") + JSON.stringify(marks || []) + color + cv.clientWidth + "x" + cv.clientHeight;
    tag.textContent = text || "";
    if (key === last) return;
    last = key;
    const W = cv.width = Math.max(50, cv.clientWidth * devicePixelRatio), H = cv.height = Math.max(20, cv.clientHeight * devicePixelRatio);
    const g = cv.getContext("2d");
    g.clearRect(0, 0, W, H);
    g.fillStyle = "#1c1f22"; g.fillRect(0, H / 2, W, 1);
    if (wave && wave.pts) {
      const n = wave.pts.length, bw = W / n;
      let top = 1; for (const p of wave.pts) top = Math.max(top, p);
      g.fillStyle = color || GR;
      for (let i = 0; i < n; i++) { const hh = Math.max(1, Math.pow(wave.pts[i] / top, 0.6) * H * 0.46); /* quiet tails stay visible */ g.fillRect(i * bw, H / 2 - hh, Math.max(1, bw * 0.72), 2 * hh); }
    }
    for (const m of marks || []) { g.fillStyle = m[1]; g.fillRect(Math.min(W - 3, m[0] * W), 0, 3 * devicePixelRatio, H); }
  }
  return { el: w, draw };
}

/* ---------- listen: the Daisy's sound in this browser ----------
   /audio.pcm is raw 16-bit 48 kHz stereo from the bridge. It goes into a ring
   buffer and out through Web Audio (ScriptProcessor: works without HTTPS and
   on iOS). About 0.15 s is kept buffered; if the network delivers late it
   refills, if it piles up the oldest is skipped, so it stays close to live. */
const Listen = {
  on: false, state: "off", err: "", ctx: null, node: null, abort: null, keep: null,
  ring: [new Float32Array(48000), new Float32Array(48000)], w: 0, r: 0, filled: false, played: 0, got: 0, under: 0,
  buffered() { return (this.w - this.r + 48000) % 48000; },
  async start() {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!AC) { this.err = "this browser has no Web Audio"; this.state = "error"; return listenBtn(); }
    this.on = true; this.state = "connecting"; this.err = ""; listenBtn();
    try {
      /* iOS: a playing <audio> element makes Web Audio ignore the silent switch */
      this.keep = new Audio("data:audio/wav;base64,UklGRigAAABXQVZFZm10IBAAAAABAAEARKwAAIhYAQACABAAZGF0YQQAAAAAAAAA");
      this.keep.loop = true; this.keep.play().catch(() => { });
      this.ctx = new AC({ latencyHint: "interactive" });
      await this.ctx.resume();
      this.w = this.r = 0; this.filled = false; this.rpos = 0;
      const ratio = 48000 / this.ctx.sampleRate, target = 48000 * 0.15;
      this.node = this.ctx.createScriptProcessor(1024, 0, 2);
      this.node.onaudioprocess = e => {
        const L = e.outputBuffer.getChannelData(0), R = e.outputBuffer.getChannelData(1), n = L.length;
        if (!this.filled && this.buffered() >= target) this.filled = true;
        if (this.filled && this.buffered() < n * ratio + 2) { this.filled = false; this.under++; }
        if (!this.filled) { L.fill(0); R.fill(0); return; }
        if (this.buffered() > 48000 * 0.5) this.r = (this.w - Math.round(target) + 48000) % 48000;   /* piled up: jump to near live */
        let p = this.rpos;
        for (let i = 0; i < n; i++) {
          const k = (this.r + Math.floor(p)) % 48000, k2 = (k + 1) % 48000, f = p - Math.floor(p);
          L[i] = this.ring[0][k] + (this.ring[0][k2] - this.ring[0][k]) * f;
          R[i] = this.ring[1][k] + (this.ring[1][k2] - this.ring[1][k]) * f;
          p += ratio;
        }
        const adv = Math.floor(p); this.rpos = p - adv; this.r = (this.r + adv) % 48000; this.played += n;
      };
      this.node.connect(this.ctx.destination);
      this.abort = new AbortController();
      const resp = await fetch("/audio.pcm", { signal: this.abort.signal, cache: "no-store" });
      if (!resp.ok) throw new Error((await resp.text()).trim() || "no audio from the T95");
      this.state = "on"; listenBtn();
      const rd = resp.body.getReader();
      let carry = new Uint8Array(0);
      for (;;) {
        const { done, value } = await rd.read();
        if (done) throw new Error("the T95 stopped sending audio");
        let buf = value;
        if (carry.length) { buf = new Uint8Array(carry.length + value.length); buf.set(carry); buf.set(value, carry.length); }
        const frames = buf.length >> 2, dv = new DataView(buf.buffer, buf.byteOffset, frames * 4);
        for (let i = 0; i < frames; i++) {
          this.ring[0][this.w] = dv.getInt16(i * 4, true) / 32768; this.ring[1][this.w] = dv.getInt16(i * 4 + 2, true) / 32768;
          this.w = (this.w + 1) % 48000;
          if (this.w === this.r) this.r = (this.r + 1) % 48000;
        }
        this.got += frames;
        carry = buf.slice(frames * 4);
      }
    } catch (e) {
      const stopped = !this.on;
      this.stop(true);
      if (!stopped) { this.state = "error"; this.err = String(e.message || e); listenBtn(); }
    }
  },
  stop(keepState) {
    this.on = false;
    try { if (this.abort) this.abort.abort(); } catch (e) { }
    try { if (this.node) this.node.disconnect(); if (this.ctx) this.ctx.close(); } catch (e) { }
    try { if (this.keep) this.keep.pause(); } catch (e) { }
    this.node = this.ctx = this.abort = this.keep = null;
    if (!keepState) { this.state = "off"; this.err = ""; listenBtn(); }
  },
  toggle() { if (this.on) this.stop(); else this.start(); },
};
window.Listen = Listen;
function listenBtn() {
  if (!T.listen) return;
  const s = Listen.state;
  T.listen.textContent = s === "on" ? "LISTENING" : s === "connecting" ? "…" : s === "error" ? "NO AUDIO" : "LISTEN";
  T.listen.className = "tb sm listen " + s;
  T.listen.title = s === "error" ? Listen.err : "hear the Daisy on this device (about 0.2 s behind; not for playing in time)";
  if (s === "error") { sendErr = "listen: " + Listen.err; topStatus(); }
}

/* ---------- top bar ---------- */
const T = {};
function buildTop() {
  const top = document.querySelector(".top");
  top.textContent = "";
  T.stop = el("button", "tb", "■"); T.play = el("button", "tb", "▶"); T.rec = el("button", "tb rec", "●");
  T.stop.onclick = () => act("stop"); T.play.onclick = () => act(st.playing ? "stop" : "play"); T.rec.onclick = () => act("rec 2");
  T.rec.title = "step record: pads and keys go into the pattern";
  T.bpm = el("span", "big"); T.beats = el("div", "beats"); T.beat = [];
  for (let i = 0; i < 4; i++) { const b = el("i"); T.beats.appendChild(b); T.beat.push(b); }
  T.pos = el("span", "big"); T.pat = el("span", "big"); T.set = el("span", "big"); T.set.style.fontSize = "20px";
  T.minus = el("button", "tb sm", "−"); T.plus = el("button", "tb sm", "+"); T.tap = el("button", "tb sm", "TAP"); T.metro = el("button", "tb sm", "CLICK");
  const bpm = d => { const v = Math.max(600, Math.min(2000, st.bpm + d)); st.bpm = v; hold("bpm"); act("tempo " + v, "tempo"); topS(); };
  T.minus.onclick = () => bpm(-10); T.plus.onclick = () => bpm(10);
  let taps = [];
  T.tap.onclick = () => { const t = performance.now(); taps = taps.filter(x => t - x < 2500).concat([t]); if (taps.length >= 3) bpm(Math.round(600000 / ((taps[taps.length - 1] - taps[0]) / (taps.length - 1))) - st.bpm); };
  T.metro.onclick = () => act("metro 2");
  T.listen = el("button", "tb sm listen", "LISTEN"); T.listen.onclick = () => Listen.toggle();
  T.leds = el("span", "leds");
  const stat = (l, e) => { const s = el("div", "stat"); if (l) s.appendChild(el("span", "lbl", l)); s.appendChild(e); return s; };
  if (SHORT) top.append(T.stop, T.play, T.rec, stat("", T.bpm), T.minus, T.plus, T.tap, T.listen, T.beats, T.pat, el("span", "sp"), T.leds);
  else if (PHONE) {
    top.append(T.stop, T.play, T.rec, stat("", T.bpm), T.beats);
    const r2 = el("div", "r2"); r2.append(T.minus, T.plus, T.tap, T.listen, T.pat, T.leds); top.appendChild(r2);
  } else {
    const sp = el("span", "sp");
    top.append(T.stop, T.play, T.rec, stat("BPM", T.bpm), T.minus, T.plus, T.tap, T.metro, T.beats, stat("Step", T.pos), stat("Pattern", T.pat), stat("Set", T.set), sp, T.listen, T.leds);
  }
  topS(); topStatus(); listenBtn();
}
function topS() {
  if (!T.play) return;
  T.play.classList.toggle("play", !!st.playing);
  T.rec.classList.toggle("on", !!st.rec);
  T.metro.classList.toggle("on", !!st.metro);
  T.bpm.textContent = (st.bpm / 10).toFixed(1);
  const b = st.playing ? st.step >> 2 : -1;
  T.beat.forEach((e, i) => e.classList.toggle("on", i === b));
  T.pos.textContent = st.playing ? (1 + (st.step >> 2)) + "." + (1 + (st.step & 3)) : "-";
  T.pat.textContent = patName(st.pat) + (st.qpat >= 0 && st.qpat !== st.pat ? " → " + patName(st.qpat) : "");
  T.set.textContent = st.set < 0 ? "not saved" + (st.dirty ? " *" : "") : "set " + (st.set + 1) + (st.dirty ? " *" : "");
}
let statusKey = "";
function topStatus() {
  if (!T.leds) return;
  const br = st.bridge, parts = [
    ["Daisy", st.daisy ? (st.ctl >= 4 ? "ok" : "warn") : "bad", st.daisy ? (st.ctl >= 4 ? "" : "firmware too old for this page: flash v13.9") : "not connected"],
    ["APC40", st.apc ? "ok" : "off", ""],
  ].concat(st.ext ? [["ext clock", "warn", ""]] : []).concat([
    ["cpu " + Math.round(st.cpu / 10) + "%", "ok", ""],
  ]);
  if (sendErr) parts.unshift([sendErr, "bad", ""]);
  if (st.ver && st.ver !== REPORT_VERSION) parts.unshift(["page and firmware don't match: reload", "bad", ""]);
  const key = JSON.stringify(parts);
  if (key === statusKey) return;
  statusKey = key;
  T.leds.textContent = "";
  for (const [t, c, tip] of parts) { const l = el("span", "led " + c, tip && !PHONE && !SHORT ? t + ": " + tip : t); l.title = tip; T.leds.appendChild(l); }
}

/* ---------- views ---------- */
const view = document.getElementById("view");
const V = {};
let cur = null, curName = "";
const sel = t => { st.sel = t; hold("sel"); act("track " + (t + 1), "track"); if (cur && cur.S) cur.S(); };
const knobAct = (t, i, v) => { st.tr[t].v[i] = v; hold("k" + t + "." + i); act(`knob ${t + 1} ${i} ${v}`, "k" + t + "." + i); };
const globAct = (i, v) => { st.glob[i] = v; hold("g" + i); act(`glob ${i} ${v}`, "g" + i); };
const acidAct = (t, i, v) => { st.tr[t].acid[i] = v; hold("a" + t + "." + i); act(`acid ${t + 1} ${i} ${v}`, "a" + t + "." + i); };

/* One mixer strip. pots: [[label, knob index, mode, colour]...]; extra buttons by view */
function Strip(t, pots, opt) {
  const s = el("div", "st"), nm = el("div", "nm"), num = el("b", "", String(t + 1)), nmt = document.createTextNode("");
  nm.append(num, nmt); nm.onclick = () => sel(t);
  s.appendChild(nm);
  const P = pots.map(p => { const k = Pot({ size: opt.pot, mode: p[2], color: p[3], label: p[0], reset: p[2] === "pan" ? 64 : undefined, onChange: v => knobAct(t, p[1], v) }); s.appendChild(k.el); return [k, p[1]]; });
  const f = Fader(v => knobAct(t, 0, v));
  s.appendChild(f.el);
  let leds = null;
  if (opt.steps) { const d = el("div", "steps"); leds = []; for (let i = 0; i < 16; i++) { const x = el("i"); d.appendChild(x); leds.push(x); } s.appendChild(d); }
  const bt = el("div", "btns");
  const m = Btn("m", PHONE || SHORT ? "M" : "MUTE", () => act(`mute ${t + 1} 2`)), so = Btn("s", PHONE || SHORT ? "S" : "SOLO", () => act(`solo ${t + 1} 2`));
  bt.append(m.el, so.el);
  let arm = null, side = null;
  if (opt.arm) { arm = Btn("a", PHONE ? "●" : "ARM", () => act(st.lrec && st.lrecT === t ? (st.lrec === 2 ? "lrec stop" : "lrec cancel") : `lrec arm ${t + 1}`)); arm.el.title = "record a live loop into this track from the Daisy's input"; bt.appendChild(arm.el);
    const roll = HoldBtn("r", PHONE ? "↻" : "ROLL", () => act(`roll ${t + 1} 1`), () => act(`roll ${t + 1} 0`)); roll.title = "hold: the track repeats"; bt.appendChild(roll); }
  if (opt.side) { side = el("button", "b act", "A+B"); side.style.gridColumn = "span 2"; side.title = "crossfader side"; side.onclick = () => knobAct(t, 12, (st.tr[t].v[12] + 1) % 3); bt.appendChild(side); }
  s.appendChild(bt);
  s.style.gridTemplateRows = "auto " + "auto ".repeat(pots.length) + "1fr " + (opt.steps ? "auto " : "") + "auto";
  let name = null, ledKey = "";
  return { el: s,
    T() { const v = st.tr[t].v; for (const [k, i] of P) if (!k.dragging && !isBusy("k" + t + "." + i)) k.set(v[i]); if (!f.dragging && !isBusy("k" + t + ".0")) f.set(v[0]);
      if (side) { const x = ["A+B", "A", "B"][v[12]] || "A+B"; if (side.textContent !== x) side.textContent = x; side.classList.toggle("on", v[12] > 0); } },
    N() { const n = trackName(t); if (n !== name) nmt.textContent = name = n; },
    S() { s.classList.toggle("sel", st.sel === t); m.set(st.mute >> t & 1); so.set(st.solo >> t & 1); if (arm) arm.set(st.lrec && st.lrecT === t, st.lrec === 1); this.G(); },
    G() { if (!leds) return; const row = st.grid.substr(t * 16, 16), key = row + (st.playing ? st.step : -1); if (key === ledKey) return; ledKey = key;
      for (let i = 0; i < 16; i++) leds[i].className = st.playing && i === st.step ? "ph" : row[i] === "2" ? "ac" : row[i] === "1" ? "on" : ""; },
    H() { f.meter(st.meters[t]); } };
}
let page8 = 0;   // phone: strips 1-8 or 9-16
function pageSub(onChange, extra) {
  const sub = el("div", "sub");
  const names = ["1-8", "9-16"].concat(extra || []);
  names.forEach((n, i) => { const b = el("span", i === page8 ? "on" : "", n); b.onclick = () => { page8 = i; onChange(); }; sub.appendChild(b); });
  return sub;
}
function patPads(count, cls) {
  const box = el("div", cls || "pads"), pads = [];
  for (let i = 0; i < count; i++) { const p = el("button", "pad"); box.appendChild(p); pads.push(p); }
  return { el: box, pads };
}
function FxPads(cls) {
  const box = el("div", cls || "fx"), pads = [];
  FXN.forEach((n, i) => {
    const p = el("button", cls ? "bigpad fx" : "pad", n);
    const down = e => { act(`fx ${i + 1} 1`, "fx" + i); p.setPointerCapture(e.pointerId); e.preventDefault(); }, up = () => act(`fx ${i + 1} 0`, "fx" + i);
    p.addEventListener("pointerdown", down); p.addEventListener("pointerup", up); p.addEventListener("pointercancel", up);
    p.style.touchAction = "none";
    box.appendChild(p); pads.push(p);
  });
  return { el: box, S() { pads.forEach((p, i) => p.classList.toggle("on", !!(st.fx >> i & 1))); } };
}
const GLOB = { master: ["master", 0, "vol", GR, fmt.pct], xfade: ["xfade", 1, "pan", AM, fmt.xf], revsize: ["rev size", 2, "vol", GR, fmt.pct], revtone: ["rev tone", 3, "vol", GR, fmt.pct],
  dlytime: ["dly time", 4, "vol", GR, fmt.div], dlyfb: ["dly fdbk", 5, "vol", GR, fmt.pct], drive: ["drive", 6, "vol", AM, fmt.pct], fxret: ["fx return", 7, "vol", GR, fmt.pct],
  swing: ["swing", 8, "vol", AM, fmt.swing], filter: ["filter", 9, "pan", BL, fmt.filter] };
function globPot(id, size, cell) { const g = GLOB[id]; const p = Pot({ size, cell, mode: g[2], color: g[3], label: g[0], fmt: g[4], reset: g[2] === "pan" ? 64 : undefined, onChange: v => globAct(g[1], v) }); p.gi = g[1]; return p; }
const globUpd = pots => { for (const p of pots) if (!p.dragging && !isBusy("g" + p.gi)) p.set(st.glob[p.gi]); };

V.perform = {
  build() {
    const tracks = PHONE ? [...Array(8).keys()].map(i => i + page8 * 8) : [...Array(16).keys()];
    this.strips = tracks.map(t => Strip(t, sz([["pan", 1, "pan", AM], ["reverb", 2, "vol", GR]], [["pan", 1, "pan", AM]], [["", 1, "pan", AM]]), { pot: sz(54, 36, 30), steps: !PHONE && !SHORT, arm: !SHORT }));
    const box = el("div", PHONE ? "pst" : "strips"); this.strips.forEach(s => box.appendChild(s.el));
    if (SHORT) { view.className = "v"; view.style.gridTemplateRows = "1fr"; view.append(box); this.gp = []; this.pp = this.fxp = null; return; }
    this.pp = patPads(8); this.fxp = FxPads();
    this.pp.pads.forEach((p, i) => p.onclick = () => act("pat " + ((st.pat >> 3) * 8 + i + 1)));
    const pbox = el("div", "box"); const h3 = el("h3"); this.bankLbl = h3; pbox.append(h3, this.pp.el);
    const fbox = el("div", "box"); fbox.append(el("h3", "", "Performance FX (hold)"), this.fxp.el);
    if (PHONE) {
      view.className = "v"; view.style.gridTemplateRows = "auto 1fr auto auto";
      const hdr = el("div", "hdr"); hdr.append(pageSub(() => show("perform")), el("span", "sp"), el("span", "lbl", "tap a name to select"));
      view.append(hdr, box, pbox, fbox);
      this.gp = [];
    } else {
      view.className = "main";
      const side = el("div", "side"), mbox = el("div", "box"), mk = el("div", "mk");
      this.gp = ["master", "xfade", "filter", "swing", "revsize", "revtone", "dlytime", "dlyfb"].map(id => globPot(id, 70));
      this.gp.forEach(p => mk.appendChild(p.el));
      mbox.append(el("h3", "", "Master"), mk); side.append(pbox, fbox, mbox);
      view.append(box, side);
    }
  },
  S() { this.strips.forEach(s => s.S()); if (!this.fxp) return; this.fxp.S(); const bank = st.pat >> 3;
    this.bankLbl.textContent = "Patterns · bank " + "ABCDEFGH"[bank];
    this.pp.pads.forEach((p, i) => { const n = bank * 8 + i; p.textContent = patName(n); p.className = "pad " + (n === st.pat ? "cur" : n === st.qpat ? "next" : (st.patmask >> BigInt(n) & 1n) ? "has" : ""); }); },
  T(t) { this.strips.forEach(s => s.T()); }, N() { this.strips.forEach(s => s.N()); }, G() { this.strips.forEach(s => s.G()); },
  H() { this.strips.forEach(s => s.H()); }, M() { globUpd(this.gp); }, O() { this.S(); },
};

V.mixer = {
  build() {
    view.className = "v"; view.style.gridTemplateRows = PHONE ? "auto 1fr" : "1fr";
    const tracks = PHONE ? (page8 === 2 ? [] : [...Array(8).keys()].map(i => i + page8 * 8)) : [...Array(16).keys()];
    this.strips = tracks.map(t => Strip(t, [[sz("pan", "pan", ""), 1, "pan", AM], [sz("reverb", "rev", ""), 2, "vol", GR], [sz("delay", "dly", ""), 3, "vol", GR]], { pot: sz(58, 36, 28), side: !SHORT }));
    const box = el("div", PHONE ? "pst" : "mx");
    this.strips.forEach(s => box.appendChild(s.el));
    this.gp = [];
    if (SHORT) { box.className = "strips"; }
    else if (!PHONE || page8 === 2) {
      const m = el("div", "st master"); m.appendChild(el("div", "nm", "MASTER"));
      this.gp = ["filter", "revsize", "dlyfb", "drive", "fxret"].slice(0, PHONE ? 5 : 3).map(id => globPot(id, PHONE ? 60 : 58));
      this.gp.forEach(p => m.appendChild(p.el));
      this.mf = Fader(v => globAct(0, v)); m.appendChild(this.mf.el);
      m.style.gridTemplateRows = "auto " + "auto ".repeat(this.gp.length) + "1fr";
      if (PHONE) box.style.gridTemplateColumns = "1fr";
      box.appendChild(m);
    } else this.mf = null;
    if (PHONE) { const hdr = el("div", "hdr"); hdr.append(pageSub(() => show("mixer"), ["master"])); view.append(hdr, box); } else view.append(box);
  },
  S() { this.strips.forEach(s => s.S()); }, T() { this.strips.forEach(s => s.T()); }, N() { this.strips.forEach(s => s.N()); },
  H() { this.strips.forEach(s => s.H()); if (this.mf) this.mf.meters(st.meters[16], st.meters[17]); },
  M() { globUpd(this.gp); if (this.mf && !this.mf.dragging && !isBusy("g0")) this.mf.set(st.glob[0]); },
};

/* Steps: all tracks, or the selected track (big steps; a note grid for a 303) */
let stepsMode = "all", acidOct = 0;
function stepTap(t, s) { const c = +st.grid[t * 16 + s], n = (c + 1) % 3; setStep(t, s, n); }
function setStep(t, s, n) { st.grid = st.grid.substr(0, t * 16 + s) + n + st.grid.substr(t * 16 + s + 1); hold("G"); act(`step ${st.pat + 1} ${t + 1} ${s + 1} ${n}`, "s" + t + "." + s); if (cur.G) cur.G(true); }
function stepsHdr(extra) {
  const hdr = el("div", "hdr"), sub = el("div", "sub");
  [["all", PHONE ? "all" : "all tracks"], ["one", "one track"]].forEach(([m, n]) => { const b = el("span", stepsMode === m ? "on" : "", n); b.onclick = () => { stepsMode = m; show("steps"); }; sub.appendChild(b); });
  const pat = el("span", "big"); hdr.append(sub, pat);
  if (extra) hdr.append(...extra);
  hdr.appendChild(el("span", "sp"));
  const ops = el("div", "sub");
  const mk = (n, f) => { const b = el("span", "", n); b.onclick = f; ops.appendChild(b); };
  mk("clear", () => act(stepsMode === "one" ? `pclear ${st.pat + 1} ${st.sel + 1}` : `pclear ${st.pat + 1} 0`));
  mk("undo", () => act("undo"));
  hdr.appendChild(ops);
  return { hdr, pat };
}
V.steps = {
  build() { (stepsMode === "all" ? this.buildAll : this.buildOne).call(this); },
  buildAll() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr";
    const { hdr, pat } = stepsHdr(PHONE || SHORT ? null : [el("span", "lbl", "tap: on · accent · off")]); this.patEl = pat;
    const NOMS = PHONE || SHORT;
    const g = el("div", "grid"); g.style.gridTemplateRows = (NOMS ? "12px" : "18px") + " repeat(16,1fr)";
    g.appendChild(el("div")); if (!NOMS) g.appendChild(el("div"));
    for (let k = 0; k < 16; k++) g.appendChild(el("div", "colh", String(k + 1)));
    this.rows = []; this.cells = [];
    for (let t = 0; t < 16; t++) {
      const tn = el("div", "tn"), b = el("b", "", String(t + 1)), nm = document.createTextNode("");
      tn.append(b, nm); tn.onclick = () => sel(t); g.appendChild(tn);
      let m = null, s = null;
      if (!NOMS) { const ms = el("div", "ms"); m = Btn("m", "M", () => act(`mute ${t + 1} 2`)); s = Btn("s", "S", () => act(`solo ${t + 1} 2`)); ms.append(m.el, s.el); g.appendChild(ms); }
      this.rows.push({ tn, nm, m, s, name: null });
      for (let k = 0; k < 16; k++) { const c = el("button", "sc" + ((k >> 2) % 2 ? "" : " q")); c.onclick = () => stepTap(t, k); g.appendChild(c); this.cells.push(c); }
    }
    this.key = new Array(256).fill("");
    view.append(hdr, g);
  },
  buildOne() {
    const t = st.sel, ty = typeOf(t);
    this.one = { t, ty };
    view.className = "v";
    const octs = [];
    if (ty === "303") { const sub = el("div", "sub"); [["oct −", -1], ["oct +", 1]].forEach(([n, d]) => { const b = el("span", "", n); b.onclick = () => { acidOct = Math.max(-1, Math.min(3, acidOct + d)); show("steps"); }; sub.appendChild(b); }); octs.push(sub); }
    const { hdr, pat } = stepsHdr([el("span", "big", (t + 1) + " · " + trackName(t))].concat(octs)); this.patEl = pat;
    const big = el("div", "bigsteps"); if (PHONE) big.style.gridTemplateRows = "1fr 1fr";
    if (SHORT) big.style.gridTemplateColumns = "repeat(16,1fr)";
    this.bs = [];
    for (let k = 0; k < 16; k++) {
      const b = el("button", "bs" + ((k >> 2) % 2 ? "" : " q")), sl = el("span", "sl", "slide"), nt = el("span", "nt"), lb = el("span", "", ""), num = el("span", "", String(k + 1));
      sl.style.visibility = "hidden";
      if (ty === "303") { b.append(sl, nt, num); sl.onclick = e => { e.stopPropagation(); this.note(k, null, true); }; } else b.append(lb, num);
      b.onclick = () => stepTap(t, k);
      big.appendChild(b); this.bs.push({ b, sl, nt, lb, key: "" });
    }
    if (ty === "303") {
      view.style.gridTemplateRows = sz("auto 1fr 150px", "auto 1.5fr 1fr", "auto 1fr 54px");
      const roll = el("div", "roll"); this.rc = [];
      const base = 36 + acidOct * 12;
      for (let n = base + 12; n >= base; n--) {
        const nm = NOTE[n % 12] + (Math.floor(n / 12) - 1);
        roll.appendChild(el("div", "k" + (nm.includes("#") ? " bk" : ""), nm));
        for (let k = 0; k < 16; k++) { const c = el("button", "c" + ((k >> 2) % 2 ? "" : " q")); c.onclick = () => this.note(k, n, false); roll.appendChild(c); this.rc.push({ c, n, k, key: "" }); }
      }
      view.append(hdr, roll, big);
    } else {
      view.style.gridTemplateRows = sz("auto 170px 1fr", "auto 90px 1.3fr", "auto 70px 1fr");
      this.wave = Wave();
      view.append(hdr, this.wave.el, big);
    }
  },
  /* 303: set a step's note (roll tap), or toggle its slide */
  note(k, n, slide) {
    const t = this.one.t, tr = st.tr[t], nb = tr.notes ? tr.notes[k] : 36, on = st.grid[t * 16 + k] !== "0";
    let note = nb & 127 || 36, sl = !!(nb & 128);
    if (slide) sl = !sl;
    else if (on && note === n) { setStep(t, k, 0); return; }
    else note = n;
    if (!tr.notes) tr.notes = new Uint8Array(16).fill(36);
    tr.notes[k] = note | (sl ? 128 : 0); hold("Q" + t);
    act(`note ${st.pat + 1} ${t + 1} ${k + 1} ${note} ${sl ? 1 : 0}`, "n" + t + "." + k);
    if (!on && !slide) setStep(t, k, 1); else this.G(true);
  },
  S() {
    this.patEl.textContent = patName(st.pat);
    if (stepsMode === "one" && (this.one.t !== st.sel || this.one.ty !== typeOf(st.sel))) return show("steps");
    if (stepsMode === "all") this.rows.forEach((r, t) => { r.tn.classList.toggle("sel", st.sel === t); if (r.m) { r.m.set(st.mute >> t & 1); r.s.set(st.solo >> t & 1); } });
    this.G();
  },
  N() { if (stepsMode === "all") this.rows.forEach((r, t) => { const n = trackName(t); if (n !== r.name) r.nm.textContent = r.name = n; }); else if (this.one.ty !== typeOf(st.sel)) show("steps"); },
  G() {
    const ph = st.playing ? st.step : -1;
    if (stepsMode === "all") {
      for (let i = 0; i < 256; i++) { const k = st.grid[i] + ((i & 15) === ph ? "p" : ""); if (k !== this.key[i]) { this.key[i] = k; this.cells[i].className = "sc" + (((i & 15) >> 2) % 2 ? "" : " q") + (st.grid[i] === "2" ? " ac" : st.grid[i] === "1" ? " on" : "") + ((i & 15) === ph ? " ph" : ""); } }
      return;
    }
    const t = this.one.t, tr = st.tr[t], acid = this.one.ty === "303";
    this.bs.forEach((x, k) => {
      const v = st.grid[t * 16 + k], nb = tr.notes ? tr.notes[k] : 36, key = v + (k === ph ? "p" : "") + (acid ? nb : "");
      if (key === x.key) return; x.key = key;
      x.b.className = "bs" + ((k >> 2) % 2 ? "" : " q") + (v === "2" ? " ac" : v === "1" ? " on" : "") + (k === ph ? " ph" : "");
      if (acid) { const n = nb & 127 || 36; x.nt.textContent = v !== "0" ? NOTE[n % 12] + (Math.floor(n / 12) - 1) : ""; x.sl.style.visibility = v !== "0" ? "visible" : "hidden"; x.sl.classList.toggle("off", !(nb & 128)); }
      else x.lb.textContent = v === "2" ? "ACC" : v === "1" ? "ON" : "";
    });
    if (acid) for (const x of this.rc) {
      const v = st.grid[t * 16 + x.k], nb = tr.notes ? tr.notes[x.k] : 36, here = v !== "0" && (nb & 127 || 36) === x.n, key = here ? v + (nb & 128 ? "s" : "") : "";
      if (key !== x.key) { x.key = key; x.c.className = "c" + ((x.k >> 2) % 2 ? "" : " q") + (here ? (v === "2" ? " a" : " n") + (nb & 128 ? " s" : "") : ""); }
    }
    else this.Z();
  },
  Q() { this.G(); },
  Z() { if (stepsMode === "one" && this.wave) { const t = this.one.t, w = st.tr[t].wave; this.wave.draw(w, [[st.tr[t].v[8] / 127, AM]], GR, w && w.len ? trackName(t) + " · " + (w.len / w.rate).toFixed(2) + " s" : ""); } },
  T() { this.Z(); },
};

/* Sound: the selected track's editor, by type */
const SAMPLE_POTS = [["pitch", 4, "pan", fmt.pitch], ["decay", 5, "vol", fmt.decay], ["filter", 6, "pan", fmt.filter], ["reso", 7, "vol", fmt.pct], ["start", 8, "vol", fmt.pct], ["reverse", 9, "vol", fmt.rev], ["drive", 10, "vol", fmt.pct], ["prob", 11, "vol", fmt.pct]];
const ACID_POTS = [["cutoff", 0, fmt.pct], ["reso", 1, fmt.pct], ["env mod", 2, fmt.pct], ["decay", 3, fmt.adecay], ["accent", 4, fmt.pct], ["tune", 5, fmt.tune], ["slide", 6, fmt.slide], ["wave", 7, fmt.wave]];
const TRACK_POTS = [["volume", 0, "vol", fmt.pct], ["pan", 1, "pan", fmt.pan], ["reverb", 2, "vol", fmt.pct], ["delay", 3, "vol", fmt.pct]];
function trackHdr(t, ty) {
  const hdr = el("div", "hdr"), sub = el("div", "sub");
  const title = el("span", "big", (t + 1) + " · " + trackName(t));
  hdr.appendChild(title);
  [["sample", "sample"], ["303", "303"]].forEach(([k, n]) => { const b = el("span", ty === k ? "on" : "", n); b.onclick = () => act(`type ${t + 1} ${k}`); sub.appendChild(b); });
  if (ty === "loop") sub.appendChild(el("span", "on", "loop"));
  hdr.append(sub, el("span", "sp"));
  const bt = el("div", "btns"); bt.style.cssText = "grid-template-columns:repeat(4,1fr);width:" + sz(360, 190, 220) + "px";
  const m = Btn("m", PHONE ? "M" : "MUTE", () => act(`mute ${t + 1} 2`)), s = Btn("s", PHONE ? "S" : "SOLO", () => act(`solo ${t + 1} 2`));
  const a = Btn("a", "ARM", () => act(st.lrec && st.lrecT === t ? (st.lrec === 2 ? "lrec stop" : "lrec cancel") : `lrec arm ${t + 1}`));
  const au = Btn("act", "▶", () => act(`audition ${t + 1}`)); au.el.title = "play it once";
  bt.append(m.el, s.el, a.el, au.el); hdr.appendChild(bt);
  return { hdr, upd() { const n = (t + 1) + " · " + trackName(t); if (title.textContent !== n) title.textContent = n; m.set(st.mute >> t & 1); s.set(st.solo >> t & 1); a.set(st.lrec && st.lrecT === t, st.lrec === 1); } };
}
V.sound = {
  build() {
    const t = st.sel, ty = typeOf(t); this.t = t; this.ty = ty;
    view.className = "v";
    this.h = trackHdr(t, ty);
    this.wave = Wave();
    const size = sz(150, 64, 50), row = n => { const r = el("div", PHONE ? "row4" : "row8"); if (PHONE) r.style.gridTemplateRows = `repeat(${n},1fr)`; return r; };
    this.dev = []; this.trk = [];
    const r1 = row(ty === "loop" ? 1 : PHONE ? 3 : 1), r2 = PHONE ? r1 : row(1);
    if (ty === "303") ACID_POTS.forEach(p => { const k = Pot({ size, cell: true, mode: p[1] === 5 ? "pan" : "vol", color: AM, label: p[0], fmt: p[2], reset: p[1] === 5 ? 64 : undefined, onChange: v => acidAct(t, p[1], v) }); k.i = p[1]; this.dev.push(k); r1.appendChild(k.el); });
    else if (ty === "sample") SAMPLE_POTS.forEach(p => { const k = Pot({ size, cell: true, mode: p[2], color: AM, label: p[0], fmt: p[3], reset: p[2] === "pan" ? 64 : undefined, onChange: v => knobAct(t, p[1], v) }); k.i = p[1]; this.dev.push(k); r1.appendChild(k.el); });
    TRACK_POTS.forEach(p => { const k = Pot({ size, cell: true, mode: p[2], color: GR, label: p[0], fmt: p[3], reset: p[2] === "pan" ? 64 : undefined, onChange: v => knobAct(t, p[1], v) }); k.i = p[1]; this.trk.push(k); r2.appendChild(k.el); });
    this.side = el("button", "cell sidebtn"); this.side.onclick = () => knobAct(t, 12, (st.tr[t].v[12] + 1) % 3); r2.appendChild(this.side);
    const mt = el("div", "cell"); mt.style.flexDirection = "row"; this.m = Meter(PHONE ? 10 : 14); mt.appendChild(this.m.el); this.m.el.style.height = "70%"; r2.appendChild(mt);
    if (ty === "loop") {
      const pg = el("div", "pg"); pg.style.gridTemplateColumns = PHONE ? "1fr 1fr" : "repeat(4,1fr)";
      [["RE-RECORD", `lrec arm ${t + 1}`], ["OVERDUB", `lrec over ${t + 1}`], ["STOP", "lrec stop"], ["UNDO / REDO", () => st.lp.undo !== t + 1 && st.lp.redo === t + 1 ? "lrec redo" : "lrec undo"]].forEach(([n, c]) => { const b = el("button", "bigpad", n); b.onclick = () => act(typeof c === "function" ? c() : c); pg.appendChild(b); });
      this.state = el("div", "hdr");
      view.style.gridTemplateRows = sz("auto auto 1fr 110px 190px", "auto auto 1fr 110px 120px", "auto auto 1fr 44px 92px");
      view.append(this.h.hdr, this.state, this.wave.el, pg, r2);
    } else if (PHONE) { view.style.gridTemplateRows = "auto 96px 1fr"; view.append(this.h.hdr, this.wave.el, r1); }
    else { view.style.gridTemplateRows = sz("auto 230px 1fr 1fr", "", "auto 56px 1fr 1fr"); view.append(this.h.hdr, this.wave.el, r1, r2); }
  },
  S() { if (st.sel !== this.t || typeOf(st.sel) !== this.ty) return show("sound"); this.h.upd();
    if (this.state) this.state.textContent = lrecText(this.t) || st.tr[this.t].name.replace(/[\[\]]/g, ""); this.Z(); },
  N() { if (typeOf(st.sel) !== this.ty) show("sound"); else { this.h.upd(); this.Z(); } },
  T() { const v = st.tr[this.t].v; if (this.ty === "sample") for (const k of this.dev) if (!k.dragging && !isBusy("k" + this.t + "." + k.i)) k.set(v[k.i]);
    for (const k of this.trk) if (!k.dragging && !isBusy("k" + this.t + "." + k.i)) k.set(v[k.i]);
    const sd = ["A+B", "A", "B"][v[12]] || "A+B"; this.side.innerHTML = `<span class="val">${sd}</span><span class="n">xfade side</span>`; this.Z(); },
  A() { if (this.ty === "303") { const a = st.tr[this.t].acid; for (const k of this.dev) if (!k.dragging && !isBusy("a" + this.t + "." + k.i)) k.set(a[k.i]); this.Z(); } },
  H() { this.m.set(st.meters[this.t]); },
  Z() {
    const t = this.t, w = st.tr[t].wave;
    if (this.ty === "303") { const a = st.tr[t].acid; this.wave.draw({ key: "f" + a[0] + "." + a[1], pts: filterCurve(a[0] / 127, a[1] / 127) }, [], AM, "Pocket 303 · filter"); }
    else if (this.ty === "loop") { const marks = []; const bars = +(st.tr[t].name.match(/\d+/) || [1])[0]; for (let b = 1; b < bars; b++) marks.push([b / bars, "#2a2d31"]);
      if (st.playing) marks.push([((st.step / 16) + 0) / 1 / bars % 1, "#ffb833"]); this.wave.draw(w, marks, GR, w && w.len ? bars + " bars · " + (w.len / w.rate).toFixed(1) + " s" : "nothing recorded"); }
    else this.wave.draw(w, [[st.tr[t].v[8] / 127, AM]], GR, w && w.len ? st.tr[t].name.replace(/[\[\]]/g, "") + " · " + (w.len / w.rate).toFixed(2) + " s" : "");
  },
};
function filterCurve(fc, q) { const pts = []; fc = 0.08 + fc * 0.8; for (let i = 0; i < 96; i++) { const r = (i / 96) / fc; pts.push(Math.min(255, 100 / Math.sqrt((1 - r * r) ** 2 + (r / (1 + q * 6)) ** 2))); } return pts; }

/* Patterns: all 64; Sets: the 8 slots on the Daisy's card */
let patMode = "";   // "", "copy", "clear"
let patTab = Q.get("pt") || "patterns", setSel = -1, setConfirm = "";
const cleanName = t => t.toLowerCase().replace(/[^a-z0-9 -]/g, "-").replace(/\s+/g, " ").trim().slice(0, 16);
function patSub() {
  const sub = el("div", "sub");
  [["patterns", "patterns"], ["sets", "sets"]].forEach(([m, n]) => { const b = el("span", patTab === m ? "on" : "", n); b.onclick = () => { patTab = m; show("patterns"); }; sub.appendChild(b); });
  return sub;
}
const Sets = {
  build() {
    view.className = "v"; view.style.gridTemplateRows = sz("auto 1fr auto auto", "auto 1fr auto auto auto", "auto 1fr auto");
    const hdr = el("div", "hdr"); this.state = el("span", "lbl");
    hdr.append(patSub(), el("span", "sp"));
    if (!PHONE) hdr.insertBefore(this.state, hdr.lastChild);
    this.trans = el("button", "b act"); this.trans.style.cssText = "padding:0 12px;height:32px"; this.trans.title = "how the switch to another set sounds";
    this.trans.onclick = () => act("set trans " + (st.trans + 1) % 4);
    hdr.appendChild(this.trans);
    const list = el("div", "pg"); list.style.cssText = `grid-template-columns:${PHONE ? "1fr" : "1fr 1fr"};grid-template-rows:repeat(${PHONE ? 8 : 4},1fr);grid-auto-flow:${PHONE ? "row" : "column"}`;
    this.rows = [];
    for (let i = 0; i < 8; i++) {
      const r = el("button", "setrow"), num = el("span", "num", String(i + 1).padStart(2, "0")), nm = el("span", "nm2"), d = el("span", "d"), tag = el("span", "tag2");
      r.append(num, nm, d, tag); r.onclick = () => { setSel = i; setConfirm = ""; if (st.sets[i].exists) this.name.value = st.sets[i].name; this.J(); };
      list.appendChild(r); this.rows.push({ r, nm, d, tag });
    }
    this.name = el("input"); this.name.type = "text"; this.name.maxLength = 16; this.name.placeholder = "name (a-z, 0-9)"; this.name.autocapitalize = "off"; this.name.autocomplete = "off"; this.name.spellcheck = false;
    this.name.addEventListener("blur", () => setTimeout(refit, 300));
    const nmrow = el("div", "hdr"); nmrow.append(el("span", "lbl", "name"), this.name);
    const ops = el("div", "pg"); ops.style.cssText = `grid-template-columns:repeat(${PHONE ? 3 : 5},1fr);grid-auto-rows:${sz(56, 46, 36)}px`;
    this.btn = {};
    [["load", "LOAD"], ["save", "SAVE HERE"], ["new", "SAVE AS NEW"], ["name", "RENAME"], ["delete", "DELETE"]].forEach(([k, n]) => { const b = el("button", "bigpad", n); b.onclick = () => this.op(k); ops.appendChild(b); this.btn[k] = b; });
    if (SHORT) { nmrow.appendChild(ops); ops.style.flex = "1"; view.append(hdr, list, nmrow); }
    else if (PHONE) { view.style.gridTemplateRows = "auto auto 1fr auto auto"; view.append(hdr, this.state, list, nmrow, ops); }
    else view.append(hdr, list, nmrow, ops);
    if (setSel < 0) setSel = st.set >= 0 ? st.set : 0;
  },
  op(k) {
    const i = setSel, nm = cleanName(this.name.value), ex = st.sets[i] && st.sets[i].exists;
    if ((k === "delete" || (k === "save" && ex && i !== st.set) || (k === "load" && st.dirty)) && setConfirm !== k) { setConfirm = k; return this.J(); }
    setConfirm = "";
    if (k === "load") { if (ex) act("set load " + (i + 1)); }
    else if (k === "save") act(("set save " + (i + 1) + " " + nm).trim());
    else if (k === "new") act(("set new " + nm).trim());
    else if (k === "name") { if (ex && nm) act("set name " + (i + 1) + " " + nm); }
    else if (k === "delete") { if (ex) act("set delete " + (i + 1)); }
    this.J();
  },
  J() {
    this.rows.forEach((x, i) => {
      const s = st.sets[i];
      x.r.className = "setrow" + (i === setSel ? " sel" : "") + (s.exists ? " has" : "") + (i === st.set ? " open" : "");
      x.nm.textContent = s.exists ? s.name : "empty";
      x.d.textContent = s.exists ? s.bpm.toFixed(0) + " bpm · " + s.samples + " samples" + (s.loops ? " · " + s.loops + " loops" : "") + (s.synths ? " · " + s.synths + " 303" : "") : "";
      x.tag.textContent = i === st.set ? (st.dirty ? "open, changed" : "open") : "";
    });
    const ex = st.sets[setSel] && st.sets[setSel].exists;
    this.btn.load.disabled = !ex || !!st.setOp; this.btn.name.disabled = !ex || !!st.setOp; this.btn.delete.disabled = !ex || !!st.setOp;
    this.btn.save.disabled = this.btn.new.disabled = !!st.setOp;
    this.btn.load.textContent = setConfirm === "load" ? "DROP CHANGES? TAP AGAIN" : "LOAD";
    this.btn.save.textContent = setConfirm === "save" ? "OVERWRITE? TAP AGAIN" : "SAVE HERE";
    this.btn.delete.textContent = setConfirm === "delete" ? "DELETE? TAP AGAIN" : "DELETE";
    for (const k of ["load", "save", "delete"]) this.btn[k].classList.toggle("warn", setConfirm === k);
    this.trans.textContent = "switch: " + (st.transName || "-").toLowerCase();
    this.state.textContent = !st.sd ? "no SD card in the Daisy: sets need one" : st.setOp === 1 ? "saving…" : st.setOp === 2 ? "loading: switches on the bar" : "sets are saved on the Daisy's card, with their sounds";
  },
  S() { this.J(); },
};
V.patterns = {
  build() {
    if (patTab === "sets") { for (const k of ["J", "S", "O"]) this[k] = Sets[k] ? Sets[k].bind(Sets) : () => { }; return Sets.build(); }
    this.S = this.S0; this.O = this.O0; this.J = null;
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr";
    const hdr = el("div", "hdr"); this.now = el("span", "big"); this.hint = el("span", "lbl");
    hdr.append(patSub(), el("span", "lbl", PHONE ? "" : "playing"), this.now, this.hint, el("span", "sp"));
    const ops = el("div", "sub"); this.ops = {};
    [["copy", "copy"], ["clear", "clear"]].forEach(([m, n]) => { const b = el("span", "", n); b.onclick = () => { patMode = patMode === m ? "" : m; this.src = -1; this.S(); }; ops.appendChild(b); this.ops[m] = b; });
    const u = el("span", "", "undo"); u.onclick = () => act("undo"); ops.appendChild(u);
    hdr.appendChild(ops);
    const pg = el("div", "pg"); pg.style.cssText = `grid-template-columns:${PHONE ? 18 : 44}px repeat(8,1fr);grid-template-rows:repeat(8,1fr)` + (PHONE ? ";gap:4px" : "");
    this.pads = [];
    for (let r = 0; r < 8; r++) { pg.appendChild(el("div", "colh", "ABCDEFGH"[r])); for (let c = 0; c < 8; c++) { const p = r * 8 + c, b = el("button", "bigpad", patName(p)); b.onclick = () => this.tap(p); pg.appendChild(b); this.pads.push(b); } }
    this.src = -1;
    view.append(hdr, pg);
  },
  tap(p) {
    if (patMode === "clear") { act(`pclear ${p + 1} 0`); patMode = ""; }
    else if (patMode === "copy") { if (this.src < 0) this.src = p; else { if (this.src !== p) act(`pcopy ${this.src + 1} ${p + 1}`); patMode = ""; this.src = -1; } }
    else act("pat " + (p + 1));
    this.S();
  },
  S() { }, O() { },
  S0() {
    this.now.textContent = patName(st.pat) + (st.qpat >= 0 && st.qpat !== st.pat ? " → " + patName(st.qpat) : "");
    this.hint.textContent = patMode === "copy" ? (this.src < 0 ? "copy: tap the source" : "copy " + patName(this.src) + ": tap where it goes") : patMode === "clear" ? "clear: tap a pattern" : "";
    for (const m in this.ops) this.ops[m].classList.toggle("on", patMode === m);
    this.pads.forEach((b, p) => { b.className = "bigpad " + (p === st.pat ? "cur" : p === st.qpat ? "next" : (st.patmask >> BigInt(p) & 1n) ? "has" : "") + (p === this.src ? " src" : ""); });
  },
  O0() { this.S(); },
};

V.fx = {
  build() {
    view.className = "v"; view.style.gridTemplateRows = sz("1fr 1fr 1fr", "1.1fr 1.6fr 1fr", "0.8fr 1.2fr 1fr");
    this.fxp = FxPads("pg"); this.fxp.el.style.gridTemplateColumns = PHONE ? "repeat(4,1fr)" : "repeat(8,1fr)"; if (PHONE) this.fxp.el.style.gridTemplateRows = "1fr 1fr";
    const row = el("div", PHONE ? "row4" : "row8"); if (PHONE) row.style.gridTemplateRows = "1fr 1fr";
    this.gp = ["revsize", "revtone", "dlytime", "dlyfb", "fxret", "filter", "drive", "swing"].map(id => globPot(id, sz(150, 72, 46), true));
    this.gp.forEach(p => row.appendChild(p.el));
    const m = el("div", "cell"); m.style.cssText = "flex-direction:row;gap:" + (PHONE ? 16 : 40) + "px";
    const a = globPot("master", sz(130, 84, 50)), b = globPot("xfade", sz(130, 84, 50)); this.gp.push(a, b);
    this.ml = Meter(); this.mr = Meter(); const mm = el("div"); mm.style.cssText = "display:flex;gap:6px;height:70%"; mm.append(this.ml.el, this.mr.el);
    m.append(a.el, b.el, mm);
    view.append(this.fxp.el, row, m);
  },
  S() { this.fxp.S(); }, M() { globUpd(this.gp); }, H() { this.ml.set(st.meters[16]); this.mr.set(st.meters[17]); },
};

/* Samples: the T95's library and SD card (waveform per file, hear it here, send to the
   selected track), record with this device's microphone, and the kits on the Daisy */
let kitEdPg = -1, kitSel = -1, kitSure = "";   /* kits view: which 8 tracks, the kit picked in the list, the button waiting for a second press */
let smpMode = Q.get("sm") || "lib", smpPage = 0, smpEdit = null;   /* smpEdit: the file open in the trim view */
const smpPath = { lib: "", sd: "" }, peakCache = new Map();
let pvCtx = null, pvSrc = null, pvKey = "";
function pvStop() { try { if (pvSrc) pvSrc.stop(); } catch (e) { } pvSrc = null; pvKey = ""; }
async function pvPlay(src, path, done, extra) {
  const key = src + ":" + path + (extra || "");
  if (pvKey === key) { pvStop(); return done(); }
  pvStop(); pvKey = key;
  try {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (!pvCtx) pvCtx = new AC();
    await pvCtx.resume();
    const r = await fetch("/samples/preview?src=" + src + "&path=" + encodeURIComponent(path) + (extra || ""));
    if (!r.ok) throw new Error((await r.json()).error || "can't play it");
    const ab = await r.arrayBuffer();
    const buf = await new Promise((ok, no) => pvCtx.decodeAudioData(ab, ok, no));
    if (pvKey !== key) return;
    pvSrc = pvCtx.createBufferSource(); pvSrc.buffer = buf; pvSrc.connect(pvCtx.destination);
    pvSrc.onended = () => { if (pvKey === key) { pvKey = ""; pvSrc = null; } done(); };
    pvSrc.start();
  } catch (e) { pvKey = ""; done(String(e.message || e)); }
}
function drawPeaks(cv, pk, color) {
  const W = cv.width = Math.max(40, cv.clientWidth * 2), H = cv.height = Math.max(16, cv.clientHeight * 2), g = cv.getContext("2d");
  g.clearRect(0, 0, W, H);
  if (!pk) return;
  const bw = W / pk.length; g.fillStyle = color || "#2f9e4f";
  for (let i = 0; i < pk.length; i++) { const h = Math.max(1, Math.pow(pk[i] / 255, 0.6) * H * 0.48); g.fillRect(i * bw, H / 2 - h, Math.max(1, bw * 0.7), 2 * h); }
}
function smpSub() {
  const sub = el("div", "sub");
  [["lib", PHONE ? "lib" : "library"], ["sd", PHONE ? "SD" : "SD card"], ["loop", "looper"], ["rec", "mic"], ["kits", "kits"]].forEach(([m, n]) => { const b = el("span", smpMode === m ? "on" : "", n); b.onclick = () => { smpMode = m; smpPage = 0; smpEdit = null; show("samples"); }; sub.appendChild(b); });
  return sub;
}
V.samples = {
  build() {
    this.X = this.K = this.P = this.H = this.Z = this.T = this.J = this.slots = null; pvStop();
    this.S = this.S1; this.N = this.S1;
    if (smpMode === "loop") return this.buildLoop();
    if (smpEdit && smpEdit.src === smpMode) return this.buildEdit();
    if (smpMode === "rec") return this.buildRec();
    if (smpMode === "kits") return this.buildKits();
    view.className = "v"; view.style.gridTemplateRows = PHONE ? "auto auto 1fr auto" : "auto 1fr auto";
    const hdr = el("div", "hdr"), hdr2 = PHONE ? el("div", "hdr") : hdr;
    this.crumb = el("span", "lbl"); this.to = el("span", "big");
    hdr.append(smpSub()); hdr2.append(this.crumb, el("span", "sp"), el("span", "lbl", "sends to"), this.to);
    if (smpMode === "lib") {
      const up = el("button", "b act", "upload"); up.style.cssText = "padding:0 12px;height:32px"; const inp = el("input"); inp.type = "file"; inp.multiple = true; inp.accept = "audio/*,.wav,.aif,.aiff,.flac,.mp3,.ogg"; inp.style.display = "none";
      up.onclick = () => inp.click(); inp.onchange = () => this.upload([...inp.files]);
      hdr2.append(up, inp);
    }
    this.rowsN = sz(9, 7, 4);
    this.list = el("div", "list"); this.list.style.gridTemplateRows = `repeat(${this.rowsN},1fr)`;
    const foot = el("div", "hdr"); this.prev = el("button", "b", "◀"); this.next = el("button", "b", "▶"); this.pageEl = el("span", "lbl"); this.msg = el("span", "lbl msg");
    for (const b of [this.prev, this.next]) b.style.cssText = "width:54px;height:34px;font-size:15px";
    this.prev.onclick = () => { smpPage--; this.render(); }; this.next.onclick = () => { smpPage++; this.render(); };
    this.bar = el("div", "xbar"); this.barIn = el("i"); this.bar.appendChild(this.barIn);
    foot.append(this.prev, this.pageEl, this.next, this.msg, this.bar);
    if (PHONE) view.append(hdr, hdr2, this.list, foot); else view.append(hdr, this.list, foot);
    this.items = []; this.X = this.X1;
    this.load();
  },
  async load() {
    const src = smpMode;
    try {
      const r = await fetch("/samples?src=" + src + "&path=" + encodeURIComponent(smpPath[src])), d = await r.json();
      if (d.error) { if (smpPath[src]) { smpPath[src] = ""; return this.load(); } this.items = []; this.say(d.error); }
      else { this.items = (smpPath[src] ? [{ up: true }] : []).concat(d.dirs.map(n => ({ dir: n })), d.files.map(f => ({ file: f.name, size: f.size }))); }
    } catch (e) { this.items = []; this.say("T95 bridge not answering"); }
    if (curName === "samples" && smpMode === src) this.render();
  },
  say(t, bad) { if (this.msg) { this.msg.textContent = t || ""; this.msg.classList.toggle("bad", !!bad); } },
  full(name) { const p = smpPath[smpMode]; return p ? p + "/" + name : name; },
  render() {
    const src = smpMode, pages = Math.max(1, Math.ceil(this.items.length / this.rowsN));
    smpPage = Math.max(0, Math.min(pages - 1, smpPage));
    this.crumb.textContent = (src === "lib" ? "library" : "SD card") + (smpPath[src] ? " / " + smpPath[src] : "");
    this.pageEl.textContent = (smpPage + 1) + " / " + pages; this.prev.disabled = smpPage === 0; this.next.disabled = smpPage >= pages - 1;
    this.list.textContent = "";
    const want = [];
    for (const it of this.items.slice(smpPage * this.rowsN, (smpPage + 1) * this.rowsN)) {
      const row = el("div", "li"), nm = el("span", "nm2"), acts = el("div", "act");
      const mk = (cls, t, f) => { const b = el("button", "b " + cls, t); b.onclick = e => { e.stopPropagation(); f(b); }; acts.appendChild(b); return b; };
      if (it.up || it.dir) {
        nm.textContent = it.up ? "‹ back" : "📁 " + it.dir; row.classList.add("dir");
        row.onclick = () => { const p = smpPath[src]; smpPath[src] = it.up ? p.split("/").slice(0, -1).join("/") : (p ? p + "/" : "") + it.dir; smpPage = 0; this.load(); };
        row.append(nm, el("span"), el("span", "d"), acts);
        if (it.dir && src === "sd") mk("", "import all", () => this.imp(this.full(it.dir)));
      } else {
        const path = this.full(it.file), cv = el("canvas"), d = el("span", "d");
        nm.textContent = it.file;
        row.append(nm, cv, d, acts);
        row.classList.add("file"); row.title = "tap: trim and normalise before sending";
        row.onclick = () => { smpEdit = { src, path, name: it.file, start: 0, end: 1, norm: false }; show("samples"); };
        const play = mk("act", "▶", b => { b.classList.add("on"); pvPlay(src, path, err => { b.classList.remove("on"); if (err) this.say(err, true); }); });
        play.title = "hear it on this device";
        mk("", "send", () => this.send(src, path));
        if (src === "sd") mk("", "import", () => this.imp(path));
        else mk("", "✕", b => { if (b.dataset.sure) this.del(path); else { b.dataset.sure = 1; b.textContent = "sure?"; b.classList.add("warn"); } });
        want.push({ key: src + ":" + path, src, path, cv, d });
      }
      this.list.appendChild(row);
    }
    this.S(); this.peaks(want);
  },
  async peaks(want) {
    const token = this.token = {};
    for (const w of want) {
      let p = peakCache.get(w.key);
      if (!p) {
        try { p = await (await fetch("/samples/peaks?src=" + w.src + "&path=" + encodeURIComponent(w.path))).json(); } catch (e) { p = { error: "?" }; }
        if (!p.error) { p.pk = p.peaks ? p.peaks.match(/../g).map(h => parseInt(h, 16)) : null; peakCache.set(w.key, p); }
      }
      if (this.token !== token) return;          /* another page is showing now */
      w.d.textContent = p.error ? "" : p.secs < 10 ? p.secs.toFixed(2) + " s" : p.secs.toFixed(0) + " s";
      drawPeaks(w.cv, p.pk);
    }
  },
  async post(path, body) { try { const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) }); return await r.json(); } catch (e) { return { error: "T95 bridge not answering" }; } },
  async send(src, path) { const r = await this.post("/samples/send", { src, path, track: st.sel + 1 }); this.say(r.error || "queued for track " + (st.sel + 1), !!r.error); },
  async imp(path) { this.say("copying to the library…"); const r = await this.post("/samples/import", { path }); this.say(r.error || "in the library: " + (r.copied !== undefined ? r.copied + " files" : "done"), !!r.error); },
  async del(path) { const r = await this.post("/samples/delete", { path }); this.say(r.error || "deleted", !!r.error); this.load(); },
  async upload(files) {
    let n = 0;
    for (const f of files) {
      this.say("uploading " + f.name + "…");
      try { const r = await (await fetch("/samples/upload?name=" + encodeURIComponent(f.name), { method: "POST", body: f })).json(); if (r.error) { this.say(f.name + ": " + r.error, true); continue; } n++; } catch (e) { this.say("upload failed", true); }
    }
    if (n) { this.say(n + " uploaded to library / uploads"); smpPath.lib = "uploads"; smpPage = 0; this.load(); }
  },
  X1() {
    const x = st.xfer; if (!x || !this.msg) return;
    const pct = x.chunks ? Math.round(x.acked / x.chunks * 100) : 0;
    this.barIn.style.width = (x.state === "done" ? 100 : pct) + "%"; this.bar.classList.toggle("on", x.state === "sending" || x.state === "converting" || x.state === "loading");
    this.say(x.state === "failed" ? x.name + ": " + (x.error || "failed") : x.state === "done" ? x.name + " is in track " + x.track + (x.cut ? " (cut to fit)" : "") + (x.kb_per_s ? " · " + x.kb_per_s + " kB/s" : "")
      : x.name + " → track " + x.track + ": " + x.state + (x.state === "sending" ? " " + pct + "%" : ""), x.state === "failed");
  },
  S1() { if (this.to) { const t = (st.sel + 1) + " · " + trackName(st.sel); if (this.to.textContent !== t) this.to.textContent = t; } },

  /* one file: cut the start and end, make it as loud as it goes, hear it, send it */
  buildEdit() {
    const e = smpEdit;
    view.className = "v"; view.style.gridTemplateRows = PHONE ? "auto auto 1fr auto auto" : "auto 1fr auto";
    const hdr = el("div", "hdr"), hdr2 = PHONE ? el("div", "hdr") : hdr; this.to = el("span", "big"); this.msg = el("span", "lbl msg");
    const back = el("button", "b", "‹ back"); back.style.cssText = "padding:0 12px;height:32px"; back.onclick = () => { smpEdit = null; show("samples"); };
    const nm = el("span", "big", e.name); nm.style.cssText = "white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0";
    hdr.append(back, nm); hdr2.append(el("span", "sp"), el("span", "lbl", "sends to"), this.to);
    const w = el("div", "wave trim"), cv = el("canvas"), tag = el("span", "tag"); tag.style.left = "10px"; w.append(cv, tag);
    const foot = el("div", "hdr"), foot2 = PHONE ? el("div", "hdr") : foot;
    const norm = Btn("act", "NORMALISE", () => { e.norm = !e.norm; norm.set(e.norm); pvStop(); });
    const play = el("button", "b act", "▶ HEAR"), send = el("button", "b a", "SEND"), reset = el("button", "b", "WHOLE");
    for (const b of [norm.el, play, send, reset]) b.style.cssText = "padding:0 14px;height:" + sz(40, 36, 28) + "px";
    foot.append(reset, norm.el); foot2.append(this.msg, el("span", "sp"), play, send);
    if (PHONE) view.append(hdr, hdr2, w, foot, foot2); else view.append(hdr, w, foot);
    let pk = null, secs = 0;
    const draw = () => {
      const W = cv.width = Math.max(50, cv.clientWidth * 2), H = cv.height = Math.max(20, cv.clientHeight * 2), g = cv.getContext("2d");
      g.clearRect(0, 0, W, H);
      if (pk) { const bw = W / pk.length; for (let i = 0; i < pk.length; i++) { const x = (i + 0.5) / pk.length, h = Math.max(1, Math.pow(pk[i] / 255, 0.6) * H * 0.46);
        g.fillStyle = x >= e.start && x <= e.end ? "#35d05a" : "#26402d"; g.fillRect(i * bw, H / 2 - h, Math.max(1, bw * 0.7), 2 * h); } }
      g.fillStyle = "#000a"; g.fillRect(0, 0, e.start * W, H); g.fillRect(e.end * W, 0, W - e.end * W, H);
      g.fillStyle = "#ffb833"; for (const x of [e.start, e.end]) { g.fillRect(Math.min(W - 4, Math.max(0, x * W - 2)), 0, 4, H); g.fillRect(Math.min(W - 28, Math.max(0, x * W - 14)), 0, 28, 28); g.fillRect(Math.min(W - 28, Math.max(0, x * W - 14)), H - 28, 28, 28); }
      const f = v => (v * secs).toFixed(secs < 10 ? 2 : 1);
      tag.textContent = secs ? f(e.start) + " – " + f(e.end) + " s · keeps " + f(e.end - e.start) + " of " + f(1) + " s" : "";
    };
    let which = null;
    const at = ev => { const r = cv.getBoundingClientRect(); return Math.max(0, Math.min(1, (ev.clientX - r.left) / r.width)); };
    w.style.touchAction = "none";
    w.addEventListener("pointerdown", ev => { const x = at(ev); which = Math.abs(x - e.start) <= Math.abs(x - e.end) ? "start" : "end"; try { w.setPointerCapture(ev.pointerId); } catch (x2) { } move(ev); ev.preventDefault(); });
    const move = ev => { if (!which) return; const x = at(ev); if (which === "start") e.start = Math.min(x, e.end - 0.01); else e.end = Math.max(x, e.start + 0.01); e.start = Math.max(0, e.start); e.end = Math.min(1, e.end); pvStop(); play.classList.remove("on"); draw(); };
    w.addEventListener("pointermove", move);
    const up = () => { which = null; }; w.addEventListener("pointerup", up); w.addEventListener("pointercancel", up);
    reset.onclick = () => { e.start = 0; e.end = 1; pvStop(); draw(); };
    const q = () => "&start=" + e.start.toFixed(4) + "&end=" + e.end.toFixed(4) + "&norm=" + (e.norm ? 1 : 0);
    play.onclick = () => { play.classList.add("on"); pvPlay(e.src, e.path, err => { play.classList.remove("on"); if (err) this.say(err, true); }, q()); };
    send.onclick = async () => { const r = await this.post("/samples/send", { src: e.src, path: e.path, track: st.sel + 1, start: +e.start.toFixed(4), end: +e.end.toFixed(4), norm: e.norm }); this.say(r.error || "queued for track " + (st.sel + 1), !!r.error); };
    norm.set(e.norm);
    this.X = this.X2; this.say("drag the markers");
    (async () => {
      const key = e.src + ":" + e.path; let p = peakCache.get(key);
      if (!p) { try { p = await (await fetch("/samples/peaks?src=" + e.src + "&path=" + encodeURIComponent(e.path))).json(); } catch (x) { p = { error: "T95 bridge not answering" }; }
        if (!p.error) { p.pk = p.peaks ? p.peaks.match(/../g).map(h => parseInt(h, 16)) : null; peakCache.set(key, p); } }
      if (smpEdit !== e || curName !== "samples") return;
      if (p.error) return this.say(p.error, true);
      pk = p.pk; secs = p.secs; draw();
    })();
    requestAnimationFrame(draw);
  },

  /* the Daisy's own input: loops, overdubs and one-shot samples into the selected track */
  buildLoop() {
    view.className = "v"; view.style.gridTemplateRows = sz("auto 1fr 150px 120px", "auto auto 1fr auto 96px 92px", "auto 1fr 62px 46px");
    const hdr = el("div", "hdr"), hdr2 = PHONE ? el("div", "hdr") : hdr; this.to = el("span", "big"); this.msg = el("span", "lbl msg");
    hdr.append(smpSub()); hdr2.append(this.msg, el("span", "sp"), el("span", "lbl", PHONE ? "into" : "records into"), this.to);
    this.wave = Wave();
    const cell = (label, inner) => { const c = el("div", "cell"); c.append(inner, el("span", "n", label)); return c; };
    const set = el("div", "lset");
    const inp = el("div", "cell"); inp.style.cssText = "flex-direction:row;gap:" + sz(18, 10, 8) + "px";
    this.m = Meter(sz(12, 10, 6)); this.m.el.style.height = "72%";
    this.gain = Pot({ size: sz(84, 54, 38), mode: "vol", color: AM, label: "input", fmt: v => v === 64 ? "x1" : "x" + ((v * 2 / 127) ** 2).toFixed(1), reset: 64, onChange: v => lpAct("gain", v, "ingain " + v) });
    inp.append(this.m.el, this.gain.el);
    this.mon = Btn("act", SHORT ? "MON" : "MONITOR", () => act("mon 2")); this.cnt = Btn("m", SHORT ? "COUNT" : "COUNT-IN", () => lpAct("count", st.lp.count ? 0 : 1, "lrec count " + (st.lp.count ? 0 : 1)));
    const tg = el("div", "cell"); tg.style.cssText = "padding:0 " + sz(14, 6, 6) + "px;gap:" + sz(8, 5, 3) + "px"; for (const b of [this.mon.el, this.cnt.el]) { b.style.cssText = "width:100%;justify-content:center;height:" + sz(40, 30, 22) + "px"; tg.appendChild(b); }
    this.len = Seg(LEN_OPTS, () => st.lp.len, v => lpAct("len", v, "lrec len " + v));
    this.qr = Seg([[0, "bar"], [1, "beat"]], () => st.lp.qrec, v => lpAct("qrec", v, "quant rec " + v));
    set.append(inp, tg, cell(PHONE || SHORT ? "bars" : "take length (bars)", this.len.el), cell("starts on the", this.qr.el));
    const pg = el("div", "pg"); pg.style.gridTemplateColumns = PHONE ? "repeat(3,1fr)" : "repeat(6,1fr)";
    const pad = (n, sm, f) => { const b = el("button", "bigpad", n); if (sm && !SHORT) b.appendChild(el("small", "", sm)); b.onclick = f; pg.appendChild(b); return b; };
    const T = () => st.sel + 1;
    this.pads = {
      rec: pad("REC LOOP", "in time, loops by the bar", () => act(st.lrec ? "lrec stop" : "lrec arm " + T())),
      over: pad("OVERDUB", "on top of the loop", () => act(st.lrec ? "lrec stop" : "lrec over " + T())),
      shot: pad("SAMPLE", "one-shot, starts with the sound", () => act(st.lrec ? "lrec stop" : "lrec shot " + T())),
      stop: pad("STOP", "", () => act("lrec stop")),
      undo: pad("UNDO TAKE", "", () => act(st.lrec ? "lrec cancel" : st.lp.undo !== T() && st.lp.redo === T() ? "lrec redo" : "lrec undo")),
      clear: pad("CLEAR", "back to the kit sound", () => act(`type ${T()} sample`)),
    };
    if (PHONE) view.append(hdr, hdr2, this.wave.el, set, pg); else view.append(hdr, this.wave.el, set, pg);
    if (PHONE) { set.style.gridTemplateColumns = "1fr 1fr"; view.style.gridTemplateRows = "auto auto 1fr 150px 124px"; }
    this.S = this.SL; this.N = this.SL; this.P = this.SL; this.Z = this.ZL; this.T = () => this.ZL();
    this.H = () => this.m.set(Math.min(255, st.inPeak * 2.55));
  },
  SL() {
    this.S1();
    const t = st.sel, ty = typeOf(t), rec = st.lrec && st.lrecT === t, m = st.lp.mode;
    const txt = lrecText(t) || (st.lrec ? "busy on track " + (st.lrecT + 1) : ty === "loop" ? "loop on this track: OVERDUB adds to it, REC LOOP replaces it" : ty === "303" ? "a 303 track: recording makes it a sample track" : "");
    this.msg.textContent = txt; this.msg.classList.toggle("bad", st.lrec === 2 && rec);
    this.mon.set(st.mon); this.cnt.set(st.lp.count); this.len.upd(); this.qr.upd();
    if (!this.gain.dragging && !isBusy("lp.gain")) this.gain.set(st.lp.gain);
    const P = this.pads, cls = (b, on, blink, dis) => { b.classList.toggle("warn", !!on); b.classList.toggle("blink", !!blink); b.disabled = !!dis; };
    cls(P.rec, rec && m === 0, rec && m === 0 && st.lrec === 1, st.lrec && !(rec && m === 0));
    cls(P.over, rec && m === 1, rec && m === 1 && st.lrec === 1, (st.lrec && !(rec && m === 1)) || (!st.lrec && ty !== "loop"));
    cls(P.shot, rec && m === 2, rec && m === 2 && st.lrec === 1, st.lrec && !(rec && m === 2));
    cls(P.stop, false, false, !st.lrec);
    cls(P.undo, false, false, !st.lrec && st.lp.undo !== t + 1 && st.lp.redo !== t + 1);
    P.undo.firstChild.textContent = st.lrec ? "CANCEL" : st.lp.undo !== t + 1 && st.lp.redo === t + 1 ? "REDO TAKE" : "UNDO TAKE";
    this.m.set(Math.min(255, st.inPeak * 2.55));
    this.ZL();
  },
  ZL() {
    const t = st.sel, w = st.tr[t].wave, ty = typeOf(t);
    if (ty === "303") return this.wave.draw(null, [], GR, "303");
    const marks = [];
    if (ty === "loop") { const bars = +(st.tr[t].name.match(/\d+/) || [1])[0]; for (let b = 1; b < bars; b++) marks.push([b / bars, "#2a2d31"]);
      this.wave.draw(w, marks, st.lrec === 2 && st.lrecT === t ? "#ff4b3e" : GR, w && w.len ? bars + " bar" + (bars > 1 ? "s" : "") + " · " + (w.len / w.rate).toFixed(1) + " s" : ""); }
    else this.wave.draw(w, marks, GR, w && w.len ? st.tr[t].name.replace(/[\[\]]/g, "") + " · " + (w.len / w.rate).toFixed(2) + " s" : "");
  },

  /* record with this device's microphone, into the library, and on to the selected track */
  buildRec() {
    view.className = "v"; view.style.gridTemplateRows = sz("auto 1fr 190px", "auto auto 1fr 150px", "auto 1fr 84px");
    const hdr = el("div", "hdr"), hdr2 = PHONE ? el("div", "hdr") : hdr; this.to = el("span", "big"); this.msg = el("span", "lbl msg");
    hdr.append(smpSub()); hdr2.append(this.msg, el("span", "sp"), el("span", "lbl", "sends to"), this.to);
    const w = el("div", "wave"), cv = el("canvas"); w.appendChild(cv); this.cv = cv;
    const row = el("div"); row.style.cssText = "display:grid;grid-template-columns:" + sz("220px 1fr", "130px 1fr", "110px 1fr") + ";gap:" + sz(12, 8, 6) + "px;min-height:0";
    const c1 = el("div", "cell"), rb = el("button", "recbtn", "REC"); c1.appendChild(rb); this.rb = rb;
    const c2 = el("div", "cell"); c2.style.cssText = "align-items:flex-start;padding:0 " + sz(24, 12, 12) + "px;gap:" + sz(10, 6, 4) + "px";
    this.auto = true;
    const tg = el("button", "b act on", "send to the selected track when done"); tg.style.cssText = "padding:0 14px;height:" + sz(36, 32, 26) + "px";
    tg.onclick = () => { this.auto = !this.auto; tg.classList.toggle("on", this.auto); };
    const info = el("span", "hint"); this.info = info; c2.append(tg, info);
    row.append(c1, c2);
    if (PHONE) view.append(hdr, hdr2, w, row); else view.append(hdr, w, row);
    this.pk = []; this.rec = null;
    const local = ["localhost", "127.0.0.1"].includes(location.hostname);
    if (!navigator.mediaDevices || !window.MediaRecorder || (!window.isSecureContext && !local)) {
      const port = st.caps.https_port;
      info.textContent = port ? "The microphone only works on the secure address. The browser warns once about the certificate: accept it." : "This browser can't record here.";
      rb.textContent = port ? "OPEN" : "–"; rb.classList.add("idle");
      rb.onclick = () => { if (port) location.href = "https://" + location.hostname + ":" + port + "/apcbox?v=samples&sm=rec"; };
      return;
    }
    info.textContent = "Records this device's microphone into the library (uploads). For the Daisy's own input, use ARM on a track.";
    rb.classList.add("idle");
    rb.onclick = () => (this.rec ? this.recStop() : this.recStart());
  },
  async recStart() {
    try {
      const stream = await navigator.mediaDevices.getUserMedia({ audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false } });
      const AC = window.AudioContext || window.webkitAudioContext, ctx = new AC(), an = ctx.createAnalyser(); an.fftSize = 2048;
      ctx.createMediaStreamSource(stream).connect(an);
      const rec = new MediaRecorder(stream), chunks = [], buf = new Float32Array(an.fftSize), t0 = performance.now();
      rec.ondataavailable = e => { if (e.data.size) chunks.push(e.data); };
      this.pk = []; this.rec = { rec, stream, ctx, chunks, t0 };
      const tick = () => {
        if (!this.rec || this.rec.rec !== rec) return;
        an.getFloatTimeDomainData(buf); let p = 0; for (const v of buf) p = Math.max(p, Math.abs(v));
        this.pk.push(Math.min(255, Math.round(p * 255))); if (this.pk.length > 600) this.pk = this.pk.filter((_, i) => i % 2 === 0);
        drawPeaks(this.cv, this.pk, "#ff4b3e");
        this.say("● recording " + ((performance.now() - t0) / 1000).toFixed(1) + " s");
        setTimeout(tick, 50);
      };
      rec.onstop = () => this.recDone(rec, chunks);
      rec.start(250); this.rb.textContent = "STOP"; this.rb.classList.remove("idle"); tick();
    } catch (e) { this.say("no microphone: " + (e.message || e), true); }
  },
  recStop() { const r = this.rec; if (!r) return; this.rec = null; try { r.rec.stop(); } catch (e) { } r.stream.getTracks().forEach(t => t.stop()); try { r.ctx.close(); } catch (e) { } this.rb.textContent = "REC"; this.rb.classList.add("idle"); },
  async recDone(rec, chunks) {
    const blob = new Blob(chunks, { type: rec.mimeType }), ext = /mp4|aac|m4a/.test(rec.mimeType) ? "mp4" : /ogg/.test(rec.mimeType) ? "ogg" : "webm";
    const d = new Date(), z = n => String(n).padStart(2, "0"), name = `rec ${z(d.getDate())}-${z(d.getMonth() + 1)} ${z(d.getHours())}.${z(d.getMinutes())}.${z(d.getSeconds())}.${ext}`;
    this.say("saving " + name + "…");
    try {
      const r = await (await fetch("/samples/upload?name=" + encodeURIComponent(name), { method: "POST", body: blob })).json();
      if (r.error) return this.say(r.error, true);
      this.say(name + " is in library / uploads");
      if (this.auto) { const q = await this.post("/samples/send", { src: "lib", path: "uploads/" + name, track: st.sel + 1 }); this.say(q.error || name + ": on its way to track " + (st.sel + 1), !!q.error); this.X = this.X2; }
    } catch (e) { this.say("couldn't save the recording", true); }
  },
  X2() { const x = st.xfer; if (x) this.say(x.state === "failed" ? x.name + ": " + (x.error || "failed") : x.state === "done" ? x.name + " is in track " + x.track : x.name + " → track " + x.track + ": " + x.state, x.state === "failed"); },

  /* kits on the Daisy (built in, and folders on its card): load one into tracks 1-8 or 9-16 */
  /* kits: the 8 tracks of a page as slots, save them as a kit, load / rename / delete kits */
  buildKits() {
    view.className = "v"; view.style.gridTemplateRows = sz("auto 150px auto 1fr auto", "auto 124px auto auto 1fr auto", "auto 58px auto 1fr");
    const hdr = el("div", "hdr"); this.to = null; this.msg = el("span", "lbl msg");
    if (kitEdPg < 0) kitEdPg = st.sel >> 3;
    this.pgSeg = Seg([[0, PHONE ? "1-8" : "tracks 1-8"], [1, PHONE ? "9-16" : "tracks 9-16"]], () => kitEdPg, v => { kitEdPg = v; kitSure = ""; this.KS(); });
    hdr.append(smpSub(), this.pgSeg.el, this.msg, el("span", "sp"));
    const slots = el("div", "kslots"); this.slots = [];
    for (let c = 0; c < 8; c++) {
      const d = el("div", "kslot"), n = el("span", "n"), w = Wave(); d.append(n, w.el);
      d.onclick = () => { const t = kitEdPg * 8 + c; act(`track ${t + 1}`); act(`audition ${t + 1}`); };
      slots.appendChild(d); this.slots.push({ d, n, w });
    }
    this.name = el("input"); this.name.type = "text"; this.name.maxLength = 16; this.name.placeholder = "kit name (a-z, 0-9)"; this.name.autocapitalize = "off"; this.name.autocomplete = "off"; this.name.spellcheck = false;
    this.name.addEventListener("blur", () => setTimeout(refit, 300)); this.name.addEventListener("input", () => { kitSure = ""; this.KS(); });
    const nmrow = el("div", "hdr"); nmrow.append(el("span", "lbl", SHORT ? "" : "name"), this.name);
    const ops = el("div", "pg"); ops.style.cssText = `grid-template-columns:repeat(4,1fr);grid-auto-rows:${sz(52, 42, 30)}px`;
    this.btn = {};
    [["save", "SAVE AS KIT"], ["name", "RENAME"], ["delete", "DELETE"], ["undo", "UNDO KIT"]].forEach(([k, n]) => { const b = el("button", "bigpad", n); b.onclick = () => this.kitOp(k); ops.appendChild(b); this.btn[k] = b; });
    this.rowsN = sz(8, 4, 3);
    this.list = el("div", "list"); this.list.style.gridTemplateRows = `repeat(${this.rowsN},1fr)`;
    const foot = el("div", "hdr"); this.prev = el("button", "b", "◀"); this.next = el("button", "b", "▶"); this.pageEl = el("span", "lbl");
    for (const b of [this.prev, this.next]) b.style.cssText = SHORT ? "width:38px;height:26px" : "width:54px;height:34px;font-size:15px";
    this.prev.onclick = () => { smpPage--; this.K(); }; this.next.onclick = () => { smpPage++; this.K(); };
    foot.append(this.prev, this.pageEl, this.next, el("span", "sp"));
    if (SHORT) { nmrow.append(ops, foot); ops.style.flex = "1"; foot.style.flex = "0 0 auto"; view.append(hdr, slots, nmrow, this.list); }
    else if (PHONE) view.append(hdr, slots, nmrow, ops, this.list, foot);
    else { nmrow.appendChild(ops); ops.style.flex = "1"; view.append(hdr, slots, nmrow, this.list, foot); }
    this.K = this.K1; this.S = this.KS; this.N = this.KS; this.Z = this.KS; this.T = this.KS; this.P = this.KS; this.J = this.KS;
    this.KS();
  },
  kitName() { return this.name.value.trim().toLowerCase().replace(/[^a-z0-9 -]/g, " ").replace(/ +/g, " ").trim(); },
  kitFind(n) { return st.kits.findIndex(k => (k || "").toLowerCase() === n); },
  kitOp(k) {
    const n = this.kitName(), nb = st.kitNb, sure = kitSure === k; kitSure = "";
    if (nb == null) return this.say("the Daisy's firmware is too old for this (needs v13.14+)", true);
    if (k === "undo") { act(st.lp.undo >= 101 ? "lrec undo" : "lrec redo"); return; }
    if (k === "save") {
      if (!n) return this.say("type a name for the kit first", true);
      const ex = this.kitFind(n);
      if (ex >= 0 && ex < nb) return this.say(st.kits[ex] + " is built in: pick another name", true);
      if (ex >= 0 && !sure) { kitSure = k; this.KS(); return this.say("replace the kit " + st.kits[ex] + "? press again"); }
      act(`kit ${ex >= 0 ? "over" : "save"} ${kitEdPg} ${n}`); this.say("saving tracks " + (kitEdPg ? "9-16" : "1-8") + " as " + n + " ...");
    } else {
      if (kitSel < nb || kitSel >= st.kits.length) return this.say(kitSel >= 0 && kitSel < nb ? "built-in kits can't be changed" : "tap a kit in the list first", true);
      if (k === "delete") {
        if (!sure) { kitSure = k; this.KS(); return this.say("delete the kit " + st.kits[kitSel] + " from the card? press again"); }
        act("kit delete " + (kitSel + 1)); kitSel = -1;
      } else {
        if (!n || n === (st.kits[kitSel] || "").toLowerCase()) return this.say("type the new name first", true);
        if (this.kitFind(n) >= 0) return this.say("there is a kit called " + n + " already", true);
        act(`kit rename ${kitSel + 1} ${n}`); kitSel = -1;
      }
    }
    this.KS();
  },
  KS() {
    if (!this.slots) return;
    this.pgSeg.upd();
    for (let c = 0; c < 8; c++) {
      const t = kitEdPg * 8 + c, o = this.slots[c], ty = typeOf(t), nm = (t + 1) + " " + (ty === "303" ? "303" : ty === "loop" ? "loop (not in kits)" : trackName(t));
      if (o.n.textContent !== nm) o.n.textContent = nm;
      o.d.classList.toggle("on", st.sel === t); o.d.classList.toggle("dim", ty === "loop");
      o.w.draw(ty === "303" ? null : st.tr[t].wave, [], ty === "loop" ? "#5a5f66" : GR, "");
    }
    const n = this.kitName(), ex = n ? this.kitFind(n) : -1, B = this.btn, nb = st.kitNb == null ? 0 : st.kitNb, sd = kitSel >= nb && kitSel < st.kits.length;
    B.save.firstChild.textContent = kitSure === "save" ? "SURE?" : ex >= nb ? "SAVE OVER" : "SAVE AS KIT"; B.save.classList.toggle("warn", kitSure === "save");
    B.delete.firstChild.textContent = kitSure === "delete" ? "SURE?" : "DELETE"; B.delete.classList.toggle("warn", kitSure === "delete");
    B.save.disabled = !!st.setOp; B.name.disabled = !sd || !!st.setOp; B.delete.disabled = !sd || !!st.setOp;
    const u = st.lp.undo >= 101, r = st.lp.redo >= 101; B.undo.disabled = !u && !r; B.undo.firstChild.textContent = r && !u ? "REDO KIT" : "UNDO KIT";
    if (st.setOp === 3) this.say("saving the kit ...");
    else if (this.msg.textContent.startsWith("saving")) this.say("");
  },
  K1() {
    const pages = Math.max(1, Math.ceil(st.kits.length / this.rowsN)), nb = st.kitNb == null ? 0 : st.kitNb;
    smpPage = Math.max(0, Math.min(pages - 1, smpPage));
    this.pageEl.textContent = (smpPage + 1) + " / " + pages; this.prev.disabled = smpPage === 0; this.next.disabled = smpPage >= pages - 1;
    this.list.textContent = "";
    for (let k = smpPage * this.rowsN; k < Math.min(st.kits.length, (smpPage + 1) * this.rowsN); k++) {
      const row = el("div", "li kit file" + (kitSel === k ? " sel" : "")), nm = el("span", "nm2", st.kits[k] || "?"), acts = el("div", "act");
      for (const pg of [0, 1]) { const b = el("button", "b act" + (st.kitPg[pg] === k ? " on" : ""), pg ? "9-16" : "1-8"); b.onclick = e => { e.stopPropagation(); act(`kit ${k + 1} ${pg}`); }; acts.appendChild(b); }
      row.onclick = () => { kitSel = kitSel === k ? -1 : k; kitSure = ""; if (kitSel >= nb) this.name.value = (st.kits[k] || "").toLowerCase().slice(0, 16); this.K(); this.KS(); };
      row.append(nm, el("span"), el("span", "d", (k < nb ? "built in" : "") + (st.kitPg[0] === k && st.kitPg[1] === k ? " · on all 16" : st.kitPg[0] === k ? " · on 1-8" : st.kitPg[1] === k ? " · on 9-16" : "")), acts);
      this.list.appendChild(row);
    }
    this.KS();
  },
};


/* ---- APC40 map: every control, what it does, and with SHIFT. Read-only for now.
   The texts follow daisy/box.cpp (OnButton, GridButton, TrackButton, OnCC); when
   the mapping becomes editable the firmware will own this table. ---- */
function apcMap() {
  const C = [], add = (o) => { C.push(o); return o; };
  const gx = c => 30 + c * 62, gy = r => 44 + r * 48;
  const rows = [
    { k: "step", fn: c => "step " + (c + 1), sh: "accent", t: "Steps 1-8 of the selected track", d: "Tap: step on / off. SHIFT + tap: accent. On a 303 track: hold a step and turn DEVICE knob 1 for its note, SHIFT + tap for slide." },
    { k: "step", fn: c => "step " + (c + 9), sh: "accent", t: "Steps 9-16 of the selected track", d: "Tap: step on / off. SHIFT + tap: accent. On a 303 track: hold a step and turn DEVICE knob 1 for its note, SHIFT + tap for slide." },
    { k: "pat", fn: c => "pattern " + (c + 1), sh: c => "bank " + "ABCDEFGH"[c], t: "Patterns 1-8 of the bank", d: "Tap: switch to that pattern (on the bar, beat or step: System > quantise). SHIFT + tap: pick bank A-H. While CLIP/TRACK is held: tap a source, then a target to copy; double tap clears. With the sets screen open: picks a set slot." },
    { k: "fx", fn: c => FXN[c], sh: "latch", t: "Performance FX", d: "Hold: the effect runs while held (start and end snap to the step or beat: System > quantise). SHIFT + tap: latch on / off. With the sets screen open, pads 1-5: load, save here, save new, delete, transition." },
    { k: "trig", fn: c => "play " + (c + 1), sh: "", t: "Play the tracks by hand", d: "Tap: plays that strip's track (and records it into the pattern while record is on). With a 303 track selected: a one-octave keyboard." },
  ];
  rows.forEach((R, r) => { for (let c = 0; c < 8; c++) add({ x: gx(c), y: gy(r), w: 54, h: 40, cls: "pad " + R.k, hw: "CLIP LAUNCH row " + (r + 1) + ", pad " + (c + 1), fn: R.fn(c), sh: typeof R.sh === "function" ? R.sh(c) : R.sh, t: R.t, d: R.d }); });
  for (let r = 0; r < 5; r++) add({ x: gx(8) + 6, y: gy(r), w: 54, h: 40, cls: "pad kit", hw: "SCENE LAUNCH " + (r + 1), fn: "kit " + (r + 1), sh: "", t: "Load a kit", d: "Loads kit " + (r + 1) + " of the current kit page into the 8 tracks on the strips. BANK SELECT up / down turns the kit page (5 kits each). A track holding a recording, and a 303 track, keep what they have. Undo: the ← under the device knobs." });
  const strip = (y, h, hw, fn, sh, t, d, cls) => { for (let c = 0; c < 8; c++) add({ x: gx(c), y, w: 54, h, cls: "btn " + (cls || ""), hw: hw + " " + (c + 1), fn: fn, sh, t, d }); };
  strip(290, 30, "CLIP STOP", "hear", "clear", "Hear the track", "Tap: plays the track's sound once. SHIFT + tap: clears that track in the current pattern (undo: SHIFT + CLIP/TRACK).");
  add({ x: gx(8) + 6, y: 290, w: 54, h: 30, cls: "btn warn", hw: "STOP ALL CLIPS", fn: "panic", sh: "reset 1 s", t: "All FX off, all sound off", d: "Tap: every held or latched FX off and all sound stops. SHIFT + hold for 1 second: full reset (patterns, kits, settings)." });
  strip(340, 34, "TRACK SELECTION", "select", "", "Select the track", "Tap: selects the track for the steps, the device knobs and the keyboard. While CLIP/TRACK is held: picks pattern bank A-H.");
  add({ x: gx(8) + 6, y: 340, w: 54, h: 34, cls: "btn", hw: "MASTER", fn: "record", sh: "", t: "Record on / off", d: "Tap: record mode. While it is on, what you play on the bottom pad row goes into the pattern." });
  strip(392, 24, "ACTIVATOR", "mute", "", "Mute", "Tap: mute on / off (at once, or on the beat or bar: System > quantise).", "g");
  strip(422, 24, "SOLO / CUE", "solo", "A / B", "Solo", "Tap: solo on / off. SHIFT + tap: which side of the crossfader the track is on: both, A (left), B (right).");
  strip(452, 24, "RECORD ARM", "roll", "", "Roll", "Hold: the track repeats (roll) for as long as it is held.");
  for (let c = 0; c < 8; c++) add({ x: gx(c) + 14, y: 492, w: 26, h: 236, cls: "fader", hw: "Fader " + (c + 1), fn: "level", sh: "", t: "Track level", d: "The track's level. After switching between tracks 1-8 and 9-16 a fader has to pass the stored level before it takes over (the screen says go up / go down)." });
  add({ x: gx(8) + 20, y: 492, w: 26, h: 236, cls: "fader", hw: "MASTER fader", fn: "master", sh: "", t: "Master level", d: "The level of everything." });
  add({ x: gx(8) + 8, y: 392, w: 50, h: 50, cls: "knob", hw: "CUE LEVEL", fn: "filter", sh: "", t: "Master filter", d: "Endless knob: low-pass to the left, high-pass to the right, off in the middle (it stops there on the way through)." });
  const kx = i => 650 + (i % 4) * 84, pages = "PAN: pan of tracks 1-8 · SEND A: reverb send · SEND B: delay send · SEND C: global (1 tempo, 2 swing, 3 reverb size, 4 reverb tone, 5 delay time, 6 delay feedback, 7 drive, 8 FX return)";
  const glob = ["tempo", "swing", "rev size", "rev tone", "dly time", "dly fb", "drive", "fx ret"];
  for (let i = 0; i < 8; i++) add({ x: kx(i), y: 44 + (i >> 2) * 66, w: 52, h: 52, cls: "knob", hw: "TRACK CONTROL knob " + (i + 1), fn: "pan/send", sh: glob[i], shl: "SEND C", t: "Per-track pan and sends, or the global knobs", d: "What the 8 knobs do depends on the button below them. " + pages + "." });
  [["PAN", "pan"], ["SEND A", "reverb"], ["SEND B", "delay"], ["SEND C", "global"]].forEach(([hw, fn], i) => add({ x: kx(i) - 8, y: 180, w: 68, h: 26, cls: "btn g", hw, fn, sh: "", t: "What the TRACK CONTROL knobs do", d: pages + "." }));
  add({ x: 640, y: 246, w: 62, h: 30, cls: "btn warn", hw: "SHIFT", fn: "SHIFT", sh: "tap: 1-8 / 9-16", shl: "", t: "Modifier", d: "Hold with another control for its second function (the amber text on this map). Tap it alone: the strips switch between tracks 1-8 and 9-16." });
  const bk = (hw, x, y, fn, sh, d) => add({ x, y, w: 40, h: 26, cls: "btn", hw: "BANK SELECT " + hw, fn, sh, t: "Kit page / sample folder", d });
  bk("up", 762, 226, "kit pg −", "", "Previous page of kits for the SCENE LAUNCH buttons. With a 303 track selected: keyboard octave up.");
  bk("down", 762, 284, "kit pg +", "", "Next page of kits for the SCENE LAUNCH buttons. With a 303 track selected: keyboard octave down.");
  bk("left", 718, 255, "folder −", "", "Previous sample folder on the card; its first sample goes into the selected track. Undo with the ← under the device knobs.");
  bk("right", 806, 255, "folder +", "", "Next sample folder on the card; its first sample goes into the selected track. Undo with the ← under the device knobs.");
  add({ x: 884, y: 226, w: 62, h: 34, cls: "btn", hw: "TAP TEMPO", fn: "tap", sh: "", t: "Tap tempo", d: "Tap in time: the tempo follows the last taps." });
  add({ x: 862, y: 276, w: 50, h: 24, cls: "btn", hw: "NUDGE −", fn: "−1 bpm", sh: "−5", t: "Tempo down", d: "Tap: 1 BPM slower. SHIFT + tap: 5 BPM slower." });
  add({ x: 918, y: 276, w: 50, h: 24, cls: "btn", hw: "NUDGE +", fn: "+1 bpm", sh: "+5", t: "Tempo up", d: "Tap: 1 BPM faster. SHIFT + tap: 5 BPM faster." });
  const dv = ["pitch", "decay", "filter", "reso", "start", "reverse", "drive", "chance"], ac = ["cutoff", "reso", "env mod", "decay", "accent", "tune", "slide", "wave"];
  for (let i = 0; i < 8; i++) add({ x: kx(i), y: 330 + (i >> 2) * 66, w: 52, h: 52, cls: "knob", hw: "DEVICE CONTROL knob " + (i + 1), fn: dv[i], sh: ac[i], shl: "303", t: "The selected track's sound", d: "Sample track: " + dv.map((n, k) => (k + 1) + " " + n).join(", ") + " (chance = how often a step plays). 303 track: " + ac.map((n, k) => (k + 1) + " " + n).join(", ") + "; knob 1 while a step is held sets that step's note." });
  const db = (i, hw, fn, sh, t, d, cls) => add({ x: kx(i % 4) - 8, y: 466 + (i >> 2) * 40, w: 68, h: 30, cls: "btn " + (cls || ""), hw, fn, sh, t, d });
  db(0, "CLIP/TRACK", "copy (hold)", "undo edit", "Copy patterns", "Hold, then on pad row 3: tap the source pattern, then the target. Double tap a pattern: clear it (with SHIFT: only the selected track). Hold + TRACK SELECTION: bank A-H. SHIFT + tap: undo the last pattern edit.");
  db(1, "DEVICE ON/OFF", "track type", "reset knobs", "Sample or 303", "Tap: the selected track goes card sample → kit sound → 303 → kit sound. SHIFT + tap: the track's knobs back to their defaults.");
  db(2, "◄", "undo", "sample −", "Undo", "Tap: undo the last looper take, a sample changed by accident, or a kit loaded by accident. SHIFT + tap: previous sample in the folder, into the selected track.", "am");
  db(3, "►", "redo", "sample +", "Redo", "Tap: redo what undo took back. SHIFT + tap: next sample in the folder, into the selected track.", "am");
  db(4, "DETAIL VIEW", "", "sets", "Sets screen", "SHIFT + tap: opens / closes the sets screen (pad row 3 picks a slot, row 4 pads 1-5: load, save here, save new, delete, transition; a second press confirms). Alone: nothing yet.");
  db(5, "REC QUANTIZATION", "", "reload", "Reload the project", "SHIFT + tap: loads the project saved on the card again. Alone: nothing yet.");
  db(6, "MIDI OVERDUB", "overdub", "", "Overdub the loop", "Tap: record on top of the selected track's loop, in time, only at the loop's own tempo. Tap again: stop.", "warn");
  db(7, "METRONOME", "click", "monitor", "Metronome", "Tap: click on / off. SHIFT + tap: hear the audio input all the time.");
  add({ x: 690, y: 566, w: 64, h: 44, cls: "pad g", hw: "PLAY", fn: "play", sh: "", t: "Play", d: "Starts the sequencer from the top of the bar." });
  add({ x: 780, y: 566, w: 64, h: 44, cls: "pad", hw: "STOP", fn: "stop", sh: "", t: "Stop", d: "Stops the sequencer." });
  add({ x: 870, y: 566, w: 64, h: 44, cls: "pad warn", hw: "REC", fn: "rec loop", sh: "drop loop", t: "Record a live loop", d: "Tap: records the audio input into the selected track as a loop, from the next bar (or beat); tap again: ends with the bar. Take length, count-in and start are set on Samples > looper. SHIFT + tap: drops the track's loop, back to its kit sound." });
  add({ x: 700, y: 672, w: 230, h: 30, cls: "fader h", hw: "Crossfader", fn: "A ◄ crossfader ► B", sh: "", t: "Crossfader", d: "Fades between the tracks on side A and side B (SHIFT + SOLO sets a track's side). The middle is both at full." });
  return C;
}
let mapSel = -1;
V.map = {
  build() {
    view.className = "v"; view.style.gridTemplateRows = PHONE ? "auto 1fr auto" : "auto 1fr";
    if (!PHONE) view.style.gridTemplateColumns = SHORT ? "1fr 230px" : "1fr 420px";
    const hdr = el("div", "hdr"); if (!PHONE) hdr.style.gridColumn = "span 2";
    const back = el("button", "b", "‹ System"); back.style.cssText = "padding:0 12px;height:" + sz(32, 30, 24) + "px"; back.onclick = () => show("system");
    const key = el("span", "lbl"); key.innerHTML = 'APC40 map · white: alone · <b style="color:var(--amber)">amber: with SHIFT</b> · <b style="color:#6fb3ff">blue: on SEND C / on a 303 track</b> · tap a control';
    hdr.append(back, key);
    const wrap = el("div", "apcwrap"), board = el("div", "apc"); wrap.appendChild(board);
    this.info = el("div", "apcinfo");
    this.ctl = apcMap(); this.els = [];
    this.ctl.forEach((c, i) => {
      const d = el("div", "ac " + c.cls); d.style.cssText = `left:${c.x}px;top:${c.y}px;width:${c.w}px;height:${c.h}px`;
      if (c.cls.startsWith("knob")) { d.append(el("i")); const t = el("span", "akn"); t.append(el("b", "", c.fn)); if (c.sh) t.append(el("em", c.shl ? "alt" : "", c.sh)); d.append(t); }
      else { if (c.fn) d.append(el("b", "", c.fn)); if (c.sh) d.append(el("em", "", c.sh)); }
      d.onclick = () => { mapSel = i; this.sel(); };
      board.appendChild(d); this.els.push(d);
    });
    [["CLIP LAUNCH", 30, 24], ["SCENE", 536, 24], ["TRACK CONTROL", 650, 24], ["DEVICE CONTROL", 650, 310], ["CLIP STOP", 30, 278], ["TRACK SELECTION", 30, 328]].forEach(([t, x, y]) => { const l = el("span", "al", t); l.style.cssText = `left:${x}px;top:${y}px`; board.appendChild(l); });
    view.append(hdr, wrap, this.info);
    const fitB = () => { const w = wrap.clientWidth, h = wrap.clientHeight, k = Math.min(w / 1000, h / 750); board.style.transform = `scale(${k})`; board.style.left = Math.max(0, (w - 1000 * k) / 2) + "px"; };
    requestAnimationFrame(fitB); this.F = fitB;
    this.sel();
  },
  sel() {
    this.els.forEach((e, i) => e.classList.toggle("sel", i === mapSel));
    const c = this.ctl[mapSel]; this.info.textContent = "";
    if (!c) { this.info.append(el("h3", "", "Tap a control"), el("p", "", "Every button, knob and fader of the APC40 as ApcBox uses it today. Changing what they do comes next.")); return; }
    this.info.append(el("span", "lbl", c.hw), el("h3", "", c.t));
    const r = el("div", "apcfn"); r.append(el("b", "", c.fn || "nothing")); if (c.sh) { const e = el("em", c.shl ? "alt" : "", (c.shl != null ? c.shl : "SHIFT") + (c.shl === "" ? "" : ": ") + c.sh); r.append(e); }
    this.info.append(r, el("p", "", c.d));
  },
  S() { },
};
V.system = {
  build() {
    view.className = "v"; view.style.cssText = PHONE ? "grid-template-rows:auto 1fr auto" : "grid-template-columns:1fr 1fr;grid-template-rows:1fr auto";
    const box = (t, inner) => { const b = el("div", "box"); b.style.cssText = "overflow:hidden;min-height:0"; b.append(el("h3", "", t), inner); return b; };
    this.kv = el("div", "kv"); this.logEl = el("div", "log");
    const pg = el("div", "pg"); pg.style.cssText = "grid-template-columns:repeat(" + (PHONE ? 2 : 4) + ",1fr);grid-auto-rows:48px" + (PHONE ? "" : ";grid-column:span 2");
    { const b = el("button", "bigpad", "APC40 map"); b.onclick = () => show("map"); pg.appendChild(b); pg.style.gridTemplateColumns = PHONE ? "repeat(3,1fr)" : "repeat(5,1fr)"; }
    [["Modules", "/"], ["Classic page", "/classic"], ["MIDI hub", "/midihub"], ["Clock", "/clock"]].forEach(([n, u]) => { const b = el("button", "bigpad", PHONE ? n.replace(" page", "") : n); b.onclick = () => { location.href = u + (tv ? "?tv=1" : ""); }; pg.appendChild(b); });
    const qb = el("div", "qrow"); if (!PHONE) qb.style.gridColumn = "span 2";
    const qc = (label, seg) => { const c = el("div", "qc"); c.append(el("span", "lbl", label), seg.el); qb.appendChild(c); return seg; };
    this.q = [
      qc(SHORT ? "patterns" : PHONE ? "pattern switch on the" : "patterns on the", Seg([[0, "bar"], [1, "beat"], [2, "step"]], () => st.lp.qpat, v => lpAct("qpat", v, "quant pat " + v))),
      qc(SHORT ? "mutes" : "mute / solo", Seg([[0, "at once"], [1, "beat"], [2, "bar"]], () => st.lp.qmute, v => lpAct("qmute", v, "quant mute " + v))),
      qc(SHORT ? "takes" : PHONE ? "loop takes start on the" : "takes start on the", Seg([[0, "bar"], [1, "beat"]], () => st.lp.qrec, v => lpAct("qrec", v, "quant rec " + v))),
      qc(SHORT ? "FX" : "held FX", Seg([[0, "at once"], [1, "step"], [2, "beat"]], () => st.lp.qfx, v => lpAct("qfx", v, "quant fx " + v)))];
    view.style.gridTemplateRows = PHONE ? "auto 1fr auto auto" : "1fr auto auto";
    view.append(box("Links", this.kv), box("Log", this.logEl), qb, pg);
    this.B(); this.L();
  },
  B() {
    const b = st.bridge || {}, p = b.ping_ms || {}, rows = [
      ["Daisy", st.daisy ? "connected · control " + st.ctl + " · report " + st.ver : "not connected"],
      ["ping", p.count ? `avg ${p.avg} · p99 ${p.p99} · max ${p.max} ms · lost ${p.lost}` : "-"],
      ["controllers", (b.controllers || []).map(c => c.name + " (" + c.kind + ")").join(", ") || "none"],
      ["network MIDI", b.net && b.net.on ? (b.net.sessions.length ? b.net.sessions.join(", ") : "on, nobody connected") : "off"],
      ["clock", b.clock && b.clock.on ? b.clock.source + " " + b.clock.tempo : "off"],
      ["APC40 on the Daisy", st.apc ? "yes" : "no"], ["SD card in the Daisy", st.sd ? "ok" : "none"],
      ["CPU", Math.round(st.cpu / 10) + "%"], ["browsers", String(b.clients || 0)]];
    this.kv.textContent = ""; for (const [k, v] of rows) this.kv.append(el("span", "", k), el("span", "", v));
  },
  S() { }, P() { this.q.forEach(x => x.upd()); }, L() { this.logEl.textContent = st.log.slice(-sz(22, 14, 9)).join("\n"); },
};

/* ---------- shell ---------- */
const TABS = [["perform", "Perform", "Perf"], ["steps", "Steps", "Steps"], ["mixer", "Mixer", "Mix"], ["sound", "Sound", "Sound"], ["samples", "Samples", "Smpl"], ["patterns", "Patterns", "Patt"], ["fx", "FX", "FX"], ["system", "System", "Sys"]];
function buildTabs() {
  const tb = document.getElementById("tabs"); tb.textContent = "";
  TABS.forEach((t, i) => { const b = el("button", "tab" + (t[0] === (curName === "map" ? "system" : curName) ? " on" : "")); if (!PHONE && !SHORT) b.appendChild(el("kbd", "", String(i + 1))); b.appendChild(document.createTextNode(t[1])); b.onclick = () => show(t[0]); tb.appendChild(b); });
  if (!PHONE && !SHORT) tb.append(el("span", "sp"), el("span", "lbl", "1-8 views · ◀ ▶ track · space play"));
}
function show(name) {
  if (!V[name]) name = "perform";
  curName = name; cur = V[name];
  try { localStorage.setItem("apcbox-view", name); } catch (e) { }
  view.textContent = ""; view.removeAttribute("style"); view.className = "";
  cur.build();
  for (const k of ["N", "T", "A", "M", "G", "Q", "O", "Z", "H", "J", "K", "X", "P", "S"]) if (cur[k]) cur[k]();
  buildTabs();
}
function call(k, arg) { if (cur && cur[k]) cur[k](arg); }

/* ---------- lines from the Daisy ---------- */
function line(l) {
  const sp = l.indexOf(" "), type = sp < 0 ? l : l.slice(0, sp), rest = sp < 0 ? "" : l.slice(sp + 1);
  switch (type) {
    case "S": {
      const f = rest.split(" ").map(Number); if (f.length < 56) return;
      st.playing = f[0]; st.step = f[1]; st.pat = f[2]; st.qpat = f[3]; st.rec = f[4]; if (!isBusy("bpm")) st.bpm = f[5]; st.metro = f[6];
      if (!isBusy("sel")) st.sel = f[7];
      st.cpu = f[11]; st.sd = f[13]; st.apc = f[14]; st.fx = f[16] | f[17]; st.mute = f[19]; st.solo = f[20]; st.loading = f[21]; st.synth = f[37];
      st.lrec = f[47]; st.lrecT = f[48]; st.lrecBars = f[49]; st.inPeak = f[50]; st.mon = f[51]; st.set = f[53]; st.dirty = f[54]; st.setOp = f[55];
      topS(); topStatus(); call("S"); break;
    }
    case "G": { const i = rest.indexOf(" "); const p = +rest.slice(0, i); if (isBusy("G") && p === st.gridPat) break; st.gridPat = p; st.grid = rest.slice(i + 1); call("G"); break; }
    case "T": { const f = rest.split(" ").map(Number), t = f[0]; if (!(t >= 0 && t < 16)) break; for (let i = 0; i < 13; i++) if (!isBusy("k" + t + "." + i)) st.tr[t].v[i] = f[i + 1]; call("T", t); break; }
    case "N": { const i = rest.indexOf(" "), t = +rest.slice(0, i); if (t >= 0 && t < 16) { st.tr[t].name = rest.slice(i + 1); call("N", t); } break; }
    case "A": { const f = rest.split(" ").map(Number), t = f[0]; if (!(t >= 0 && t < 16)) break; for (let i = 0; i < 8; i++) if (!isBusy("a" + t + "." + i)) st.tr[t].acid[i] = f[i + 1]; call("A", t); break; }
    case "Q": { const f = rest.split(" "), t = +f[1]; if (!(t >= 0 && t < 16) || isBusy("Q" + t)) break; const n = new Uint8Array(16); for (let i = 0; i < 16; i++) n[i] = parseInt(f[2].substr(i * 2, 2), 16); st.tr[t].notes = n; st.tr[t].notesPat = +f[0]; call("Q", t); break; }
    case "M": { const f = rest.split(" ").map(Number); for (let i = 0; i < 10; i++) if (!isBusy("g" + i)) st.glob[i] = f[i]; call("M"); break; }
    case "H": { for (let i = 0; i < 18; i++) st.meters[i] = parseInt(rest.substr(i * 2, 2), 16) || 0; call("H"); break; }
    case "O": { try { st.patmask = BigInt("0x" + rest.trim()); } catch (e) { } call("O"); break; }
    case "Z": { const f = rest.split(" "), t = +f[0]; if (!(t >= 0 && t < 16)) break; const len = +f[1], rate = +f[2] || 48000; let pts = null;
      if (len && f[3] && f[3] !== "-") { pts = []; for (let i = 0; i < f[3].length; i += 2) pts.push(parseInt(f[3].substr(i, 2), 16)); }
      st.tr[t].wave = { len, rate, pts, key: rest.length + ":" + rest.slice(-40) + len }; call("Z", t); break; }
    case "J": { const f = rest.split(" "), i = +f[0]; if (!(i >= 0 && i < 8)) break;
      st.sets[i] = f[1] === "1" ? { exists: true, bpm: +f[2] / 10, samples: +f[3], loops: +f[4], synths: +f[5], name: f.slice(6).join(" ") } : { exists: false }; call("J"); break; }
    case "JT": { const i = rest.indexOf(" "); st.trans = +rest.slice(0, i); st.transName = rest.slice(i + 1); call("J"); break; }
    case "KL": { const i = rest.indexOf(" "), k = +rest.slice(0, i); if (k >= 0 && k < 64) { st.kits[k] = rest.slice(i + 1); call("K"); } break; }
    case "KP": { const f = rest.split(" ").map(Number); st.kitPg = [f[0], f[1]]; st.kitNb = f.length > 3 ? f[3] : null; if (st.kits.length > f[2]) st.kits.length = f[2]; call("K"); break; }
    case "CK": st.ext = rest[0] === "1"; topStatus(); break;
    case "LP": { const f = rest.split(" ").map(Number); if (f.length < 8) break;
      const o = { len: f[0], count: f[1], qrec: f[2], qpat: f[3], qmute: f[4], gain: f[5], mode: f[6], undo: f[7], redo: f[8] || 0, qfx: f.length > 9 ? f[9] : 0 };
      for (const k in o) if (!isBusy("lp." + k)) st.lp[k] = o[k]; call("P"); call("S"); break; }
    case "L": st.log.push(new Date().toTimeString().slice(0, 8) + "  " + rest); if (st.log.length > 200) st.log.splice(0, 100); call("L"); break;
    case "V": st.ver = +rest; topStatus(); call("B"); break;
  }
}
const REPORT_VERSION = 15;
let es = null;
function connect() {
  es = new EventSource("/events?view=apcbox");
  es.onmessage = e => line(e.data);
  es.addEventListener("bridge", e => { try { const b = JSON.parse(e.data); st.bridge = b; st.daisy = !!b.daisy; st.ctl = b.control || 0; topStatus(); call("B"); } catch (err) { } });
  es.addEventListener("xfer", e => { try { st.xfer = JSON.parse(e.data); call("X"); } catch (err) { } });
  es.addEventListener("ir", e => { try { remote(JSON.parse(e.data).action); } catch (err) { } });
  es.onerror = () => { st.daisy = false; sendErr = "T95 bridge not answering"; topStatus(); };
  es.onopen = () => { sendErr = ""; topStatus(); };
}
setInterval(async () => { try { const b = await (await fetch("/health")).json(); st.bridge = b; st.daisy = !!b.daisy; st.ctl = b.control || 0; topStatus(); call("B"); } catch (e) { } }, 2000);

/* ---------- remote and keyboard ---------- */
function remote(a) {
  if (/^[1-8]$/.test(a)) show(TABS[+a - 1][0]);
  else if (a === "left" || a === "right") sel((st.sel + (a === "right" ? 1 : 15)) % 16);
  else if (a === "up" || a === "down") { const f = document.activeElement; if (f && f.classList.contains("pot")) f.dispatchEvent(new KeyboardEvent("keydown", { key: a === "up" ? "ArrowUp" : "ArrowDown", bubbles: true })); else knobStep(a === "up" ? 3 : -3); }
  else if (a === "play" || a === "ok") act(st.playing ? "stop" : "play");
  else if (a === "stop") act("stop");
  else if (a === "next" || a === "prev") show(TABS[(TABS.findIndex(t => t[0] === curName) + (a === "next" ? 1 : 7)) % 8][0]);
  else if (a === "home" || a === "menu" || a === "back") location.href = "/" + (tv ? "?tv=1" : "");
}
function knobStep(d) { const t = st.sel, v = Math.max(0, Math.min(127, st.tr[t].v[0] + d)); knobAct(t, 0, v); call("T", t); }   // no pot focused: the selected track's volume
document.addEventListener("keydown", e => {
  if (e.target.tagName === "INPUT" || e.metaKey || e.ctrlKey) return;
  if (/^[1-8]$/.test(e.key)) { show(TABS[+e.key - 1][0]); e.preventDefault(); }
  else if (e.key === " ") { act(st.playing ? "stop" : "play"); e.preventDefault(); }
  else if (e.key === "ArrowLeft" || e.key === "ArrowRight") { remote(e.key === "ArrowLeft" ? "left" : "right"); e.preventDefault(); }
});
fetch("/caps").then(r => r.json()).then(c => { st.caps = c; }).catch(() => { });
function refit() { if (document.activeElement && document.activeElement.tagName === "INPUT") return;   /* the keyboard is up */
  if (fit()) { buildTop(); show(curName); } else if (cur && cur.Z) cur.Z(); }
window.addEventListener("resize", refit);
window.addEventListener("orientationchange", () => setTimeout(refit, 200));
if (window.visualViewport) visualViewport.addEventListener("resize", refit);
document.addEventListener("gesturestart", e => e.preventDefault());   /* iOS: no pinch zoom over the controls */

buildTop();
let start = Q.get("v"); try { start = start || localStorage.getItem("apcbox-view"); } catch (e) { }
show(start || "perform");
connect();
