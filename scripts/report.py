#!/usr/bin/env python3
"""Builds results/REPORT.md and results/plots/*.svg from results/*.json.

Standard library only: plots are written as plain SVG, so nothing needs installing.

    python3 scripts/report.py
"""
import json
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "results")
PLOTS = os.path.join(RES, "plots")

# Reference palette: series slots in fixed order, text and grid inks.
SERIES = ["#257de0", "#f47230"]
TEXT, TEXT2, GRID, SURFACE = "#191919", "#666666", "#e6e6e6", "#ffffff"
FONT = "-apple-system, BlinkMacSystemFont, 'Segoe UI', Helvetica, Arial, sans-serif"


def load(name):
    p = os.path.join(RES, name)
    if not os.path.exists(p):
        return None
    with open(p) as f:
        return json.load(f)


def esc(s):
    return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


class Svg:
    def __init__(self, w, h):
        self.w, self.h, self.parts = w, h, []

    def add(self, s):
        self.parts.append(s)

    def text(self, x, y, s, size=12, color=TEXT, anchor="start", weight="normal"):
        self.add(f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" fill="{color}" text-anchor="{anchor}" '
                 f'font-weight="{weight}">{esc(s)}</text>')

    def line(self, x1, y1, x2, y2, color=GRID, width=1):
        self.add(f'<line x1="{x1:.1f}" y1="{y1:.1f}" x2="{x2:.1f}" y2="{y2:.1f}" stroke="{color}" stroke-width="{width}"/>')

    def save(self, path):
        body = "\n".join(self.parts)
        with open(path, "w") as f:
            f.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {self.w} {self.h}" width="{self.w}" '
                    f'height="{self.h}" font-family="{FONT}">\n<rect width="100%" height="100%" fill="{SURFACE}"/>\n'
                    f'{body}\n</svg>\n')


# ---------- latency percentile curves (HdrHistogram style) ----------

NINES = [0.0, 0.9, 0.99, 0.999, 0.9999, 0.99999]
NINES_LABEL = ["p0", "p90", "p99", "p99.9", "p99.99", "p99.999"]


def nines_x(p):
    return -math.log10(max(1e-12, 1.0 - p))


def curve(hist, ns_per_tick, points=600):
    """[(ticks, count)] -> [(percentile, ns)] sampled on an even grid of the nines axis."""
    hist = sorted(hist)
    total = sum(c for _, c in hist)
    xmax = nines_x(0.99999)
    pts, i, seen = [], 0, 0
    for k in range(points + 1):
        p = 1.0 - 10 ** (-xmax * k / points)
        rank = p * total
        while i < len(hist) - 1 and seen + hist[i][1] < rank:
            seen += hist[i][1]
            i += 1
        pts.append((p, hist[i][0] * ns_per_tick))
    return pts


def latency_plot(path, title, series, subtitle):
    """series: [(label, hist, ns_per_tick)]"""
    W, H, L, R, T, B = 760, 440, 70, 150, 80, 50
    s = Svg(W, H)
    s.text(L, 24, title, 16, weight="600")
    s.text(L, 42, subtitle, 12, TEXT2)
    curves = [(lab, curve(h, npt)) for lab, h, npt in series if h]
    if not curves:
        return False
    ys = [ns for _, c in curves for _, ns in c if ns > 0]
    ymin = 10 ** math.floor(math.log10(max(1.0, min(ys))))
    ymax = 10 ** math.ceil(math.log10(max(ys)))
    xmax = nines_x(0.99999)
    px = lambda p: L + (W - L - R) * nines_x(p) / xmax
    py = lambda ns: T + (H - T - B) * (1 - (math.log10(max(ns, ymin)) - math.log10(ymin)) / (math.log10(ymax) - math.log10(ymin)))
    # Grid and axes.
    d = ymin
    while d <= ymax * 1.0001:
        s.line(L, py(d), W - R, py(d))
        lab = f"{d / 1e6:g} ms" if d >= 1e6 else f"{d / 1000:g} µs" if d >= 1000 else f"{d:g} ns"
        s.text(L - 8, py(d) + 4, lab, 11, TEXT2, "end")
        d *= 10
    for p, lab in zip(NINES, NINES_LABEL):
        s.line(px(p), T, px(p), H - B)
        s.text(px(p), H - B + 18, lab, 11, TEXT2, "middle")
    s.text((L + W - R) / 2, H - 10, "percentile", 11, TEXT2, "middle")
    # Curves, then hover targets, then direct labels + legend.
    for i, (lab, c) in enumerate(curves):
        color = SERIES[i % len(SERIES)]
        d_attr = " ".join(f"{'M' if j == 0 else 'L'}{px(p):.1f},{py(ns):.1f}" for j, (p, ns) in enumerate(c) if p <= 0.99999)
        s.add(f'<path d="{d_attr}" fill="none" stroke="{color}" stroke-width="2" stroke-linejoin="round"/>')
        for p in [0.5, 0.9, 0.99, 0.999, 0.9999]:
            ns = next((v for q, v in c if q >= p), c[-1][1])
            s.add(f'<circle cx="{px(p):.1f}" cy="{py(ns):.1f}" r="4" fill="{color}" stroke="{SURFACE}" stroke-width="2">'
                  f'<title>{esc(lab)} p{p * 100:g}: {ns:,.0f} ns</title></circle>')
        last_ns = next((v for q, v in c if q >= 0.9999), c[-1][1])
        s.text(W - R + 10, py(last_ns) + 4 + i * 2, lab, 12, TEXT)
    lx = L  # legend: one row under the subtitle, outside the plot area
    for i, (lab, _) in enumerate(curves):
        s.add(f'<rect x="{lx}" y="56" width="14" height="4" rx="2" fill="{SERIES[i % len(SERIES)]}"/>')
        s.text(lx + 20, 62, lab, 12)
        lx += 20 + 7.5 * len(lab) + 24
    s.save(path)
    return True


# ---------- horizontal bars with range whiskers ----------

def bar_plot(path, title, subtitle, rows, unit, better):
    """rows: [(label, median, lo, hi)] drawn top to bottom."""
    W, L, R, T, row = 820, 360, 90, 56, 34
    H = T + row * len(rows) + 40
    s = Svg(W, H)
    s.text(16, 24, title, 16, weight="600")
    s.text(16, 42, f"{subtitle} ({better} is better; whisker = min..max over runs)", 12, TEXT2)
    vmax = max(hi for _, _, _, hi in rows) * 1.08
    px = lambda v: L + (W - L - R) * v / vmax
    step = 10 ** math.floor(math.log10(vmax / 2))
    for mult in (1, 2, 5):
        if vmax / (step * mult) <= 6:
            step *= mult
            break
    v = 0.0
    while v <= vmax:
        s.line(px(v), T - 6, px(v), T + row * len(rows))
        s.text(px(v), T + row * len(rows) + 16, f"{v:g}", 11, TEXT2, "middle")
        v += step
    s.text((L + W - R) / 2, H - 6, unit, 11, TEXT2, "middle")
    for i, (lab, med, lo, hi) in enumerate(rows):
        y = T + i * row
        s.text(L - 10, y + row / 2 + 4, lab, 12, TEXT, "end")
        w = max(1.0, px(med) - L)
        s.add(f'<path d="M{L},{y + 7} h{w - 4:.1f} a4,4 0 0 1 4,4 v{row - 22} a4,4 0 0 1 -4,4 h-{w - 4:.1f} z" '
              f'fill="{SERIES[0]}"><title>{esc(lab)}: {med:,.2f} {unit} (range {lo:,.2f} .. {hi:,.2f})</title></path>')
        if hi > lo:
            cy = y + row / 2
            s.line(px(lo), cy, px(hi), cy, TEXT, 1.5)
            s.line(px(lo), cy - 5, px(lo), cy + 5, TEXT, 1.5)
            s.line(px(hi), cy - 5, px(hi), cy + 5, TEXT, 1.5)
        s.text(max(px(hi), px(med)) + 8, y + row / 2 + 4, f"{med:,.1f}", 12, TEXT)
    s.save(path)


# ---------- report ----------

def fmt(v, nd=1):
    return "—" if v is None else f"{v:,.{nd}f}"


def update_readme(parse, bchk, fchk, abl, lfast, lmap, eng, pipe, headline, bullet):
    """Rewrites the block between the results markers in README.md."""
    path = os.path.join(ROOT, "README.md")
    if not os.path.exists(path):
        return
    text = open(path).read()
    begin, end = "<!-- results:begin -->", "<!-- results:end -->"
    if begin not in text or end not in text:
        return
    rows = ["| Component | Result |", "|---|---|"]
    if parse:
        rows.append(f"| ITCH 5.0 parser (zero-copy) | {parse['mmsgs_per_s_best']:.0f}M msgs/s, {parse['gb_per_s_best']:.1f} GB/s, "
                    f"{parse['ns_per_msg_best']:.1f} ns/msg |")
    if headline:
        rows.append(f"| Order book replay (parse + book) | {headline['mps']:.1f}M msgs/s, **{headline['speedup']:.2f}×** "
                    f"the std::map baseline |")
    if lfast:
        rows.append(f"| Book latency per message | p50 {lfast['p50_ns']:.0f} ns, **p99 {lfast['p99_ns']:.0f} ns**, "
                    f"p99.9 {lfast['p99_9_ns']:.0f} ns |")
    if eng:
        rows.append(f"| Matching engine (price-time) | {eng['fast_orders_per_s'] / 1e6:.1f}M orders/s, "
                    f"{eng['fast_ns_per_order']:.1f} ns/order, {eng['speedup']:.2f}× the reference |")
    if pipe and "throughput_mmsgs_per_s" in pipe:
        t = pipe["throughput_mmsgs_per_s"]
        rows.append(f"| Two-thread SPSC pipeline | {t['pipeline']['median']:.1f}M msgs/s; queue adds "
                    f"{pipe['latency_pipeline']['p50_ns'] - pipe['latency_inline']['p50_ns']:.0f} ns at p50 |")
    checks = []
    if fchk:
        checks.append(f"{fchk['level_checks'] / 1e6:.1f}M per-message level checks and {fchk.get('best_checks', 0) / 1e6:.1f}M "
                      f"best bid/ask checks, {fchk['level_mismatches'] + fchk['audit_mismatches'] + fchk.get('best_mismatches', 0)} mismatches")
    if eng:
        checks.append(f"{eng['diff_passed']}/{eng['diff_seeds']} randomized engine seeds × {eng['diff_orders_per_seed'] / 1e6:.0f}M orders identical to the reference")
    if checks:
        rows.append(f"| Correctness | {'; '.join(checks)} |")
    block = [begin, "", *rows, ""]
    if headline:
        block.append(f"Input: {headline['n']:.1f}M messages of real NASDAQ data (12/30/2019 sample). Full tables, ablations and "
                     "latency plots: [`results/REPORT.md`](results/REPORT.md).")
        block.append("")
        block.append("![Order book ablation](results/plots/ablation_book.svg)")
        block.append("")
    if bullet:
        block += ["**Resume bullet:**", "", f"> {bullet}", ""]
    block.append(end)
    a, b = text.index(begin), text.index(end) + len(end)
    with open(path, "w") as f:
        f.write(text[:a] + "\n".join(block) + text[b:])
    print("updated README.md results block")


def main():
    os.makedirs(PLOTS, exist_ok=True)
    parse, bchk, fchk = load("parse.json"), load("baseline_check.json"), load("fast_check.json")
    lmap, lfast = load("latency_map.json"), load("latency_fast.json")
    abl, eng, pipe = load("ablation.json"), load("engine.json"), load("pipeline.json")
    md = ["# Benchmark report", "",
          "Generated by `scripts/report.py` from `results/*.json`. Regenerate everything with `scripts/run_all.sh`.", ""]
    msgs = (abl or parse or {}).get("messages")
    if msgs:
        md += [f"Input: NASDAQ TotalView-ITCH 5.0, {msgs / 1e6:.2f}M messages.", ""]

    # Correctness.
    md += ["## Correctness", "", "| Check | Result |", "|---|---|"]
    if parse:
        md.append(f"| Parser framing | {parse['status']}, {parse['messages']:,} messages |")
    for name, c in (("std::map book", bchk), ("fast book", fchk)):
        if c:
            extra = f", {c['best_checks']:,} best bid/ask checks" if "best_checks" in c else ""
            md.append(f"| {name} vs independent level sums | {'passed' if c['passed'] else 'FAILED'}: "
                      f"{c['level_checks']:,} per-message level checks{extra}, {c['audit_levels']:,}-level final audit, "
                      f"{c['level_mismatches'] + c['audit_mismatches'] + c.get('best_mismatches', 0)} mismatches |")
    if abl:
        md.append(f"| All book variants build the same final books | {'yes' if abl['books_identical'] else 'NO'} |")
    if eng:
        md.append(f"| Matching engine vs reference (randomized) | {eng['diff_passed']}/{eng['diff_seeds']} seeds × "
                  f"{eng['diff_orders_per_seed']:,} orders, {eng['diff_fills_checked']:,} fills compared |")
    if pipe:
        md.append(f"| Two-thread pipeline builds the same books as one thread | {'yes' if pipe['books_match'] else 'NO'} |")
    md.append("")

    # Book ablation.
    if abl:
        v = {x["key"]: x for x in abl["variants"]}
        md += ["## Order book: what each optimization bought", "",
               f"Each variant replays the full file; {abl['reps']} interleaved runs, median and range of ns per message "
               "(parse + book update).", "",
               "| Variant | ns/msg (median) | Range | vs baseline | vs previous step | M msgs/s | adds to overflow | ladder RAM |",
               "|---|---|---|---|---|---|---|---|"]
        for x in abl["variants"]:
            n = x["ns_per_msg"]
            prev = f"{v[x['prev']]['ns_per_msg']['median'] / n['median']:.2f}×" if x["prev"] else "—"
            ov = f"{x['overflow_adds']:,}" if "ladder" in x["key"] or x["key"] in ("recenter", "window_8k", "window_32k") else "—"
            ram = (f"{x['ladder_resident_mb']:.0f} MB of {x['ladder_reserved_mb']:.0f} MB reserved"
                   if x.get("ladder_reserved_mb") else "—")
            md.append(f"| {x['label'].strip()} | {n['median']:.1f} | {n['min']:.1f} – {n['max']:.1f} | "
                      f"{x['speedup_vs_baseline']:.2f}× | {prev} | {1e3 / n['median']:.1f} | {ov} | {ram} |")
        md += ["", "“vs previous step” follows baseline → flat orders → flat ladder → recenter; the window rows compare "
               "against the 2,048-tick recentering book; “flat ladder + std::unordered_map” isolates the ladder alone. "
               "Ladders are mmap'd, so a wide window reserves address space but only touched pages use RAM "
               "(measured with `mincore`).", ""]
        rc = v["recenter"]
        wins = [k for k in ("recenter", "window_8k", "window_32k") if k in v]
        best = min(wins, key=lambda k: v[k]["ns_per_msg"]["median"])
        win_ticks = {"recenter": 2048, "window_8k": 8192, "window_32k": 32768}
        md += [f"Fastest window: **{win_ticks[best]:,} ticks** ({v[best]['ns_per_msg']['median']:.1f} ns/msg, "
               f"{v[best]['overflow_adds']:,} overflow adds). Build with it: "
               f"`cmake -S . -B build -DITCH_LADDER_WINDOW={win_ticks[best]}`.", ""]
        c = rc["counters"]
        if c.get("hw"):
            md += ["Hardware counters per message (final variant vs baseline):", "",
                   "| Variant | IPC | instructions | branch misses | L1d misses | LLC misses |", "|---|---|---|---|---|---|"]
            for x in (v["baseline"], rc):
                k = x["counters"]
                md.append(f"| {x['label']} | {k['ipc']:.2f} | {k['instructions_per']:.0f} | {k['branch_misses_per']:.3f} | "
                          f"{k['l1d_misses_per']:.3f} | {k['llc_misses_per']:.3f} |")
            md.append("")
        else:
            md += ["Hardware counters: not available on this machine. On Linux, `ablation` reads cycles, instructions, "
                   "branch and cache misses through `perf_event_open`; macOS exposes them only to Instruments or to root "
                   "via the private kperf framework, and most VMs don't expose them at all.", ""]
        bar_plot(os.path.join(PLOTS, "ablation_book.svg"), "Order book: ns per ITCH message",
                 f"{msgs / 1e6:.1f}M NASDAQ messages, parse + book update",
                 [(x["label"], x["ns_per_msg"]["median"], x["ns_per_msg"]["min"], x["ns_per_msg"]["max"]) for x in abl["variants"]],
                 "ns per message", "shorter")
        md += ["![Order book ablation](plots/ablation_book.svg)", ""]

    # Book latency.
    if lmap or lfast:
        md += ["## Order book latency per message", "",
               "Each message timed individually (parse + book update), including timer overhead.", "",
               "| Book | mean | p50 | p90 | p99 | p99.9 | p99.99 | max |", "|---|---|---|---|---|---|---|---|"]
        for name, l in (("std::map", lmap), ("fast", lfast)):
            if l:
                md.append(f"| {name} | {l['mean_ns']:.0f} | {l['p50_ns']:.0f} | {l['p90_ns']:.0f} | {l['p99_ns']:.0f} | "
                          f"{l['p99_9_ns']:.0f} | {l['p99_99_ns']:.0f} | {l['max_ns']:,.0f} |")
        res = (lfast or lmap).get("resolution_ns")
        if res:
            md += ["", f"Clock resolution {res:.0f} ns: every sample is a multiple of it, so percentiles below about "
                   f"{res:.0f} ns are not meaningful."]
        latency_plot(os.path.join(PLOTS, "latency_book.svg"), "Order book latency per ITCH message",
                     [(n, l.get("histogram_ticks"), l.get("ns_per_tick", 1)) for n, l in (("std::map book", lmap), ("fast book", lfast)) if l],
                     "percentile distribution, log scale (HdrHistogram style)")
        md += ["", "![Order book latency](plots/latency_book.svg)", ""]

    # Engine.
    if eng:
        fs = eng.get("fast_ns_per_order_spread", {})
        md += ["## Matching engine", "",
               f"{eng['orders']:,} random orders ({eng['limits']:,} limit, {eng['markets']:,} market, {eng['cancels']:,} cancel), "
               f"{eng['fills']:,} fills.", "",
               "| Engine | M orders/s | ns/order | M fills/s |", "|---|---|---|---|",
               f"| reference (std::map + std::deque) | {eng['ref_orders_per_s'] / 1e6:.2f} | {eng['ref_ns_per_order']:.1f} | "
               f"{eng['fills'] / (eng['ref_ns_per_order'] * 1e-9 * eng['orders']) / 1e6:.2f} |",
               f"| fast | {eng['fast_orders_per_s'] / 1e6:.2f} | {eng['fast_ns_per_order']:.1f}"
               f"{' (' + format(fs['min'], '.1f') + ' – ' + format(fs['max'], '.1f') + ')' if fs else ''} | "
               f"{eng['fast_fills_per_s'] / 1e6:.2f} |", "",
               f"Speedup {eng['speedup']:.2f}×. Per-order latency (fast): p50 {eng['latency_p50_ns']:.0f} ns, "
               f"p99 {eng['latency_p99_ns']:.0f} ns, p99.9 {eng['latency_p99_9_ns']:.0f} ns, p99.99 {eng['latency_p99_99_ns']:.0f} ns "
               f"(clock resolution {eng['latency_resolution_ns']:.0f} ns).", ""]
        if eng.get("capacity_sweep"):
            md += ["Order-pool capacity vs cost per order. The id map is 2× capacity × 16 B; once it outgrows cache, "
                   "every lookup misses:", "", "| Capacity | id map | ns/order (median) |", "|---|---|---|"]
            for c in eng["capacity_sweep"]:
                md.append(f"| {c['capacity']:,} | {c['capacity'] * 32 / 1e6:.1f} MB | {c['ns_per_order']['median']:.1f} |")
            bar_plot(os.path.join(PLOTS, "engine_capacity.svg"), "Matching engine: ns per order vs pool capacity",
                     "id map = 2 × capacity × 16 B",
                     [(f"{c['capacity']:,} orders ({c['capacity'] * 32 / 1e6:.1f} MB map)", c["ns_per_order"]["median"],
                       c["ns_per_order"]["min"], c["ns_per_order"]["max"]) for c in eng["capacity_sweep"]],
                     "ns per order", "shorter")
            md += ["", "![Engine capacity](plots/engine_capacity.svg)", ""]
        if eng.get("hist_ticks"):
            latency_plot(os.path.join(PLOTS, "latency_engine.svg"), "Matching engine latency per order",
                         [("fast engine", eng["hist_ticks"], eng.get("ns_per_tick", 1))],
                         "percentile distribution, log scale")
            md += ["![Engine latency](plots/latency_engine.svg)", ""]

    # Pipeline.
    if pipe and "throughput_mmsgs_per_s" in pipe:
        t = pipe["throughput_mmsgs_per_s"]
        names = {"inline": "inline (1 thread, no queue)", "pipeline": "pipeline: padded + cached + batched head",
                 "no_head_batch": "− head batching", "no_padding": "− cache-line padding", "naive": "− index caching (naive)"}
        md += ["## Two-thread pipeline", "",
               f"Parser thread → SPSC ring ({pipe['queue_slots']} × {pipe['event_bytes']} B) → book thread; "
               f"{t['inline']['n']} interleaved runs, placement: {pipe['placement']}.", "",
               "| Variant | M msgs/s (median) | Range | vs inline |", "|---|---|---|---|"]
        for k, lab in names.items():
            x = t[k]
            md.append(f"| {lab} | {x['median']:.2f} | {x['min']:.2f} – {x['max']:.2f} | {x['median'] / t['inline']['median']:.2f}× |")
        li, lp = pipe["latency_inline"], pipe["latency_pipeline"]
        md += ["", f"End-to-end latency at {pipe['paced_rate_mmsgs_per_s']:.1f}M msgs/s, paced "
               f"(producer late for {100 * pipe['paced_late_fraction']:.2f}% of sends):", "",
               "| Path | p50 | p90 | p99 | p99.9 | p99.99 |", "|---|---|---|---|---|---|"]
        for lab, l in (("inline, no queue", li), ("through the queue", lp)):
            md.append(f"| {lab} | {l['p50_ns']:.0f} | {l['p90_ns']:.0f} | {l['p99_ns']:.0f} | {l['p99_9_ns']:,.0f} | {l['p99_99_ns']:,.0f} |")
        pc = pipe.get("paced_counters")
        if pc:
            md += ["", f"During the paced run: {pc['involuntary_cs']:,} involuntary and {pc['voluntary_cs']:,} voluntary "
                   "context switches. Each involuntary switch stalls a spinning thread for microseconds and shows up "
                   "directly in the p99.9+ tail. macOS has no hard core pinning, so these can't be eliminated there."]
        if pipe["paced_late_fraction"] > 0.01:
            md += ["", "**Warning:** the producer fell behind the paced rate, so these latencies include queueing. "
                   "Rerun `pipeline_bench` with a lower `--rate`."]
        bar_plot(os.path.join(PLOTS, "ablation_pipeline.svg"), "Pipeline throughput by queue optimization",
                 "ITCH messages per second, end to end",
                 [(names[k], t[k]["median"], t[k]["min"], t[k]["max"]) for k in names], "M msgs/s", "longer")
        latency_plot(os.path.join(PLOTS, "latency_pipeline.svg"), "Pipeline end-to-end latency per order message",
                     [("inline, no queue", pipe.get("hist_inline_ticks"), pipe.get("ns_per_tick", 1)),
                      ("through the queue", pipe.get("hist_pipeline_ticks"), pipe.get("ns_per_tick", 1))],
                     f"paced at {pipe['paced_rate_mmsgs_per_s']:.1f}M msgs/s, log scale")
        md += ["", "![Pipeline ablation](plots/ablation_pipeline.svg)", "", "![Pipeline latency](plots/latency_pipeline.svg)", ""]

    # Resume numbers.
    headline = {}
    if abl and lfast:
        vv = {x["key"]: x for x in abl["variants"]}
        key = {2048: "recenter", 8192: "window_8k", 32768: "window_32k"}.get(lfast.get("ladder_window", 2048), "recenter")
        rc = vv.get(key, vv["recenter"])
        mps = 1e3 / rc["ns_per_msg"]["median"]
        md += ["## Headline numbers", "",
               f"- Messages replayed: **{abl['messages'] / 1e6:.1f}M**",
               f"- Fast book throughput (parse + book): **{mps:.1f}M msgs/s** ({rc['ns_per_msg']['median']:.1f} ns/msg)",
               f"- Fast book p99 per message: **{lfast['p99_ns']:.0f} ns** (p99.9 {lfast['p99_9_ns']:.0f} ns)",
               f"- Speedup over std::map baseline: **{rc['speedup_vs_baseline']:.2f}×**",
               f"- Build: {lfast.get('ladder_window', 2048):,}-tick price window"]
        headline = {"n": abl["messages"] / 1e6, "mps": mps, "p99": lfast["p99_ns"], "speedup": rc["speedup_vs_baseline"]}
        if parse:
            md.append(f"- Parser alone: **{parse['mmsgs_per_s_best']:.0f}M msgs/s** ({parse['gb_per_s_best']:.1f} GB/s)")
        if eng:
            md.append(f"- Matching engine: **{eng['fast_orders_per_s'] / 1e6:.1f}M orders/s**, "
                      f"{eng['speedup']:.2f}× the reference, {eng['diff_passed']}/{eng['diff_seeds']} randomized seeds passed")
        md.append("")

    bullet = None
    if headline:
        bullet = (f"Replayed {headline['n']:.1f}M NASDAQ ITCH 5.0 messages at {headline['mps']:.1f}M msgs/s with "
                  f"{headline['p99']:.0f} ns p99 latency, {headline['speedup']:.1f}× faster than a std::map baseline, "
                  "using a flat price ladder and a lock-free SPSC queue.")
        md += ["## Resume bullet", "", f"> {bullet}", ""]

    with open(os.path.join(RES, "REPORT.md"), "w") as f:
        f.write("\n".join(md))
    update_readme(parse, bchk, fchk, abl, lfast, lmap, eng, pipe, headline, bullet)
    print("wrote results/REPORT.md and", ", ".join(sorted(os.listdir(PLOTS))))


if __name__ == "__main__":
    sys.exit(main())
