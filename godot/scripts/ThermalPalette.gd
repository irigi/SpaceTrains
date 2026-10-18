extends RefCounted

# False colour for part temperatures (inferno scale, log temperature from 200 K to 6000 K),
# shared by the ship schematic and the map's radiator wings. A thermal view, not what the
# eye would see: visible glow starts near 800 K, so 594 K radiators would look dark.

const T_MIN_K := 200.0
const T_MAX_K := 6000.0

# matplotlib's inferno at eight even stops.
const STOPS := [
    Color(0.000, 0.000, 0.016), Color(0.122, 0.047, 0.282), Color(0.333, 0.059, 0.427),
    Color(0.533, 0.133, 0.416), Color(0.729, 0.212, 0.333), Color(0.890, 0.349, 0.200),
    Color(0.976, 0.557, 0.035), Color(0.988, 1.000, 0.643),
]

# Position on the scale, 0 at T_MIN_K and 1 at T_MAX_K.
static func scale_of(temperature_k: float) -> float:
    var t := (log(maxf(temperature_k, 1.0)) - log(T_MIN_K)) / (log(T_MAX_K) - log(T_MIN_K))
    return clampf(t, 0.0, 1.0)

# The bottom of the scale is nearly black; parts start at 15% so cold ones stay visible.
static func color(temperature_k: float) -> Color:
    return at(0.15 + 0.85 * scale_of(temperature_k))

static func at(x: float) -> Color:
    var f := clampf(x, 0.0, 1.0) * float(STOPS.size() - 1)
    var i := mini(int(f), STOPS.size() - 2)
    return (STOPS[i] as Color).lerp(STOPS[i + 1], f - float(i))
