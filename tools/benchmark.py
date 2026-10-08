#!/usr/bin/env python3
"""Economy benchmark over several starting dates.

One 730-day run is a poor judge of a change: the simulation has no randomness, but small
changes cascade (a purchase a few hours later sends ships elsewhere), and the unmet demand of
a single run moved by 3-12 points between two near-identical versions. This runs the same
economy from several starting dates (planets elsewhere on their orbits, --start-day) in
parallel and reports the mean and the spread.

    tools/benchmark.py [--binary build/bin/spacetrains_headless] [--days 730] [--starts 0,90,180,270]
"""

import argparse
import os
import re
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor


def run_one(binary, days, start, threads, extra):
    env = dict(os.environ, SPACETRAINS_THREADS=str(threads))
    interval = max(1, days // 5)
    began = time.time()
    output = subprocess.run(
        [binary, "--days", str(days), "--report-interval", str(interval), "--econ-audit", "--trajectory-audit",
         "--start-day", str(start)] + extra,
        capture_output=True, text=True, env=env, check=False).stdout
    seconds = time.time() - began
    result = {"start": start, "seconds": seconds}
    previous = (0.0, 0.0)
    for line in output.splitlines():
        m = re.search(r"Unmet demand: ([\d.]+)% of (\d+) cr consumed.*\((\d+) cr unmet", line)
        if m:
            demand, unmet = float(m.group(2)), float(m.group(3))
            result["unmet"] = float(m.group(1))
            result["unmet_last"] = 100.0 * (unmet - previous[1]) / max(1.0, demand - previous[0])
            previous = (demand, unmet)
        m = re.search(r"-> (\d+)/(\d+) ships profitable", line)
        if m:
            result["profitable"] = int(m.group(1))
            result["ships"] = int(m.group(2))
        m = re.search(r"Money supply: .*\(target \d+, ([+-][\d.]+)%\)", line)
        if m:
            result["money"] = float(m.group(1))
        m = re.search(r"drift ([-\d.]+)\)", line)
        if m:
            result["drift"] = abs(float(m.group(1)))
        m = re.search(r"Exports: (\d+) cr", line)
        if m:
            result["exports"] = float(m.group(1))
        m = re.search(r"Emergencies: (\d+) opened, (\d+) open now, (\d+) cr paid", line)
        if m:
            result["emergencies"] = int(m.group(1))
            result["emergency_cr"] = float(m.group(3))
        m = re.search(r"Events: (\d+) struck", line)
        if m:
            result["events"] = int(m.group(1))
        m = re.search(r"interest paid (\d+) cr; factions over their limit: (\d+)", line)
        if m:
            result["interest"] = float(m.group(1))
            result["over_limit"] = int(m.group(2))
        m = re.search(r"Money supply: stations (-?\d+) \+ ships (-?\d+) =", line)
        if m:
            total = float(m.group(1)) + float(m.group(2))
            result["ship_cash"] = 100.0 * float(m.group(2)) / total if total else float("nan")
        m = re.search(r"fleet holds now: (\d+) u", line)
        if m:
            result["holds"] = float(m.group(1))
        m = re.search(r"flagged=\s*(\d+)", line)
        if m:
            result["flagged"] = result.get("flagged", 0) + int(m.group(1))
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--binary", default="build/bin/spacetrains_headless")
    parser.add_argument("--days", type=int, default=730)
    parser.add_argument("--starts", default="0,90,180,270")
    parser.add_argument("--threads", type=int, default=0, help="per run (default: cores / runs)")
    parser.add_argument("--no-events", action="store_true", help="switch random events off (step 28)")
    parser.add_argument("extra", nargs="*", help="more arguments for the binary")
    args = parser.parse_args()
    if args.no_events:
        args.extra.append("--no-events")
    starts = [float(x) for x in args.starts.split(",")]
    threads = args.threads or max(1, (os.cpu_count() or 4) // len(starts))
    with ThreadPoolExecutor(max_workers=len(starts)) as pool:
        results = list(pool.map(lambda s: run_one(args.binary, args.days, s, threads, args.extra), starts))

    columns = [("unmet", "unmet %"), ("unmet_last", "last 1/5 %"), ("profitable", "profitable"), ("ships", "ships"),
               ("holds", "holds u"), ("money", "money %"), ("exports", "exports"), ("flagged", "traj flags"),
               ("emergencies", "emergencies"), ("emergency_cr", "emerg. cr"), ("ship_cash", "ship cash %"),
               ("interest", "interest"), ("over_limit", "over limit"), ("events", "events"), ("seconds", "run s")]
    print(f"{'start day':>10}" + "".join(f"{title:>12}" for _, title in columns))
    for result in results:
        print(f"{result['start']:>10.0f}" + "".join(
            f"{result.get(key, float('nan')):>12.1f}" for key, _ in columns))
    means = []
    for key, _ in columns:
        values = [r[key] for r in results if key in r]
        mean = statistics.mean(values) if values else float("nan")
        spread = statistics.stdev(values) if len(values) > 1 else 0.0
        means.append(f"{mean:>7.1f}±{spread:<4.1f}")
    print(f"{'mean±sd':>10}" + "".join(f"{m:>12}" for m in means))
    if any(r.get("drift", 0.0) > 1e-6 for r in results):
        print("MONEY DRIFT in at least one run", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
