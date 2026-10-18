#!/usr/bin/env python3
"""Mock-ups of to-scale ship schematics (top view) from data/ship_classes/ship_components.csv.

The same layout rules as the inspector's godot/scripts/ui/ShipSchematic.gd; it also draws the
fleet at one scale (docs/archive/ship_visuals_shelved.md). Not game code.

    python3 tools/ship_schematic_mockup.py . <out_dir>
"""
import csv, math, sys
from collections import defaultdict
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Polygon, FancyBboxPatch

root, out_dir = sys.argv[1], sys.argv[2]
rows = defaultdict(list)
for r in csv.DictReader(open(f'{root}/data/ship_classes/ship_components.csv')):
    rows[r['class_id']].append(r)
classes = {r['id']: r for r in csv.DictReader(open(f'{root}/data/ship_classes/ship_classes.csv'))}
f = lambda r, k: float(r[k]) if r[k] else 0.0

COL = {'engine': '#e07a3f', 'radiator': '#c94040', 'tank': '#5aa0d8', 'habitat': '#e8e2c8',
       'hold': '#7dbf6a', 'structure': '#8a8f99', 'shield': '#555b66'}
HAB_DIAMETER = 4.5  # m, DSH module diameter (NTRS 20120014530)
WING = {}

def temp_color(T):
    # false colour: 250 K blue -> 600 K red -> 3000 K+ white-yellow
    import matplotlib.colors as mc
    t = min(max((math.log(T) - math.log(200)) / (math.log(6000) - math.log(200)), 0), 1)
    return plt.cm.inferno(0.15 + 0.85 * t)

def layout(cid):
    """Blocks along the ship axis, x from bow (0) to stern; each block (kind, length, width, label)."""
    comps = {r['component_id']: r for r in rows[cid]}
    plasma = 'radiators' in comps
    blocks = []
    hab = comps['habitat']; vol = f(hab, 'volume_m3')
    blocks.append(('habitat', vol / (math.pi * (HAB_DIAMETER / 2) ** 2), HAB_DIAMETER, f"habitat\n{int(f(hab,'crew_capacity'))} crew"))
    hold = comps['hold']; hv = f(hold, 'volume_m3')
    hw = max(3.0, min(12.0, hv ** (1 / 3) * 1.2)); blocks.append(('hold', hv / (hw * hw), hw, f"hold\n{hv:.0f} m³"))
    tank = comps['tanks']; tv = f(tank, 'volume_m3')
    td = max(2.0, min(10.0, tv ** (1 / 3) * 0.7))
    blocks.append(('tank', tv / (math.pi * (td / 2) ** 2), td, f"{'LH₂' if f(tank,'temperature_k') < 100 else 'water'}\n{tv:.0f} m³"))
    if not plasma:
        blocks.append(('shield', 0.6, max(3.0, td * 0.6), ''))
    eng = comps['engine']; em = f(eng, 'mass_kg')
    el = max(2.0, (em / 1000) ** (1 / 3) * 2.2)
    blocks.append(('engine', el, el * 0.8, 'engine'))
    return comps, blocks, plasma

def draw(ax, cid, x0=0.0, y0=0.0, labels=True, title=True):
    comps, blocks, plasma = layout(cid)
    x = x0
    centers = {}
    for kind, length, width, label in blocks:
        if kind == 'engine' and not plasma:
            ax.add_patch(Rectangle((x, y0 - width * 0.35), length * 0.55, width * 0.7, color=temp_color(f(comps['engine'], 'temperature_k')), ec='k', lw=0.4))
            ax.add_patch(Polygon([(x + length * 0.55, y0 - width * 0.2), (x + length, y0 - width * 0.5), (x + length, y0 + width * 0.5), (x + length * 0.55, y0 + width * 0.2)], color=COL['engine'], ec='k', lw=0.4))
        elif kind == 'engine':
            ax.add_patch(Rectangle((x, y0 - width * 0.3), length * 0.6, width * 0.6, color=COL['engine'], ec='k', lw=0.4))
            ax.add_patch(Polygon([(x + length * 0.6, y0 - width * 0.15), (x + length, y0 - width * 0.45), (x + length, y0 + width * 0.45), (x + length * 0.6, y0 + width * 0.15)], color='#b78cf0', ec='k', lw=0.4))
        else:
            c = COL[kind]
            if kind == 'tank':
                c = '#bfe3ff' if f(comps['tanks'], 'temperature_k') < 100 else '#4a8cc9'
            ax.add_patch(FancyBboxPatch((x, y0 - width / 2), length, width, boxstyle='round,pad=0,rounding_size=%.2f' % min(width / 2, length / 2) if kind in ('habitat', 'tank') else 'square,pad=0', fc=c, ec='k', lw=0.4))
        centers[kind] = (x + length / 2, width)
        if labels and label:
            ax.text(x + length / 2, y0, label, ha='center', va='center', fontsize=6.5, color='k')
        x += length + 1.0
    # truss under everything
    ax.plot([x0, x - 1.0], [y0, y0], color=COL['structure'], lw=1.0, zorder=0)
    if plasma:
        rad = comps['radiators']; area = f(rad, 'area_m2')
        span = math.sqrt(area / 2 / 2.5); wing_len = 2.5 * span   # two wings, 2.5:1
        hx = (x0 + x - 1.0) / 2
        for side in (-1, 1):
            yb = y0 + side * 2.5
            ax.add_patch(Rectangle((hx - wing_len / 2, yb if side > 0 else yb - span), wing_len, span, fc=temp_color(f(rad, 'temperature_k')), ec='k', lw=0.4, zorder=-1))
        ax.plot([hx, hx], [y0 - 2.5, y0 + 2.5], color=COL['structure'], lw=1.0, zorder=-1)
        if labels:
            ax.text(hx, y0 + 2.5 + span / 2, f"drive radiators {area:.0f} m² at {f(rad,'temperature_k'):.0f} K", ha='center', va='center', fontsize=6.5, color='w')
        WING[cid] = span + 2.5
    if title:
        ax.text(x0, y0 + (WING.get(cid, 0) + 1 if plasma else 8), classes[cid]['name'], fontsize=8, fontweight='bold', va='bottom')
    return x - 1.0 - x0

def mass_bar(ax, cid):
    top = [r for r in rows[cid] if not r['parent_id']]
    total = sum(f(r, 'mass_kg') for r in top)
    left = 0
    for r in top:
        w = f(r, 'mass_kg') / total
        ax.barh(0, w, left=left, color=COL.get(r['kind'], '#999'), ec='k', lw=0.4)
        if w > 0.06:
            ax.text(left + w / 2, 0, f"{r['kind']}\n{f(r,'mass_kg')/1000:.1f} t", ha='center', va='center', fontsize=6.5)
        left += w
    ax.set_xlim(0, 1); ax.axis('off')
    ax.set_title(f"dry mass {total/1000:.1f} t by part", fontsize=8)

# 1. One schematic + mass bar per showcase class
for cid in ('plasma_freighter', 'light_freighter', 'ntr_freighter', 'plasma_bulk_tanker'):
    fig = plt.figure(figsize=(8, 3.6))
    ax = fig.add_axes([0.03, 0.28, 0.94, 0.68]); ax.set_aspect('equal'); ax.axis('off')
    length = draw(ax, cid)
    ax.set_xlim(-2, length + 2); ax.autoscale_view()
    ax.plot([0, 10], [-9, -9], color='k', lw=1); ax.text(5, -10, '10 m', ha='center', va='top', fontsize=6.5)
    ax.relim(); ax.autoscale_view()
    bx = fig.add_axes([0.05, 0.03, 0.9, 0.16]); mass_bar(bx, cid)
    lx = fig.add_axes([0.80, 0.30, 0.17, 0.03])
    import numpy as np
    temps = np.exp(np.linspace(math.log(200), math.log(6000), 64))
    lx.imshow([[temp_color(T) for T in temps]], aspect='auto', extent=(0, 1, 0, 1))
    lx.set_yticks([]); lx.set_xticks([0, 0.5, 1]); lx.set_xticklabels(['200 K', '1100 K', '6000 K'], fontsize=6)
    lx.set_title('part temperature', fontsize=6.5)
    fig.savefig(f'{out_dir}/schematic_{cid}.png', dpi=130); plt.close(fig)

# 2. The whole fleet at one scale
hulls = [c for c in classes if classes[c]['hull_id'] == c]
fig, ax = plt.subplots(figsize=(10, 9)); ax.set_aspect('equal'); ax.axis('off')
y = 0
for cid in hulls:
    plasma = any(r['kind'] == 'radiator' for r in rows[cid])
    if plasma:
        layout(cid); comps = {r['component_id']: r for r in rows[cid]}
        half = math.sqrt(f(comps['radiators'], 'area_m2') / 2 / 2.5) + 2.5
    else:
        half = 6
    y -= half + 6
    draw(ax, cid, 0, y, labels=False)
    y -= half
ax.plot([0, 50], [y - 12, y - 12], color='k', lw=1); ax.text(25, y - 14, '50 m', ha='center', va='top', fontsize=7)
ax.relim(); ax.autoscale_view()
fig.savefig(f'{out_dir}/fleet_to_scale.png', dpi=110, bbox_inches='tight'); plt.close(fig)
print('ok')
