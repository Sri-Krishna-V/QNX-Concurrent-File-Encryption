/* app.js - the workflow page. All state lives in the backend; this page shows it
 * (pushed over /api/stream) and sends clicks back as POST requests. */
"use strict";

const $ = (sel, root = document) => root.querySelector(sel);
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));

let WF = [];            // step definitions from /api/workflow
let PI = {};            // {host, user, dir}
let S = null;           // latest state
const cards = {};       // step id -> DOM refs
const chosen = {};      // step id -> {option: value}
const shownResult = {}; // step id -> JSON of the result on screen
const userOpen = {};    // step id -> true/false once the user opened or folded its results
let activeTab = "workflow";
let analyticsKey = "";
let benchTableOpen = false;   // the statistics table under the scaling charts

/* ------------------------------------------------------------ formatting */

function fmtMs(ms) {
  if (ms == null || isNaN(ms)) return "-";
  if (ms >= 1000) return (ms / 1000).toFixed(2) + " s";
  if (ms >= 100) return ms.toFixed(0) + " ms";
  if (ms >= 10) return ms.toFixed(1) + " ms";
  return ms.toFixed(2) + " ms";
}
function fmtBytes(n) {
  if (n == null) return "-";
  if (n >= 1 << 30) return (n / (1 << 30)).toFixed(2) + " GB";
  if (n >= 1 << 20) return (n / (1 << 20)).toFixed(1) + " MB";
  if (n >= 1024) return (n / 1024).toFixed(1) + " KB";
  return n + " B";
}
function fmtSecs(s) {
  if (s == null) return "";
  if (s < 60) return s.toFixed(1) + " s";
  return Math.floor(s / 60) + " min " + Math.round(s % 60) + " s";
}
function clock(t) {
  return t ? new Date(t * 1000).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit", second: "2-digit" }) : "";
}

function toast(msg) {
  const t = $("#toast");
  t.textContent = msg;
  t.hidden = false;
  clearTimeout(toast.timer);
  toast.timer = setTimeout(() => { t.hidden = true; }, 3500);
}

async function post(path, body = {}) {
  try {
    const r = await fetch(path, { method: "POST", headers: { "Content-Type": "application/json" }, body: JSON.stringify(body) });
    const data = await r.json().catch(() => ({}));
    if (!r.ok) toast(data.message || data.error || `request failed (${r.status})`);
    return { ok: r.ok, data };
  } catch (e) {
    toast("The dashboard backend is not answering. Is its window still open?");
    return { ok: false, data: {} };
  }
}

/* ------------------------------------------------------------ building the step cards */

function buildCards() {
  const root = $("#steps");
  root.innerHTML = "";
  const rail = $("#rail");
  rail.innerHTML = "";
  let phase = null, section = null;
  for (const step of WF) {
    if (step.phase !== phase) {
      phase = step.phase;
      section = document.createElement("section");
      section.className = "phase";
      section.innerHTML = `<h2>${esc(phase)}</h2>`;
      root.appendChild(section);
    }
    chosen[step.id] = Object.fromEntries(step.options.map(o => [o.name, o.default]));
    const card = document.createElement("article");
    card.className = "step";
    card.id = "step-" + step.id;
    card.innerHTML = `
      <div class="step-head">
        <span class="step-num">${step.n}</span>
        <div class="titles"><h3>${esc(step.title)}</h3><p>${esc(step.summary)}</p></div>
        <div class="status"></div>
      </div>
      <div class="step-controls">
        ${step.options.map(o => optionHtml(step, o)).join("")}
        <span class="controls-spacer"></span>
        <span class="lock" hidden></span>
        <button class="btn ghost" data-act="toggle" type="button" hidden>Results</button>
        <button class="btn" data-act="console"type="button" title="Open this step's output in a command-prompt window">View output</button>
        <button class="btn danger" data-act="cancel" type="button" hidden>Cancel</button>
        <button class="btn primary" data-act="run" type="button">Run</button>
      </div>
      <div class="step-error" hidden></div>
      <div class="result" hidden></div>`;
    section.appendChild(card);

    card.querySelectorAll("[data-opt]").forEach(input => {
      input.addEventListener("change", () => {
        const o = step.options.find(x => x.name === input.dataset.opt);
        chosen[step.id][o.name] = o.type === "check" ? input.checked : input.value;
      });
    });
    card.querySelector('[data-act="run"]').addEventListener("click", () => runStep(step.id));
    card.querySelector('[data-act="cancel"]').addEventListener("click", () => post("/api/cancel"));
    card.querySelector('[data-act="toggle"]').addEventListener("click", () => {
      userOpen[step.id] = cards[step.id].result.hidden;
      render();
    });
    card.querySelector('[data-act="console"]').addEventListener("click", async () => {
      const r = await post(`/api/steps/${step.id}/console`);
      if (r.ok) toast(`Output of step ${step.n} opened in a command-prompt window`);
    });
    cards[step.id] = {
      card, num: $(".step-num", card), status: $(".status", card), lock: $(".lock", card),
      run: $('[data-act="run"]', card), cancel: $('[data-act="cancel"]', card),
      console: $('[data-act="console"]', card), toggle: $('[data-act="toggle"]', card),
      error: $(".step-error", card), result: $(".result", card),
      inputs: card.querySelectorAll("[data-opt]"),
    };

    const li = document.createElement("li");
    li.innerHTML = `<button type="button" title="${esc(step.title)}"><span class="bar"></span><span class="lbl">${step.n}. ${esc(step.title)}</span></button>`;
    li.querySelector("button").addEventListener("click", () => card.scrollIntoView({ behavior: "smooth", block: "center" }));
    rail.appendChild(li);
    cards[step.id].rail = li;
  }
}

function optionHtml(step, o) {
  if (o.type === "check") {
    return `<label class="opt"><input type="checkbox" data-opt="${o.name}" ${o.default ? "checked" : ""}> ${esc(o.label)}</label>`;
  }
  const opts = o.choices.map(c => `<option value="${esc(c.value)}" ${String(c.value) === String(o.default) ? "selected" : ""}>${esc(c.label)}</option>`).join("");
  return `<label class="opt">${esc(o.label)} <select data-opt="${o.name}">${opts}</select></label>`;
}

async function runStep(id) {
  const r = await post(`/api/steps/${id}/run`, { options: chosen[id] });
  if (r.ok && S && S.settings && S.settings.auto_console) {
    toast("Running - follow the work in the console window");
  }
}

/* ------------------------------------------------------------ rendering the state */

function nextStep() {
  // The first step not done yet that can run; "Stop and clean up" only once everything else is done.
  for (const pass of [false, true]) {
    for (const step of WF) {
      const st = S.steps[step.id];
      if ((step.id === "finish") !== pass) continue;
      if ((!st || st.state !== "done") && !S.blocked[step.id]) return step.id;
    }
  }
  return null;
}

function latestFinished() {
  let best = null, t = 0;
  for (const [id, st] of Object.entries(S.steps)) {
    if (st.finished && st.finished > t) { t = st.finished; best = id; }
  }
  return best;
}

function render() {
  if (!S || !WF.length) return;
  renderHeader();
  const next = S.busy ? null : nextStep();
  const latest = latestFinished();
  let done = 0;
  for (const step of WF) {
    const c = cards[step.id];
    const st = S.steps[step.id] || null;
    const state = st ? st.state : "idle";
    const reason = S.blocked[step.id];
    const running = state === "running";
    if (state === "done") done++;

    c.card.className = "step " + state + (reason && !running && !S.busy ? " locked" : "") + (next === step.id ? " next" : "");
    c.rail.className = state + (next === step.id ? " next" : "");
    c.num.textContent = state === "done" ? "✓" : state === "failed" ? "!" : step.n;

    c.status.innerHTML = statusHtml(st);
    c.run.hidden = running;
    c.run.disabled = !!reason;
    c.run.textContent = st && st.state !== "running" ? "Run again" : "Run";
    if (next === step.id) c.run.textContent = "Run";
    c.cancel.hidden = !running;
    c.console.hidden = !st || !S.settings.consoles_available;
    c.inputs.forEach(i => { i.disabled = !!S.busy; });
    const showLock = reason && !S.busy && !running;
    c.lock.hidden = !showLock;
    c.lock.textContent = showLock ? reason : "";

    if (st && st.error && state !== "running") {
      c.error.hidden = false;
      c.error.innerHTML = state === "cancelled" ? "<strong>Cancelled.</strong> Run the step again when you are ready."
        : `<strong>Failed:</strong> ${esc(st.error)}`;
    } else {
      c.error.hidden = true;
    }
    const key = st && st.result ? JSON.stringify(st.result) : "";
    if (key !== shownResult[step.id]) {
      if (shownResult[step.id] !== undefined) delete userOpen[step.id];   // new result: back to automatic
      shownResult[step.id] = key;
      c.result.innerHTML = key ? resultHtml(st.result) : "";
    }
    // Results stay on screen for the latest step; the others fold away behind "Results".
    const open = step.id in userOpen ? userOpen[step.id] : step.id === latest;
    c.result.hidden = !key || running || !open;
    c.toggle.hidden = !key || running;
    c.toggle.textContent = open ? "Hide results" : "Results";
  }
  $("#progressCount").textContent = `${done}/${WF.length}`;
  const hasData = analyticsAvailable();
  $("#analyticsDot").hidden = !hasData || activeTab === "analytics";
  if (activeTab === "analytics") renderAnalytics();
}

function statusHtml(st) {
  if (!st) return "";
  if (st.state === "running") {
    return `<span class="badge run"><span class="spinner"></span>Running <span data-since="${st.started}">${fmtSecs(Date.now() / 1000 - st.started)}</span></span>`;
  }
  const when = clock(st.finished);
  if (st.state === "done") return `<span class="badge ok">✓ Done</span><br>${fmtSecs(st.seconds)} · ${when}`;
  if (st.state === "failed") return `<span class="badge bad">✕ Failed</span><br>${fmtSecs(st.seconds)} · ${when}`;
  if (st.state === "cancelled") return `<span class="badge">Cancelled</span><br>${when}`;
  return "";
}

function resultHtml(r) {
  let html = "";
  if (r.facts && r.facts.length) {
    html += `<dl class="facts">${r.facts.map(f => `<div><dt>${esc(f.label)}</dt><dd>${esc(f.value)}</dd></div>`).join("")}</dl>`;
  }
  for (const t of r.tables || []) {
    html += `<h4>${esc(t.title)}</h4>` + tableHtml(t.columns, t.rows);
  }
  for (const t of r.texts || []) {
    html += `<h4>${esc(t.title)}</h4><pre class="block">${esc(t.text)}</pre>`;
  }
  return html;
}

function tableHtml(columns, rows) {
  const isNum = v => typeof v === "number" || /^-?[\d.,]+\s*(x|ms|s|MB\/s|KB|MB|B|GB)?$/.test(String(v).trim());
  const numCol = columns.map((_, i) => rows.length > 0 && rows.every(r => r[i] == null || r[i] === "-" || isNum(r[i])));
  const cell = (v, i) => {
    const s = String(v ?? "-");
    const cls = numCol[i] ? "num" : (s === "yes" || s === "same size" ? "yes" : (s === "NO" || s === "DIFFERENT" ? "no" : ""));
    return `<td class="${cls}">${esc(s)}</td>`;
  };
  return `<div class="table-wrap"><table class="data"><thead><tr>${columns.map((c, i) => `<th class="${numCol[i] ? "num" : ""}">${esc(c)}</th>`).join("")}</tr></thead>
    <tbody>${rows.map(r => `<tr>${r.map(cell).join("")}</tr>`).join("")}</tbody></table></div>`;
}

function renderHeader() {
  const piChip = $("#chipPi"), srvChip = $("#chipServer");
  const connect = S.steps.connect;
  if (S.pi) {
    setChip(piChip, "ok", `Pi ${S.pi.host} · QNX ${S.pi.release || ""} · ${S.pi.cpus || "?"} CPUs`);
  } else if (connect && connect.state === "failed") {
    setChip(piChip, "bad", `Pi ${PI.host} not reachable`);
  } else if (connect && connect.state === "running") {
    setChip(piChip, "busy", `Connecting to ${PI.host}…`);
  } else {
    setChip(piChip, "", "Pi not connected");
  }
  const sv = S.server;
  if (sv.running && sv.external) setChip(srvChip, "busy", "encserver running (started outside the dashboard)");
  else if (sv.running) setChip(srvChip, "ok", `encserver · ${sv.workers} workers · ${S.jobs.count} Jobs`);
  else if (sv.note) setChip(srvChip, "", `encserver stopped`);
  else setChip(srvChip, "", "encserver not started");
  srvChip.title = sv.note || "";

  const auto = $("#autoConsole");
  auto.checked = !!S.settings.auto_console;
  auto.disabled = !S.settings.consoles_available;
  $("#btnConsole").disabled = !S.settings.consoles_available;
  $("#btnServerLog").disabled = !S.settings.consoles_available || !S.steps.server;
  $("#btnReset").disabled = !!S.busy;
  $("#target").textContent = `Raspberry Pi 4B · QNX 8.0 · ${PI.user}@${PI.host}:${PI.dir}`;
}

function setChip(chip, cls, label) {
  chip.className = "chip " + cls;
  $(".chip-text", chip).textContent = label;
}

/* ------------------------------------------------------------ analytics */

function analyticsAvailable() {
  if (!S) return false;
  const A = S.analytics;
  return !!(A.benchmark || A.roundtrips.length || A.inflight.length || A.multiclient.length || S.jobs.count);
}

function renderAnalytics(force) {
  const root = $("#analytics");
  const key = JSON.stringify([S.analytics, S.jobs.count, S.jobs.per_worker, S.jobs.failed, root.clientWidth]);
  if (!force && key === analyticsKey) return;
  analyticsKey = key;
  const A = S.analytics;

  if (!analyticsAvailable()) {
    const item = (label, step, have) => `<li class="${have ? "have" : ""}"><span>${label}</span><span>${have ? "measured" : "step " + step}</span></li>`;
    root.innerHTML = `<div class="empty"><h2>No measurements yet</h2>
      <p>Every number on this page comes from a step you run on the Pi. Nothing is shown until then.</p>
      <ul>${item("Worker load balance (live encserver)", 5, false)}${item("File round-trips", 6, false)}${item("Jobs in parallel", 7, false)}${item("Several clients at once", 8, false)}
      ${item("Worker Pool scaling", 9, false)}</ul></div>`;
    return;
  }

  let html = tilesHtml();
  if (A.benchmark) html += benchmarkHtml(A.benchmark);
  if (S.jobs.count) html += jobsHtml();
  if (A.inflight.length || A.multiclient.length) html += concurrencyHtml();
  if (A.roundtrips.length) html += roundtripHtml(A.roundtrips);
  html += `<p style="color:var(--muted);font-size:12.5px">Session ${esc(S.session.id)} ·
    <a class="linkbtn" href="/api/session.json" download>Download all results (JSON)</a></p>`;
  root.innerHTML = html;

  // charts need the laid-out width, so draw them after the HTML is in place
  if (A.benchmark) drawBenchmark(A.benchmark);
  if (S.jobs.count) drawJobs();
  if (A.inflight.length) drawInflight(A.inflight[0]);
  if (A.multiclient.length) drawMulticlient(A.multiclient[0]);
  const toggle = $("#benchTableToggle");
  if (toggle) {
    const show = () => {
      $("#benchTable").hidden = !benchTableOpen;
      toggle.textContent = benchTableOpen ? "Hide table" : "Show table";
    };
    show();                         // keep the choice when new data redraws the page
    toggle.addEventListener("click", () => { benchTableOpen = !benchTableOpen; show(); });
  }
}

function tilesHtml() {
  const A = S.analytics, tiles = [];
  if (A.benchmark) {
    const best = A.benchmark.stats.reduce((a, b) => (b.speedup > a.speedup ? b : a));
    const fast = A.benchmark.stats.reduce((a, b) => (b.mb_per_s > a.mb_per_s ? b : a));
    tiles.push(["Best speedup", best.speedup.toFixed(2) + "×", `with ${best.workers} workers vs 1`]);
    tiles.push(["Fastest encryption", fast.mb_per_s.toFixed(1) + " MB/s", `${fast.workers} workers, ${fmtBytes(fast.bytes)} Data Block`]);
  }
  if (S.jobs.count) {
    tiles.push(["Jobs encrypted", S.jobs.count.toLocaleString(), `${fmtBytes(S.jobs.bytes)} by the live encserver, ${S.jobs.pids.length} client process${S.jobs.pids.length === 1 ? "" : "es"}`]);
    tiles.push(["Most Jobs at once", String(S.jobs.peak_in_progress), "held by encserver at the same time"]);
  }
  if (A.roundtrips.length) {
    const ok = A.roundtrips.filter(r => r.ok).length;
    tiles.push(["Files identical after decryption", `${ok}/${A.roundtrips.length}`, "byte-for-byte, every block verified"]);
  }
  return `<div class="tiles">${tiles.map(([l, v, s]) => `<div class="tile"><div class="label">${esc(l)}</div><div class="value">${esc(v)}</div><div class="sub">${esc(s)}</div></div>`).join("")}</div>`;
}

function benchmarkHtml(b) {
  const rows = b.stats.map(r => [r.workers, r.segments, r.runs, fmtMs(r.enc_mean), fmtMs(r.enc_median), fmtMs(r.enc_min), fmtMs(r.enc_max),
    fmtMs(r.enc_stdev), fmtMs(r.rtt_mean), r.mb_per_s.toFixed(1), r.speedup.toFixed(2) + "x", (r.efficiency * 100).toFixed(0) + " %"]);
  return `<section class="panel">
    <div class="panel-head"><h2>Worker Pool scaling</h2>
      <span class="meta">${b.rows} measured runs · ${fmtBytes(b.size)} Data Block · ${b.runs} runs per size · ${clock(Date.parse(b.at) / 1000)}</span>
      <span class="spacer"></span>
      <button class="linkbtn" id="benchTableToggle" type="button">Show table</button>
      <a class="linkbtn" href="/api/benchmark.csv" download>results.csv</a></div>
    <p class="lead">Encryption Time on the Pi for each Worker Pool size. Every size ran on a fresh encserver after 5 warm-up requests;
      speedup is the mean time with 1 worker divided by the mean time with N workers.</p>
    <div class="grid-3">
      <div class="chart"><h3>Speedup</h3><div class="legend"><span class="key"><span class="sw" style="background:var(--series-1)"></span>measured</span><span class="key"><span class="sw ref"></span>ideal (N×)</span></div><div id="chSpeedup"></div></div>
      <div class="chart"><h3>Mean Encryption Time</h3><div class="note">whisker: fastest to slowest run</div><div id="chEnc"></div></div>
      <div class="chart"><h3>Throughput</h3><div class="note">MB encrypted per second (mean)</div><div id="chMbs"></div></div>
    </div>
    <div id="benchTable" hidden><h4 style="margin:16px 0 6px;font-size:12.5px;color:var(--ink-2)">All statistics</h4>
      ${tableHtml(["Workers", "Segments", "Runs", "Mean", "Median", "Min", "Max", "Std dev", "Round trip", "MB/s", "Speedup", "Efficiency"], rows)}</div>
  </section>`;
}

function drawBenchmark(b) {
  const xs = b.stats.map(r => r.workers);
  Charts.line($("#chSpeedup"), {
    xs, xLabel: "Workers: ", series: [{ name: "measured", values: b.stats.map(r => r.speedup), color: "var(--series-1)" }],
    reference: { name: "ideal", values: xs.map(w => w) }, format: v => v.toFixed(2) + "×", tickFormat: v => v + "×",
  });
  Charts.columns($("#chEnc"), {
    categories: xs.map(w => w + (w === 1 ? " worker" : " workers")), values: b.stats.map(r => r.enc_mean),
    whiskers: b.stats.map(r => [r.enc_min, r.enc_max]), format: v => fmtMs(v), tickFormat: v => v + " ms",
    tips: b.stats.map(r => `<b>${r.workers} worker${r.workers > 1 ? "s" : ""}</b><br>mean ${fmtMs(r.enc_mean)}<br>min ${fmtMs(r.enc_min)} · max ${fmtMs(r.enc_max)}<br>${r.segments} Segments, ${r.runs} runs`),
  });
  Charts.columns($("#chMbs"), {
    categories: xs.map(w => String(w)), values: b.stats.map(r => r.mb_per_s), format: v => v.toFixed(1), tickFormat: v => String(v),
    tips: b.stats.map(r => `<b>${r.workers} worker${r.workers > 1 ? "s" : ""}</b><br>${r.mb_per_s.toFixed(1)} MB/s<br>efficiency ${(r.efficiency * 100).toFixed(0)} %`),
  });
}

function jobsHtml() {
  const J = S.jobs;
  const recent = J.list.filter(j => j.state === "done").slice(-12).reverse();
  const maxSeg = Math.max(1, ...recent.flatMap(j => j.per_worker || [0]));
  const rows = recent.map(j => `<tr><td class="num">#${j.id}</td><td class="num">${j.pid}</td><td class="num">${esc(j.size)}</td>
    <td class="num">${j.segments}</td><td class="num">${fmtMs(j.ms)}</td><td class="num">${j.in_progress ?? "-"}</td>
    <td><span class="mini-bars" data-tip="${esc((j.per_worker || []).map((n, i) => `w${i}: ${n}`).join("<br>"))}">${(j.per_worker || []).map(n => `<i style="height:${Math.max(2, (n / maxSeg) * 16)}px"></i>`).join("")}</span></td></tr>`).join("");
  return `<section class="panel">
    <div class="panel-head"><h2>Load balance across the Worker Pool</h2>
      <span class="meta">live encserver · ${J.count} Jobs · ${J.failed} failed</span></div>
    <p class="lead">Each Job's Data Block is split into Segments that idle workers take from one shared queue,
      so the work spreads across the workers without a fixed assignment.</p>
    <div class="grid-2">
      <div class="chart"><h3>Segments encrypted per worker</h3><div class="note">all Jobs of this session</div><div id="chWorkers"></div></div>
      <div class="chart"><h3>Latest Jobs</h3><div class="note">"At once": Jobs encserver held when this one arrived, itself included</div>
        <div class="table-wrap"><table class="data"><thead><tr><th class="num">Job</th><th class="num">Client pid</th><th class="num">Size</th>
        <th class="num">Segments</th><th class="num">Time</th><th class="num">At once</th><th>Per worker</th></tr></thead><tbody>${rows}</tbody></table></div></div>
    </div></section>`;
}

function drawJobs() {
  const pw = S.jobs.per_worker;
  const total = pw.reduce((a, b) => a + b, 0) || 1;
  Charts.columns($("#chWorkers"), {
    categories: pw.map((_, i) => "w" + i), values: pw, format: v => String(v),
    tips: pw.map((n, i) => `<b>worker ${i}</b><br>${n} Segments (${((n / total) * 100).toFixed(1)} %)`),
  });
}

function concurrencyHtml() {
  const A = S.analytics;
  let parts = "";
  if (A.inflight.length) {
    const e = A.inflight[0];
    const last = e.runs[e.runs.length - 1];
    parts += `<div class="chart"><h3>${e.size_mb} MB file: Data Blocks in flight</h3>
      <div class="note">${e.speedup ? `${e.speedup.toFixed(2)}× faster with ${last.in_flight} in flight` : ""}${last.peak ? ` · up to ${last.peak} Jobs at once on encserver` : ""}</div>
      <div id="chInflight"></div></div>`;
  }
  if (A.multiclient.length) {
    const m = A.multiclient[0];
    parts += `<div class="chart"><h3>${m.clients} clients at once: Round-Trip Time</h3>
      <div class="legend"><span class="key"><span class="sw dot" style="background:var(--muted)"></span>one client alone</span><span class="key"><span class="sw dot" style="background:var(--series-1)"></span>clients together</span></div>
      <div class="note">${m.pids} client processes seen by encserver${m.peak ? `, up to ${m.peak} Jobs at once` : ""}</div>
      <div id="chMulti"></div></div>`;
  }
  return `<section class="panel"><div class="panel-head"><h2>Concurrency</h2></div>
    <p class="lead">Several Jobs in progress on encserver at once: one client sending several Data Blocks, and several clients at the same time.</p>
    <div class="grid-2">${parts}</div></section>`;
}

function drawInflight(e) {
  Charts.hbars($("#chInflight"), {
    labels: e.runs.map(r => r.label), values: e.runs.map(r => r.wall_ms), format: fmtMs, tickFormat: v => v + " ms",
    tips: e.runs.map(r => `<b>${r.label}</b><br>wall time ${fmtMs(r.wall_ms)}<br>${r.mb_per_s ? r.mb_per_s.toFixed(1) + " MB/s" : ""}<br>${r.blocks} Data Blocks, ${r.segments} Segments`),
  });
}

function drawMulticlient(m) {
  const labels = ["alone"].concat(m.together.map(c => "client " + c.client));
  const values = [m.solo.rtt_mean_ms].concat(m.together.map(c => c.rtt_mean_ms || 0));
  Charts.hbars($("#chMulti"), {
    labels, values, format: fmtMs, tickFormat: v => v + " ms",
    colors: ["var(--muted)"].concat(m.together.map(() => "var(--series-1)")),
    tips: labels.map((l, i) => `<b>${l}</b><br>mean round trip ${fmtMs(values[i])}<br>${(values[i] / values[0]).toFixed(2)}× the single client`),
  });
}

function roundtripHtml(list) {
  const rows = list.map(r => [r.file, fmtBytes(r.bytes), r.blocks ?? "-", r.segments ?? "-", fmtMs(r.server_ms), fmtMs(r.enc_wall_ms), fmtMs(r.dec_wall_ms), r.identical ? "yes" : "NO"]);
  return `<section class="panel"><div class="panel-head"><h2>File round-trips</h2><span class="meta">encrypt on encserver, decrypt on the client, compare</span></div>
    ${tableHtml(["File", "Size", "Data Blocks", "Segments", "Server encryption", "Client encrypt (wall)", "Client decrypt", "Identical"], rows)}</section>`;
}

/* ------------------------------------------------------------ wiring */

function switchTab(name) {
  activeTab = name;
  document.querySelectorAll(".tab").forEach(t => {
    const on = t.dataset.tab === name;
    t.classList.toggle("active", on);
    t.setAttribute("aria-selected", on);
  });
  $("#tab-workflow").hidden = name !== "workflow";
  $("#tab-analytics").hidden = name !== "analytics";
  if (name === "analytics" && S) renderAnalytics(true);
  render();
}

function tick() {
  document.querySelectorAll("[data-since]").forEach(e => {
    e.textContent = fmtSecs(Date.now() / 1000 - parseFloat(e.dataset.since));
  });
}

function connectStream() {
  const es = new EventSource("/api/stream");
  es.addEventListener("state", ev => {
    S = JSON.parse(ev.data);
    render();
  });
  es.onerror = () => {
    setChip($("#chipPi"), "bad", "Dashboard backend not answering");
  };
}

async function init() {
  Charts.initTooltip($("#tooltip"));
  document.querySelectorAll(".tab").forEach(t => t.addEventListener("click", () => switchTab(t.dataset.tab)));
  $("#autoConsole").addEventListener("change", e => post("/api/settings", { auto_console: e.target.checked }));
  $("#btnConsole").addEventListener("click", async () => {
    const r = await post("/api/console/activity");
    if (r.ok) toast("Console window opened");
  });
  $("#btnServerLog").addEventListener("click", () => post("/api/console/server"));
  $("#btnReset").addEventListener("click", async () => {
    if (!confirm("Start over? This clears every result on the page and stops encserver if the dashboard started it.")) return;
    const r = await post("/api/reset");
    if (r.ok) {
      Object.keys(shownResult).forEach(k => delete shownResult[k]);
      Object.keys(userOpen).forEach(k => delete userOpen[k]);
      analyticsKey = "";
      toast("New session started");
    }
  });
  let resizeTimer;
  window.addEventListener("resize", () => {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(() => { if (activeTab === "analytics" && S) renderAnalytics(true); }, 150);
  });

  const wf = await fetch("/api/workflow").then(r => r.json());
  WF = wf.steps;
  PI = wf.pi;
  buildCards();
  S = await fetch("/api/state").then(r => r.json());
  render();
  connectStream();
  setInterval(tick, 1000);
}

init();
