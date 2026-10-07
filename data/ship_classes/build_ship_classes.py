#!/usr/bin/env python3
"""Build ship_classes.csv bottom-up from the reference numbers.

Every class is assembled from engine, (radiators), tankage, habitat, cargo hold and
structure, using the era numbers in docs/plans/tech_plausibility_review.md
("Reference Numbers"). Dry mass, Δv, acceleration and the variable-Isp α follow from
the parts, so changing a reference number and re-running keeps the classes consistent.

    python3 data/ship_classes/build_ship_classes.py            # write the CSV, print a summary
    python3 data/ship_classes/build_ship_classes.py --markdown # also print the breakdown table
"""

from __future__ import annotations

import argparse
import csv
import math
from dataclasses import dataclass
from pathlib import Path

G0 = 9.80665
AU_M = 1.495978707e11
MU_SUN = 1.32712440018e20
KAPPA_SCALE = AU_M**2.5 / MU_SUN**1.5

# --- Reference numbers (tech_plausibility_review.md) ---------------------------------
# Nuclear-thermal generations: Isp [s] and engine thrust-to-weight.
NTR = {
    "solid_core": {"isp_s": 950.0, "thrust_to_weight": 3.0},
    "liquid_core": {"isp_s": 1400.0, "thrust_to_weight": 1.0},
}
LH2_TANK_FRACTION = 0.18          # in-space LH2 tank (MLI + zero boil-off) per kg of LH2
# Variable-Isp drive, linopt Set A: 10 kW/kg engine, 30% waste heat, 4 kW/kg radiators.
PLASMA_ENGINE_W_PER_KG = 10_000.0
PLASMA_HEAT_FRACTION = 0.30
PLASMA_RADIATOR_W_PER_KG = 4_000.0
PLASMA_TANK_FRACTION = 0.05       # dense propellant (water, argon)
# Crew and structure.
HABITAT_KG_PER_CREW = {"long": 8_000.0, "short": 4_000.0}
STRUCTURE_FRACTION = 0.10         # of all other dry mass
HOLD_FRACTION = 0.10              # of the rated cargo mass
RATED_KG_PER_CARGO_UNIT = 200.0   # metals; commodities range 20-500 kg/unit

# Hardware prices [$ per kg]. linopt: plasma engine, radiators, dense-propellant tanks,
# habitat (its constant-payload price), structure and hold at its tank price.
# Estimates: NTR engines (solid core half the plasma engine price, liquid core the
# same) and LH2 tanks (twice linopt's tank price for insulation and cryocoolers).
USD_PER_KG = {
    "solid_core": 5_000.0,
    "liquid_core": 10_000.0,
    "plasma": 10_000.0,
    "radiator": 1_500.0,
    "lh2_tank": 600.0,
    "plasma_tank": 300.0,
    "habitat": 1_500.0,
    "hold": 300.0,
    "structure": 300.0,
}
# Credits per dollar, chosen once (2026-10-07) so the starting fleet's total value stayed
# at the old hand-set 1.65 M cr: relative prices follow the hardware, the economy's total
# capital burden does not move. Cross-check: linopt's 20 $/kg propellant -> 0.016 cr/kg
# (the game's fuel base price is 0.08 cr/kg).
CR_PER_USD = 0.00078


@dataclass(frozen=True)
class ClassSpec:
    id: str
    name: str
    drive: str                 # "solid_core" | "liquid_core" | "plasma"
    crew: int
    habitat: str               # "long" | "short"
    cargo_units: float
    propellant_kg: float
    accel_full_mps2: float = 0.0   # NTR: thrust / full mass (ship + propellant), sizes the engine
    jet_power_w: float = 0.0       # plasma: jet power, sizes engine + radiators


# Holds: freighters rate about their own dry mass of cargo (at 200 kg/unit), couriers
# 15-25% of theirs. A full hold cuts Δv a lot; dispatch takes part loads when it must.
CLASSES = [
    ClassSpec("light_freighter", "Light Freighter", "solid_core", crew=3, habitat="long",
              cargo_units=200, propellant_kg=90_000, accel_full_mps2=0.5),
    ClassSpec("tanker", "Orbital Tanker", "solid_core", crew=3, habitat="long",
              cargo_units=300, propellant_kg=90_000, accel_full_mps2=0.5),
    ClassSpec("fast_courier", "Fast Courier", "liquid_core", crew=2, habitat="long",
              cargo_units=30, propellant_kg=90_000, accel_full_mps2=0.5),
    ClassSpec("plasma_freighter", "Plasma Freighter", "plasma", crew=3, habitat="long",
              cargo_units=150, propellant_kg=30_000, jet_power_w=10.0e6),
    ClassSpec("plasma_courier", "Plasma Courier", "plasma", crew=2, habitat="long",
              cargo_units=30, propellant_kg=30_000, jet_power_w=18.0e6),
    ClassSpec("deep_space_freighter", "Deep Space Freighter", "liquid_core", crew=4, habitat="long",
              cargo_units=300, propellant_kg=180_000, accel_full_mps2=0.3),
    ClassSpec("ntr_freighter", "NTR Freighter", "liquid_core", crew=4, habitat="long",
              cargo_units=400, propellant_kg=250_000, accel_full_mps2=0.3),
]


@dataclass
class Build:
    spec: ClassSpec
    engine_kg: float
    radiator_kg: float
    tank_kg: float
    habitat_kg: float
    hold_kg: float
    structure_kg: float
    dry_kg: float
    thrust_n: float
    isp_s: float

    @property
    def cargo_kg(self) -> float:
        return self.spec.cargo_units * RATED_KG_PER_CARGO_UNIT

    @property
    def wet_kg(self) -> float:
        return self.dry_kg + self.spec.propellant_kg

    def delta_v(self, payload_kg: float = 0.0) -> float:
        if self.isp_s <= 0.0:
            return 0.0
        return self.isp_s * G0 * math.log((self.wet_kg + payload_kg) / (self.dry_kg + payload_kg))

    @property
    def cruise_accel(self) -> float:
        return self.thrust_n / self.wet_kg if self.thrust_n > 0.0 else 0.0

    @property
    def value_usd(self) -> float:
        plasma = self.spec.drive == "plasma"
        return (self.engine_kg * USD_PER_KG[self.spec.drive]
                + self.radiator_kg * USD_PER_KG["radiator"]
                + self.tank_kg * USD_PER_KG["plasma_tank" if plasma else "lh2_tank"]
                + self.habitat_kg * USD_PER_KG["habitat"]
                + self.hold_kg * USD_PER_KG["hold"]
                + self.structure_kg * USD_PER_KG["structure"])

    @property
    def value_cr(self) -> float:
        return self.value_usd * CR_PER_USD

    @property
    def alpha(self) -> float:
        return self.spec.jet_power_w / self.dry_kg

    @property
    def kappa(self) -> float:
        p = self.spec.jet_power_w
        return 2.0 * p * (1.0 / self.dry_kg - 1.0 / self.wet_kg) * KAPPA_SCALE if p > 0.0 else 0.0


def build(spec: ClassSpec) -> Build:
    habitat = spec.crew * HABITAT_KG_PER_CREW[spec.habitat]
    hold = HOLD_FRACTION * spec.cargo_units * RATED_KG_PER_CARGO_UNIT
    if spec.drive == "plasma":
        jet_fraction = 1.0 - PLASMA_HEAT_FRACTION
        engine = spec.jet_power_w / (jet_fraction * PLASMA_ENGINE_W_PER_KG)
        radiator = spec.jet_power_w * PLASMA_HEAT_FRACTION / (jet_fraction * PLASMA_RADIATOR_W_PER_KG)
        tank = PLASMA_TANK_FRACTION * spec.propellant_kg
        rest = engine + radiator + tank + habitat + hold
        structure = STRUCTURE_FRACTION * rest
        return Build(spec, engine, radiator, tank, habitat, hold, structure, rest + structure, 0.0, 0.0)

    ntr = NTR[spec.drive]
    tank = LH2_TANK_FRACTION * spec.propellant_kg
    # The engine is sized by thrust / full mass, and the full mass includes the engine:
    # (1 + s) (engine + fixed) + propellant = full, engine = a * full / (T/W g0).
    fixed = tank + habitat + hold
    k = spec.accel_full_mps2 / (ntr["thrust_to_weight"] * G0)
    s = STRUCTURE_FRACTION
    full = ((1.0 + s) * fixed + spec.propellant_kg) / (1.0 - (1.0 + s) * k)
    engine = k * full
    structure = s * (engine + fixed)
    dry = engine + fixed + structure
    return Build(spec, engine, 0.0, tank, habitat, hold, structure, dry,
                 spec.accel_full_mps2 * full, ntr["isp_s"])


def write_csv(builds: list[Build], path: Path) -> None:
    header = ["id", "name", "propulsion_type", "dry_mass_kg", "propellant_capacity_kg",
              "cargo_capacity_units", "max_delta_v_mps", "cruise_accel_mps2",
              "specific_engine_power_w_per_kg", "ship_value_cr", "crew_size"]
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        for b in builds:
            plasma = b.spec.drive == "plasma"
            writer.writerow([
                b.spec.id,
                b.spec.name,
                "variable_isp" if plasma else "nuclear_thermal",
                round(b.dry_kg),
                round(b.spec.propellant_kg),
                round(b.spec.cargo_units),
                0 if plasma else round(b.delta_v()),
                0 if plasma else round(b.cruise_accel, 3),
                round(b.alpha) if plasma else 0,
                int(round(b.value_cr, -2)),
                b.spec.crew,
            ])


def summary(builds: list[Build]) -> None:
    for b in builds:
        if b.spec.drive == "plasma":
            perf = f"alpha={b.alpha:5.0f} W/kg  kappa={b.kappa:7.2f}"
        else:
            perf = (f"Isp={b.isp_s:4.0f}s  dv={b.delta_v() / 1000:5.1f} km/s  "
                    f"laden={b.delta_v(b.cargo_kg) / 1000:5.1f}  a={b.cruise_accel:.2f} m/s2")
        print(f"{b.spec.id:22s} dry={b.dry_kg / 1000:6.1f} t  prop={b.spec.propellant_kg / 1000:5.0f} t  "
              f"MR={b.wet_kg / b.dry_kg:4.2f}  {perf}  value={b.value_usd / 1e6:5.1f} M$ = {b.value_cr:6.0f} cr")


def markdown(builds: list[Build]) -> None:
    print("| Class | Drive | Crew | Engine | Radiators | Tanks | Habitat | Hold | Structure | Dry | "
          "Propellant | Performance | Price |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    t = lambda kg: f"{kg / 1000:.1f} t"
    for b in builds:
        if b.spec.drive == "plasma":
            perf = f"{b.spec.jet_power_w / 1e6:.0f} MW jet, α = {b.alpha:.0f} W/kg, κ = {b.kappa:.1f}"
        else:
            perf = (f"{b.isp_s:.0f} s, Δv {b.delta_v() / 1000:.1f} km/s "
                    f"({b.delta_v(b.cargo_kg) / 1000:.1f} laden), {b.cruise_accel:.2f} m/s²")
        print(f"| {b.spec.name} | {b.spec.drive.replace('_', ' ')} | {b.spec.crew} | {t(b.engine_kg)} | "
              f"{t(b.radiator_kg) if b.radiator_kg else '-'} | {t(b.tank_kg)} | {t(b.habitat_kg)} | "
              f"{t(b.hold_kg)} | {t(b.structure_kg)} | {t(b.dry_kg)} | {t(b.spec.propellant_kg)} | {perf} | "
              f"{b.value_usd / 1e6:.0f} M$ = {b.value_cr / 1000:.0f}k cr |")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--markdown", action="store_true", help="print the per-class mass breakdown table")
    parser.add_argument("--dry-run", action="store_true", help="do not write the CSV")
    args = parser.parse_args()
    builds = [build(spec) for spec in CLASSES]
    if not args.dry_run:
        write_csv(builds, Path(__file__).with_name("ship_classes.csv"))
    summary(builds)
    if args.markdown:
        markdown(builds)


if __name__ == "__main__":
    main()
