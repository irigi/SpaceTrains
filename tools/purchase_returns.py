#!/usr/bin/env python3
"""Purchase check: the return the fleet investor expected from each ship it bought, against what the
ship earned.

Every purchase logs the return its valuation promised ("ordered at ... : N%/yr expected"); the
--econ-audit ship table gives each ship's profit since commissioning. This runs the headless game
from several starting dates (verbose, so the purchase events are printed), matches the two and
reports the medians by ship group. Run it after changing purchase valuations, fares, prices or ship
classes: the estimate should stay within ~2x of the result (as cargo was on 2026-10-22), and a group
whose earned return sits far below its expected one is being overbought (docs/plans/passenger_routes.md,
"Check: are passenger purchases overvalued?").

    tools/purchase_returns.py [--binary build/bin/spacetrains_headless] [--days 1460] [--starts 0,90]
                              [--min-days 180] [--ships] [--keep DIR] [--from FILE ...]

Earned return = profit since commissioning x 365 / days in service / purchase price; ships sold for
salvage also show the return after the hull's loss. Only ships in service --min-days or longer count.
"""

import argparse
import os
import re
import statistics as st
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

ORDER = re.compile(r"\[day\s+([\d.]+)\] (.+?) ordered at (.+?) for (\d+) cr: (-?\d+)%/yr expected \((.*)\)$")
SOLD = re.compile(r"\[day\s+([\d.]+)\] (.+?) sold for salvage at .+? \((\d+) cr\)")
SHIP_ROW = re.compile(r"\s{4}(.+?)\s{2,}(-?\d+) cr\s+([+-]\d+)\s+(-?\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\d+)\s+(\S+)")
LINERS = ("large_plasma_liner", "plasma_liner", "luna_ferry", "passenger_shuttle")


def run_one(binary, days, start, threads):
    env = dict(os.environ, SPACETRAINS_THREADS=str(threads))
    return subprocess.run(
        [binary, "--days", str(days), "--start-day", str(start), "--report-interval", str(days),
         "--verbose", "--econ-audit"],
        capture_output=True, text=True, env=env, check=False).stdout


def parse(text, end_day, build_days):
    orders, sold, table = {}, {}, {}
    for line in text.splitlines():
        if m := ORDER.search(line):
            orders[m.group(2)] = dict(day=float(m.group(1)), price=float(m.group(4)), expected=float(m.group(5)),
                                      label=m.group(6))
        elif m := SOLD.search(line):
            sold[m.group(2)] = (float(m.group(1)), float(m.group(3)))
        elif m := SHIP_ROW.match(line):
            table[m.group(1).strip()] = dict(profit=float(m.group(3)), cls=m.group(11))
    rows = []
    for name, order in orders.items():
        if name not in table:
            continue
        end, salvage = sold.get(name, (end_day, None))
        days = max(1.0, end - (order["day"] + build_days))
        profit = table[name]["profit"]
        loss = order["price"] - salvage if salvage is not None else 0.0
        rows.append(dict(name=name, cls=table[name]["cls"], label=order["label"], ordered=order["day"],
                         price=order["price"], expected=order["expected"], days=days, sold=salvage is not None,
                         passengers="berths" in order["label"],
                         earned=100.0 * profit * 365.0 / days / order["price"],
                         after_sale=100.0 * (profit - loss) * 365.0 / days / order["price"]))
    return rows


def group(row):
    if not row["passengers"]:
        return "cargo"
    for liner in LINERS:
        if row["cls"].startswith(liner):
            return liner
    return "cargo hull + cabins"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--binary", default="build/bin/spacetrains_headless")
    parser.add_argument("--days", type=int, default=1460)
    parser.add_argument("--starts", default="0,90")
    parser.add_argument("--threads", type=int, default=0, help="per run (default: cores / runs)")
    parser.add_argument("--build-days", type=float, default=30.0, help="fleet_investment build_days")
    parser.add_argument("--min-days", type=float, default=180.0, help="shortest service counted")
    parser.add_argument("--ships", action="store_true", help="list every passenger purchase")
    parser.add_argument("--keep", help="save each run's output in this directory")
    parser.add_argument("--from", dest="inputs", nargs="+", help="parse saved outputs instead of running")
    args = parser.parse_args()

    if args.inputs:
        texts = [open(path).read() for path in args.inputs]
    else:
        starts = [int(s) for s in args.starts.split(",")]
        threads = args.threads or max(1, (os.cpu_count() or 4) // len(starts))
        with ThreadPoolExecutor(len(starts)) as pool:
            texts = list(pool.map(lambda s: run_one(args.binary, args.days, s, threads), starts))
        if args.keep:
            os.makedirs(args.keep, exist_ok=True)
            for start, text in zip(starts, texts):
                with open(os.path.join(args.keep, f"returns_{start}.out"), "w") as out:
                    out.write(text)
    rows = [row for text in texts for row in parse(text, args.days, args.build_days)]
    counted = [row for row in rows if row["days"] >= args.min_days]
    if not counted:
        sys.exit("no purchases in service long enough (check the runs' output with --keep)")

    print(f"Purchases in service >= {args.min_days:.0f} days: {len(counted)} of {len(rows)}; returns in %/yr (medians)")
    print(f"{'group':22s} {'ships':>5s} {'sold':>4s} {'expected':>8s} {'earned':>6s} {'after sale':>10s} "
          f"{'< 0':>4s} {'< hurdle':>8s} {'spent M cr':>10s}")
    groups = {}
    for row in counted:
        groups.setdefault(group(row), []).append(row)
    order = ["cargo", "cargo hull + cabins", *LINERS]
    for name in sorted(groups, key=lambda g: order.index(g) if g in order else len(order)):
        r = groups[name]
        print(f"{name:22s} {len(r):5d} {sum(x['sold'] for x in r):4d} {st.median(x['expected'] for x in r):8.0f} "
              f"{st.median(x['earned'] for x in r):6.0f} {st.median(x['after_sale'] for x in r):10.0f} "
              f"{sum(x['earned'] < 0 for x in r):4d} {sum(x['earned'] < 30 for x in r):8d} "
              f"{sum(x['price'] for x in r) / 1e6:10.1f}")
    for kind, label in ((False, "all cargo"), (True, "all passengers")):
        r = [x for x in counted if x["passengers"] == kind]
        if r:
            print(f"{label:22s} {len(r):5d} {sum(x['sold'] for x in r):4d} {st.median(x['expected'] for x in r):8.0f} "
                  f"{st.median(x['earned'] for x in r):6.0f} {st.median(x['after_sale'] for x in r):10.0f}")
    if args.ships:
        print("\nPassenger purchases:")
        for x in sorted((x for x in rows if x["passengers"]), key=lambda x: x["ordered"]):
            print(f"  day {x['ordered']:6.0f} {x['cls'][:36]:36s} {x['price']:9.0f} cr  expected {x['expected']:5.0f}%"
                  f"  earned {x['earned']:5.0f}%  {'SOLD' if x['sold'] else '    '} ({x['days']:.0f} d)  {x['label']}")


if __name__ == "__main__":
    main()
