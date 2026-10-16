/* charts.js - small SVG charts for the analytics page: columns, a line, horizontal bars.
 * Thin marks, rounded data ends, hairline grid, one y-axis, hover tooltips on every mark. */
"use strict";

const Charts = (() => {
  const NS = "http://www.w3.org/2000/svg";

  function el(tag, attrs, parent) {
    const node = document.createElementNS(NS, tag);
    for (const [k, v] of Object.entries(attrs || {})) node.setAttribute(k, v);
    if (parent) parent.appendChild(node);
    return node;
  }

  function text(parent, x, y, str, attrs) {
    const t = el("text", Object.assign({ x, y }, attrs), parent);
    t.textContent = str;
    return t;
  }

  /* A round axis maximum and its tick values. */
  function niceScale(max, ticks = 4) {
    if (!(max > 0)) max = 1;
    const raw = max / ticks;
    const mag = Math.pow(10, Math.floor(Math.log10(raw)));
    const step = [1, 2, 2.5, 5, 10].map(m => m * mag).find(s => s >= raw);
    const top = Math.ceil(max / step) * step;
    const values = [];
    for (let v = 0; v <= top + step / 2; v += step) values.push(+v.toFixed(10));
    return { top, values };
  }

  function frame(container, height) {
    container.innerHTML = "";
    const width = Math.max(260, Math.floor(container.clientWidth || 360));
    const svg = el("svg", { viewBox: `0 0 ${width} ${height}`, width, height, role: "img" }, container);
    return { svg, width, height };
  }

  /* Bar with a 4px rounded data end and a square base. */
  function columnPath(x, y, w, h) {
    const r = Math.min(4, w / 2, h);
    return `M${x},${y + h}V${y + r}A${r},${r} 0 0 1 ${x + r},${y}H${x + w - r}A${r},${r} 0 0 1 ${x + w},${y + r}V${y + h}Z`;
  }
  function barPath(x, y, w, h) {
    const r = Math.min(4, h / 2, w);
    return `M${x},${y}H${x + w - r}A${r},${r} 0 0 1 ${x + w},${y + r}V${y + h - r}A${r},${r} 0 0 1 ${x + w - r},${y + h}H${x}Z`;
  }

  function yAxis(svg, m, plotW, plotH, scale, fmt) {
    const g = el("g", { class: "ax" }, svg);
    for (const v of scale.values) {
      const y = m.top + plotH - (v / scale.top) * plotH;
      el("line", { x1: m.left, x2: m.left + plotW, y1: y, y2: y, class: v === 0 ? "base" : "" }, g);
      text(g, m.left - 8, y + 4, fmt(v), { "text-anchor": "end" });
    }
    return g;
  }

  /*
   * columns(container, {categories, values, whiskers?, format, tickFormat, color, tips})
   * One series of vertical columns; whiskers [[min,max],...] draw a min-max range.
   */
  function columns(container, o) {
    const { svg, width, height } = frame(container, o.height || 220);
    const m = { top: 22, right: 10, bottom: 30, left: 50 };
    const plotW = width - m.left - m.right, plotH = height - m.top - m.bottom;
    const hi = Math.max(...o.values.map((v, i) => Math.max(v || 0, o.whiskers ? o.whiskers[i][1] : 0)));
    const scale = niceScale(hi);
    const fmt = o.format || (v => String(v));
    yAxis(svg, m, plotW, plotH, scale, o.tickFormat || fmt);
    const band = plotW / o.categories.length;
    const bw = Math.min(24, band * 0.5);
    const color = o.color || "var(--series-1)";
    const g = el("g", {}, svg);
    o.categories.forEach((c, i) => {
      const v = o.values[i] || 0;
      const cx = m.left + band * i + band / 2;
      const h = (v / scale.top) * plotH;
      const y = m.top + plotH - h;
      if (o.whiskers) {
        const [lo, up] = o.whiskers[i];
        const y1 = m.top + plotH - (lo / scale.top) * plotH, y2 = m.top + plotH - (up / scale.top) * plotH;
        el("line", { x1: cx + bw / 2 + 6, x2: cx + bw / 2 + 6, y1, y2, stroke: "var(--muted)", "stroke-width": 1.5 }, g);
        el("line", { x1: cx + bw / 2 + 3, x2: cx + bw / 2 + 9, y1: y2, y2: y2, stroke: "var(--muted)", "stroke-width": 1.5 }, g);
        el("line", { x1: cx + bw / 2 + 3, x2: cx + bw / 2 + 9, y1, y2: y1, stroke: "var(--muted)", "stroke-width": 1.5 }, g);
      }
      el("path", { d: columnPath(cx - bw / 2, y, bw, Math.max(h, 0.5)), fill: o.colors ? o.colors[i] : color, class: "mark" }, g);
      if (o.labels !== false) text(g, cx, y - 7, fmt(v), { class: "val", "text-anchor": "middle" });
      text(g, cx, height - 10, c, { class: "lbl-end", "text-anchor": "middle", fill: "var(--muted)" });
      const hit = el("rect", { x: m.left + band * i, y: m.top, width: band, height: plotH, class: "hit" }, g);
      hit.setAttribute("data-tip", o.tips ? o.tips[i] : `<b>${c}</b><br>${fmt(v)}`);
    });
  }

  /*
   * line(container, {xs, series:[{name, values, color}], reference?:{name, values}, format, xFormat})
   * Numeric x (e.g. worker counts). The reference line is drawn thin and dashed.
   */
  function line(container, o) {
    const { svg, width, height } = frame(container, o.height || 220);
    const m = { top: 22, right: 54, bottom: 30, left: 44 };
    const plotW = width - m.left - m.right, plotH = height - m.top - m.bottom;
    const all = o.series.flatMap(s => s.values).concat(o.reference ? o.reference.values : []);
    const scale = niceScale(Math.max(...all));
    const fmt = o.format || (v => String(v));
    yAxis(svg, m, plotW, plotH, scale, o.tickFormat || fmt);
    // Evenly spaced x positions: worker counts 1, 2, 4, 8 would bunch up on a linear axis.
    const last = o.xs.length - 1;
    const X = v => m.left + (last === 0 ? plotW / 2 : (o.xs.indexOf(v) / last) * plotW);
    const Y = v => m.top + plotH - (v / scale.top) * plotH;
    const g = el("g", {}, svg);
    o.xs.forEach(x => text(g, X(x), height - 10, (o.xFormat || String)(x), { class: "lbl-end", "text-anchor": "middle", fill: "var(--muted)" }));
    if (o.reference) {
      const pts = o.xs.map((x, i) => `${X(x)},${Y(o.reference.values[i])}`).join(" ");
      el("polyline", { points: pts, fill: "none", stroke: "var(--muted)", "stroke-width": 1.5, "stroke-dasharray": "4 4" }, g);
      text(g, X(o.xs[last]) + 8, Y(o.reference.values[last]) + 4, o.reference.name, { class: "lbl-end" });
    }
    for (const s of o.series) {
      const pts = o.xs.map((x, i) => `${X(x)},${Y(s.values[i])}`).join(" ");
      el("polyline", { points: pts, fill: "none", stroke: s.color, "stroke-width": 2, "stroke-linejoin": "round", "stroke-linecap": "round" }, g);
      o.xs.forEach((x, i) => {
        el("circle", { cx: X(x), cy: Y(s.values[i]), r: 4.5, fill: s.color, stroke: "var(--surface)", "stroke-width": 2, class: "mark" }, g);
      });
      text(g, X(o.xs[last]) + 8, Y(s.values[last]) + 4, fmt(s.values[last]), { class: "val" });
    }
    // hover targets: one column per x
    o.xs.forEach((x, i) => {
      const left = i === 0 ? m.left - 10 : (X(o.xs[i - 1]) + X(x)) / 2;
      const right = i === o.xs.length - 1 ? m.left + plotW + 10 : (X(x) + X(o.xs[i + 1])) / 2;
      const hit = el("rect", { x: left, y: m.top, width: right - left, height: plotH, class: "hit" }, g);
      const lines = o.series.map(s => `${s.name}: <b>${fmt(s.values[i])}</b>`);
      if (o.reference) lines.push(`${o.reference.name}: ${fmt(o.reference.values[i])}`);
      hit.setAttribute("data-tip", `<b>${(o.xLabel || "")}${(o.xFormat || String)(x)}</b><br>${lines.join("<br>")}`);
    });
  }

  /* hbars(container, {labels, values, format, colors}) - horizontal bars, one per row. */
  function hbars(container, o) {
    const row = 30;
    const { svg, width, height } = frame(container, o.labels.length * row + 26);
    const m = { top: 6, right: 80, bottom: 20, left: 118 };
    const plotW = width - m.left - m.right;
    const scale = niceScale(Math.max(...o.values));
    const fmt = o.format || (v => String(v));
    const g = el("g", { class: "ax" }, svg);
    for (const v of scale.values) {
      const x = m.left + (v / scale.top) * plotW;
      el("line", { x1: x, x2: x, y1: m.top, y2: height - m.bottom, class: v === 0 ? "base" : "" }, g);
      text(g, x, height - 5, (o.tickFormat || fmt)(v), { "text-anchor": "middle" });
    }
    const marks = el("g", {}, svg);
    o.labels.forEach((label, i) => {
      const v = o.values[i] || 0;
      const y = m.top + i * row + 6;
      const w = Math.max((v / scale.top) * plotW, 0.5);
      text(marks, m.left - 10, y + 13, label, { class: "lbl-end", "text-anchor": "end" });
      el("path", { d: barPath(m.left, y, w, 18), fill: o.colors ? o.colors[i] : "var(--series-1)", class: "mark" }, marks);
      text(marks, m.left + w + 6, y + 13, fmt(v), { class: "val" });
      const hit = el("rect", { x: 0, y: y - 4, width, height: row, class: "hit" }, marks);
      hit.setAttribute("data-tip", o.tips ? o.tips[i] : `<b>${label}</b><br>${fmt(v)}`);
    });
  }

  /* One tooltip for every chart. */
  function initTooltip(tipEl) {
    document.addEventListener("pointermove", e => {
      const t = e.target.closest && e.target.closest("[data-tip]");
      if (!t) { tipEl.hidden = true; return; }
      tipEl.innerHTML = t.getAttribute("data-tip");
      tipEl.hidden = false;
      const r = tipEl.getBoundingClientRect();
      let x = e.clientX + 14, y = e.clientY + 14;
      if (x + r.width > window.innerWidth - 8) x = e.clientX - r.width - 14;
      if (y + r.height > window.innerHeight - 8) y = e.clientY - r.height - 14;
      tipEl.style.left = x + "px";
      tipEl.style.top = y + "px";
    });
    document.addEventListener("pointerleave", () => { tipEl.hidden = true; });
    window.addEventListener("scroll", () => { tipEl.hidden = true; }, { passive: true });
  }

  return { columns, line, hbars, initTooltip };
})();
