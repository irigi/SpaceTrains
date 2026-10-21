#!/usr/bin/env python3
"""The ship class builder: components decompose the classes exactly, and the CSVs in
data/ship_classes are what the builder writes (no hand edits, no stale files).

    python3 tests/test_build_ship_classes.py   (also run by ctest)
"""

import importlib.util
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("builder", ROOT / "data" / "ship_classes" / "build_ship_classes.py")
builder = importlib.util.module_from_spec(spec)
sys.modules["builder"] = builder
spec.loader.exec_module(builder)

nominal = [builder.build(s) for s in builder.CLASSES]
variants = [builder.tank_variant(b, option) for b in nominal for option in builder.TANK_OPTIONS]
builder.check(variants)

with tempfile.TemporaryDirectory() as tmp:
    for name, write in (("ship_classes.csv", builder.write_csv), ("ship_components.csv", builder.write_components_csv)):
        fresh = Path(tmp) / name
        write(variants, fresh)
        if fresh.read_text() != (ROOT / "data" / "ship_classes" / name).read_text():
            sys.exit(f"data/ship_classes/{name} differs from the builder's output: re-run build_ship_classes.py")
    builder.write_modules_csv(nominal, Path(tmp) / "modules.csv", Path(tmp) / "hull_slots.csv")
    builder.write_spares_csv(Path(tmp) / "spares.csv")
    for name in ("modules.csv", "hull_slots.csv", "spares.csv"):
        if (Path(tmp) / name).read_text() != (ROOT / "data" / "ship_classes" / name).read_text():
            sys.exit(f"data/ship_classes/{name} differs from the builder's output: re-run build_ship_classes.py")

# Physics spot checks: the plasma radiators of linopt Set A (4 kW/kg at 3 kg/m2) run near 594 K,
# and a liquid-core engine is hotter than a solid-core one.
temperature = builder.radiator_temperature_k(builder.PLASMA_RADIATOR_W_PER_KG)
assert 585.0 < temperature < 600.0, temperature
assert builder.NTR_CORE_TEMPERATURE_K["liquid_core"] > builder.NTR_CORE_TEMPERATURE_K["solid_core"]
for b in variants:
    kinds = {c["kind"] for c in builder.components(b)}
    assert ("radiator" in kinds) == (b.spec.drive == "plasma"), b.id
# Wearing parts are priced from goods and wages: a whole part at base prices is its list price.
for b in nominal:
    for c in builder.components(b):
        spec_parts = builder.ENGINE_WEAR_PARTS["nuclear_thermal"] + builder.ENGINE_WEAR_PARTS["plasma"] + builder.RADIATOR_WEAR_PARTS
        match = [p for p in spec_parts if p["kind"] == c["kind"]]
        if match:
            assert abs(c["price_cr"] - c["mass_kg"] * builder.wear_part_cr_per_kg(match[0])) <= 0.5, (b.id, c["kind"])
print("ship class builder: ok")
