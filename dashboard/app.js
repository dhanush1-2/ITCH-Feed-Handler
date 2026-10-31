// Dashboard for the ITCH replay. Loads four JSON files written by scripts/export_dashboard.py
// from results/*.json and draws them. No numbers live in this file: it formats what it is given.
(function () {
  "use strict";

  var fmt = new Intl.NumberFormat("en-US");
  function n(x) { return fmt.format(x); }
  function mil(x, d) { return (x / 1e6).toFixed(d === undefined ? 2 : d) + " M"; }
  function gb(x) { return (x / 1e9).toFixed(1) + " GB"; }
  function mb(x) { return (x / 1e6).toFixed(1) + " MB"; }
  function pct(x, d) { return (x * 100).toFixed(d === undefined ? 1 : d) + "%"; }
  function clock(s) {
    s = Math.round(s);
    return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + (s % 60 ? ":" + pad(s % 60) : "");
  }
  function clockS(s) { s = Math.round(s); return pad(Math.floor(s / 3600)) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60); }
  function pad(v) { return v < 10 ? "0" + v : "" + v; }

  function el(tag, attrs, kids) {
    var e = document.createElement(tag);
    if (attrs) for (var k in attrs) {
      if (k === "class") e.className = attrs[k];
      else if (k === "text") e.textContent = attrs[k];
      else e.setAttribute(k, attrs[k]);
    }
    (kids || []).forEach(function (c) { e.appendChild(typeof c === "string" ? document.createTextNode(c) : c); });
    return e;
  }
  function $(id) { return document.getElementById(id); }
  function css(name) { return getComputedStyle(document.documentElement).getPropertyValue(name).trim(); }

  // ---- theme ---------------------------------------------------------------------------
  var themeBtn = $("theme");
  var themes = ["auto", "light", "dark"];
  var theme = "auto";
  try { theme = localStorage.getItem("theme") || "auto"; } catch (e) { /* storage blocked */ }
  if (themes.indexOf(theme) < 0) theme = "auto";
  function applyTheme() {
    if (theme === "auto") document.documentElement.removeAttribute("data-theme");
    else document.documentElement.setAttribute("data-theme", theme);
    themeBtn.textContent = "Theme: " + theme;
  }
  applyTheme();
  themeBtn.addEventListener("click", function () {
    theme = themes[(themes.indexOf(theme) + 1) % themes.length];
    try { localStorage.setItem("theme", theme); } catch (e) { /* ignore */ }
    applyTheme();
    redraw();
  });
  if (window.matchMedia) {
    window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", function () { redraw(); });
  }

  // ---- charts --------------------------------------------------------------------------
  var charts = [];       // {make: function -> uPlot, host, plot}
  var tip = $("tip");

  function tipPlugin(fmtX, rows) {
    // rows: [{label, key color var, fmt}] in series order (series index 1..)
    return {
      hooks: {
        setCursor: function (u) {
          var idx = u.cursor.idx;
          if (idx == null || u.cursor.left < 0) { tip.hidden = true; return; }
          tip.textContent = "";
          tip.appendChild(el("div", { class: "t", text: fmtX(u.data[0][idx]) }));
          var any = false;
          rows.forEach(function (r, i) {
            var v = u.data[i + 1][idx];
            var row = el("div", { class: "r" });
            row.appendChild(el("span", { class: "k", style: "border-color:" + css(r.color) }));
            row.appendChild(el("span", { class: "v", text: v == null ? "no value" : r.fmt(v) }));
            row.appendChild(el("span", { text: r.label }));
            tip.appendChild(row);
            any = any || v != null;
          });
          tip.hidden = false;
          var box = u.over.getBoundingClientRect();
          var x = box.left + u.cursor.left + 14, y = box.top + (u.cursor.top < 0 ? 10 : u.cursor.top) + 14;
          var w = tip.offsetWidth, h = tip.offsetHeight;
          if (x + w > window.innerWidth - 8) x = box.left + u.cursor.left - w - 14;
          if (y + h > window.innerHeight - 8) y = window.innerHeight - h - 8;
          tip.style.left = Math.max(4, x) + "px";
          tip.style.top = Math.max(4, y) + "px";
        }
      }
    };
  }

  function axes(xfmt, yfmt, ylabel) {
    var grid = { stroke: css("--grid"), width: 1 };
    var ticks = { stroke: css("--axis"), width: 1, size: 4 };
    var font = "12px system-ui, sans-serif";
    return [
      { stroke: css("--ink-3"), grid: grid, ticks: ticks, font: font, values: function (u, v) { return v.map(xfmt); },
        incrs: [60, 300, 600, 900, 1800, 3600, 7200, 10800], space: 64 },
      { stroke: css("--ink-3"), grid: grid, ticks: ticks, font: font, size: 62, label: ylabel || undefined, labelFont: font,
        values: function (u, v) { return v.map(yfmt); } }
    ];
  }

  function addChart(host, build) {
    var c = { host: host, build: build, plot: null };
    charts.push(c);
    draw(c);
    return c;
  }
  function draw(c) {
    if (c.plot) { c.plot.destroy(); c.plot = null; }
    var w = Math.max(280, c.host.clientWidth);
    var opts = c.build(w, c.host.clientHeight || 240);
    c.plot = new uPlot(opts.opts, opts.data, c.host);
  }
  function redraw() { charts.forEach(draw); }
  var resizeTimer = null;
  window.addEventListener("resize", function () {
    clearTimeout(resizeTimer);
    resizeTimer = setTimeout(function () {
      charts.forEach(function (c) { if (c.plot) c.plot.setSize({ width: Math.max(280, c.host.clientWidth), height: (c.host.clientHeight || 240) - legendH(c) }); });
    }, 80);
  });
  function legendH(c) { var l = c.host.querySelector(".u-legend"); return l ? l.offsetHeight : 0; }

  function lineSeries(label, colorVar, extra) {
    var s = { label: label, stroke: css(colorVar), width: 2, points: { show: false }, spanGaps: false };
    for (var k in (extra || {})) s[k] = extra[k];
    return s;
  }
  function baseOpts(w, h, series, cursorOn) {
    // uPlot's height excludes the legend; leave room for it.
    var legend = series.length > 2;
    return {
      width: w, height: Math.max(140, h - (legend ? 28 : 0)),
      legend: { show: legend, live: false },
      cursor: { show: cursorOn !== false, points: { size: 8, width: 2, stroke: css("--surface") }, drag: { x: false, y: false } },
      scales: { x: { time: false } },
      series: series
    };
  }

  // ---- sections ------------------------------------------------------------------------
  function srcLine(p) {
    return "Data: " + p.file + ", commit " + p.commit.slice(0, 7) + (p.commit.length > 40 ? p.commit.slice(40) : "") + ", file " + p.data_file;
  }
  function fillSources(name, sources) {
    var parts = Object.keys(sources).map(function (k) { return srcLine(sources[k]); });
    // unique lines only
    var seen = {}; parts = parts.filter(function (s) { if (seen[s]) return false; seen[s] = 1; return true; });
    document.querySelectorAll('[data-src="' + name + '"]').forEach(function (p) {
      p.textContent = parts.join("   |   ");
    });
  }

  function provenance(p) {
    var parts = p.commits.map(function (c) { return c.commit.slice(0, 7) + " (" + c.files.join(", ") + ")"; });
    $("prov").textContent = "Results from " + p.commit_count + " commit" + (p.commit_count === 1 ? "" : "s") + ": " + parts.join("; ") + ".";
  }

  function kpis(day, verify, bench) {
    var apply = bench.table.filter(function (r) { return r.key === "apply"; })[0];
    var items = [
      { v: n(day.kpi.messages), l: "messages", n: day.kpi.first_message + " to " + day.kpi.last_message + " feed time" },
      { v: n(day.kpi.symbols), l: "symbols", n: day.kpi.symbols_note },
      { v: n(day.kpi.peak_msgs_per_sec), l: "peak messages / second", n: "at " + day.kpi.peak_second },
      { v: mil(apply.msgs_per_sec) , l: "applied messages / second", n: "median of " + apply.runs + " runs, parse and book" },
      { v: n(day.kpi.unknown_refs), l: "unknown order references", n: "of every execute, cancel, delete, replace" },
      { v: verify.checks.bit_identical ? "Yes" : "No", l: "bit-identical after loss", n: "noon state and whole-stream hash" }
    ];
    var box = $("kpis");
    items.forEach(function (it) {
      box.appendChild(el("div", { class: "kpi" }, [
        el("div", { class: "v", text: it.v }), el("div", { class: "l", text: it.l }), el("div", { class: "n", text: it.n })
      ]));
    });
  }

  function daySection(day) {
    var m = day.minute;
    var xs = m.avg.map(function (_, i) { return i * 60 + m.start_s; });
    addChart($("c-day"), function (w, h) {
      var s = [{}, lineSeries("Peak second in the minute", "--s2", { width: 1.5 }), lineSeries("Average over the minute", "--s1")];
      var o = baseOpts(w, h, s);
      o.axes = axes(clock, function (v) { return v >= 1000 ? (v / 1000) + "k" : "" + v; });
      o.plugins = [tipPlugin(clockS, [
        { label: "peak second", color: "--s2", fmt: n }, { label: "average per second", color: "--s1", fmt: function (v) { return n(Math.round(v)); } }])];
      return { opts: o, data: [xs, m.peak, m.avg] };
    });
    // table view: by hour
    var rows = [];
    for (var hr = 3; hr <= 20; hr++) {
      var a = m.avg.slice(hr * 60, hr * 60 + 60), p = m.peak.slice(hr * 60, hr * 60 + 60);
      var tot = a.reduce(function (x, y) { return x + y * 60; }, 0);
      if (tot === 0) continue;
      rows.push([pad(hr) + ":00", n(Math.round(tot)), n(Math.round(tot / 3600)), n(Math.max.apply(null, p))]);
    }
    $("t-day").appendChild(table(["Hour", "Messages", "Average per second", "Peak second"], rows, [1, 2, 3]));

    var s = day.session;
    var xo = s.msgs_per_sec.map(function (_, i) { return s.start_s + i; });
    addChart($("c-open"), function (w, h) {
      var ser = [{}, lineSeries("Messages per second", "--s1", { width: 1.25 })];
      var o = baseOpts(w, h, ser);
      o.axes = axes(clock, function (v) { return v >= 1000 ? (v / 1000) + "k" : "" + v; });
      o.plugins = [tipPlugin(clockS, [{ label: "messages in this second", color: "--s1", fmt: n }])];
      return { opts: o, data: [xo, s.msgs_per_sec] };
    });
  }

  var symbol = null;
  function bookSection(day) {
    var b = day.book, names = Object.keys(b.symbols).sort();
    $("book-lede").textContent = "Best bid and best ask for five liquid symbols, 09:30 to 16:00, sampled every " + b.step_s +
      " seconds as the state after the last change. A side with no resting orders is a gap, not zero, and the spread across a one-sided book is a gap too.";
    var xs = []; for (var t = b.start_s; t <= b.end_s; t += b.step_s) xs.push(t);
    var group = $("symbols");
    symbol = names.indexOf("AAPL") >= 0 ? "AAPL" : names[0];
    names.forEach(function (nm) {
      var btn = el("button", { type: "button", role: "radio", "aria-checked": nm === symbol ? "true" : "false", text: nm });
      btn.addEventListener("click", function () { symbol = nm; sync(); });
      group.appendChild(btn);
    });
    var px = null, sp = null;
    function sync() {
      Array.prototype.forEach.call(group.children, function (btn) { btn.setAttribute("aria-checked", btn.textContent === symbol ? "true" : "false"); });
      var g = b.symbols[symbol];
      draw(px); draw(sp);
      var bad = g.samples_one_sided + g.samples_empty;
      $("book-gaps").textContent = symbol + ": " + n(g.samples_one_sided) + " one-sided and " + n(g.samples_empty) +
        " empty samples of " + n(xs.length) + (bad ? ", shown as gaps." : ". No gaps in this session.");
      tableBook(g);
    }
    px = addChart($("c-px"), function (w, h) {
      var g = b.symbols[symbol];
      var step = { paths: uPlot.paths.stepped({ align: 1 }) };
      var s = [{}, lineSeries("Best bid", "--s1", step), lineSeries("Best ask", "--s2", step)];
      var o = baseOpts(w, h, s);
      o.axes = axes(clock, function (v) { return "$" + v.toFixed(2); });
      o.scales.y = { range: function (u, lo, hi) { var pad = (hi - lo) * 0.08 || 0.05; return [lo - pad, hi + pad]; } };
      o.plugins = [tipPlugin(clockS, [
        { label: "best bid", color: "--s1", fmt: function (v) { return "$" + v.toFixed(2); } },
        { label: "best ask", color: "--s2", fmt: function (v) { return "$" + v.toFixed(2); } }])];
      return { opts: o, data: [xs, g.bid, g.ask] };
    });
    sp = addChart($("c-spread"), function (w, h) {
      var g = b.symbols[symbol];
      var s = [{}, lineSeries("Spread", "--s1", { paths: uPlot.paths.stepped({ align: 1 }) })];
      var o = baseOpts(w, h, s);
      o.axes = axes(clock, function (v) { return v.toFixed(v < 10 ? 1 : 0); }, "cents");
      o.scales.y = { range: function (u, lo, hi) { return [0, Math.max(hi * 1.1, 1)]; } };
      o.plugins = [tipPlugin(clockS, [{ label: "spread", color: "--s1", fmt: function (v) { return v.toFixed(2) + " cents"; } }])];
      return { opts: o, data: [xs, g.spread.map(function (v) { return v == null ? null : v * 100; })] };
    });
    function tableBook(g) {
      var rows = [], marks = [9.5, 10, 11, 12, 13, 14, 15, 16];
      marks.forEach(function (hr) {
        var i = Math.round((hr * 3600 - b.start_s) / b.step_s);
        if (i < 0 || i >= xs.length) return;
        var f = function (v, d) { return v == null ? "none" : v.toFixed(d); };
        rows.push([clock(xs[i]), f(g.bid[i], 2), f(g.ask[i], 2), g.spread[i] == null ? "none" : (g.spread[i] * 100).toFixed(2)]);
      });
      var host = $("t-book"); host.textContent = "";
      host.appendChild(table(["Time", "Best bid ($)", "Best ask ($)", "Spread (cents)"], rows, [1, 2, 3]));
    }
    sync();
  }

  function mixSection(day) {
    var total = day.mix.reduce(function (a, r) { return a + r.count; }, 0), max = day.mix[0].count;
    var box = $("mix");
    day.mix.forEach(function (r) {
      var row = el("div", { class: "bar-row", role: "listitem", title: r.type + " " + r.name + ": " + n(r.count) + " messages" }, [
        el("div", { class: "name" }, [el("b", { text: r.type }), r.name]),
        el("div", { class: "track" }, [el("div", { class: "fill", style: "width:" + Math.max(0.15, r.count / max * 100) + "%" })]),
        el("div", { class: "val" }, [n(r.count), el("span", { text: pct(r.count / total, r.count / total < 0.001 ? 3 : 1) })])
      ]);
      box.appendChild(row);
    });
  }

  function table(head, rows, numCols) {
    var t = el("table");
    var tr = el("tr");
    head.forEach(function (h, i) { tr.appendChild(el("th", { class: numCols && numCols.indexOf(i) >= 0 ? "num" : "", text: h })); });
    t.appendChild(el("thead", null, [tr]));
    var tb = el("tbody");
    rows.forEach(function (r) {
      var row = el("tr");
      r.forEach(function (c, i) {
        var td = el("td", { class: numCols && numCols.indexOf(i) >= 0 ? "num" : "" });
        if (c && c.nodeType) td.appendChild(c); else td.textContent = c;
        row.appendChild(td);
      });
      tb.appendChild(row);
    });
    t.appendChild(tb);
    return t;
  }

  function lossSection(loss) {
    var t = $("loss-table");
    t.textContent = "";
    var head = el("tr");
    ["Scenario", "Messages", "Lost downstream", "Gaps detected", "Recovered", "State at checkpoint (cmp)"].forEach(function (h, i) {
      head.appendChild(el("th", { class: i >= 1 && i <= 3 ? "num" : "", text: h }));
    });
    t.appendChild(el("thead", null, [head]));
    var tb = el("tbody");
    function groupRow(txt) { tb.appendChild(el("tr", { class: "group" }, [el("th", { colspan: "6", text: txt })])); }
    function mark(ok, txt) { return el("span", { class: ok ? "yes" : "no", text: (ok ? "\u2713 " : "\u2717 ") + txt }); }
    function sc(r) {
      if (r.kind === "baseline") return "No loss (baseline)";
      if (r.kind === "loss_only") return pct(r.loss, 1).replace(/\.0%$/, "%") + " packet loss";
      return pct(r.loss, 0) + " loss, " + pct(r.dup, 1) + " duplicates, reorder " + r.reorder_window + ", bursts, lossy retransmit";
    }
    // Recovered is judged here from the recorded stream result, not asserted: the delivered
    // stream must have the source's message count and hash.
    function recovered(r) {
      var ok = r.stream_matches_source === true && r.delivered === r.messages;
      if (ok) return mark(true, r.lost === 0 ? "nothing lost; stream hash equals source" : "all, stream hash equals source");
      return mark(false, "not all: " + n(r.delivered) + " of " + n(r.messages) + " delivered" + (r.stream_matches_source ? "" : ", stream hash differs"));
    }
    function cmp(r) {
      if (r.cmp === "reference") return el("span", { text: "n/a (reference)" });
      if (r.cmp === "identical") return mark(true, "identical, " + r.checkpoint);
      return mark(false, "differs, " + r.checkpoint);
    }
    function addRow(label, r) {
      tb.appendChild(el("tr", null, [
        el("td", { text: label }),
        el("td", { class: "num", text: n(r.messages) }),
        el("td", { class: "num", text: n(r.lost) }),
        el("td", { class: "num", text: n(r.gaps_detected) }),
        el("td", null, [recovered(r)]),
        el("td", null, [cmp(r)])
      ]));
    }
    groupRow("Loss only (first " + n(loss.table_messages) + " messages of the day)");
    loss.rows.filter(function (r) { return r.kind === "baseline" || r.kind === "loss_only"; }).forEach(function (r) { addRow(sc(r), r); });
    groupRow("Stress (loss with duplicates, reordering, bursts and lossy retransmits), reported separately");
    loss.rows.filter(function (r) { return r.kind !== "baseline" && r.kind !== "loss_only"; }).forEach(function (r) { addRow(sc(r), r); });
    groupRow("Full day (" + n(loss.full_day[0].messages) + " messages)");
    loss.full_day.forEach(function (f) {
      var ref = f.cmp === "reference";
      addRow(ref ? "No loss (reference)" : "Stress: " + pct(f.loss, 0) + " loss, " + pct(f.dup, 1) + " dup, reorder " + f.reorder_window, f);
    });
    t.appendChild(tb);

    var f = loss.full_day.filter(function (x) { return x.cmp !== "reference"; })[0];
    var txt;
    if (!f) {
      txt = "No stressed full-day run in the data.";
    } else {
      var ok = f.stream_matches_source === true && f.delivered === f.messages;
      txt = "Full-day stress run: " + n(f.lost) + " messages lost downstream, " + (ok ? "all recovered" : "NOT all recovered") + ". State " + f.checkpoint + ": " +
        n(f.noon_resting_orders) + " resting orders, ";
      if (f.cmp === "identical") txt += "byte for byte identical to the lossless run and to the max-speed replay's dump.";
      else txt += "DIFFERENT from " + [f.same_as_lossless ? null : "the lossless run", f.same_as_max_speed_replay ? null : "the max-speed replay's dump"].filter(Boolean).join(" and ") + ".";
    }
    $("loss-note").textContent = txt + " " + loss.note;
  }

  function verifySection(v) {
    var c = v.checks;
    var items = [
      c.verify_passed ? "Full-day --verify passed: " + n(c.whole_state_checks) + " whole-state checks" : "Full-day --verify FAILED",
      n(c.unknown_refs) + " unknown order references",
      n(c.conservation_failures) + " share-conservation failures",
      "Known-answer comparison with an independent Python book: " + c.known_answer_identical + " of " + c.known_answer_symbols + " symbols identical (" + n(c.known_answer_resting_orders[c.known_answer_checkpoint]) + " resting orders at the feed-second-" + n(Number(c.known_answer_checkpoint)) + " checkpoint)",
      "State at feed second " + n(c.checkpoint_second) + " with and without loss: " + (c.noon_loss_equals_lossless ? "identical" : "DIFFERENT") + " (" + n(c.noon_resting_orders) + " orders); equal to the max-speed replay's dump: " + (c.noon_equals_max_speed_dump ? "yes" : "NO"),
      "Whole-stream hash after loss recovery: " + (c.stream_hash_equal ? "equal to the source" : "DIFFERENT"),
      "Robustness day 2025-12-08, " + n(v.robustness.messages) + " messages: " + (v.robustness.verify_passed ? "passed" : "FAILED") + ", " + n(v.robustness.unknown_refs) + " unknown references"
    ];
    var ul = $("checks");
    items.forEach(function (t) {
      var bad = /FAILED|DIFFERENT/.test(t);
      ul.appendChild(el("li", null, [el("span", { class: bad ? "badge serious" : "ok", text: bad ? "✗" : "✓" }), t]));
    });
    $("bit-note").textContent = "Bit-identical means: " + c.bit_identical_note.replace(/^The state/, "the state") +
      " (" + n(c.end_of_day_resting_orders) + " orders remain at the end of day.)";

    var t = $("disc-table"); t.textContent = "";
    var head = el("tr");
    ["Exception", "Count", "Status"].forEach(function (h, i) { head.appendChild(el("th", { class: i === 1 ? "num" : "", text: h })); });
    t.appendChild(el("thead", null, [head]));
    var tb = el("tbody");
    var kind = {
      "Found, fixed, re-verified": ["good", "✓"], "Explained": ["good", "✓"], "Agrees": ["good", "✓"],
      "Characterised, mechanism not shown": ["warn", "◐"], "Unexplained": ["serious", "?"]
    };
    v.discrepancies.forEach(function (r) {
      var k = kind[r.status] || ["warn", "◐"];
      var cnt = n(r.count) + (r.before != null ? " (was " + n(r.before) + ")" : "") + (r.extra ? ", " + r.extra : "");
      tb.appendChild(el("tr", null, [
        el("td", null, [r.what, el("span", { class: "sub-d", text: r.detail })]),
        el("td", { class: "num", text: cnt }),
        el("td", null, [el("span", { class: "badge " + k[0] }, [el("i", { text: k[1], "aria-hidden": "true" }), r.status])])
      ]));
    });
    t.appendChild(tb);
  }

  function benchSection(b, day) {
    $("bench-lede").textContent = b.machine.cpu + ", " + gb(b.machine.memory_bytes) + " (" + Math.round(b.machine.memory_bytes / 1073741824) + " GiB), " + b.machine.os + ". Each row is the " +
      b.protocol.published + " of " + b.protocol.processes + " fresh processes over the whole recorded day, input locked in memory. Spread is (max - min) / median.";
    var t = $("bench-table"); t.textContent = "";
    var head = el("tr");
    ["Mode", "Messages / second", "Spread", "ns / message"].forEach(function (h, i) { head.appendChild(el("th", { class: i ? "num" : "", text: h })); });
    t.appendChild(el("thead", null, [head]));
    var tb = el("tbody");
    b.table.forEach(function (r) {
      tb.appendChild(el("tr", null, [
        el("td", null, [r.label, el("span", { class: "sub-d", text: r.note + (r.messages < 2e8 ? ", " + mil(r.messages, 1) + " messages" : "") })]),
        el("td", { class: "num", text: mil(r.msgs_per_sec) }),
        el("td", { class: "num", text: pct(r.spread) }),
        el("td", { class: "num", text: r.ns_per_msg.toFixed(1) })
      ]));
    });
    t.appendChild(tb);
    var lat = b.latency_ns;
    tb.appendChild(el("tr", { class: "group" }, [el("th", { colspan: "4", text: "Per-message apply latency, every message in the file (" + b.clock_granularity_ns + " ns clock granularity)" })]));
    tb.appendChild(el("tr", null, [el("td", { text: "p50 / p90 / p99 / p99.9" }), el("td", { class: "num", colspan: "3", text: [lat.p50_ns, lat.p90_ns, lat.p99_ns, lat.p99_9_ns].join(" / ") + " ns" })]));

    var l = b.ladder, box = $("ladder");
    box.appendChild(el("p", null, [el("strong", { text: "Project 1 ladder book: " }),
      l.speedup.toFixed(2) + "x faster on the ten busiest symbols (" + mil(l.ladder_msgs_per_sec) + " against " + mil(l.vector_msgs_per_sec) + " messages / second)."]));
    box.appendChild(el("p", null, ["It is not used for the market: a ladder per symbol needs " + gb(l.full_market_bytes) + " for all " + n(l.symbols) +
      " symbols with orders, more than this machine's " + gb(l.machine_memory_bytes) + ", where the vector books hold " + mb(l.vector_level_bytes) + "."]));

    var m = b.handler_memory;
    var mt = $("mem-table"); mt.textContent = "";
    mt.appendChild(el("thead", null, [el("tr", null, [el("th", { text: "Table" }), el("th", { class: "num", text: "Size" })])]));
    var mtb = el("tbody");
    [["Order pool", m.order_pool_bytes], ["Order id index", m.id_index_bytes], ["Book array", m.books_array_bytes],
     ["Level vectors, all " + n(m.books_with_levels) + " books, high-water", m.level_vectors_bytes]].forEach(function (r) {
      mtb.appendChild(el("tr", null, [el("td", { text: r[0] }), el("td", { class: "num", text: mb(r[1]) })]));
    });
    mt.appendChild(mtb);
    mt.parentNode.appendChild(el("p", { class: "note", text: m.note }));

    var p = b.paced_replay;
    if (p) {
      var sched = p.paced_feed_s / p.speed;
      var txt = "Paced replay at " + p.speed + "x: " + (p.paced_feed_s / 3600).toFixed(2) + " h of feed time in " + (p.paced_wall_s / 3600).toFixed(2) +
        " h of wall time (" + pct(p.paced_wall_s / sched, 1) + " of the schedule). Worst sampled lag behind schedule: " + (p.max_lag_ns / 1e6).toFixed(1) + " ms (" + n(p.max_lag_ns) + " ns)";
      if (p.max_lag_ns > 0) {
        if (p.max_lag_at_feed_s != null) {
          var near = Math.abs(p.max_lag_at_feed_s - day.kpi.peak_second_s) <= 5;
          txt += ", seen at feed time " + clockS(p.max_lag_at_feed_s) + (near ? ", the day's peak-rate second (" + day.kpi.peak_second + "), the closing-cross burst" : "");
        } else {
          txt += "; this file does not record where in the feed it occurred";
        }
        txt += ". The replay was behind schedule by that much at its worst, so " + p.speed + "x was not held at every moment";
      }
      $("paced").textContent = txt + ". The books it built equal the max-speed run's.";
    } else {
      $("paced").textContent = "No paced replay was recorded in the data.";
    }
    $("method").setAttribute("href", "data/" + b.methodology);
  }

  // ---- load ----------------------------------------------------------------------------
  function load(name) {
    return fetch("data/" + name + ".json", { cache: "no-cache" }).then(function (r) {
      if (!r.ok) throw new Error("data/" + name + ".json: HTTP " + r.status);
      return r.json();
    });
  }
  Promise.all(["day", "loss", "verify", "bench", "provenance"].map(load)).then(function (all) {
    var day = all[0], loss = all[1], verify = all[2], bench = all[3], prov = all[4];
    provenance(prov);
    kpis(day, verify, bench);
    daySection(day);
    bookSection(day);
    mixSection(day);
    lossSection(loss);
    verifySection(verify);
    benchSection(bench, day);
    fillSources("day", { a: day.sources.day }); fillSources("top_of_book", { a: day.sources.top_of_book });
    fillSources("loss", loss.sources); fillSources("verify", verify.sources); fillSources("bench", bench.sources);
    $("status").hidden = true;
    document.body.setAttribute("data-ready", "1");
  }).catch(function (e) {
    var s = $("status");
    s.textContent = "Could not load the results: " + e.message;
    s.style.color = css("--serious-ink");
  });
})();
