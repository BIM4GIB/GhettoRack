// Static mock of the ApcBox views. No logic: every value is made up.
const Q = new URLSearchParams(location.search), V = Q.get("v") || "perform", PHONE = Q.has("phone");
if (PHONE) document.body.classList.add("phone");
let seed = 7; const rnd = () => { seed = (seed * 1103515245 + 12345) & 0x7fffffff; return seed / 0x7fffffff; };

function pot(val, size, mode, color) {
  const n = 15, r = size / 2 - 4, c = size / 2, col = color || "#ff9a1f"; let s = `<svg width="${size}" height="${size}" viewBox="0 0 ${size} ${size}">`;
  const lit = i => { const p = i / (n - 1); if (mode === "pan") return val >= .5 ? (p >= .499 && p <= val + .001) : (p <= .501 && p >= val - .001); return p <= val + .001; };
  for (let i = 0; i < n; i++) { const a = (135 + 270 * i / (n - 1)) * Math.PI / 180, x = c + r * Math.cos(a), y = c + r * Math.sin(a), on = lit(i);
    s += `<circle cx="${x.toFixed(1)}" cy="${y.toFixed(1)}" r="${size / 26}" fill="${on ? col : "#23262a"}" ${on ? `style="filter:drop-shadow(0 0 ${size / 22}px ${col})"` : ""}/>`; }
  const kr = r - size / 7, a = (135 + 270 * val) * Math.PI / 180;
  return s + `<circle cx="${c}" cy="${c}" r="${kr}" fill="url(#g)" stroke="#000" stroke-width="1.5"/>
  <line x1="${c + kr * .35 * Math.cos(a)}" y1="${c + kr * .35 * Math.sin(a)}" x2="${c + kr * .92 * Math.cos(a)}" y2="${c + kr * .92 * Math.sin(a)}" stroke="#fff" stroke-width="${Math.max(2, size / 30)}" stroke-linecap="round"/>
  <defs><radialGradient id="g" cx="35%" cy="30%"><stop offset="0" stop-color="#5a6067"/><stop offset="1" stop-color="#1b1e21"/></radialGradient></defs></svg>`;
}
// kind: kick (decaying), loop (4 bars of hits), voice (blobs), hat
function wave(kind, opt) {
  opt = opt || {}; const W = 1000, H = 200, n = opt.n || 250, played = opt.played ?? -1; let b = "";
  for (let i = 0; i < n; i++) { const t = i / n; let env;
    if (kind === "kick") env = Math.exp(-t * 5.5) * (t < .012 ? t / .012 : 1);
    else if (kind === "hat") env = Math.exp(-t * 14);
    else if (kind === "loop") { const p = (t * 16) % 1; env = Math.exp(-p * (Math.floor(t * 16) % 4 === 0 ? 3 : 7)) * (Math.floor(t * 16) % 2 ? .5 : .95); }
    else env = .15 + .8 * Math.abs(Math.sin(t * 9) * Math.sin(t * 23 + 1));
    const h = Math.max(1, env * (.55 + .45 * rnd()) * H * .46);
    b += `<rect x="${(i * W / n).toFixed(1)}" y="${(H / 2 - h).toFixed(1)}" width="${(W / n * .75).toFixed(1)}" height="${(2 * h).toFixed(1)}" fill="${t < played ? (opt.col || "#35d05a") : (opt.dim || "#2f6b3f")}"/>`; }
  let marks = "";
  for (const m of opt.marks || []) marks += `<line x1="${m[0] * W}" x2="${m[0] * W}" y1="0" y2="${H}" stroke="${m[1]}" stroke-width="${m[2] || 3}" vector-effect="non-scaling-stroke"/>`;
  if (opt.bars) for (let k = 1; k < opt.bars; k++) marks += `<line x1="${k * W / opt.bars}" x2="${k * W / opt.bars}" y1="0" y2="${H}" stroke="#2a2d31" stroke-width="1" vector-effect="non-scaling-stroke"/>`;
  return `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><line x1="0" x2="${W}" y1="${H / 2}" y2="${H / 2}" stroke="#1c1f22"/>${marks}${b}</svg>`;
}
const names = ["kick", "snare", "cl hat", "op hat", "clap", "tom", "rim", "cowbell", "303 bass", "303 lead", "perc", "loop 1", "loop 2", "vox", "fx", "ride"];
const kinds = ["s", "s", "s", "s", "s", "s", "s", "s", "3", "3", "s", "L", "L", "s", "s", "s"];
const pat = [[1, 5, 9, 13], [5, 13], [1, 3, 5, 7, 9, 11, 13, 15], [15], [13], [], [], [8], [1, 4, 7, 10, 13], [3, 11, 12], [2, 6], [1], [9], [], [], [9]];
const acc = [[1], [13], [3, 11], [], [], [], [], [], [7], [], [], [], [], [], [], []];
const lv = [.8, .55, .45, .25, .35, 0, 0, .2, .9, .45, .2, .65, .5, 0, 0, .15], fd = [.75, .65, .6, .55, .7, .55, .55, .55, .8, .65, .5, .7, .65, .45, .45, .55];
const pan = [.5, .5, .35, .65, .5, .3, .7, .5, .5, .6, .5, .2, .8, .5, .5, .55], snd = [.1, .4, .2, .3, .6, 0, 0, 0, .15, .7, 0, .3, .3, 0, 0, .2];
const meter = l => `<div class="mt">${Array.from({ length: 14 }, (_, k) => { const on = k < Math.round(l * 14); return `<i class="${on ? (k > 11 ? "r" : k > 8 ? "y" : "g") : ""}"></i>`; }).join("")}</div>`;
const btn = (cls, txt, on, style) => `<div class="b ${cls} ${on ? "on" : ""}" ${style ? `style="${style}"` : ""}>${txt}</div>`;
const knob = (label, val, size, mode, col) => `<div class="kn">${pot(val, size, mode, col)}${label}</div>`;
const cell = (label, txt, val, mode, col) => `<div class="cell">${pot(val, 150, mode, col)}<span class="val">${txt}</span><span class="n">${label}</span></div>`;
const AM = "#ff9a1f", GR = "#35d05a", BL = "#4aa3ff";
const trackHdr = (i, extra) => `<div class="hdr"><span class="lbl">Track</span><span class="big">${i + 1} · ${names[i]}</span>
  <div class="sub"><span class="${kinds[i] === "s" ? "on" : ""}">sample</span><span class="${kinds[i] === "3" ? "on" : ""}">303</span><span class="${kinds[i] === "L" ? "on" : ""}">live loop</span></div>${extra || ""}
  <span class="sp"></span><div class="btns" style="width:300px;grid-template-columns:repeat(3,1fr)">${btn("m", "MUTE")}${btn("s", "SOLO")}${btn("a", "REC ARM", kinds[i] === "L")}</div></div>`;

const TABS = [["perform", "Perform"], ["steps", "Steps"], ["mixer", "Mixer"], ["sound", "Sound"], ["samples", "Samples"], ["patterns", "Patterns"], ["fx", "FX"], ["system", "System"]];
const TABOF = { perform: "perform", grid: "steps", track: "steps", acid: "steps", mixer: "mixer", sound: "sound", sound303: "sound", loop: "sound", samples: "samples", sampler: "samples", kits: "samples", patterns: "patterns", sets: "patterns", fx: "fx", system: "system" };
document.getElementById("tabs").innerHTML = TABS.map((t, i) => `<div class="tab ${t[0] === TABOF[V] ? "on" : ""}"><kbd>${i + 1}</kbd>${t[1]}</div>`).join("")
  + `<span class="sp"></span><span class="lbl">remote: 1-8 views · ◀▶ track · ▲▼ value · ok select</span>`;
const view = document.getElementById("view");
const strip = i => `<div class="st ${i === 0 ? "sel" : ""}"><div class="nm"><b>${i + 1}</b>${names[i]}</div>
  ${knob("pan", pan[i], PHONE ? 38 : 54, "pan", AM)}${knob("send A", snd[i], PHONE ? 38 : 54, "vol", GR)}
  <div class="fd">${meter(lv[i])}<div class="slot"></div>${meter(lv[i] * .9)}<div class="cap" style="top:${(1 - fd[i]) * 86}%"></div></div>
  <div class="steps">${Array.from({ length: 16 }, (_, k) => `<i class="${k === 4 ? "ph" : pat[i].includes(k + 1) ? "on" : ""}"></i>`).join("")}</div>
  <div class="btns">${btn("act", "ON", lv[i] > 0, "grid-column:span 2")}${btn("m", "MUTE", [5, 6].includes(i))}${btn("s", "SOLO", i === 8)}${btn("a", "REC ARM", i === 11, "grid-column:span 2")}</div></div>`;
const patPads = `<div class="pads">${Array.from({ length: 16 }, (_, i) => `<div class="pad ${i === 0 ? "cur" : i === 1 ? "next" : i < 4 ? "has" : ""}">A${i + 1}</div>`).join("")}</div>`;
const fxNames = ["rpt 1/4", "rpt 1/8", "rpt 1/16", "rpt 1/32", "tape stop", "reverse", "crush", "gate"];

const views = {
  perform() {
    view.className = "main";
    if (PHONE) { view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">1-8</span><span>9-16</span></div><span class="sp"></span><span class="led">listen</span></div>
      <div class="strips">${names.slice(0, 8).map((_, i) => strip(i)).join("")}</div><div class="box"><h3>Patterns · bank A</h3>${patPads.replace("repeat(8", "repeat(8")}</div>`; return; }
    view.innerHTML = `<div class="strips">${names.map((_, i) => strip(i)).join("")}</div><div class="side">
      <div class="box"><h3>Patterns · bank A</h3>${patPads}</div>
      <div class="box"><h3>Performance FX (hold)</h3><div class="fx">${fxNames.map((f, i) => `<div class="pad ${i === 1 ? "on" : ""}">${f}</div>`).join("")}</div></div>
      <div class="box"><h3>Master</h3><div class="mk">${[["master", .8, "vol", GR], ["xfade", .5, "pan", AM], ["filter", .5, "pan", BL], ["swing", .5, "vol", AM], ["reverb", .55, "vol", GR], ["tone", .6, "vol", GR], ["delay", .4, "vol", GR], ["fdbk", .35, "vol", GR]].map(k => knob(k[0], k[1], 70, k[2], k[3])).join("")}</div></div></div>`;
  },
  grid() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr";
    let g = `<div></div><div></div>${Array.from({ length: 16 }, (_, k) => `<div class="colh">${k + 1}</div>`).join("")}`;
    names.forEach((n, i) => { g += `<div class="tn ${i === 0 ? "sel" : ""}"><b>${i + 1}</b>${n}</div><div class="ms">${btn("m", "M", [5, 6].includes(i))}${btn("s", "S", i === 8)}</div>`
      + Array.from({ length: 16 }, (_, k) => `<div class="sc ${Math.floor(k / 4) % 2 ? "" : "q"} ${acc[i].includes(k + 1) ? "ac" : pat[i].includes(k + 1) ? "on" : ""} ${k === 4 ? "ph" : ""}"></div>`).join(""); });
    view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">all tracks</span><span>one track</span></div><span class="lbl">Pattern</span><span class="big">A1</span>
      <span class="lbl">tap = on · tap again = accent · again = off</span><span class="sp"></span><div class="sub"><span>copy</span><span>clear</span><span>undo</span></div></div>
      <div class="grid" style="grid-template-rows:18px repeat(16,1fr)">${g}</div>`;
  },
  track() {
    view.className = "v"; view.style.gridTemplateRows = "auto 150px 1fr 190px";
    const i = 0;
    view.innerHTML = trackHdr(i, `<div class="sub"><span>all tracks</span><span class="on">one track</span></div>`)
      + `<div class="wave">${wave("kick", { played: .31, marks: [[.002, AM], [.31, "#ffb833", 2]] })}<span class="tag" style="left:10px;color:var(--dim)">kick.wav · 0.42 s</span></div>
      <div class="bigsteps">${Array.from({ length: 16 }, (_, k) => `<div class="bs ${Math.floor(k / 4) % 2 ? "" : "q"} ${acc[i].includes(k + 1) ? "ac" : pat[i].includes(k + 1) ? "on" : ""} ${k === 4 ? "ph" : ""}">${acc[i].includes(k + 1) ? "ACC" : pat[i].includes(k + 1) ? "ON" : ""}<span>${k + 1}</span></div>`).join("")}</div>
      <div class="row8">${[["probability", "100%", 1], ["swing", "50%", .5], ["length", "16", 1], ["volume", "78%", .78]].map(k => cell(k[0], k[1], k[2], "vol", GR)).join("")}
        ${[["pitch", "0 st", .5, "pan"], ["decay", "full", 1, "vol"], ["filter", "off", .5, "pan"], ["start", "0%", 0, "vol"]].map(k => cell(k[0], k[1], k[2], k[3], AM)).join("")}</div>`;
    view.querySelectorAll(".row8 .cell svg").forEach(s => { s.setAttribute("width", 96); s.setAttribute("height", 96); });
  },
  acid() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr 150px";
    const i = 8, notes = { 1: 36, 4: 36, 7: 48, 10: 39, 13: 43 }, slides = [4, 10], accs = [7];
    const rows = []; for (let n = 48; n >= 36; n--) rows.push(n);
    const nn = n => ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][n % 12] + (Math.floor(n / 12) - 1);
    let r = ""; rows.forEach(n => { r += `<div class="k ${nn(n).includes("#") ? "bk" : ""}">${nn(n)}</div>` + Array.from({ length: 16 }, (_, k) => `<div class="c ${Math.floor(k / 4) % 2 ? "" : "q"} ${notes[k + 1] === n ? (accs.includes(k + 1) ? "a" : "n") + (slides.includes(k + 1) ? " s" : "") : ""}"></div>`).join(""); });
    view.innerHTML = trackHdr(i, `<span class="lbl">green = note · red = accent · yellow edge = slide into the next</span>`)
      + `<div class="roll">${r}</div>
      <div class="bigsteps">${Array.from({ length: 16 }, (_, k) => `<div class="bs ${Math.floor(k / 4) % 2 ? "" : "q"} ${accs.includes(k + 1) ? "ac" : notes[k + 1] ? "on" : ""} ${k === 4 ? "ph" : ""}">${slides.includes(k + 1) ? '<span class="sl">slide</span>' : ""}<span class="nt">${notes[k + 1] ? nn(notes[k + 1]) : ""}</span><span>${k + 1}</span></div>`).join("")}</div>`;
  },
  mixer() {
    view.className = "v"; view.style.gridTemplateRows = "1fr";
    const s = i => `<div class="st ${i === 0 ? "sel" : ""}" style="grid-template-rows:auto auto auto auto auto 1fr auto"><div class="nm"><b>${i + 1}</b>${names[i]}</div>
      ${knob("pan", pan[i], 58, "pan", AM)}${knob("reverb", snd[i], 58, "vol", GR)}${knob("delay", snd[(i + 5) % 16], 58, "vol", GR)}${knob("send C", snd[(i + 9) % 16] * .5, 58, "vol", BL)}
      <div class="fd">${meter(lv[i])}<div class="slot"></div>${meter(lv[i] * .9)}<div class="cap" style="top:${(1 - fd[i]) * 86}%"></div></div>
      <div class="btns">${btn("m", "MUTE", [5, 6].includes(i))}${btn("s", "SOLO", i === 8)}${btn("act", "A", i % 3 === 0)}${btn("act", "B", i % 3 === 1)}</div></div>`;
    view.innerHTML = `<div class="mx">${names.map((_, i) => s(i)).join("")}
      <div class="st" style="grid-template-rows:auto auto auto auto auto 1fr auto;border-color:#3a3f45"><div class="nm"><b>&nbsp;</b>MASTER</div>
      ${knob("filter", .5, 58, "pan", BL)}${knob("rev size", .55, 58, "vol", GR)}${knob("dly fb", .35, 58, "vol", GR)}${knob("drive", 0, 58, "vol", AM)}
      <div class="fd">${meter(.8)}<div class="slot"></div>${meter(.75)}<div class="cap" style="top:17%"></div></div>
      <div class="btns">${btn("a", "LIMIT", false, "grid-column:span 2")}<div class="b" style="grid-column:span 2">XFADE A│B</div></div></div></div>`;
  },
  sound() {
    view.className = "v"; view.style.gridTemplateRows = "auto 230px 1fr 1fr";
    view.innerHTML = trackHdr(0, `<span class="lbl">kit 808 · kick.wav</span>`)
      + `<div class="wave">${wave("kick", { n: 470, played: .31, marks: [[.002, AM], [.31, "#ffb833", 2]] })}<span class="tag" style="left:10px;color:var(--dim)">kick.wav · 0.42 s · 48 kHz mono</span><span class="tag" style="right:10px;color:var(--green)">decay full</span></div>
      <div class="row8">${[["pitch", "0 st", .5, "pan"], ["decay", "full", 1, "vol"], ["filter", "off", .5, "pan"], ["reso", "20%", .2, "vol"], ["start", "0%", 0, "vol"], ["reverse", "fwd", 0, "vol"], ["drive", "0%", 0, "vol"], ["prob", "100%", 1, "vol"]].map(k => cell(k[0], k[1], k[2], k[3], AM)).join("")}</div>
      <div class="row8">${[["volume", "78%", .78, "vol"], ["pan", "C", .5, "pan"], ["send A reverb", "10%", .1, "vol"], ["send B delay", "35%", .35, "vol"], ["send C", "0%", 0, "vol"], ["xfade side", "A", 0, "vol"], ["swing", "50%", .5, "vol"], ["length", "16", 1, "vol"]].map(k => cell(k[0], k[1], k[2], k[3], GR)).join("")}</div>`;
  },
  sound303() {
    view.className = "v"; view.style.gridTemplateRows = "auto 230px 1fr 1fr";
    // filter curve: low-pass with a resonant peak at the cutoff
    const W = 1000, H = 200, fc = .42, q = .7; let d = "M0," + H * .35;
    for (let x = 0; x <= W; x += 5) { const f = x / W, r = f / fc; const mag = 1 / Math.sqrt((1 - r * r) ** 2 + (r / (1 + q * 6)) ** 2); const y = H * .9 - Math.min(1.9, mag) * H * .28 - H * .0; d += ` L${x},${Math.max(6, y).toFixed(1)}`; }
    const curve = `<svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><path d="${d} L${W},${H} L0,${H} Z" fill="#ff9a1f22"/><path d="${d}" fill="none" stroke="#ff9a1f" stroke-width="3" vector-effect="non-scaling-stroke" style="filter:drop-shadow(0 0 5px #ff9a1f)"/>
      <line x1="${fc * W}" x2="${fc * W}" y1="0" y2="${H}" stroke="#ffb833" stroke-dasharray="6 6" vector-effect="non-scaling-stroke"/></svg>`;
    view.innerHTML = trackHdr(8, `<span class="lbl">Pocket 303 voice</span>`)
      + `<div style="display:grid;grid-template-columns:2fr 1fr;gap:12px;min-height:0"><div class="wave">${curve}<span class="tag" style="left:10px;color:var(--amber)">filter · cutoff 1.2 kHz · reso 70%</span></div>
        <div class="wave">${wave("kick", { n: 120, col: BL, dim: "#24507e", played: .45 })}<span class="tag" style="left:10px;color:var(--blue)">envelope · decay 320 ms</span></div></div>
      <div class="row8">${[["cutoff", "1.2 kHz", .42, "vol"], ["reso", "70%", .7, "vol"], ["env mod", "55%", .55, "vol"], ["decay", "320 ms", .5, "vol"], ["accent", "60%", .6, "vol"], ["tune", "+0.0 st", .5, "pan"], ["slide", "60 ms", .5, "vol"], ["wave", "saw", 0, "vol"]].map(k => cell(k[0], k[1], k[2], k[3], AM)).join("")}</div>
      <div class="row8">${[["volume", "85%", .85, "vol"], ["pan", "C", .5, "pan"], ["send A reverb", "15%", .15, "vol"], ["send B delay", "40%", .4, "vol"], ["send C", "0%", 0, "vol"], ["xfade side", "A+B", .5, "pan"], ["drive", "30%", .3, "vol"], ["filter", "off", .5, "pan"]].map(k => cell(k[0], k[1], k[2], k[3], GR)).join("")}</div>`;
  },
  loop() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr 190px";
    view.innerHTML = trackHdr(11, `<span class="lbl">recorded from the Daisy input</span>`)
      + `<div class="wave">${wave("loop", { n: 480, played: .29, bars: 4, marks: [[.29, "#ffb833", 2]] })}<span class="tag" style="left:10px;color:var(--dim)">loop 1 · 4 bars · 8.0 s at 120 BPM</span>
        <span class="tag" style="left:25.5%;color:var(--dim)">bar 2</span><span class="tag" style="left:50.5%;color:var(--dim)">bar 3</span><span class="tag" style="left:75.5%;color:var(--dim)">bar 4</span></div>
      <div style="display:grid;grid-template-columns:200px 1fr 1fr;gap:12px;min-height:0">
        <div class="cell"><div class="recbtn" style="width:110px;height:110px;font-size:16px">ARMED</div><span class="n">starts on the next bar</span></div>
        <div class="cell" style="flex-direction:row;gap:24px">${knob("input", .6, 110, "vol", GR)}<div style="display:flex;gap:8px;height:120px">${meter(.6)}${meter(.5)}</div>
          <div class="kv"><span>state</span><span style="color:var(--red)">armed</span><span>bars so far</span><span>0</span><span>stop</span><span>ends on the bar</span></div></div>
        <div class="cell" style="flex-direction:row;gap:14px">${["STOP AT BAR", "CANCEL", "CLEAR LOOP", "BACK TO STEPS"].map(t => `<div class="bigpad" style="width:120px;height:100px;font-size:13px;text-align:center">${t}</div>`).join("")}</div></div>`;
  },
  samples() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr 210px";
    const files = [["📁", "breaks/", null, "", ""], ["📁", "uploads/", null, "", ""], ["♪", "amen_170.wav", "loop", "5.6 s", "stereo 44.1"], ["♪", "kick_long.wav", "kick", "0.9 s", "mono 48"], ["♪", "vox_hey.wav", "voice", "1.2 s", "mono 48"], ["♪", "hat_open_01.flac", "hat", "0.4 s", "mono 44.1"], ["♪", "rec 23-11 01.20.webm", "voice", "3.1 s", "phone mic"], ["♪", "ride_bell.aif", "hat", "1.8 s", "mono 48"], ["♪", "bass_stab_F.wav", "kick", "0.7 s", "mono 48"]];
    view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">library</span><span>SD card</span><span>record</span><span>kits</span></div><span class="lbl">/ library</span><span class="sp"></span>
        <span class="lbl">sends to</span><span class="big">1 · kick</span><div class="sub"><span>upload</span><span>import folder</span></div></div>
      <div class="list">${files.map((f, i) => `<div class="li ${i === 2 ? "sel" : ""}"><span class="ic">${f[0]}</span><span class="nm2">${f[1]}</span>${f[2] ? wave(f[2], { n: 140, played: i === 2 ? .4 : -1 }) : "<span></span>"}<span class="d">${f[3]}</span>
        <div class="act">${f[2] ? btn("act", "▶", i === 2) + btn("", "send") : btn("", "open")}</div></div>`).join("")}</div>
      <div class="wave">${wave("loop", { n: 480, played: .4, marks: [[.4, "#ffb833", 2]] })}<span class="tag" style="left:10px;color:var(--dim)">amen_170.wav · 5.6 s · stereo 44.1 kHz · converts to mono 48 kHz on send</span><span class="tag" style="right:10px;color:var(--green)">▶ playing on the T95</span></div>`;
  },
  sampler() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr 190px";
    view.innerHTML = `<div class="hdr"><div class="sub"><span>library</span><span>SD card</span><span class="on">record</span><span>kits</span></div>
        <div class="sub"><span class="on">Daisy input</span><span>phone mic</span></div><span class="sp"></span><span class="lbl">sends to</span><span class="big">14 · vox</span></div>
      <div class="wave">${wave("voice", { n: 300, col: "#ff4b3e", dim: "#ff4b3e", played: 2, marks: [[.08, AM], [.86, AM]] })}<span class="tag" style="left:10px;color:var(--red)">● recording 0:03.1</span>
        <span class="tag" style="left:8.5%;color:var(--amber)">trim start</span><span class="tag" style="left:86.5%;color:var(--amber)">trim end</span></div>
      <div style="display:grid;grid-template-columns:200px 1fr 1fr;gap:12px;min-height:0">
        <div class="cell"><div class="recbtn" style="width:120px;height:120px">STOP</div></div>
        <div class="cell" style="flex-direction:row;gap:24px">${knob("input gain", .6, 110, "vol", GR)}<div style="display:flex;gap:8px;height:120px">${meter(.75)}${meter(.7)}</div>${knob("trim start", .08, 110, "vol", AM)}${knob("trim end", .86, 110, "vol", AM)}</div>
        <div class="cell" style="flex-direction:row;gap:14px">${["▶ PLAY", "NORMALISE", "SAVE TO LIBRARY", "SEND TO TRACK"].map((t, i) => `<div class="bigpad ${i === 3 ? "cur" : ""}" style="width:120px;height:100px;font-size:13px;text-align:center">${t}</div>`).join("")}</div></div>`;
  },
  patterns() {
    view.className = "v"; view.style.gridTemplateRows = "auto 1fr"; view.style.gridTemplateColumns = "1fr 420px";
    const has = [0, 1, 2, 3, 8, 9, 16, 24, 25, 26];
    const mini = p => `<svg viewBox="0 0 16 8" preserveAspectRatio="none" style="width:80%;height:34%">${Array.from({ length: 8 }, (_, t) => Array.from({ length: 16 }, (_, k) => ((k * 7 + t * 3 + p) % (3 + t) === 0) ? `<rect x="${k}" y="${t}" width=".8" height=".7" fill="${p === 0 ? "#35d05a" : "#ff9a1f"}"/>` : "").join("")).join("")}</svg>`;
    view.innerHTML = `<div class="hdr" style="grid-column:span 2"><div class="sub"><span class="on">patterns</span><span>sets</span></div><span class="lbl">playing</span><span class="big">A1</span><span class="lbl">next</span><span class="big" style="color:var(--green)">A2</span>
        <span class="sp"></span><div class="sub"><span>copy</span><span>clear</span><span>undo</span></div></div>
      <div class="pg" style="grid-template-columns:44px repeat(8,1fr);grid-template-rows:repeat(8,1fr)">${"ABCDEFGH".split("").map((b, r) => `<div class="colh" style="align-self:center;font-size:18px">${b}</div>` + Array.from({ length: 8 }, (_, c) => { const p = r * 8 + c;
        return `<div class="bigpad ${p === 0 ? "cur" : p === 1 ? "next" : has.includes(p) ? "has" : ""}">${has.includes(p) ? mini(p) : ""}${b}${c + 1}</div>`; }).join("")).join("")}</div>
      <div class="list" style="grid-auto-rows:auto;align-content:start"><div class="li" style="grid-template-columns:1fr auto;height:44px"><span class="lbl">Sets on the card · switch on the bar</span><span></span></div>
        ${[["night jam *", "now"], ["acid tuesday", ""], ["breaks 170", "queued"], ["empty", ""], ["demo", ""]].map(s => `<div class="li ${s[1] === "now" ? "sel" : ""}" style="grid-template-columns:1fr auto;height:58px"><span class="nm2">${s[0]}</span><span class="d" style="color:${s[1] === "queued" ? "var(--green)" : "var(--dim)"}">${s[1]}</span></div>`).join("")}
        <div class="li" style="grid-template-columns:1fr 1fr 1fr;height:70px;gap:8px">${btn("act", "SAVE")}${btn("", "SAVE AS")}${btn("", "LOAD")}</div></div>`;
  },
  fx() {
    view.className = "v"; view.style.gridTemplateRows = "1fr 1fr 1fr";
    view.innerHTML = `<div class="pg" style="grid-template-columns:repeat(8,1fr)">${fxNames.map((f, i) => `<div class="bigpad fx ${i === 1 ? "on" : ""}">${f}<small style="color:inherit">hold</small></div>`).join("")}</div>
      <div class="row8">${[["reverb size", "55%", .55, GR], ["reverb tone", "60%", .6, GR], ["delay time", "1/4", .5, GR], ["delay feedback", "35%", .35, GR], ["fx return", "80%", .8, GR], ["master filter", "off", .5, BL], ["drive", "0%", 0, AM], ["cue filter", "off", 0, BL]].map(k => cell(k[0], k[1], k[2], k[3] === BL && k[0] === "master filter" ? "pan" : "vol", k[3])).join("")}</div>
      <div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;min-height:0"><div class="wave">${wave("loop", { n: 300, played: 2, col: "#35d05a" })}<span class="tag" style="left:10px;color:var(--dim)">master out, live</span></div>
        <div class="cell" style="flex-direction:row;gap:30px">${knob("master", .8, 120, "vol", GR)}${knob("crossfader", .5, 120, "pan", AM)}${knob("swing", .5, 120, "vol", AM)}<div style="display:flex;gap:8px;height:130px">${meter(.8)}${meter(.75)}</div></div></div>`;
  },
  system() {
    view.className = "v"; view.style.gridTemplateColumns = "1fr 1fr 1fr"; view.style.gridTemplateRows = "1fr 1fr";
    const box = (t, inner, span) => `<div class="box" style="${span ? "grid-row:span 2;" : ""}overflow:hidden"><h3>${t}</h3>${inner}</div>`;
    view.innerHTML = box("Links", `<div class="kv"><span>Daisy</span><span style="color:var(--green)">connected · firmware 13.9</span><span>ping</span><span>avg 2.4 ms · p99 9 ms</span><span>T95 bridge</span><span style="color:var(--green)">ok · 2 browsers</span>
        <span>controllers</span><span>APC40, LPK25</span><span>network MIDI</span><span>1 session</span><span>clock</span><span>internal 120.0</span><span>SD card</span><span>ok</span><span>CPU</span><span>10% avg · 20% max</span><span>memory</span><span>93% of sample RAM</span></div>`)
      + box("OLED mirror", `<div style="background:#000;border-radius:6px;height:80%;display:flex;align-items:center;justify-content:center;font-family:var(--mono);color:#6fd6ff;font-size:26px;line-height:1.5">A1 ■ &nbsp; &nbsp; 120<br>1 kick &nbsp; &nbsp;1-8<br>kit 808 &nbsp;bank A</div>`)
      + box("Log", `<div class="log">${["01:20:03 connected to the T95 bridge", "01:20:03 Daisy on the T95, control 3", "01:20:07 APC40 port 1: identified", "01:20:09 LPK25 port 2: keys", "01:21:40 sample amen_170 sent to track 12 (4.1 s)", "01:22:02 set night jam saved", "01:22:15 network MIDI: MacRene connected", "01:23:31 pattern A2 queued", "01:23:33 pattern A2"].join("<br>")}</div>`, true)
      + box("APC40 LEDs", `<div class="ledm">${Array.from({ length: 48 }, (_, k) => `<i style="${[0, 4, 8, 12, 16].includes(k) ? "background:#ff4b3e" : k >= 24 && k < 32 ? "background:#ffb833" : k >= 40 && k < 45 ? "background:#35d05a" : ""}"></i>`).join("")}</div>`)
      + box("Settings", `<div class="pg" style="grid-template-columns:1fr 1fr;height:80%">${["Remote buttons", "Start-up", "Sound on the TV", "Network name", "Flash firmware", "Classic monitor page"].map(t => `<div class="bigpad" style="font-size:15px">${t}</div>`).join("")}</div>`);
  },
};
(views[V] || views.perform)();
