#!/usr/bin/env python3
"""Build ship_classes.csv bottom-up from the reference numbers.

Every class is assembled from engine, (radiators), tankage, habitat, cargo hold and
structure, using the era numbers in docs/ship_technology.md
("Reference Numbers"). Dry mass, Δv, acceleration and the variable-Isp α follow from
the parts, so changing a reference number and re-running keeps the classes consistent.

    python3 data/ship_classes/build_ship_classes.py            # write the CSV, print a summary
    python3 data/ship_classes/build_ship_classes.py --markdown # also print the breakdown table
    python3 data/ship_classes/build_ship_classes.py --check    # components add up to the classes

It also writes ship_components.csv: the parts of every class (engine, radiators, tanks,
habitat and its parts, hold, structure) with their physical numbers. The parts add up
exactly to each class's dry mass and value; docs/plans/ship_internals.md (tier 1).

Every class also gets tank variants (TANK_OPTIONS): the same hull, engine and hold with
smaller or larger propellant tanks. Ships refit between the variants of their hull at their
home base (part C, docs/modules/ships_and_fleet.md). The variants share the class's
`hull_id`; the nominal variant keeps the class id.
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

# --- Reference numbers (docs/ship_technology.md) ---------------------------------
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

# --- Component details (ship_components.csv; docs/plans/ship_internals.md) ------------
# These describe the parts; they do not change any mass or price above. Status as in the
# tech review: sourced / derived / estimate / game.
STEFAN_BOLTZMANN = 5.670374e-8
# Radiators: two-sided heat-pipe panels. Areal density 3 kg/m2 is NASA's target for nuclear
# power radiators, reached by carbon-carbon and Ti/Cs designs (sourced: NTRS 20080048181,
# UNM ISNPS Tech Report 99). Emissivity 0.85 (estimate, typical coating). The panel
# temperature follows from the heat per kg: 2 eps sigma T^4 * (m2 per kg) = W per kg (derived).
RADIATOR_AREAL_KG_PER_M2 = 3.0
RADIATOR_EMISSIVITY = 0.85
# Reactor core temperatures. Solid core: NERVA-class fuel ran 2,300-2,700 K (estimate within
# the sourced NERVA range). Liquid core: Isp grows as sqrt(T) for the same propellant, so
# 1400 s needs (1400/950)^2 times the solid-core temperature (derived, ~5,900 K).
NTR_CORE_TEMPERATURE_K = {"solid_core": 2_700.0}
NTR_CORE_TEMPERATURE_K["liquid_core"] = (
    NTR_CORE_TEMPERATURE_K["solid_core"] * (NTR["liquid_core"]["isp_s"] / NTR["solid_core"]["isp_s"]) ** 2)
# Propellants: liquid hydrogen for nuclear-thermal ships, water for the plasma drive (linopt
# allows water or argon; game choice).
PROPELLANT = {
    "lh2": {"name": "Liquid hydrogen tanks", "density_kg_m3": 71.0, "temperature_k": 20.0},
    "water": {"name": "Water propellant tanks", "density_kg_m3": 1_000.0, "temperature_k": 293.0},
}
# Habitat: NASA Deep Space Habitat, 500-day case, 4 crew (sourced: NTRS 20120014530,
# "Mass Summary" and "Crew Systems Mass by Mission"). Dry mass 32,022 kg by subsystem; crew
# systems split into quarters (crew quarters, restraints, laptops, lighting) and common
# rooms (galley, wardroom, medical, exercise). The game's 8 t/crew habitat is split in the
# same proportions.
DSH_DRY_KG = {
    "pressure_hull": 14_116.0,     # Structures
    "shielding": 4_826.0,          # Environment Protection (meteoroid shield, MLI)
    "life_support": 6_890.0,       # ECLSS
    "thermal_control": 2_868.0,    # Thermal (internal loops, small radiators)
    "power_avionics": 924.0 + 1_321.0,
    "crew_quarters": 807.0 * 329.0 / 802.0,
    "common_rooms": 807.0 * 473.0 / 802.0,
    "airlock_eva": 272.0,
}
HABITAT_PART_NAMES = {
    "pressure_hull": "Pressure hull",
    "shielding": "Meteoroid and thermal shielding",
    "life_support": "Life support (ECLSS)",
    "thermal_control": "Cabin thermal control",
    "power_avionics": "Power and avionics",
    "crew_quarters": "Crew quarters",
    "common_rooms": "Galley, wardroom, medical, exercise",
    "airlock_eva": "Airlock and EVA suits",
}
DSH_CREW = 4
DSH_PRESSURIZED_M3 = 193.0        # sourced (DSH 500-day); habitable volume 90 m3
DSH_HABITABLE_M3 = 90.0
DSH_HEAT_W = 12_925.0             # sourced: heat dissipated to the thermal control system
CABIN_TEMPERATURE_K = 295.0
CABIN_RADIATOR_TEMPERATURE_K = 277.0  # estimate: ISS-style external ammonia loop, 2-6 C
HOLD_M3_PER_CARGO_UNIT = 1.0      # game: one unit of cargo is about a cubic metre

COMPONENT_HEADER = ["class_id", "component_id", "parent_id", "kind", "name", "mass_kg", "price_cr",
                    "power_w", "heat_w", "temperature_k", "volume_m3", "area_m2", "capacity_kg",
                    "crew_capacity", "isp_s", "thrust_n"]

# --- Modules (tier 2, docs/plans/ship_structures_tier2.md) ----------------------------
# Optional parts fitted at the home yard, sized per hull. Each module's mass includes its share
# of truss (STRUCTURE_FRACTION), so a fitted design is the class plus the module rows.
MODULE_HEADER = ["hull_id", "module_id", "slot", "name", "mass_kg", "price_cr", "power_w", "heat_w",
                 "temperature_k", "volume_m3", "area_m2", "capacity_kg", "crew_capacity", "effects"]
SLOT_HEADER = ["hull_id", "slot", "count"]
# Storm shelter: a refuge lined with water where the crew sits out a solar particle event.
# 20 g/cm2 of water (200 kg/m2) keeps even an August-1972-class event under the 30-day limits
# (estimate from NASA shielding studies, to be sourced in step D); 2 m3 of refuge per crew
# member, a cylinder as tall as it is wide (game geometry). The water doubles as a reserve.
SHELTER_AREAL_KG_PER_M2 = 200.0
SHELTER_M3_PER_CREW = 2.0
SHELTER_TANK_FRACTION = 0.10      # tank walls per kg of water (estimate, like the plasma tanks' 5-10%)

# Tank refit options, as multiples of the class's nominal propellant capacity. The engine
# keeps the nominal size (thrust, jet power); tanks and structure follow the propellant.
TANK_OPTIONS = (0.5, 1.0, 1.5, 2.0)


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
    # Bulk tankers carry fuel from the fuel factories to the other depots: 2000 units of fuel
    # are 200 t, half the hold's rating.
    ClassSpec("bulk_tanker", "Bulk Tanker", "solid_core", crew=3, habitat="long",
              cargo_units=2000, propellant_kg=300_000, accel_full_mps2=0.2),
    ClassSpec("plasma_bulk_tanker", "Plasma Bulk Tanker", "plasma", crew=3, habitat="long",
              cargo_units=2000, propellant_kg=100_000, jet_power_w=40.0e6),
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
    propellant_kg: float
    tank_option: float = 1.0

    @property
    def id(self) -> str:
        return self.spec.id if self.tank_option == 1.0 else f"{self.spec.id}_tank{round(self.tank_option * 100):03d}"

    @property
    def name(self) -> str:
        return self.spec.name if self.tank_option == 1.0 else f"{self.spec.name} ({round(self.tank_option * 100)}% tanks)"

    @property
    def cargo_kg(self) -> float:
        return self.spec.cargo_units * RATED_KG_PER_CARGO_UNIT

    @property
    def wet_kg(self) -> float:
        return self.dry_kg + self.propellant_kg

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
        return Build(spec, engine, radiator, tank, habitat, hold, structure, rest + structure, 0.0, 0.0,
                     spec.propellant_kg)

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
                 spec.accel_full_mps2 * full, ntr["isp_s"], spec.propellant_kg)


def tank_variant(nominal: Build, option: float) -> Build:
    """The nominal build with its tanks scaled: same engine, radiators, habitat and hold."""
    if option == 1.0:
        return nominal
    propellant = nominal.propellant_kg * option
    plasma = nominal.spec.drive == "plasma"
    tank = (PLASMA_TANK_FRACTION if plasma else LH2_TANK_FRACTION) * propellant
    rest = nominal.engine_kg + nominal.radiator_kg + tank + nominal.habitat_kg + nominal.hold_kg
    structure = STRUCTURE_FRACTION * rest
    return Build(nominal.spec, nominal.engine_kg, nominal.radiator_kg, tank, nominal.habitat_kg, nominal.hold_kg,
                 structure, rest + structure, nominal.thrust_n, nominal.isp_s, propellant, option)


def write_csv(builds: list[Build], path: Path) -> None:
    header = ["id", "name", "propulsion_type", "dry_mass_kg", "propellant_capacity_kg",
              "cargo_capacity_units", "max_delta_v_mps", "cruise_accel_mps2",
              "specific_engine_power_w_per_kg", "ship_value_cr", "crew_size", "hull_id"]
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(header)
        for b in builds:
            plasma = b.spec.drive == "plasma"
            writer.writerow([
                b.id,
                b.name,
                "variable_isp" if plasma else "nuclear_thermal",
                round(b.dry_kg),
                round(b.propellant_kg),
                round(b.spec.cargo_units),
                0 if plasma else round(b.delta_v()),
                0 if plasma else round(b.cruise_accel, 3),
                round(b.alpha) if plasma else 0,
                int(round(b.value_cr, -2)),
                b.spec.crew,
                b.spec.id,
            ])


def radiator_temperature_k(heat_w_per_kg: float) -> float:
    """Panel temperature that rejects this much heat per kg of two-sided radiator."""
    flux = heat_w_per_kg * RADIATOR_AREAL_KG_PER_M2 / 2.0
    return (flux / (RADIATOR_EMISSIVITY * STEFAN_BOLTZMANN)) ** 0.25


def split_rounded(total: float, parts: dict[str, float]) -> dict[str, int]:
    """Round the parts to whole numbers that add up exactly to round(total)."""
    scale = total / sum(parts.values())
    rounded = {k: round(v * scale) for k, v in parts.items()}
    largest = max(parts, key=parts.get)
    rounded[largest] += round(total) - sum(rounded.values())
    return rounded


def components(b: Build) -> list[dict]:
    """The parts of one class variant. Masses add up to the CSV's dry mass and prices to
    its ship value exactly; the rounding remainder goes to the largest part."""
    plasma = b.spec.drive == "plasma"
    rate = lambda key: USD_PER_KG[key] * CR_PER_USD
    rows: dict[str, dict] = {}

    if plasma:
        heat = b.spec.jet_power_w * PLASMA_HEAT_FRACTION / (1.0 - PLASMA_HEAT_FRACTION)
        rows["engine"] = {"kind": "engine", "name": "Plasma drive, magnetic nozzle", "mass": b.engine_kg,
                          "price": b.engine_kg * rate("plasma"), "power_w": b.spec.jet_power_w, "heat_w": heat}
        rows["radiators"] = {"kind": "radiator", "name": "Drive radiator panels", "mass": b.radiator_kg,
                             "price": b.radiator_kg * rate("radiator"), "heat_w": heat,
                             "temperature_k": radiator_temperature_k(PLASMA_RADIATOR_W_PER_KG),
                             "area_m2": b.radiator_kg / RADIATOR_AREAL_KG_PER_M2}
        propellant = PROPELLANT["water"]
        tank_rate = rate("plasma_tank")
    else:
        drive_name = {"solid_core": "Solid-core nuclear thermal engine",
                      "liquid_core": "Liquid-core nuclear thermal engine"}[b.spec.drive]
        jet_power = 0.5 * b.thrust_n * b.isp_s * G0
        rows["engine"] = {"kind": "engine", "name": drive_name, "mass": b.engine_kg,
                          "price": b.engine_kg * rate(b.spec.drive), "power_w": jet_power,
                          "temperature_k": NTR_CORE_TEMPERATURE_K[b.spec.drive],
                          "isp_s": b.isp_s, "thrust_n": b.thrust_n}
        propellant = PROPELLANT["lh2"]
        tank_rate = rate("lh2_tank")
    rows["tanks"] = {"kind": "tank", "name": propellant["name"], "mass": b.tank_kg, "price": b.tank_kg * tank_rate,
                     "temperature_k": propellant["temperature_k"], "capacity_kg": b.propellant_kg,
                     "volume_m3": b.propellant_kg / propellant["density_kg_m3"]}
    crew = b.spec.crew
    rows["habitat"] = {"kind": "habitat", "name": "Crew habitat", "mass": b.habitat_kg,
                       "price": b.habitat_kg * rate("habitat"), "crew_capacity": crew,
                       "power_w": DSH_HEAT_W / DSH_CREW * crew, "heat_w": DSH_HEAT_W / DSH_CREW * crew,
                       "temperature_k": CABIN_TEMPERATURE_K, "volume_m3": DSH_PRESSURIZED_M3 / DSH_CREW * crew}
    rows["hold"] = {"kind": "hold", "name": "Cargo hold", "mass": b.hold_kg, "price": b.hold_kg * rate("hold"),
                    "capacity_kg": b.cargo_kg, "volume_m3": b.spec.cargo_units * HOLD_M3_PER_CARGO_UNIT}
    rows["structure"] = {"kind": "structure", "name": "Truss and structure", "mass": b.structure_kg,
                         "price": b.structure_kg * rate("structure")}
    if b.radiator_kg <= 0.0:
        rows.pop("radiators", None)

    masses = split_rounded(b.dry_kg, {k: r["mass"] for k, r in rows.items()})
    prices = split_rounded(round(b.value_cr, -2), {k: r["price"] for k, r in rows.items()})
    out = []
    for key, r in rows.items():
        out.append({**r, "component_id": key, "parent_id": "", "mass_kg": masses[key], "price_cr": prices[key]})
        if key != "habitat":
            continue
        child_mass = split_rounded(masses[key], DSH_DRY_KG)
        child_price = split_rounded(prices[key], DSH_DRY_KG)
        for part in DSH_DRY_KG:
            child = {"kind": part, "name": HABITAT_PART_NAMES[part], "component_id": f"habitat.{part}",
                     "parent_id": "habitat", "mass_kg": child_mass[part], "price_cr": child_price[part]}
            if part == "crew_quarters":
                child["volume_m3"] = DSH_HABITABLE_M3 / DSH_CREW * crew
                child["crew_capacity"] = crew
            if part == "thermal_control":
                child["heat_w"] = r["heat_w"]
                child["temperature_k"] = CABIN_RADIATOR_TEMPERATURE_K
            out.append(child)
    return out


def hull_slots(spec: ClassSpec) -> dict[str, int]:
    """Module slots by hull (game): couriers have room for less."""
    small = spec.cargo_units < 100
    slots = {"habitat": 2 if small else 3, "hold": 0 if small else 1}
    slots["drive" if spec.drive == "plasma" else "tank"] = 1
    return slots


def modules(b: Build) -> list[dict]:
    """The modules a nominal hull can fit, sized for it."""
    rate = lambda key: USD_PER_KG[key] * CR_PER_USD
    out = []
    # Storm shelter: water wall around the refuge (cylinder, height = diameter).
    volume = SHELTER_M3_PER_CREW * b.spec.crew
    radius = (volume / (2.0 * math.pi)) ** (1.0 / 3.0)
    area = 6.0 * math.pi * radius ** 2
    water = SHELTER_AREAL_KG_PER_M2 * area
    walls = SHELTER_TANK_FRACTION * water
    truss = STRUCTURE_FRACTION * (water + walls)
    out.append({"module_id": "storm_shelter", "slot": "habitat", "name": "Storm shelter (water wall)",
                "mass_kg": round(water + walls + truss),
                "price_cr": round((walls * USD_PER_KG["plasma_tank"] + truss * USD_PER_KG["structure"]) * CR_PER_USD),
                "volume_m3": volume, "area_m2": area, "capacity_kg": water,
                "effects": f"spe_shield_g_cm2={SHELTER_AREAL_KG_PER_M2 / 10.0:g}"})
    return out


def write_modules_csv(nominal: list[Build], modules_path: Path, slots_path: Path) -> None:
    def cell(value) -> str:
        if value is None or value == "":
            return ""
        if isinstance(value, float):
            return f"{value:.6g}"
        return str(value)
    with modules_path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(MODULE_HEADER)
        for b in nominal:
            for m in modules(b):
                writer.writerow([b.spec.id] + [cell(m.get(k)) for k in MODULE_HEADER[1:]])
    with slots_path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(SLOT_HEADER)
        for b in nominal:
            for slot, count in hull_slots(b.spec).items():
                if count > 0:
                    writer.writerow([b.spec.id, slot, count])


def write_components_csv(builds: list[Build], path: Path) -> None:
    def cell(value) -> str:
        if value is None or value == "":
            return ""
        if isinstance(value, float):
            return f"{value:.6g}" if abs(value) < 1e6 else str(round(value))
        return str(value)
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(COMPONENT_HEADER)
        for b in builds:
            for c in components(b):
                writer.writerow([b.id, c["component_id"], c["parent_id"], c["kind"], c["name"],
                                 c["mass_kg"], c["price_cr"]]
                                + [cell(c.get(k)) for k in COMPONENT_HEADER[7:]])


def check(builds: list[Build]) -> None:
    """Components must decompose the class rows exactly (what the C++ loader validates)."""
    for b in builds:
        parts = components(b)
        top = [c for c in parts if not c["parent_id"]]
        assert sum(c["mass_kg"] for c in top) == round(b.dry_kg), b.id
        assert sum(c["price_cr"] for c in top) == int(round(b.value_cr, -2)), b.id
        habitat = next(c for c in top if c["kind"] == "habitat")
        children = [c for c in parts if c["parent_id"] == "habitat"]
        assert sum(c["mass_kg"] for c in children) == habitat["mass_kg"], b.id
        assert sum(c["price_cr"] for c in children) == habitat["price_cr"], b.id
    print(f"check: components of {len(builds)} classes add up")


def summary(builds: list[Build]) -> None:
    for b in builds:
        if b.spec.drive == "plasma":
            perf = f"alpha={b.alpha:5.0f} W/kg  kappa={b.kappa:7.2f}"
        else:
            perf = (f"Isp={b.isp_s:4.0f}s  dv={b.delta_v() / 1000:5.1f} km/s  "
                    f"laden={b.delta_v(b.cargo_kg) / 1000:5.1f}  a={b.cruise_accel:.2f} m/s2")
        print(f"{b.id:29s} dry={b.dry_kg / 1000:6.1f} t  prop={b.propellant_kg / 1000:5.0f} t  "
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
              f"{t(b.hold_kg)} | {t(b.structure_kg)} | {t(b.dry_kg)} | {t(b.propellant_kg)} | {perf} | "
              f"{b.value_usd / 1e6:.0f} M$ = {b.value_cr / 1000:.0f}k cr |")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--markdown", action="store_true", help="print the per-class mass breakdown table")
    parser.add_argument("--dry-run", action="store_true", help="do not write the CSVs")
    parser.add_argument("--check", action="store_true", help="verify that the components add up to the classes")
    args = parser.parse_args()
    nominal = [build(spec) for spec in CLASSES]
    variants = [tank_variant(b, option) for b in nominal for option in TANK_OPTIONS]
    if args.check:
        check(variants)
        return
    if not args.dry_run:
        write_csv(variants, Path(__file__).with_name("ship_classes.csv"))
        write_components_csv(variants, Path(__file__).with_name("ship_components.csv"))
        write_modules_csv(nominal, Path(__file__).with_name("modules.csv"), Path(__file__).with_name("hull_slots.csv"))
    summary(variants)
    if args.markdown:
        markdown(nominal)


if __name__ == "__main__":
    main()
