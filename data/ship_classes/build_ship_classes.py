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
# Passenger cabins (docs/plans/passengers.md). The user's linopt fare study
# (passenger_ticket_optimizer.py defaults): 2,500 kg of cabin per passenger ("cabins, seats,
# personal life-support and safety hardware") at 1,000 $/kg, and 300 t of shared structure,
# controls and common life support for 600 passengers at 1,500 $/kg. A cabin scales the shared
# part per berth (0.5 t), so small cabins and liners are not charged a 600-berth ship's core.
BERTH_KG = 2_500.0
BERTH_SHARED_KG = 300_000.0 / 600.0
USD_PER_KG_BERTH = 1_000.0
USD_PER_KG_BERTH_SHARED = 1_500.0
# Volume and heat per berth (estimates): the Deep Space Habitat's pressurized volume per kg of
# habitat, and a crew member's heat load.
BERTH_M3 = DSH_PRESSURIZED_M3 / DSH_CREW * (BERTH_KG + BERTH_SHARED_KG) / 8_000.0
BERTH_HEAT_W = DSH_HEAT_W / DSH_CREW
# Shielded cabins (cosmic rays). Water cuts the galactic-cosmic-ray dose by 1/e per ~46 g/cm2
# (data/economy/ship_environment.csv); NASA studies find little more gain beyond a few tens of
# g/cm2. A water wall of 20 g/cm2 (the storm shelter's 200 kg/m2) round the whole cabin, full
# time, cuts it by ~35%. The cabin is a cylinder as tall as it is wide (the storm shelter's game
# geometry). The water costs nothing (it is the ship's reserve), its tank walls and truss do.
CABIN_WALL_KG_PER_M2 = 200.0
# Plasma ships burn water: a liner can wrap its cabins in the propellant tanks instead. The
# jacket tanks have ~50% more wall than compact tanks (estimate); the shield thins as the
# propellant burns (the simulation counts the tanks' mean fill over a leg).
TANK_WRAP_EXTRA_WALL = 0.5

# Wearing and lasting parts (tier 2 follow-up, docs/plans/tier2_log.md). A part's price is
# materials (commodities at base price) plus work. The parts that wear out are priced that way
# from the economy's own goods and wages, so an overhaul that replaces them costs what they
# cost; the parts that last the ship's life (pressure vessel, magnets, frames) carry the rest
# of linopt's price: precision work, not goods.
#   NTR fuel elements: 40% of the engine (estimate; NERVA-class cores were mostly fuel
#   elements by mass); the game's reactor fuel is the finished element.
#   Plasma drive: the fission reactor's fuel (10%) and its power conversion and RF electronics
#   (15%, degraded by radiation) wear (estimates); magnets, nozzle and structure last.
#   Radiators: the heat-pipe segments (50% of a panel, as before) wear; frames and manifolds last.
# Hours per kg: a jet-engine shop visit takes ~1.25 h/kg (CFM56-7B: 2,500-3,500 h on a 2.4 t
# engine, Safe Fly Aviation) and the game's yards are automated ten-fold (estimate):
# 0.125 h/kg to fit a part; fabricating electronics or heat pipes from the goods 0.1 h/kg.
ENGINE_WEAR_PARTS = {
    "nuclear_thermal": [
        {"kind": "fuel_elements", "name": "Reactor fuel elements", "mass_share": 0.40,
         "materials": {"reactor_fuel": 1.0}, "fabrication_h_per_kg": 0.0, "install_h_per_kg": 0.125,
         "grade": "nuclear_specialist"},
    ],
    "plasma": [
        {"kind": "reactor_fuel_load", "name": "Reactor fuel load", "mass_share": 0.10,
         "materials": {"reactor_fuel": 1.0}, "fabrication_h_per_kg": 0.0, "install_h_per_kg": 0.125,
         "grade": "nuclear_specialist"},
        {"kind": "power_electronics", "name": "Power conversion and RF electronics", "mass_share": 0.15,
         "materials": {"electronics": 0.7, "machinery": 0.3}, "fabrication_h_per_kg": 0.1, "install_h_per_kg": 0.125,
         "grade": "technician"},
    ],
}
ENGINE_LASTING = {"nuclear_thermal": ("reactor_vessel", "Pressure vessel, turbopumps and nozzle"),
                  "plasma": ("magnets_structure", "Magnets, nozzle and structure")}
RADIATOR_WEAR_PARTS = [
    {"kind": "heat_pipes", "name": "Heat-pipe segments", "mass_share": 0.50,
     "materials": {"machinery": 1.0}, "fabrication_h_per_kg": 0.1, "install_h_per_kg": 0.125, "grade": "technician"},
]
RADIATOR_LASTING = ("panel_frame", "Panel frames and manifolds")
SPARES_HEADER = ["kind", "wears_with", "grade", "fabrication_h_per_kg", "install_h_per_kg", "materials"]


def _read_key_values(path: Path) -> dict[str, str]:
    with path.open() as handle:
        return {row["key"]: row["value"] for row in csv.DictReader(handle)}


DATA_DIR = Path(__file__).resolve().parent.parent
COMMODITY_CR_PER_KG = {}
with (DATA_DIR / "commodities" / "commodities.csv").open() as _handle:
    for _row in csv.DictReader(_handle):
        COMMODITY_CR_PER_KG[_row["id"]] = float(_row["base_price"]) / float(_row["mass_per_unit_kg"])
_LABOUR = _read_key_values(DATA_DIR / "economy" / "labour.csv")


def wage_cr_per_hour(grade: str) -> float:
    return float(_LABOUR["reference_wage_cr_per_day"]) * float(_LABOUR["grade." + grade]) / float(_LABOUR["shift_hours"])


def wear_part_cr_per_kg(part: dict) -> float:
    """Materials at base price plus fabricating and fitting them."""
    materials = sum(share * COMMODITY_CR_PER_KG[good] for good, share in part["materials"].items())
    return materials + (part["fabrication_h_per_kg"] + part["install_h_per_kg"]) * wage_cr_per_hour(part["grade"])


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

# Zero-boil-off cryocooler for liquid-hydrogen tanks (nuclear-thermal hulls). Heat leaks through
# high-performance MLI at ~0.2 W/m2 near 1 AU (estimate within NASA cryogenic-storage studies'
# 0.1-0.4); tank area = sphere of the tank volume x 1.3 (several cylinders). The same numbers
# drive the simulation (data/economy/ship_environment.csv). The cooler lifts 1.5x the nominal
# tank's leak at 1 AU (covers the 200% tanks there, the nominal tanks down to ~0.67 AU).
# A 20 K cooler rejecting at 300 K has Carnot COP 20/280; practical turbo-Brayton machines
# reach ~14% of Carnot: ~100 W input per W lifted (estimate); ~5 kg per W lifted (estimate).
# Power from the engine's bimodal Brayton loop (NASA bimodal NTR studies, Borowski: ~25 kWe
# per stage), ~10 kg/kWe (estimate), and 300 K two-sided panels for the input power plus lift.
MLI_FLUX_W_M2 = 0.2
TANK_AREA_FACTOR = 1.3
ZBO_LIFT_MARGIN = 1.5
ZBO_W_INPUT_PER_W_LIFT = 100.0
ZBO_KG_PER_W_LIFT = 5.0
BRAYTON_KG_PER_KWE = 10.0
ZBO_RADIATOR_K = 300.0
USD_PER_KG_CRYOCOOLER = 10_000.0   # estimate, like the plasma drive's precision hardware
# Reduced boil-off: a 90 K broad-area-cooled shield in the MLI (NASA Glenn, Christie et al. 2011,
# NTRS 20110016177: up to 70% of the heat load, theoretical) intercepts 60% of the leak (estimate).
# A 90 K cooler (Carnot COP 90/210) at ~20% of Carnot: ~12 W per W lifted; ~1 kg per W lifted;
# shield tubing ~0.3 kg/m2 of tank (estimates). Lift sized like the 20 K cooler (1.5x at 1 AU).
BAC_INTERCEPT = 0.6
BAC_W_INPUT_PER_W_LIFT = 12.0
BAC_KG_PER_W_LIFT = 1.0
BAC_TUBING_KG_PER_M2 = 0.3

# Cold hold: the whole hold lined and kept at 255 K (-18 C), where frozen food keeps a year or
# more and medicines their shelf life; perishables then spoil at a tenth of the game's ambient
# rate (estimate). Liner: MLI blanket (~0.5 kg/m2) on a 2 kg/m2 insulated panel over the hold's
# surface (a cube of the hold volume); heat leak ~0.5 W/m2 in vacuum (estimate); a 255 K cooler at
# ~1 W input per W lifted, ~1 kg per W lifted, on 300 K panels (estimates).
COLD_HOLD_LINER_KG_PER_M2 = 2.5
COLD_HOLD_LEAK_W_PER_M2 = 0.5
COLD_HOLD_DECAY_FACTOR = 0.1

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
    berths: int = 0                # passenger liners: built-in cabins


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
    # Passenger liners (docs/plans/passengers.md): 40 berths, a small hold for mail and
    # luggage. Only in service when data/economy/passengers.csv switches passengers on.
    # The plasma liner has a courier's power to mass (~500 W/kg) for the outer system; the
    # nuclear-thermal shuttle (the NTR Freighter's engine and tanks) flies fast inner-system hops.
    ClassSpec("plasma_liner", "Plasma Liner", "plasma", crew=4, habitat="long",
              cargo_units=20, propellant_kg=60_000, jet_power_w=100.0e6, berths=40),
    ClassSpec("ntr_shuttle", "Passenger Shuttle", "liquid_core", crew=3, habitat="long",
              cargo_units=20, propellant_kg=250_000, accel_full_mps2=0.3, berths=40),
    # A large liner (linopt `fare --passengers 200`: $147k a seat against $295k at 60): the drive
    # and hull are shared by five times the berths. Same power to mass and propellant per berth
    # as the 40-berth liner; crew of 8 (estimate: two watches of four).
    ClassSpec("large_plasma_liner", "Large Plasma Liner", "plasma", crew=8, habitat="long",
              cargo_units=50, propellant_kg=300_000, jet_power_w=500.0e6, berths=200),
    # A cislunar ferry (docs/plans/passenger_routes.md, "Luna ferry"): the cheapest cost per
    # passenger on Earth <-> Luna in a design study at the liners' cabin standard and power to mass
    # (<= ~540 W/kg): 30 berths for a flow of ~3 people a day, a crew of two, and tanks for a few
    # spiral legs (~3.5 t a leg out of Earth). The crew habitat is the long one, as every class's:
    # nothing keeps a ship on short hops, and it is the cheapest small liner on some long routes too.
    ClassSpec("luna_ferry", "Luna Ferry", "plasma", crew=2, habitat="long",
              cargo_units=5, propellant_kg=12_000, jet_power_w=65.0e6, berths=30),
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
    cabin_kg: float = 0.0

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
                + self.structure_kg * USD_PER_KG["structure"]
                + cabin_usd(self.spec.berths))

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


def cabin_kg(berths: int) -> float:
    return berths * (BERTH_KG + BERTH_SHARED_KG)


def cabin_area_m2(berths: int) -> float:
    """Wall area of a cabin block of this many berths (cylinder, height = diameter)."""
    radius = (BERTH_M3 * berths / (2.0 * math.pi)) ** (1.0 / 3.0)
    return 6.0 * math.pi * radius ** 2


def water_wall(area_m2: float) -> tuple[float, float, float]:
    """Water, tank walls and truss of a 20 g/cm2 wall over this area."""
    water = CABIN_WALL_KG_PER_M2 * area_m2
    walls = SHELTER_TANK_FRACTION * water
    return water, walls, STRUCTURE_FRACTION * (water + walls)


def cabin_usd(berths: int) -> float:
    return berths * (BERTH_KG * USD_PER_KG_BERTH + BERTH_SHARED_KG * USD_PER_KG_BERTH_SHARED)


def build(spec: ClassSpec) -> Build:
    # Built-in cabins count with the habitat in the mass budget (structure, engine sizing).
    cabins = cabin_kg(spec.berths)
    habitat = spec.crew * HABITAT_KG_PER_CREW[spec.habitat]
    hold = HOLD_FRACTION * spec.cargo_units * RATED_KG_PER_CARGO_UNIT
    if spec.drive == "plasma":
        jet_fraction = 1.0 - PLASMA_HEAT_FRACTION
        engine = spec.jet_power_w / (jet_fraction * PLASMA_ENGINE_W_PER_KG)
        radiator = spec.jet_power_w * PLASMA_HEAT_FRACTION / (jet_fraction * PLASMA_RADIATOR_W_PER_KG)
        tank = PLASMA_TANK_FRACTION * spec.propellant_kg
        rest = engine + radiator + tank + habitat + hold + cabins
        structure = STRUCTURE_FRACTION * rest
        return Build(spec, engine, radiator, tank, habitat, hold, structure, rest + structure, 0.0, 0.0,
                     spec.propellant_kg, cabin_kg=cabins)

    ntr = NTR[spec.drive]
    tank = LH2_TANK_FRACTION * spec.propellant_kg
    # The engine is sized by thrust / full mass, and the full mass includes the engine:
    # (1 + s) (engine + fixed) + propellant = full, engine = a * full / (T/W g0).
    fixed = tank + habitat + hold + cabins
    k = spec.accel_full_mps2 / (ntr["thrust_to_weight"] * G0)
    s = STRUCTURE_FRACTION
    full = ((1.0 + s) * fixed + spec.propellant_kg) / (1.0 - (1.0 + s) * k)
    engine = k * full
    structure = s * (engine + fixed)
    dry = engine + fixed + structure
    return Build(spec, engine, 0.0, tank, habitat, hold, structure, dry,
                 spec.accel_full_mps2 * full, ntr["isp_s"], spec.propellant_kg, cabin_kg=cabins)


def tank_variant(nominal: Build, option: float) -> Build:
    """The nominal build with its tanks scaled: same engine, radiators, habitat and hold."""
    if option == 1.0:
        return nominal
    propellant = nominal.propellant_kg * option
    plasma = nominal.spec.drive == "plasma"
    tank = (PLASMA_TANK_FRACTION if plasma else LH2_TANK_FRACTION) * propellant
    rest = nominal.engine_kg + nominal.radiator_kg + tank + nominal.habitat_kg + nominal.hold_kg + nominal.cabin_kg
    structure = STRUCTURE_FRACTION * rest
    return Build(nominal.spec, nominal.engine_kg, nominal.radiator_kg, tank, nominal.habitat_kg, nominal.hold_kg,
                 structure, rest + structure, nominal.thrust_n, nominal.isp_s, propellant, option,
                 cabin_kg=nominal.cabin_kg)


def write_csv(builds: list[Build], path: Path) -> None:
    header = ["id", "name", "propulsion_type", "dry_mass_kg", "propellant_capacity_kg",
              "cargo_capacity_units", "max_delta_v_mps", "cruise_accel_mps2",
              "specific_engine_power_w_per_kg", "ship_value_cr", "crew_size", "hull_id", "berths"]
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
                b.spec.berths,
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
    if b.spec.berths > 0:
        rows["cabins"] = {"kind": "cabin", "name": "Passenger cabins", "mass": b.cabin_kg,
                          "price": cabin_usd(b.spec.berths) * CR_PER_USD, "crew_capacity": b.spec.berths,
                          "power_w": BERTH_HEAT_W * b.spec.berths, "heat_w": BERTH_HEAT_W * b.spec.berths,
                          "temperature_k": CABIN_TEMPERATURE_K, "volume_m3": BERTH_M3 * b.spec.berths,
                          "area_m2": cabin_area_m2(b.spec.berths)}
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
    for key, wear_parts, lasting in (
            ("engine", ENGINE_WEAR_PARTS["plasma" if plasma else "nuclear_thermal"],
             ENGINE_LASTING["plasma" if plasma else "nuclear_thermal"]),
            ("radiators", RADIATOR_WEAR_PARTS, RADIATOR_LASTING)):
        if key not in rows:
            continue
        shares = {p["kind"]: p["mass_share"] for p in wear_parts}
        shares[lasting[0]] = 1.0 - sum(shares.values())
        child_mass = split_rounded(masses[key], shares)
        for p in wear_parts:
            out.append({"kind": p["kind"], "name": p["name"], "component_id": f"{key}.{p['kind']}", "parent_id": key,
                        "mass_kg": child_mass[p["kind"]], "price_cr": round(child_mass[p["kind"]] * wear_part_cr_per_kg(p))})
        worn_price = sum(c["price_cr"] for c in out if c["parent_id"] == key)
        out.append({"kind": lasting[0], "name": lasting[1], "component_id": f"{key}.{lasting[0]}", "parent_id": key,
                    "mass_kg": child_mass[lasting[0]], "price_cr": prices[key] - worn_price})
    return out


def write_spares_csv(path: Path) -> None:
    """What an overhaul replaces: the wearing parts, their goods, hours and grade."""
    with path.open("w", newline="") as handle:
        writer = csv.writer(handle, lineterminator="\n")
        writer.writerow(SPARES_HEADER)
        for wears_with, parts in (("drive", ENGINE_WEAR_PARTS["nuclear_thermal"] + ENGINE_WEAR_PARTS["plasma"]),
                                  ("radiators", RADIATOR_WEAR_PARTS)):
            for p in parts:
                writer.writerow([p["kind"], wears_with, p["grade"], p["fabrication_h_per_kg"], p["install_h_per_kg"],
                                 ";".join(f"{good}={share}" for good, share in p["materials"].items())])


def hull_slots(spec: ClassSpec) -> dict[str, int]:
    """Module slots by hull (game): couriers have room for less."""
    small = spec.cargo_units < 100
    # Liners have room for a storm shelter and both cabin shields (water wall, tank jacket).
    slots = {"habitat": 2 if small and spec.berths == 0 else 3, "hold": 0 if small else 1}
    slots["drive" if spec.drive == "plasma" else "tank"] = 1
    slots["cabin"] = 0 if spec.berths > 0 else 1
    return slots


def cabin_module_berths(spec: ClassSpec) -> int:
    """Berths of the cabin module a hull can fit (game): couriers 12, freighters 24, bulk hulls 60."""
    if spec.cargo_units < 100:
        return 12
    return 24 if spec.cargo_units < 1000 else 60


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
    if hull_slots(b.spec).get("cabin", 0) > 0:
        berths = cabin_module_berths(b.spec)
        cabins = cabin_kg(berths)
        truss = STRUCTURE_FRACTION * cabins
        out.append({"module_id": "cabin", "slot": "cabin", "name": f"Passenger cabins ({berths} berths)",
                    "mass_kg": round(cabins + truss),
                    "price_cr": round((cabin_usd(berths) + truss * USD_PER_KG["structure"]) * CR_PER_USD),
                    "power_w": BERTH_HEAT_W * berths, "heat_w": BERTH_HEAT_W * berths,
                    "temperature_k": CABIN_TEMPERATURE_K, "volume_m3": BERTH_M3 * berths,
                    "area_m2": cabin_area_m2(berths), "crew_capacity": berths, "effects": f"berths={berths}"})
        water, walls, wall_truss = water_wall(cabin_area_m2(berths))
        out.append({"module_id": "shielded_cabin", "slot": "cabin",
                    "name": f"Shielded passenger cabins ({berths} berths, water wall)",
                    "mass_kg": round(cabins + truss + water + walls + wall_truss),
                    "price_cr": round((cabin_usd(berths) + walls * USD_PER_KG["plasma_tank"]
                                       + (truss + wall_truss) * USD_PER_KG["structure"]) * CR_PER_USD),
                    "power_w": BERTH_HEAT_W * berths, "heat_w": BERTH_HEAT_W * berths,
                    "temperature_k": CABIN_TEMPERATURE_K, "volume_m3": BERTH_M3 * berths,
                    "area_m2": cabin_area_m2(berths), "capacity_kg": water, "crew_capacity": berths,
                    "effects": f"berths={berths};cabin_shield_g_cm2={CABIN_WALL_KG_PER_M2 / 10.0:g}"})
    if b.spec.berths > 0:
        # Liners: a water wall round the built-in cabins, or (plasma) the cabins inside the tanks.
        area = cabin_area_m2(b.spec.berths)
        water, walls, truss = water_wall(area)
        out.append({"module_id": "cabin_water_wall", "slot": "habitat", "name": "Cabin water wall",
                    "mass_kg": round(water + walls + truss),
                    "price_cr": round((walls * USD_PER_KG["plasma_tank"] + truss * USD_PER_KG["structure"]) * CR_PER_USD),
                    "area_m2": area, "capacity_kg": water,
                    "effects": f"cabin_shield_g_cm2={CABIN_WALL_KG_PER_M2 / 10.0:g}"})
        if b.spec.drive == "plasma":
            jacket = TANK_WRAP_EXTRA_WALL * b.tank_kg
            truss = STRUCTURE_FRACTION * jacket
            out.append({"module_id": "tank_wrap", "slot": "habitat", "name": "Cabins inside the propellant tanks",
                        "mass_kg": round(jacket + truss),
                        "price_cr": round((jacket * USD_PER_KG["plasma_tank"] + truss * USD_PER_KG["structure"]) * CR_PER_USD),
                        "area_m2": area, "effects": f"tank_wrap_m2={area:.6g}"})
    if hull_slots(b.spec).get("hold", 0) > 0:
        hold_volume = b.spec.cargo_units * HOLD_M3_PER_CARGO_UNIT
        area = 6.0 * hold_volume ** (2.0 / 3.0)
        liner = COLD_HOLD_LINER_KG_PER_M2 * area
        lift = COLD_HOLD_LEAK_W_PER_M2 * area
        cooler = 1.0 * lift
        heat = 2.0 * lift
        panel_w_per_m2 = 2.0 * RADIATOR_EMISSIVITY * STEFAN_BOLTZMANN * ZBO_RADIATOR_K ** 4
        radiator = heat / panel_w_per_m2 * RADIATOR_AREAL_KG_PER_M2
        truss = STRUCTURE_FRACTION * (liner + cooler + radiator)
        out.append({"module_id": "cold_hold", "slot": "hold", "name": "Cold hold (255 K)",
                    "mass_kg": round(liner + cooler + radiator + truss),
                    "price_cr": round((liner * USD_PER_KG["hold"] + cooler * USD_PER_KG_CRYOCOOLER
                                       + radiator * USD_PER_KG["radiator"] + truss * USD_PER_KG["structure"]) * CR_PER_USD),
                    "power_w": lift, "heat_w": heat, "temperature_k": 255.0, "volume_m3": hold_volume,
                    "area_m2": area, "effects": f"perishable_decay_factor={COLD_HOLD_DECAY_FACTOR:g}"})
    if spec_is_thermal(b):
        volume = b.propellant_kg / PROPELLANT["lh2"]["density_kg_m3"]
        area = TANK_AREA_FACTOR * 4.0 * math.pi * (3.0 * volume / (4.0 * math.pi)) ** (2.0 / 3.0)
        lift = ZBO_LIFT_MARGIN * MLI_FLUX_W_M2 * area
        power = ZBO_W_INPUT_PER_W_LIFT * lift
        heat = power + lift
        panel_w_per_m2 = 2.0 * RADIATOR_EMISSIVITY * STEFAN_BOLTZMANN * ZBO_RADIATOR_K ** 4
        radiator = heat / panel_w_per_m2 * RADIATOR_AREAL_KG_PER_M2
        cooler = ZBO_KG_PER_W_LIFT * lift
        brayton = BRAYTON_KG_PER_KWE * power / 1000.0
        truss = STRUCTURE_FRACTION * (cooler + brayton + radiator)
        out.append({"module_id": "zbo_cryocooler", "slot": "tank", "name": "Zero-boil-off cryocooler (20 K)",
                    "mass_kg": round(cooler + brayton + radiator + truss),
                    "price_cr": round((cooler * USD_PER_KG_CRYOCOOLER + brayton * USD_PER_KG["liquid_core"]
                                       + radiator * USD_PER_KG["radiator"] + truss * USD_PER_KG["structure"]) * CR_PER_USD),
                    "power_w": power, "heat_w": heat, "temperature_k": 20.0,
                    "area_m2": heat / panel_w_per_m2, "effects": f"zbo_lift_w={lift:.6g}"})
        bac_lift = BAC_INTERCEPT * lift
        bac_power = BAC_W_INPUT_PER_W_LIFT * bac_lift
        bac_heat = bac_power + bac_lift
        bac_radiator = bac_heat / panel_w_per_m2 * RADIATOR_AREAL_KG_PER_M2
        bac_cooler = BAC_KG_PER_W_LIFT * bac_lift
        tubing = BAC_TUBING_KG_PER_M2 * area
        bac_brayton = BRAYTON_KG_PER_KWE * bac_power / 1000.0
        bac_truss = STRUCTURE_FRACTION * (bac_cooler + tubing + bac_brayton + bac_radiator)
        out.append({"module_id": "boiloff_shield", "slot": "tank", "name": "90 K cooled tank shield (reduced boil-off)",
                    "mass_kg": round(bac_cooler + tubing + bac_brayton + bac_radiator + bac_truss),
                    "price_cr": round((bac_cooler * USD_PER_KG_CRYOCOOLER + tubing * USD_PER_KG["lh2_tank"]
                                       + bac_brayton * USD_PER_KG["liquid_core"] + bac_radiator * USD_PER_KG["radiator"]
                                       + bac_truss * USD_PER_KG["structure"]) * CR_PER_USD),
                    "power_w": bac_power, "heat_w": bac_heat, "temperature_k": 90.0,
                    "area_m2": area, "effects": f"boiloff_intercept={BAC_INTERCEPT:g}"})
    return out


def spec_is_thermal(b: Build) -> bool:
    return b.spec.drive != "plasma"


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
        for parent in top:
            children = [c for c in parts if c["parent_id"] == parent["component_id"]]
            if parent["kind"] in ("habitat", "engine", "radiator"):
                assert children, (b.id, parent["component_id"])
            if children:
                assert sum(c["mass_kg"] for c in children) == parent["mass_kg"], (b.id, parent["component_id"])
                assert sum(c["price_cr"] for c in children) == parent["price_cr"], (b.id, parent["component_id"])
                # The lasting part carries the precision work: it must cost more per kg than the goods.
                assert all(c["price_cr"] > 0 for c in children), (b.id, parent["component_id"])
        del habitat
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
        write_spares_csv(Path(__file__).with_name("spares.csv"))
    summary(variants)
    if args.markdown:
        markdown(nominal)


if __name__ == "__main__":
    main()
