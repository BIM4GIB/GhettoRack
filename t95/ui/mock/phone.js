// Phone (portrait) versions of the mock views: views.html?v=<name>&phone=1
// Same parts as views.js, laid out for 390x844 with no scrolling.
if (PHONE) {
  document.querySelector(".top").innerHTML = `<div class="tb">■</div><div class="tb play">▶</div><div class="tb rec">●</div>
    <div class="stat"><span class="big">120.0</span></div><div class="beats"><i class="on"></i><i></i><i></i><i></i></div>
    <div class="r2"><span>pos <b>3.2</b></span><span>pat <b>A1 → A2</b></span><span>set <b>night jam *</b></span><span class="sp"></span><span class="led">listen</span></div>`;
  const short = { perform: "Perf", steps: "Steps", mixer: "Mix", sound: "Sound", samples: "Smpl", patterns: "Patt", fx: "FX", system: "Sys" };
  document.getElementById("tabs").innerHTML = TABS.map(t => `<div class="tab ${t[0] === TABOF[V] ? "on" : ""}">${short[t[0]]}</div>`).join("");
}
const P8 = `<div class="sub"><span class="on">1-8</span><span>9-16</span></div>`;
const pcell = (label, txt, val, mode, col, size) => `<div class="cell">${pot(val, size || 64, mode, col)}<span class="val">${txt}</span><span class="n">${label}</span></div>`;
const phdr = (i, extra) => `<div class="hdr"><span class="big">${i + 1} · ${names[i]}</span>${extra || ""}<span class="sp"></span>
  <div class="btns" style="width:132px;grid-template-columns:repeat(3,1fr)">${btn("m", "M")}${btn("s", "S")}${btn("a", "ARM", kinds[i] === "L")}</div></div>`;
const typeSub = i => `<div class="sub"><span class="${kinds[i] === "s" ? "on" : ""}">sample</span><span class="${kinds[i] === "3" ? "on" : ""}">303</span><span class="${kinds[i] === "L" ? "on" : ""}">loop</span></div>`;
const vset = rows => { view.className = "v"; view.style.gridTemplateRows = rows; view.style.gridTemplateColumns = "1fr"; };
const bigsteps = (i, label) => `<div class="bigsteps" style="grid-template-rows:1fr 1fr">${Array.from({ length: 16 }, (_, k) => `<div class="bs ${Math.floor(k / 4) % 2 ? "" : "q"} ${acc[i].includes(k + 1) ? "ac" : pat[i].includes(k + 1) ? "on" : ""} ${k === 4 ? "ph" : ""}">${label ? label(k) : ""}<span>${k + 1}</span></div>`).join("")}</div>`;

window.PV = {
  perform() {
    vset("auto 1fr auto auto");
    const s = i => `<div class="st ${i === 0 ? "sel" : ""}" style="grid-template-rows:auto auto 1fr auto"><div class="nm"><b>${i + 1}</b>${names[i]}</div>
      ${knob("pan", pan[i], 36, "pan", AM)}
      <div class="fd" style="grid-template-columns:1fr 8px 1fr">${meter(lv[i])}<div class="slot"></div>${meter(lv[i] * .9)}<div class="cap" style="top:${(1 - fd[i]) * 84}%"></div></div>
      <div class="btns" style="grid-template-columns:1fr">${btn("m", "MUTE", [5, 6].includes(i))}${btn("s", "SOLO", i === 8)}${btn("a", "ARM", i === 11)}</div></div>`;
    view.innerHTML = `<div class="hdr">${P8}<span class="sp"></span><span class="lbl">tap a name to select</span></div>
      <div class="pst">${names.slice(0, 8).map((_, i) => s(i)).join("")}</div>
      <div class="box"><div class="pads">${Array.from({ length: 8 }, (_, i) => `<div class="pad ${i === 0 ? "cur" : i === 1 ? "next" : i < 4 ? "has" : ""}">A${i + 1}</div>`).join("")}</div></div>
      <div class="box"><div class="fx">${fxNames.map((f, i) => `<div class="pad ${i === 1 ? "on" : ""}">${f}</div>`).join("")}</div></div>`;
  },
  grid() {
    vset("auto 1fr");
    let g = `<div></div>${Array.from({ length: 16 }, (_, k) => `<div class="colh" style="font-size:9px">${k + 1}</div>`).join("")}`;
    names.forEach((n, i) => { g += `<div class="tn ${i === 0 ? "sel" : ""}"><b>${i + 1}</b>${n}</div>`
      + Array.from({ length: 16 }, (_, k) => `<div class="sc ${Math.floor(k / 4) % 2 ? "" : "q"} ${acc[i].includes(k + 1) ? "ac" : pat[i].includes(k + 1) ? "on" : ""} ${k === 4 ? "ph" : ""}"></div>`).join(""); });
    view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">all</span><span>one track</span></div><span class="big">A1</span><span class="sp"></span><div class="sub"><span>copy</span><span>clear</span><span>undo</span></div></div>
      <div class="grid" style="grid-template-rows:14px repeat(16,1fr)">${g}</div>`;
  },
  track() {
    vset("auto auto 90px 1.3fr 1fr"); const i = 0;
    view.innerHTML = phdr(i) + `<div class="hdr"><div class="sub"><span>all</span><span class="on">one track</span></div><span class="sp"></span><span class="lbl">tap: on · accent · off</span></div>
      <div class="wave">${wave("kick", { n: 130, played: .31, marks: [[.004, AM], [.31, "#ffb833", 2]] })}<span class="tag" style="left:6px;color:var(--dim)">kick.wav · 0.42 s</span></div>
      ${bigsteps(i, k => acc[i].includes(k + 1) ? "ACC" : pat[i].includes(k + 1) ? "ON" : "")}
      <div class="row4" style="grid-template-rows:1fr 1fr">${[["prob", "100%", 1, "vol", GR], ["swing", "50%", .5, "vol", GR], ["length", "16", 1, "vol", GR], ["volume", "78%", .78, "vol", GR], ["pitch", "0 st", .5, "pan", AM], ["decay", "full", 1, "vol", AM], ["filter", "off", .5, "pan", AM], ["start", "0%", 0, "vol", AM]].map(k => pcell(k[0], k[1], k[2], k[3], k[4], 50)).join("")}</div>`;
  },
  acid() {
    vset("auto 1.5fr 1fr"); const i = 8, notes = { 1: 36, 4: 36, 7: 48, 10: 39, 13: 43 }, slides = [4, 10], accs = [7];
    const nn = n => ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"][n % 12] + (Math.floor(n / 12) - 1);
    let r = ""; for (let n = 48; n >= 36; n--) r += `<div class="k ${nn(n).includes("#") ? "bk" : ""}">${nn(n)}</div>` + Array.from({ length: 16 }, (_, k) => `<div class="c ${Math.floor(k / 4) % 2 ? "" : "q"} ${notes[k + 1] === n ? (accs.includes(k + 1) ? "a" : "n") + (slides.includes(k + 1) ? " s" : "") : ""}"></div>`).join("");
    view.innerHTML = phdr(i, `<div class="sub"><span>oct -</span><span>oct +</span></div>`) + `<div class="roll">${r}</div>
      <div class="bigsteps" style="grid-template-rows:1fr 1fr">${Array.from({ length: 16 }, (_, k) => `<div class="bs ${Math.floor(k / 4) % 2 ? "" : "q"} ${accs.includes(k + 1) ? "ac" : notes[k + 1] ? "on" : ""} ${k === 4 ? "ph" : ""}">${slides.includes(k + 1) ? '<span class="sl">slide</span>' : ""}<span class="nt">${notes[k + 1] ? nn(notes[k + 1]) : ""}</span><span>${k + 1}</span></div>`).join("")}</div>`;
  },
  mixer() {
    vset("auto 1fr");
    const s = i => `<div class="st ${i === 0 ? "sel" : ""}" style="grid-template-rows:auto auto auto auto 1fr auto"><div class="nm"><b>${i + 1}</b>${names[i]}</div>
      ${knob("pan", pan[i], 36, "pan", AM)}${knob("rev", snd[i], 36, "vol", GR)}${knob("dly", snd[(i + 5) % 16], 36, "vol", GR)}
      <div class="fd" style="grid-template-columns:1fr 8px 1fr">${meter(lv[i])}<div class="slot"></div>${meter(lv[i] * .9)}<div class="cap" style="top:${(1 - fd[i]) * 84}%"></div></div>
      <div class="btns" style="grid-template-columns:1fr">${btn("m", "MUTE", [5, 6].includes(i))}${btn("s", "SOLO", i === 8)}</div></div>`;
    view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">1-8</span><span>9-16</span><span>master</span></div><span class="sp"></span><div class="sub"><span class="on">sends</span><span>send C · A/B</span></div></div>
      <div class="pst">${names.slice(0, 8).map((_, i) => s(i)).join("")}</div>`;
  },
  sound() {
    vset("auto auto 96px 1fr");
    view.innerHTML = phdr(0) + `<div class="hdr">${typeSub(0)}<span class="sp"></span><span class="lbl">808 · kick.wav</span></div>
      <div class="wave">${wave("kick", { n: 130, played: .31, marks: [[.004, AM], [.31, "#ffb833", 2]] })}<span class="tag" style="left:6px;color:var(--dim)">0.42 s · 48 kHz mono</span></div>
      <div class="row4" style="grid-template-rows:repeat(4,1fr)">${[["pitch", "0 st", .5, "pan"], ["decay", "full", 1, "vol"], ["filter", "off", .5, "pan"], ["reso", "20%", .2, "vol"], ["start", "0%", 0, "vol"], ["reverse", "fwd", 0, "vol"], ["drive", "0%", 0, "vol"], ["prob", "100%", 1, "vol"]].map(k => pcell(k[0], k[1], k[2], k[3], AM)).join("")}
        ${[["volume", "78%", .78, "vol"], ["pan", "C", .5, "pan"], ["reverb", "10%", .1, "vol"], ["delay", "35%", .35, "vol"], ["send C", "0%", 0, "vol"], ["xfade", "A", 0, "vol"], ["swing", "50%", .5, "vol"], ["length", "16", 1, "vol"]].map(k => pcell(k[0], k[1], k[2], k[3], GR)).join("")}</div>`;
  },
  sound303() {
    vset("auto auto 96px 1fr");
    const W = 1000, H = 200, fc = .42, q = .7; let d = "M0," + H * .35;
    for (let x = 0; x <= W; x += 10) { const r = x / W / fc; const mag = 1 / Math.sqrt((1 - r * r) ** 2 + (r / (1 + q * 6)) ** 2); d += ` L${x},${Math.max(6, H * .9 - Math.min(1.9, mag) * H * .28).toFixed(1)}`; }
    view.innerHTML = phdr(8) + `<div class="hdr">${typeSub(8)}<span class="sp"></span><span class="lbl">Pocket 303</span></div>
      <div class="wave"><svg viewBox="0 0 ${W} ${H}" preserveAspectRatio="none"><path d="${d} L${W},${H} L0,${H} Z" fill="#ff9a1f22"/><path d="${d}" fill="none" stroke="#ff9a1f" stroke-width="2.5" vector-effect="non-scaling-stroke"/></svg><span class="tag" style="left:6px;color:var(--amber)">cutoff 1.2 kHz · reso 70%</span></div>
      <div class="row4" style="grid-template-rows:repeat(4,1fr)">${[["cutoff", "1.2k", .42, "vol"], ["reso", "70%", .7, "vol"], ["env mod", "55%", .55, "vol"], ["decay", "320ms", .5, "vol"], ["accent", "60%", .6, "vol"], ["tune", "+0.0", .5, "pan"], ["slide", "60ms", .5, "vol"], ["wave", "saw", 0, "vol"]].map(k => pcell(k[0], k[1], k[2], k[3], AM)).join("")}
        ${[["volume", "85%", .85, "vol"], ["pan", "C", .5, "pan"], ["reverb", "15%", .15, "vol"], ["delay", "40%", .4, "vol"], ["send C", "0%", 0, "vol"], ["xfade", "A+B", .5, "pan"], ["drive", "30%", .3, "vol"], ["filter", "off", .5, "pan"]].map(k => pcell(k[0], k[1], k[2], k[3], GR)).join("")}</div>`;
  },
  loop() {
    vset("auto auto 1fr 150px 120px");
    view.innerHTML = phdr(11) + `<div class="hdr">${typeSub(11)}<span class="sp"></span><span class="lbl">4 bars · 8.0 s</span></div>
      <div class="wave">${wave("loop", { n: 150, played: .29, bars: 4, marks: [[.29, "#ffb833", 2]] })}<span class="tag" style="left:6px;color:var(--dim)">bar 1</span><span class="tag" style="left:26%;color:var(--dim)">2</span><span class="tag" style="left:51%;color:var(--dim)">3</span><span class="tag" style="left:76%;color:var(--dim)">4</span></div>
      <div style="display:grid;grid-template-columns:130px 1fr;gap:8px;min-height:0"><div class="cell"><div class="recbtn">ARMED</div><span class="n">starts on the next bar</span></div>
        <div class="cell" style="flex-direction:row;gap:14px">${knob("input", .6, 84, "vol", GR)}<div style="display:flex;gap:6px;height:100px">${meter(.6)}${meter(.5)}</div><div class="kv"><span>state</span><span style="color:var(--red)">armed</span><span>bars</span><span>0</span></div></div></div>
      <div class="pg" style="grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr">${["STOP AT BAR", "CANCEL", "CLEAR LOOP", "BACK TO STEPS"].map(t => `<div class="bigpad">${t}</div>`).join("")}</div>`;
  },
  samples() {
    vset("auto auto 1fr 110px");
    const files = [["📁 breaks/", null], ["📁 uploads/", null], ["amen_170.wav · 5.6 s", "loop"], ["kick_long.wav · 0.9 s", "kick"], ["vox_hey.wav · 1.2 s", "voice"], ["hat_open_01.flac · 0.4 s", "hat"], ["rec 23-11 01.20 · 3.1 s", "voice"], ["ride_bell.aif · 1.8 s", "hat"]];
    view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">library</span><span>SD</span><span>record</span><span>kits</span></div></div>
      <div class="hdr"><span class="lbl">sends to</span><span class="big">1 · kick</span><span class="sp"></span><div class="sub"><span>upload</span></div></div>
      <div class="list">${files.map((f, i) => `<div class="li ${i === 2 ? "sel" : ""}"><span class="nm2">${f[0]}</span>${f[1] ? wave(f[1], { n: 90, played: i === 2 ? .4 : -1 }) : "<span></span>"}
        <div class="act">${f[1] ? btn("act", "▶", i === 2, "padding:0 9px") + btn("", "send", false, "padding:0 9px") : btn("", "open", false, "padding:0 12px")}</div></div>`).join("")}</div>
      <div class="wave">${wave("loop", { n: 150, played: .4, marks: [[.4, "#ffb833", 2]] })}<span class="tag" style="left:6px;color:var(--green)">▶ amen_170.wav</span></div>`;
  },
  sampler() {
    vset("auto auto 1fr 150px 120px");
    view.innerHTML = `<div class="hdr"><div class="sub"><span>library</span><span>SD</span><span class="on">record</span><span>kits</span></div></div>
      <div class="hdr"><div class="sub"><span>Daisy input</span><span class="on">phone mic</span></div><span class="sp"></span><span class="lbl">to</span><span class="big">14 · vox</span></div>
      <div class="wave">${wave("voice", { n: 120, col: "#ff4b3e", dim: "#ff4b3e", played: 2, marks: [[.08, AM], [.86, AM]] })}<span class="tag" style="left:6px;color:var(--red)">● 0:03.1</span></div>
      <div style="display:grid;grid-template-columns:130px 1fr;gap:8px;min-height:0"><div class="cell"><div class="recbtn">STOP</div></div>
        <div class="cell" style="flex-direction:row;gap:12px"><div style="display:flex;gap:6px;height:100px">${meter(.75)}${meter(.7)}</div>${knob("trim start", .08, 72, "vol", AM)}${knob("trim end", .86, 72, "vol", AM)}</div></div>
      <div class="pg" style="grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr">${["▶ PLAY", "NORMALISE", "SAVE TO LIBRARY", "SEND TO TRACK"].map((t, i) => `<div class="bigpad ${i === 3 ? "cur" : ""}">${t}</div>`).join("")}</div>`;
  },
  patterns() {
    vset("auto 1fr auto");
    const has = [0, 1, 2, 3, 8, 9, 16, 24, 25, 26];
    view.innerHTML = `<div class="hdr"><div class="sub"><span class="on">patterns</span><span>sets</span></div><span class="big">A1</span><span class="big" style="color:var(--green)">→ A2</span><span class="sp"></span></div>
      <div class="pg" style="grid-template-columns:18px repeat(8,1fr);grid-template-rows:repeat(8,1fr);gap:4px">${"ABCDEFGH".split("").map((b, r) => `<div class="colh" style="align-self:center">${b}</div>` + Array.from({ length: 8 }, (_, c) => { const p = r * 8 + c;
        return `<div class="bigpad ${p === 0 ? "cur" : p === 1 ? "next" : has.includes(p) ? "has" : ""}">${b}${c + 1}</div>`; }).join("")).join("")}</div>
      <div class="pg" style="grid-template-columns:repeat(3,1fr);height:44px">${["COPY", "CLEAR", "UNDO"].map(t => `<div class="bigpad">${t}</div>`).join("")}</div>`;
  },
  fx() {
    vset("1.1fr 1.6fr 1fr");
    view.innerHTML = `<div class="pg" style="grid-template-columns:repeat(4,1fr);grid-template-rows:1fr 1fr">${fxNames.map((f, i) => `<div class="bigpad fx ${i === 1 ? "on" : ""}">${f}<small style="color:inherit">hold</small></div>`).join("")}</div>
      <div class="row4" style="grid-template-rows:1fr 1fr">${[["rev size", "55%", .55, "vol", GR], ["rev tone", "60%", .6, "vol", GR], ["dly time", "1/4", .5, "vol", GR], ["dly fdbk", "35%", .35, "vol", GR], ["fx return", "80%", .8, "vol", GR], ["filter", "off", .5, "pan", BL], ["drive", "0%", 0, "vol", AM], ["cue filter", "off", 0, "vol", BL]].map(k => pcell(k[0], k[1], k[2], k[3], k[4], 72)).join("")}</div>
      <div class="cell" style="flex-direction:row;gap:16px">${knob("master", .8, 84, "vol", GR)}${knob("xfade", .5, 84, "pan", AM)}${knob("swing", .5, 84, "vol", AM)}<div style="display:flex;gap:6px;height:100px">${meter(.8)}${meter(.75)}</div></div>`;
  },
  system() {
    vset("auto 1fr auto");
    const box = (t, inner) => `<div class="box" style="overflow:hidden;min-height:0"><h3>${t}</h3>${inner}</div>`;
    view.innerHTML = box("Links", `<div class="kv"><span>Daisy</span><span style="color:var(--green)">connected · 13.9</span><span>ping</span><span>avg 2.4 · p99 9 ms</span><span>controllers</span><span>APC40, LPK25</span>
        <span>network MIDI</span><span>1 session</span><span>clock</span><span>internal 120.0</span><span>CPU</span><span>10% · max 20%</span><span>memory</span><span>93% of sample RAM</span></div>`)
      + box("Log", `<div class="log">${["01:20:03 connected to the T95 bridge", "01:20:07 APC40 port 1: identified", "01:20:09 LPK25 port 2: keys", "01:21:40 amen_170 sent to track 12 (4.1 s)", "01:22:02 set night jam saved", "01:22:15 network MIDI: MacRene connected", "01:23:31 pattern A2 queued"].join("<br>")}</div>`)
      + `<div class="pg" style="grid-template-columns:1fr 1fr;grid-template-rows:repeat(3,44px)">${["Remote buttons", "Start-up", "Sound on the TV", "Network name", "OLED + LED mirror", "Classic page"].map(t => `<div class="bigpad">${t}</div>`).join("")}</div>`;
  },
};
run();
