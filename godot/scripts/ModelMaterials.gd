extends RefCounted

# Materials for the ship and station models, one per mesh slot (MeshBuilder). Muted and
# matte, like real spacecraft: white and gold insulation blankets, grey truss and steel,
# dark engine metal. Nothing glows except windows, hot nozzles, plumes and radiators in
# false colour (ThermalPalette) while a drive runs. Lit surfaces stay below the bloom
# threshold so models don't flare near the Sun.
#
# Shared materials are cached; slots that change per ship (faction band, radiators, hot
# nozzle) get their own instance (per_instance()).

const Thermal := preload("res://scripts/ThermalPalette.gd")

const PER_INSTANCE := ["band", "radiator", "nozzle_hot"]
# Ship and station meshes are on render layers 1 and 2; the camera's fill light (Main.gd)
# lights layer 2 only.
const FILL_LAYER := 2
const MODEL_LAYERS := 3
const FILL_ENERGY := 0.25

static var _shared := {}
# Thermal view (T): radiators in false colour (ThermalPalette) instead of their paint.
static var thermal_view := false
static var _radiator_shader: Shader
static var _plume_shader: Shader

static func _standard(albedo: Color, roughness: float, metallic := 0.0) -> StandardMaterial3D:
    var m := StandardMaterial3D.new()
    m.albedo_color = albedo
    m.roughness = roughness
    m.metallic = metallic
    return m

static func shared(slot: String) -> Material:
    if _shared.has(slot):
        return _shared[slot]
    var m: StandardMaterial3D
    match slot:
        "blanket":  # white beta-cloth insulation
            m = _standard(Color(0.48, 0.48, 0.47), 0.9)
        "foil":  # gold-coloured MLI (Kapton), on reactors and drives
            m = _standard(Color(0.6, 0.5, 0.32), 0.55, 0.25)
        "silver":  # aluminised sunshades
            m = _standard(Color(0.52, 0.54, 0.56), 0.45, 0.3)
        "truss":
            m = _standard(Color(0.42, 0.43, 0.45), 0.6, 0.2)
        "tank":  # foam-insulated cryogenic tanks
            m = _standard(Color(0.58, 0.57, 0.54), 0.85)
        "steel":
            m = _standard(Color(0.5, 0.52, 0.55), 0.5, 0.25)
        "metal":
            m = _standard(Color(0.2, 0.21, 0.23), 0.45, 0.3)
        "copper":
            m = _standard(Color(0.5, 0.36, 0.25), 0.45, 0.35)
        "solar":
            m = _standard(Color(0.1, 0.13, 0.22), 0.35, 0.2)
        "glass":  # greenhouse panes
            m = _standard(Color(0.35, 0.45, 0.42), 0.25, 0.1)
        "window":
            m = _standard(Color(0.05, 0.06, 0.08), 0.3)
            m.emission_enabled = true
            m.emission = Color(1.0, 0.86, 0.62)
            m.emission_energy_multiplier = 0.6
        "cargo_body":  # container walls and frames
            m = _standard(Color(0.42, 0.42, 0.41), 0.8)
        "cargo_mark":  # doors, stripes and tank shells, tinted per container (MultiMesh colour)
            m = _standard(Color.WHITE, 0.7)
            m.vertex_color_use_as_albedo = true
        "cargo_ore":
            m = _standard(Color(0.24, 0.22, 0.2), 0.95)
        _:
            m = _standard(Color(0.6, 0.6, 0.6), 0.7)
    _shared[slot] = m
    return m

# The faction colour as paint: saturation and brightness capped so it reads as a marking,
# not a light.
static func faction_paint(color: Color) -> Color:
    return Color.from_hsv(color.h, minf(color.s, 0.55), minf(color.v, 0.72))

static func per_instance(slot: String) -> Material:
    match slot:
        "band":
            return _standard(Color(0.5, 0.5, 0.5), 0.7)
        "radiator":
            var m := ShaderMaterial.new()
            m.shader = _radiator()
            return m
        "nozzle_hot":
            var m := _standard(Color(0.08, 0.08, 0.09), 0.5, 0.2)
            m.emission_enabled = true
            m.emission = Color.BLACK
            m.emission_energy_multiplier = 0.0
            return m
    return shared(slot)

# The container's own band, radiator and nozzle materials (made once, kept across LOD
# and model changes).
static func instance_materials(container: Node3D) -> Dictionary:
    if container.has_meta("materials"):
        return container.get_meta("materials")
    var own := {}
    for slot in PER_INSTANCE:
        own[slot] = per_instance(slot)
    container.set_meta("materials", own)
    return own

# Puts a MeshBuilder result on a MeshInstance3D: shared materials per slot, the
# container's own where it has one; on the layers the fill light reaches.
static func apply_mesh(instance: MeshInstance3D, built: Dictionary, container: Node3D) -> void:
    instance.mesh = built["mesh"]
    instance.layers = MODEL_LAYERS
    var own := instance_materials(container)
    var slots: Array = built["slots"]
    for i in range(slots.size()):
        var slot := String(slots[i])
        instance.set_surface_override_material(i, own[slot] if own.has(slot) else shared(slot))

static func set_band(material: Material, color: Color) -> void:
    if material is StandardMaterial3D:
        (material as StandardMaterial3D).albedo_color = faction_paint(color)

# Radiator panels: grey with a silver root manifold and heat-pipe ribs. While the drive
# runs, the pipes and manifold glow a faint dull orange (594 K panels would not glow to
# the eye; this is a hint, kept below the bloom threshold). In the thermal view the panels
# show the radiator temperature in false colour while running and the scale's cold end
# otherwise. lost: the share of each panel lost to meteoroid wear (tier 2), dark at the tips.
static func set_radiator(material: Material, running: bool, temperature_k: float, lost: float) -> void:
    var m := material as ShaderMaterial
    if m == null:
        return
    var hot := running and temperature_k > 0.0
    m.set_shader_parameter("running", 1.0 if hot else 0.0)
    m.set_shader_parameter("thermal", 1.0 if thermal_view else 0.0)
    m.set_shader_parameter("thermal_color", Thermal.color(temperature_k) if hot else Thermal.at(0.15))
    m.set_shader_parameter("lost_fraction", clampf(lost, 0.0, 1.0))

static func set_thermal_view(on: bool) -> void:
    thermal_view = on

static func set_nozzle(material: Material, color: Color, energy: float) -> void:
    var m := material as StandardMaterial3D
    if m == null:
        return
    m.emission = color
    m.emission_energy_multiplier = energy

static func _radiator() -> Shader:
    if _radiator_shader != null:
        return _radiator_shader
    _radiator_shader = Shader.new()
    # UV.x runs from the panel root (0) to the tip (1), UV.y along the ship.
    _radiator_shader.code = """
shader_type spatial;
render_mode cull_disabled;
uniform vec3 base_color : source_color = vec3(0.12, 0.125, 0.13);
uniform vec3 pipe_glow : source_color = vec3(1.0, 0.35, 0.1);
uniform vec3 thermal_color : source_color = vec3(0.0);
uniform float running = 0.0;
uniform float thermal = 0.0;
uniform float lost_fraction = 0.0;
uniform float panels = 6.0;
uniform float pipes = 10.0;

float grid_line(float x, float count, float width_px) {
    float f = fract(x * count);
    float d = min(f, 1.0 - f) / count;
    float w = max(fwidth(x), 1e-5);
    return 1.0 - smoothstep(width_px * 0.5 * w, width_px * 1.5 * w, d);
}

void fragment() {
    float lost = step(1.0 - lost_fraction, UV.x) * step(0.001, lost_fraction);
    float seam = grid_line(UV.x, panels, 1.0);
    float pipe = grid_line(UV.y, pipes, 1.0);
    float manifold = 1.0 - smoothstep(0.03, 0.045, UV.x);
    vec3 albedo = base_color;
    albedo = mix(albedo, vec3(0.06, 0.06, 0.065), seam * 0.8);
    albedo = mix(albedo, vec3(0.2, 0.2, 0.21), pipe * 0.5);
    albedo = mix(albedo, vec3(0.45, 0.47, 0.5), manifold);
    vec3 emission = pipe_glow * running * (0.1 * pipe + 0.35 * manifold) * (1.0 - lost);
    if (thermal > 0.5) {
        albedo = mix(thermal_color * 0.2, vec3(0.06, 0.06, 0.07), seam * 0.7);
        emission = thermal_color * 0.3 * running * (1.0 - 0.5 * seam);
    }
    ALBEDO = mix(albedo, vec3(0.04, 0.04, 0.05), lost);
    ROUGHNESS = 0.55;
    METALLIC = 0.2;
    EMISSION = emission * (1.0 - lost);
}
"""
    return _radiator_shader

# Engine plume: additive, unshaded, fading along its length (UV.y) and towards its edges.
static func plume_material(color: Color) -> ShaderMaterial:
    if _plume_shader == null:
        _plume_shader = Shader.new()
        _plume_shader.code = """
shader_type spatial;
render_mode unshaded, blend_add, cull_disabled, depth_draw_never, shadows_disabled;
uniform vec3 color : source_color = vec3(1.0, 0.85, 0.6);
uniform float intensity = 1.0;

void fragment() {
    float along = clamp(UV.y, 0.0, 1.0);
    float edge = pow(abs(dot(normalize(NORMAL), normalize(VIEW))), 1.5);
    float fade = pow(1.0 - along, 2.2) * smoothstep(0.0, 0.04, along);
    ALBEDO = color * intensity * 0.45 * fade * edge;
}
"""
    var m := ShaderMaterial.new()
    m.shader = _plume_shader
    m.set_shader_parameter("color", color)
    return m
