#!/usr/bin/env python3
"""Scale producers' outputs to a multiple of what the stations consume (step 19).

For every good, system production P and station consumption C are summed over the stations, with each
recipe row scaled by the station's population / 10,000 (as EconomySystem::get_station_net_rates does).
Where P exceeds `multiple` x C, every producing row of that good is scaled by multiple x C / P, and each
material-input row by the mean factor of the outputs it feeds (a smaller farm uses less water). Inputs are
consumption too, so this repeats until the factors settle. Fuel (its own factories, and ships burn far more
than stations use) and export goods (production-limited, calibrated separately) are left alone, as are
upkeep and consume rows (what the residents need) and station-specific rows: local resources such as Lunar
Gateway's polar ice, which Earth L1's farms depend on (scaling it with the rest left Earth L1 short of water).

Usage: tools/rebalance_production.py [--multiple 3] [--data data] [--dry-run]
"""
import argparse
import csv
from collections import defaultdict
from pathlib import Path

REFERENCE_POPULATION = 10_000.0


def read_rows(path):
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def write_rows(path, rows, fields):
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)


def tidy(value):
    return f"{value:.4g}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--multiple", type=float, default=3.0)
    parser.add_argument("--data", default="data")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    data = Path(args.data)

    stations = read_rows(data / "stations" / "stations.csv")
    profile_rows = read_rows(data / "recipes" / "recipes.csv")
    station_rows = read_rows(data / "recipes" / "station_recipes.csv")
    exports = {row["commodity_id"] for row in read_rows(data / "economy" / "export_markets.csv")}
    excluded = exports | {"fuel"}

    # Population weight of each row: the stations it applies to.
    weight = {}
    for i, row in enumerate(profile_rows):
        weight[("p", i)] = sum(float(s["population"]) for s in stations
                               if s["economy_profile_id"] == row["profile_id"]) / REFERENCE_POPULATION
    population = {s["id"]: float(s["population"]) for s in stations}
    for i, row in enumerate(station_rows):
        weight[("s", i)] = population[row["station_id"]] / REFERENCE_POPULATION
    rows = [(("p", i), row) for i, row in enumerate(profile_rows)] + [(("s", i), row) for i, row in enumerate(station_rows)]
    rate = {key: float(row["units_per_day"]) for key, row in rows}

    def balance():
        produced, consumed = defaultdict(float), defaultdict(float)
        for key, row in rows:
            amount = rate[key] * weight[key]
            (produced if amount > 0 else consumed)[row["commodity_id"]] += abs(amount)
        return produced, consumed

    before = balance()
    for _ in range(50):
        produced, consumed = balance()
        fixed = defaultdict(float)
        for key, row in rows:
            if key[0] == "s" and rate[key] > 0:
                fixed[row["commodity_id"]] += rate[key] * weight[key]
        factor = {good: min(1.0, max(0.0, args.multiple * consumed[good] - fixed[good]) / (produced[good] - fixed[good]))
                  for good in produced
                  if good not in excluded and consumed[good] > 0 and produced[good] > fixed[good]}
        if all(abs(f - 1.0) < 1e-9 for f in factor.values()):
            break
        for key, row in rows:
            good = row["commodity_id"]
            if key[0] == "s":
                continue  # local resources keep their rate
            if rate[key] > 0 and good in factor:
                rate[key] *= factor[good]
            elif row["role"].startswith("input:"):
                outputs = row["role"][len("input:"):].split("|")
                rate[key] *= sum(factor.get(output, 1.0) for output in outputs) / len(outputs)
    after = balance()

    print(f"{'good':16s} {'produced':>10s} {'consumed':>10s} {'ratio':>7s}  ->  {'produced':>10s} {'consumed':>10s} {'ratio':>7s}")
    for good in sorted(set(before[0]) | set(before[1])):
        p0, c0, p1, c1 = before[0][good], before[1][good], after[0][good], after[1][good]
        ratio = lambda p, c: f"{p / c:7.2f}" if c > 0 else "      -"
        print(f"{good:16s} {p0:10.2f} {c0:10.2f} {ratio(p0, c0)}  ->  {p1:10.2f} {c1:10.2f} {ratio(p1, c1)}")

    if args.dry_run:
        return
    for key, row in rows:
        row["units_per_day"] = tidy(rate[key])
    write_rows(data / "recipes" / "recipes.csv", profile_rows, ["profile_id", "commodity_id", "units_per_day", "role"])
    write_rows(data / "recipes" / "station_recipes.csv", station_rows, ["station_id", "commodity_id", "units_per_day", "role"])
    print("Rewrote recipes.csv and station_recipes.csv; regenerate data/economy/reference_prices.csv.")


if __name__ == "__main__":
    main()
