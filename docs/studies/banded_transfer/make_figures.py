"""Figures and tables of the banded-exhaust study from the driver's CSV output.

    spacetrains_banded_study study  <data> 36 [threads] [done results.csv] [stride]
    spacetrains_banded_study timing <data> 6
    spacetrains_banded_study tune   <data> <variant>   (0..8, one process each)
    python3 docs/studies/banded_transfer/make_figures.py <data> [<more data dirs>]

Writes figures/*.png and tables.tex next to this script.
"""
import csv
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = Path(__file__).resolve().parent
FIG = HERE / "figures"

# Reference categorical palette (dataviz skill, light mode), fixed order.
BLUE, ORANGE, AQUA, YELLOW, MAGENTA, GREEN, VIOLET, RED = (
    "#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300", "#6250d6", "#e34948")
INK, INK2, MUTED, GRID = "#0b0b0b", "#52514e", "#8a8984", "#e4e3df"
HULLS = ["plasma_freighter", "plasma_courier", "plasma_bulk_tanker", "plasma_liner", "large_plasma_liner"]
HULL_NAME = {"plasma_freighter": "Freighter", "plasma_courier": "Courier", "plasma_bulk_tanker": "Bulk tanker",
             "plasma_liner": "Liner", "large_plasma_liner": "Large liner"}
HULL_COLOR = dict(zip(HULLS, [BLUE, ORANGE, AQUA, YELLOW, MAGENTA]))
ROUTES = ["earth-mars", "mars-earth", "earth-venus", "earth-mercury", "earth-ceres", "mars-ceres", "earth-jupiter",
          "jupiter-saturn"]
BODY = {"earth": "Earth", "mars": "Mars", "venus": "Venus", "mercury": "Mercury", "ceres": "Ceres", "jupiter": "Jupiter",
        "saturn": "Saturn"}
ARC_NAME = ["coast", "ceiling (250 km/s)", "inside the band", "floor (50 km/s)"]
ARC_COLOR = [MUTED, BLUE, AQUA, ORANGE]


def route_name(r):
    a, b = r.split("-")
    return f"{BODY[a]} → {BODY[b]}"


def style():
    plt.rcParams.update({
        "font.family": "DejaVu Sans", "font.size": 8.5, "axes.titlesize": 9, "axes.labelsize": 8.5,
        "axes.edgecolor": MUTED, "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
        "axes.grid": True, "grid.color": GRID, "grid.linewidth": 0.6, "axes.spines.top": False,
        "axes.spines.right": False, "lines.linewidth": 1.6, "legend.frameon": False, "savefig.dpi": 200,
        "figure.dpi": 100, "text.color": INK,
    })


def read(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def truthy(v):
    return v in ("1", "true", "True")


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return float("nan")


def save(fig, name):
    FIG.mkdir(exist_ok=True)
    fig.savefig(FIG / f"{name}.png", bbox_inches="tight")
    plt.close(fig)


def stats(values):
    v = [x for x in values if math.isfinite(x)]
    if not v:
        return (float("nan"),) * 3 + (0,)
    return min(v), max(v), statistics.fmean(v), len(v)


# B as a planner would use it: the banded optimiser, and A's plan wherever the optimiser falls short
# of it (B's optimum can never be worse than A, so a shortfall is a solver miss, counted separately).

def b_time(r):
    t = [num(r[f"{k}_tmin_d"]) for k in ("A", "B") if truthy(r[f"{k}_ok"])]
    return min(t) if t else float("nan")


def b_cost(r):
    c = [(num(r[f"econ{k}_cost"]), k) for k in ("A", "B") if truthy(r[f"econ{k}_ok"])]
    return min(c) if c else (float("nan"), "")


def b_time_miss(r):
    return truthy(r["A_ok"]) and (not truthy(r["B_ok"]) or num(r["B_tmin_d"]) > 1.001 * num(r["A_tmin_d"]))


def b_cost_miss(r):
    return truthy(r["econA_ok"]) and (not truthy(r["econB_ok"]) or num(r["econB_cost"]) > 1.001 * num(r["econA_cost"]))


def same_ok(r):
    """Same-T pair on the same branch: B solved, and not below the ideal engine (a different local optimum)."""
    a, b, i = num(r["same_A_kg"]), num(r["same_B_kg"]), num(r["same_ideal_kg"])
    return a > 0 and b > 0 and b >= 0.999 * i


def branch_switch(r):
    return num(r["same_B_kg"]) > 0 and num(r["same_B_kg"]) < 0.999 * num(r["same_ideal_kg"])


# ---- Tables -------------------------------------------------------------------------------------

def tex_row(cells):
    return " & ".join(cells) + r" \\"


def fmt(x, d=1):
    return "--" if not math.isfinite(x) else f"{x:.{d}f}"


def tables(results, timing, tune, ships, out):
    base = [r for r in results if num(r["tank"]) == 1.0]
    lines = []

    # 0. Ships.
    lines.append(r"\newcommand{\tabships}{")
    lines.append(r"\begin{tabular}{lrrrrrrrr}\toprule")
    lines.append(tex_row(["Ship", "dry (t)", "tank (t)", "payload (t)", "$m_0$ (t)", "power (MW)", r"$a_{\min}$ (mm/s$^2$)",
                          r"$a_{\max}$ (mm/s$^2$)", "cr/day"]) + r" \midrule")
    for h in HULLS if ships else []:
        sp = next(r for r in ships if r["ship"] == h)
        lines.append(tex_row([HULL_NAME[h], fmt(num(sp["dry_kg"]) / 1e3, 1), fmt(num(sp["capacity_kg"]) / 1e3, 0),
                              fmt(num(sp["payload_kg"]) / 1e3, 1), fmt(num(sp["m0_kg"]) / 1e3, 1), fmt(num(sp["power_w"]) / 1e6, 1),
                              fmt(1e3 * num(sp["a_full_power_250_mps2"]), 2), fmt(1e3 * num(sp["a_full_power_50_mps2"]), 1),
                              fmt(num(sp["daily_cost"]), 0)]))
    lines.append(r"\bottomrule\end{tabular}}")

    # 1. Same transfer time: A's excess propellant over B (%), path separation.
    lines.append(r"\newcommand{\tabsametime}{")
    lines.append(r"\begin{tabular}{lrrrrrrrrr}\toprule")
    lines.append(tex_row(["Ship", "cases", r"min \%", r"max \%", r"mean \%", "coast", r"sep.\ mean", r"sep.\ max", "no B", "branch"]) )
    lines.append(r" & & \multicolumn{3}{c}{A's extra propellant over B} & of $T$ & \multicolumn{2}{c}{($10^6$ km)} & & \\ \midrule")
    for h in HULLS:
        rows = [r for r in base if r["hull"] == h and same_ok(r)]
        ex = [100 * (num(r["same_A_kg"]) / num(r["same_B_kg"]) - 1) for r in rows]
        lo, hi, mean, n = stats(ex)
        coast = statistics.fmean([num(r["coast_frac"]) for r in rows]) if rows else float("nan")
        sep = [num(r["sep_max_km"]) / 1e6 for r in rows]
        all_rows = [r for r in base if r["hull"] == h and num(r["same_A_kg"]) > 0]
        no_b = sum(1 for r in all_rows if not num(r["same_B_kg"]) > 0)
        br = sum(1 for r in all_rows if branch_switch(r))
        lines.append(tex_row([HULL_NAME[h], str(n), fmt(lo, 2), fmt(hi, 1), fmt(mean, 2), fmt(100 * coast, 0) + r"\%",
                              fmt(statistics.fmean(sep) if sep else float('nan'), 1), fmt(max(sep) if sep else float('nan'), 1),
                              str(no_b), str(br)]))
    lines.append(r"\bottomrule\end{tabular}}")

    # 2. Full tank: shortest transfer time, A against B.
    lines.append(r"\newcommand{\tabmintime}{")
    lines.append(r"\begin{tabular}{lrrrrrrr}\toprule")
    lines.append(tex_row(["Ship", "departures", "A flies", "B flies", "solver miss", r"min \%", r"max \%", r"mean \%"]))
    lines.append(r" & & \multicolumn{3}{c}{} & \multicolumn{3}{c}{A's extra time over B} \\ \midrule")
    for h in HULLS:
        rows = [r for r in base if r["hull"] == h]
        fly_a = sum(1 for r in rows if truthy(r["A_ok"]))
        fly_b = sum(1 for r in rows if math.isfinite(b_time(r)))
        miss = sum(1 for r in rows if b_time_miss(r))
        dt = [100 * (num(r["A_tmin_d"]) / b_time(r) - 1) for r in rows if truthy(r["A_ok"])]
        lo, hi, mean, _ = stats(dt)
        lines.append(tex_row([HULL_NAME[h], str(len(rows)), str(fly_a), str(fly_b), str(miss), fmt(lo, 1), fmt(hi, 1),
                              fmt(mean, 2)]))
    lines.append(r"\bottomrule\end{tabular}}")

    # 3. Economic optimum (fuel bought + time): A against B, same tank.
    lines.append(r"\newcommand{\tabeconomic}{")
    lines.append(r"\begin{tabular}{lrrrrrrr}\toprule")
    lines.append(tex_row(["Ship", "cases", r"min \%", r"max \%", r"mean \%", r"$T_A/T_B$", r"fuel$_A$/fuel$_B$", "solver miss"]))
    lines.append(r" & & \multicolumn{3}{c}{A's extra trip cost over B} & \multicolumn{2}{c}{mean, B solved} & \\ \midrule")
    for h in HULLS:
        rows = [r for r in base if r["hull"] == h and truthy(r["econA_ok"])]
        dc = [100 * (num(r["econA_cost"]) / b_cost(r)[0] - 1) for r in rows]
        lo, hi, mean, n = stats(dc)
        won = [r for r in rows if b_cost(r)[1] == "B"]
        tr = statistics.fmean([num(r["econA_T_d"]) / num(r["econB_T_d"]) for r in won]) if won else float("nan")
        fr = statistics.fmean([num(r["econA_kg"]) / num(r["econB_kg"]) for r in won]) if won else float("nan")
        miss = sum(1 for r in rows if b_cost_miss(r))
        lines.append(tex_row([HULL_NAME[h], str(n), fmt(lo, 2), fmt(hi, 2), fmt(mean, 2), fmt(tr, 3), fmt(fr, 3), str(miss)]))
    lines.append(r"\bottomrule\end{tabular}}")

    # 4. Tank choice: which tank variant gives the cheapest trip, per approach.
    lines.append(r"\newcommand{\tabtanks}{")
    lines.append(r"\begin{tabular}{lrrrrrrrrr}\toprule")
    lines.append(tex_row(["Ship", r"\multicolumn{4}{c}{A: best tank}", r"\multicolumn{4}{c}{B: best tank}", "differ"]))
    lines.append(tex_row(["", r"50\%", r"100\%", r"150\%", r"200\%", r"50\%", r"100\%", r"150\%", r"200\%", ""]) + r" \midrule")
    groups = defaultdict(list)
    for r in results:
        groups[(r["hull"], r["route"], r["index"])].append(r)
    for h in HULLS:
        ca, cb = defaultdict(int), defaultdict(int)
        differ = 0
        for (hh, _, _), rows in groups.items():
            if hh != h:
                continue
            best = {}
            ok = [r for r in rows if truthy(r["econA_ok"])]
            if ok:
                best["A"] = min(ok, key=lambda r: num(r["econA_cost"]))["tank"]
            ok = [r for r in rows if math.isfinite(b_cost(r)[0])]
            if ok:
                best["B"] = min(ok, key=lambda r: b_cost(r)[0])["tank"]
            if "A" in best:
                ca[best["A"]] += 1
            if "B" in best:
                cb[best["B"]] += 1
            if "A" in best and "B" in best and best["A"] != best["B"]:
                differ += 1
        tanks = ["0.5", "1", "1.5", "2"]
        lines.append(tex_row([HULL_NAME[h]] + [str(ca[t]) for t in tanks] + [str(cb[t]) for t in tanks] + [str(differ)]))
    lines.append(r"\bottomrule\end{tabular}}")

    # 5. Timing (one thread).
    if timing:
        lines.append(r"\newcommand{\tabtiming}{")
        lines.append(r"\begin{tabular}{lrrrrrrr}\toprule")
        lines.append(tex_row(["", "solved", "median", "mean", "p90", "p99", "max", r"over 100\,ms"]) + r" \midrule")

        def pct(v, q):
            return v[min(len(v) - 1, int(q * len(v)))]

        specs = [("Earliest arrival, ideal engine", "ideal_ms", "ideal_ok"),
                 ("Earliest arrival, A", "A_ms", "A_ok"),
                 ("Earliest arrival, B", "B_ms", "B_ok"),
                 ("One B solve, from the ideal seed", "cold_ms", "B_ok"),
                 ("One B solve, warm (neighbouring $T$)", "warm_ms", "B_ok")]
        for label, key, okk in specs:
            v = sorted(num(r[key]) for r in timing if truthy(r[okk]) and num(r[key]) > 0)
            n_ok = sum(1 for r in timing if truthy(r[okk]))
            lines.append(tex_row([label, f"{n_ok}/{len(timing)}", fmt(statistics.median(v), 1), fmt(statistics.fmean(v), 1),
                                  fmt(pct(v, 0.9), 1), fmt(pct(v, 0.99), 0), fmt(v[-1], 0), str(sum(1 for x in v if x > 100))]))
        fails = [num(r["B_ms"]) for r in timing if not truthy(r["B_ok"])]
        lines.append(tex_row(["B search that fails", f"{len(fails)}", "", fmt(statistics.fmean(fails), 0) if fails else "--",
                              "", "", fmt(max(fails), 0) if fails else "--", ""]))
        lines.append(r"\bottomrule\end{tabular}}")

    # 6. Tuning.
    if tune:
        lines.append(r"\newcommand{\tabtuning}{")
        lines.append(r"\begin{tabular}{lrrrrrr}\toprule")
        lines.append(tex_row(["Variant", "solved", "A flies: solved", "median", "mean", "mean", "propagations"]))
        lines.append(r" & & & \multicolumn{2}{c}{ms, solved} & ms, failed & mean \\ \midrule")
        for name, rows in tune:
            tried = [r for r in rows if truthy(r["ideal_ok"])]
            ok = [r for r in tried if truthy(r["converged"])]
            feas = [r for r in tried if num(r["a_floor"]) <= 1.0]
            feas_ok = [r for r in feas if truthy(r["converged"])]
            ms_ok = [num(r["ms"]) for r in ok]
            ms_bad = [num(r["ms"]) for r in tried if not truthy(r["converged"])]
            props = statistics.fmean(num(r["propagations"]) for r in tried) if tried else float("nan")
            lines.append(tex_row([name.replace("%", r"\%"), f"{len(ok)}/{len(tried)}", f"{len(feas_ok)}/{len(feas)}",
                                  fmt(statistics.median(ms_ok), 1), fmt(statistics.fmean(ms_ok), 0),
                                  fmt(statistics.fmean(ms_bad) if ms_bad else float("nan"), 0), fmt(props, 0)]))
        lines.append(r"\bottomrule\end{tabular}}")

    # Headline numbers for the text.
    def headline(name, value):
        lines.append(rf"\newcommand{{\{name}}}{{{value}}}")

    same = [100 * (num(r["same_A_kg"]) / num(r["same_B_kg"]) - 1) for r in base if same_ok(r)]
    headline("sameN", str(len(same)))
    headline("sameMin", fmt(min(same), 2))
    headline("sameMean", fmt(statistics.fmean(same), 1))
    headline("sameMedian", fmt(statistics.median(same), 1))
    headline("sameMax", fmt(max(same), 0))
    headline("sameNoB", str(sum(1 for r in base if num(r["same_A_kg"]) > 0 and not num(r["same_B_kg"]) > 0)))
    headline("sameBranch", str(sum(1 for r in base if branch_switch(r))))
    dt = [100 * (num(r["A_tmin_d"]) / b_time(r) - 1) for r in base if truthy(r["A_ok"])]
    headline("timeMean", fmt(statistics.fmean(dt), 1))
    headline("timeMax", fmt(max(dt), 0))
    headline("timeMiss", str(sum(1 for r in base if b_time_miss(r))))
    headline("onlyB", str(sum(1 for r in base if math.isfinite(b_time(r)) and not truthy(r["A_ok"]))))
    headline("nBase", str(len(base)))
    headline("nAll", str(len(results)))
    econ = [100 * (num(r["econA_cost"]) / b_cost(r)[0] - 1) for r in base if truthy(r["econA_ok"])]
    headline("econMean", fmt(statistics.fmean(econ), 2))
    headline("econMax", fmt(max(econ), 1))
    headline("econMiss", str(sum(1 for r in base if b_cost_miss(r))))
    solves = sum(num(r["b_solves"]) for r in results)
    fails = sum(num(r["b_fail"]) for r in results)
    headline("bSolves", f"{solves:,.0f}".replace(",", "\\,"))
    headline("bFailPct", fmt(100 * fails / solves, 1))
    if timing:
        miss = sorted(num(r["B_check_miss_km"]) for r in timing if truthy(r["B_ok"]))
        headline("checkMedianM", fmt(1000 * statistics.median(miss), 0))
        headline("checkNinetyNine", fmt(miss[int(0.99 * len(miss))], 0))
        headline("checkBad", str(sum(1 for x in miss if x > 1000)))
        headline("timingN", str(len(timing)))
    out.write_text("\n".join(lines) + "\n")


# ---- Figures ------------------------------------------------------------------------------------

def fig_by_phase(base, value, ylabel, name, ylim=None):
    fig, axes = plt.subplots(2, 4, figsize=(10, 4.6), sharey=True)
    for ax, route in zip(axes.flat, ROUTES):
        for h in HULLS:
            rows = sorted((r for r in base if r["hull"] == h and r["route"] == route), key=lambda r: num(r["phase_deg"]))
            pts = [(num(r["phase_deg"]), value(r)) for r in rows]
            pts = [p for p in pts if math.isfinite(p[1])]
            if pts:
                ax.plot([p[0] for p in pts], [p[1] for p in pts], "o-", color=HULL_COLOR[h], ms=2.2, lw=1.1,
                        label=HULL_NAME[h])
        ax.set_title(route_name(route))
        ax.set_xlim(0, 360)
        ax.set_xticks([0, 90, 180, 270, 360])
        if ylim:
            ax.set_ylim(*ylim)
    for ax in axes[1]:
        ax.set_xlabel("phase angle at departure (deg)")
    for ax in axes[:, 0]:
        ax.set_ylabel(ylabel)
    axes[0, 0].legend(loc="upper left", fontsize=7, ncol=1, handlelength=1.2)
    fig.tight_layout()
    save(fig, name)


SEMI_MAJOR = {"earth": 1.0, "mars": 1.52368, "venus": 0.723339, "mercury": 0.387098, "ceres": 2.76541, "jupiter": 5.20289,
              "saturn": 9.58256}


def fig_paths(paths, cases, name):
    fig, axes = plt.subplots(3, len(cases), figsize=(2.9 * len(cases), 7.6), gridspec_kw={"height_ratios": [2.2, 1.2, 0.9]},
                             squeeze=False)
    import numpy as np
    for col, key in enumerate(cases):
        hull, route, _ = key.split(",")
        rows = [r for r in paths if f"{r['ship']},{r['route']},{r['index']}" == key]
        a = [r for r in rows if r["approach"] == "A"]
        b = [r for r in rows if r["approach"] == "B"]
        ax = axes[0, col]
        th = np.linspace(0, 2 * np.pi, 200)
        for body in route.split("-"):
            ax.plot(SEMI_MAJOR[body] * np.cos(th), SEMI_MAJOR[body] * np.sin(th), color=GRID, lw=0.8, zorder=0)
        for i in range(len(b) - 1):
            arc = int(b[i]["arc"])
            ax.plot([num(b[i]["x_au"]), num(b[i + 1]["x_au"])], [num(b[i]["y_au"]), num(b[i + 1]["y_au"])],
                    color=ARC_COLOR[arc], lw=2.6, solid_capstyle="butt", zorder=2)
        ax.plot([num(r["x_au"]) for r in a], [num(r["y_au"]) for r in a], color=INK, lw=0.8, ls=(0, (3, 2)), zorder=3)
        ax.plot([0], [0], marker="o", color=YELLOW, ms=6, zorder=1)
        ax.set_aspect("equal")
        ax.set_title(f"{HULL_NAME[hull]}\n{route_name(route)}")
        ax.tick_params(labelsize=7)
        ax.set_xlabel("x (AU)")
        if col == 0:
            ax.set_ylabel("y (AU)")
        ax2 = axes[1, col]
        t = [num(r["t_d"]) for r in b]
        ax2.fill_between(t, [1e3 * num(r["amin_mps2"]) for r in b], [1e3 * num(r["amax_mps2"]) for r in b], color=BLUE, alpha=0.10,
                         lw=0, label="band at full power")
        ax2.plot([num(r["t_d"]) for r in a], [1e3 * num(r["a_mps2"]) for r in a], color=INK, lw=0.9, ls=(0, (3, 2)), label="A")
        ax2.plot(t, [1e3 * num(r["a_mps2"]) for r in b], color=BLUE, lw=1.4, label="B")
        ax2.set_xlabel("days")
        ax2.set_ylim(bottom=0)
        if col == 0:
            ax2.set_ylabel("thrust (mm/s²)")
            ax2.legend(fontsize=7, loc="upper center")
        ax3 = axes[2, col]
        sep = [math.hypot(num(ra["x_au"]) - num(rb["x_au"]), num(ra["y_au"]) - num(rb["y_au"])) * 149.6 for ra, rb in zip(a, b)]
        ax3.plot(t[:len(sep)], sep, color=VIOLET, lw=1.3)
        ax3.set_xlabel("days")
        ax3.set_ylim(bottom=0)
        if col == 0:
            ax3.set_ylabel("A–B distance\n(10⁶ km)")
    handles = [plt.Line2D([], [], color=INK, ls=(0, (3, 2)), lw=0.9, label="A: ideal path, throttled")]
    handles += [plt.Line2D([], [], color=c, lw=2.4, label=f"B: {n}") for c, n in zip(ARC_COLOR, ARC_NAME)]
    fig.legend(handles=handles, loc="lower center", ncol=5, fontsize=7.5, bbox_to_anchor=(0.5, -0.015))
    fig.tight_layout(rect=(0, 0.03, 1, 1))
    save(fig, name)


def fig_curves(curves, keys, name):
    fig, axes = plt.subplots(1, len(keys), figsize=(2.6 * len(keys), 2.6))
    for ax, (key, budget) in zip(axes, keys):
        rows = [r for r in curves if f"{r['ship']},{r['route']},{r['index']}" == key]
        rows.sort(key=lambda r: num(r["T_d"]))
        T = [num(r["T_d"]) for r in rows]
        ax.plot(T, [num(r["ideal_kg"]) / 1e3 for r in rows], color=MUTED, lw=1.2, label="ideal engine")
        ax.plot([num(r["T_d"]) for r in rows if truthy(r["A_ok"])], [num(r["A_kg"]) / 1e3 for r in rows if truthy(r["A_ok"])],
                color=ORANGE, lw=1.4, ls="--", label="A")
        ax.plot([num(r["T_d"]) for r in rows if truthy(r["B_ok"])], [num(r["B_kg"]) / 1e3 for r in rows if truthy(r["B_ok"])],
                color=BLUE, lw=1.6, label="B")
        ax.axhline(budget / 1e3, color=INK2, lw=0.8, ls=":")
        ax.text(T[-1], budget / 1e3, " tank", va="bottom", ha="right", fontsize=7, color=INK2)
        ax.set_title(f"{HULL_NAME[key.split(',')[0]]}, {route_name(key.split(',')[1])}")
        ax.set_xlabel("transfer time (days)")
        ax.set_ylim(0, budget / 1e3 * 1.25)
        hi = [num(r["T_d"]) for r in rows if num(r["ideal_kg"]) < budget * 1.25]
        if hi:
            ax.set_xlim(min(hi) * 0.95, max(T))
    axes[0].set_ylabel("propellant burned (t)")
    axes[0].legend(fontsize=7)
    fig.tight_layout()
    save(fig, name)


def fig_coast(base, name):
    fig, axes = plt.subplots(1, 2, figsize=(8, 2.8))
    for i, h in enumerate(HULLS):
        rows = [r for r in base if r["hull"] == h and same_ok(r)]
        coast = sorted(100 * num(r["coast_frac"]) for r in rows)
        floor = sorted(100 * num(r["ceiling_frac"]) for r in rows)
        y = [100 * (k + 1) / len(coast) for k in range(len(coast))]
        axes[0].plot(coast, y, color=HULL_COLOR[h], label=HULL_NAME[h])
        axes[1].plot(floor, y, color=HULL_COLOR[h], label=HULL_NAME[h])
    axes[0].set_xlabel("share of the transfer spent coasting (%)")
    axes[1].set_xlabel("share at full power and 250 km/s (%)")
    axes[0].set_ylabel("cases at or below (%)")
    axes[0].legend(fontsize=7)
    fig.tight_layout()
    save(fig, name)


def fig_timing(timing, name):
    fig, axes = plt.subplots(1, 2, figsize=(8, 2.8))
    ok = [r for r in timing if truthy(r["B_ok"])]
    import numpy as np
    bins = np.logspace(-1, 5, 49)
    kw = dict(histtype="step", lw=1.6)
    axes[0].hist([num(r["ideal_ms"]) for r in timing if truthy(r["ideal_ok"])], bins=bins, color=MUTED, label="ideal engine", **kw)
    axes[0].hist([num(r["A_ms"]) for r in timing if truthy(r["A_ok"])], bins=bins, color=ORANGE, label="A", **kw)
    axes[0].hist([num(r["B_ms"]) for r in ok], bins=bins, color=BLUE, label="B", **kw)
    axes[0].set_xscale("log")
    axes[0].set_xlabel("earliest arrival for a full tank (ms, one thread)")
    axes[0].set_ylabel("cases")
    axes[0].legend(fontsize=7)
    axes[1].hist([num(r["cold_ms"]) for r in ok], bins=bins, color=BLUE, label="from the ideal seed", **kw)
    axes[1].hist([num(r["warm_ms"]) for r in ok if num(r["warm_ms"]) > 0], bins=bins, color=AQUA, label="warm (neighbouring T)", **kw)
    axes[1].set_xscale("log")
    axes[1].set_xlabel("one banded fixed-time solve (ms)")
    axes[1].legend(fontsize=7)
    fig.tight_layout()
    save(fig, name)


def fig_tank_cost(results, name):
    """Mean trip cost of each tank variant relative to the standard tank, per hull, A and B."""
    groups = defaultdict(dict)
    for r in results:
        groups[(r["hull"], r["route"], r["index"])][r["tank"]] = r
    fig, axes = plt.subplots(1, 2, figsize=(8, 2.8), sharey=True)
    tanks = ["0.5", "1", "1.5", "2"]
    for ax, key in zip(axes, ("A", "B")):
        for h in HULLS:
            ys = []
            for t in tanks:
                rel = []
                for (hh, _, _), by in groups.items():
                    if hh != h or "1" not in by or t not in by:
                        continue
                    a, s = by[t], by["1"]
                    ca = num(a["econA_cost"]) if key == "A" else b_cost(a)[0]
                    cs = num(s["econA_cost"]) if key == "A" else b_cost(s)[0]
                    if math.isfinite(ca) and math.isfinite(cs) and (key == "B" or (truthy(a["econA_ok"]) and truthy(s["econA_ok"]))):
                        rel.append(100 * (ca / cs - 1))
                ys.append(statistics.fmean(rel) if rel else float("nan"))
            ax.plot([50, 100, 150, 200], ys, "o-", color=HULL_COLOR[h], ms=3, label=HULL_NAME[h])
        ax.set_title(f"{'A: plan ideal, fly banded' if key == 'A' else 'B: plan banded'}")
        ax.set_xlabel("tank size (% of standard)")
        ax.set_xticks([50, 100, 150, 200])
        ax.axhline(0, color=INK2, lw=0.8)
    axes[0].set_ylabel("trip cost vs standard tank (%)")
    axes[0].legend(fontsize=7)
    fig.tight_layout()
    save(fig, name)


def main():
    dirs = [Path(a) for a in sys.argv[1:]]
    style()

    def gather(name):
        return [r for d in dirs if (d / name).exists() for r in read(d / name)]

    results = gather("results.csv")
    base = [r for r in results if num(r["tank"]) == 1.0]
    curves = gather("curves.csv")
    paths = gather("paths.csv")
    timing = gather("timing.csv")
    tune = []
    for k in range(9):
        rows = gather(f"tune_{k}.csv")
        if rows:
            tune.append((rows[0]["variant"], rows))
    ships = read(dirs[0] / "ships.csv") if (dirs[0] / "ships.csv").exists() else []
    tables(results, timing, tune, ships, HERE / "tables.tex")

    def same_excess(r):
        return 100 * (num(r["same_A_kg"]) / num(r["same_B_kg"]) - 1) if same_ok(r) else float("nan")

    def time_excess(r):
        return 100 * (num(r["A_tmin_d"]) / b_time(r) - 1) if truthy(r["A_ok"]) and not b_time_miss(r) else float("nan")

    def sep(r):
        return num(r["sep_max_km"]) / 1e6 if same_ok(r) else float("nan")

    fig_by_phase(base, same_excess, "A's extra propellant (%)", "fuel_by_phase")
    fig_by_phase(base, time_excess, "A's extra trip time (%)", "time_by_phase")
    fig_by_phase(base, sep, "largest separation (10⁶ km)", "separation_by_phase")
    fig_coast(base, "arc_shares")
    fig_tank_cost(results, "tank_cost")
    if timing:
        fig_timing(timing, "timing")
    for name, cases in (("paths_inner", ["plasma_freighter,earth-mars,0", "plasma_courier,earth-venus,0",
                                         "plasma_bulk_tanker,earth-mercury,9"]),
                        ("paths_outer", ["plasma_liner,earth-jupiter,0", "large_plasma_liner,mars-ceres,0",
                                         "plasma_freighter,earth-ceres,18"])):
        present = [c for c in cases if any(f"{r['ship']},{r['route']},{r['index']}" == c for r in paths)]
        if present:
            fig_paths(paths, present, name)
    budget = {r["hull"]: num(r["budget_kg"]) for r in base}
    keys = [("plasma_freighter,earth-mars,0", budget.get("plasma_freighter", 0)),
            ("plasma_liner,earth-jupiter,0", budget.get("plasma_liner", 0)),
            ("plasma_courier,earth-venus,0", budget.get("plasma_courier", 0))]
    keys = [k for k in keys if any(f"{r['ship']},{r['route']},{r['index']}" == k[0] for r in curves)]
    if keys:
        fig_curves(curves, keys, "fuel_curves")


if __name__ == "__main__":
    main()
