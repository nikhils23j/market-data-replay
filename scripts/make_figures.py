#!/usr/bin/env python3
"""Draw the README figures from results.json. Reads nothing else.

usage: python3 scripts/make_figures.py      (writes docs/img/*.png)

Every plotted value comes from results.json (written by `make eval`).
Nothing here re-runs a benchmark.
"""
import json
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FuncFormatter, NullFormatter  # noqa: E402

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "docs" / "img"

# Palette: fixed categorical order (validated for CVD separation), text inks, hairline grid.
BOOKS = ["bitmap", "std_map", "sorted_vector"]
BOOK_LABEL = {"bitmap": "bitmap", "std_map": "std::map", "sorted_vector": "sorted vector"}
BOOK_COLOR = {"bitmap": "#2a78d6", "std_map": "#eb6834", "sorted_vector": "#1baf7a"}
INK, INK2, MUTED, GRID = "#0b0b0b", "#52514e", "#8a8984", "#e4e3df"

plt.rcParams.update({
    "figure.facecolor": "white", "axes.facecolor": "white", "savefig.facecolor": "white",
    "font.size": 11, "axes.titlesize": 12.5, "axes.titleweight": "bold", "axes.titlelocation": "left",
    "axes.edgecolor": GRID, "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
    "text.color": INK, "axes.spines.top": False, "axes.spines.right": False,
    "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.8, "axes.axisbelow": True,
    "legend.frameon": False,
})


def fmt_ns(v, _pos=None):
    if v >= 1e6:
        return f"{v / 1e6:g} ms"
    if v >= 1e3:
        return f"{v / 1e3:g} µs"
    return f"{v:g} ns"


def throughput(bench):
    sections = [
        ("apply_only", "Book only\n(pre-decoded events, 1 thread)"),
        ("inline_1t", "A+B arbiter + decode + book\n(1 thread)"),
        ("pipeline_2t", "A+B arbiter + decode + book\n(2-thread SPSC pipeline)"),
    ]
    rows = {(r["section"], r["book"]): r for r in bench["throughput_msgs_per_sec"]}
    fig, ax = plt.subplots(figsize=(10, 5.4))
    width = 0.26
    for i, book in enumerate(BOOKS):
        xs, med, lo, hi = [], [], [], []
        for s, (sec, _) in enumerate(sections):
            r = rows[(sec, book)]
            xs.append(s + (i - 1) * (width + 0.02))
            med.append(r["median"] / 1e6)
            lo.append((r["median"] - r["min"]) / 1e6)
            hi.append((r["max"] - r["median"]) / 1e6)
        ax.bar(xs, med, width, color=BOOK_COLOR[book], label=BOOK_LABEL[book], zorder=2)
        ax.errorbar(xs, med, yerr=[lo, hi], fmt="none", ecolor=INK, elinewidth=1.2,
                    capsize=4, capthick=1.2, zorder=3)
        for x, m, h in zip(xs, med, hi):
            ax.text(x, m + h + 0.6, f"{m:.1f}M", ha="center", va="bottom", fontsize=9.5, color=INK2)
    ax.set_xticks(range(len(sections)), [lab for _, lab in sections])
    ax.tick_params(axis="x", length=0)
    ax.grid(axis="x", visible=False)
    ax.set_ylabel("messages / second (millions)")
    ax.set_ylim(0, max(r["max"] for r in rows.values()) / 1e6 * 1.18)
    ax.set_title(f"Book throughput on {bench['messages'] / 1e6:g}M messages "
                 f"(bar = median of {bench['reps']} runs, whisker = min–max)")
    ax.legend(loc="upper right", ncols=3)
    fig.tight_layout()
    fig.savefig(OUT / "throughput.png", dpi=200)
    plt.close(fig)


def latency(bench):
    pcts = [("p50", "p50"), ("p90", "p90"), ("p99", "p99"), ("p99_9", "p99.9")]
    lat = bench["latency_ns"]
    panels = [
        ("apply_latency", "apply() alone, 1 thread\n(each call timed, 1 run)"),
        ("paced_2t_clean", "Paced 1M msg/s, clean line\n(no recoveries, 3 runs)"),
        ("paced_2t_ab", f"Paced 1M msg/s, A+B lines\n({bench['arbiter']['recoveries']} snapshot recoveries, 3 runs)"),
    ]
    tick_ns = 1e9 / bench["timer_hz"]
    fig, axes = plt.subplots(1, 3, figsize=(12, 5.2), sharey=True)
    for ax, (sec, title) in zip(axes, panels):
        for i, book in enumerate(BOOKS):
            runs = [r for r in lat if r["section"] == sec and r["book"] == book]
            xs = [p + (i - 1) * 0.14 for p in range(len(pcts))]
            vals = [sorted(r[k] for r in runs) for k, _ in pcts]
            med = [v[len(v) // 2] for v in vals]
            ax.plot(xs, med, color=BOOK_COLOR[book], lw=2, marker="o", ms=7,
                    mec="white", mew=1.5, label=BOOK_LABEL[book], zorder=3)
            if len(runs) > 1:
                ax.errorbar(xs, med, yerr=[[m - v[0] for m, v in zip(med, vals)],
                                           [v[-1] - m for m, v in zip(med, vals)]],
                            fmt="none", ecolor=BOOK_COLOR[book], elinewidth=1.4, capsize=3, zorder=2)
        ax.axhline(tick_ns, color=MUTED, lw=1, zorder=1)
        ax.set_xticks(range(len(pcts)), [lab for _, lab in pcts])
        ax.grid(axis="x", visible=False)
        ax.set_title(title, fontsize=11)
        ax.set_yscale("log")
    axes[0].text(len(pcts) - 1.05, tick_ns * 0.78, f"1 timer tick = {tick_ns:.1f} ns",
                 ha="right", va="top", fontsize=9, color=INK2)
    axes[0].yaxis.set_major_formatter(FuncFormatter(fmt_ns))
    axes[0].yaxis.set_minor_formatter(NullFormatter())
    axes[0].set_ylabel("latency (log scale)")
    axes[0].legend(loc="upper left")
    fig.suptitle("Latency percentiles: dot = median run, whisker = min–max across runs",
                 x=0.01, ha="left", fontweight="bold", fontsize=12.5)
    fig.tight_layout()
    fig.savefig(OUT / "latency_percentiles.png", dpi=200)
    plt.close(fig)


def recovery(bench):
    """Where the A+B stream's messages came from, and how long each recovery stalled the book."""
    stalls = {b: sorted(r["restore_max_us"] for r in bench["latency_ns"]
                        if r["section"] == "paced_2t_ab" and r["book"] == b) for b in BOOKS}
    restore = {r["book"]: r for r in bench["snapshot_restore_us"]}
    fig, ax = plt.subplots(figsize=(10, 4.4))
    ys = range(len(BOOKS))
    for y, book in zip(ys, BOOKS):
        r = restore[book]
        ax.errorbar([r["median"]], [y + 0.15], xerr=[[r["median"] - r["min"]], [r["max"] - r["median"]]],
                    fmt="s", ms=8, color=BOOK_COLOR[book], mec="white", mew=1.5, ecolor=BOOK_COLOR[book],
                    elinewidth=1.4, capsize=3)
        s = stalls[book]
        ax.plot(s, [y - 0.15] * len(s), "o", ms=8, color=BOOK_COLOR[book], mec="white", mew=1.5, alpha=0.95)
        ax.hlines(y - 0.15, s[0], s[-1], color=BOOK_COLOR[book], lw=1.4)
    ax.set_yticks(list(ys), [BOOK_LABEL[b] for b in BOOKS])
    ax.invert_yaxis()
    ax.grid(axis="y", visible=False)
    ax.set_xlabel("microseconds (axis does not start at 0)")
    lo = min(min(restore[b]["min"] for b in BOOKS), min(min(v) for v in stalls.values()))
    hi = max(max(restore[b]["max"] for b in BOOKS), max(max(v) for v in stalls.values()))
    ax.set_xlim(lo - 0.12 * (hi - lo), hi + 0.08 * (hi - lo))  # dots, not bars: no zero baseline needed
    from matplotlib.lines import Line2D
    handles = [
        Line2D([], [], marker="s", ls="none", color=INK2, ms=8,
               label=f"offline snapshot rebuild ({restore['bitmap']['orders']:,} orders; median, min–max)"),
        Line2D([], [], marker="o", ls="none", color=INK2, ms=8,
               label="longest stall seen by the book thread, per paced A+B run"),
    ]
    ax.legend(handles=handles, loc="upper center", bbox_to_anchor=(0.5, -0.17), ncols=1, fontsize=9.5)
    ax.set_title("What a snapshot recovery costs the book thread")
    fig.tight_layout()
    fig.savefig(OUT / "recovery_stall.png", dpi=200)
    plt.close(fig)


def main():
    results = json.loads((ROOT / "results.json").read_text())
    bench = results["bench"]
    OUT.mkdir(parents=True, exist_ok=True)
    throughput(bench)
    latency(bench)
    recovery(bench)
    for p in sorted(OUT.glob("*.png")):
        print(f"{p.relative_to(ROOT)}  {p.stat().st_size / 1024:.0f} KB")


if __name__ == "__main__":
    main()
