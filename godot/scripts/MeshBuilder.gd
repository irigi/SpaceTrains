extends RefCounted

# Builds one ArrayMesh from many primitives, with one surface per material slot, so a
# whole ship or station is a single MeshInstance3D with a handful of draw calls.
# Usage: var b := MeshBuilder.new(); b.lathe("tank", profile, 16, xf); ...; b.commit()
# returns {mesh, slots (slot name per surface index), aabb}.
#
# Winding: Godot treats clockwise triangles as front faces. Every triangle here is given
# the outward normal it should have and is reordered to match, so callers never think
# about winding.

var _tools := {}
var _order: Array[String] = []
var _aabb := AABB()
var _has_point := false

func _tool(slot: String) -> SurfaceTool:
    if not _tools.has(slot):
        var st := SurfaceTool.new()
        st.begin(Mesh.PRIMITIVE_TRIANGLES)
        _tools[slot] = st
        _order.append(slot)
    return _tools[slot]

func _grow(p: Vector3) -> void:
    if _has_point:
        _aabb = _aabb.expand(p)
    else:
        _aabb = AABB(p, Vector3.ZERO)
        _has_point = true

func tri(slot: String, a: Vector3, b: Vector3, c: Vector3, na: Vector3, nb: Vector3, nc: Vector3,
        ua := Vector2.ZERO, ub := Vector2.ZERO, uc := Vector2.ZERO) -> void:
    if (b - a).cross(c - a).dot(na + nb + nc) > 0.0:
        var p := b; b = c; c = p
        var n := nb; nb = nc; nc = n
        var u := ub; ub = uc; uc = u
    var st := _tool(slot)
    st.set_normal(na); st.set_uv(ua); st.add_vertex(a)
    st.set_normal(nb); st.set_uv(ub); st.add_vertex(b)
    st.set_normal(nc); st.set_uv(uc); st.add_vertex(c)
    _grow(a); _grow(b); _grow(c)

func quad(slot: String, a: Vector3, b: Vector3, c: Vector3, d: Vector3, n: Vector3,
        ua := Vector2(0, 0), ub := Vector2(1, 0), uc := Vector2(1, 1), ud := Vector2(0, 1)) -> void:
    tri(slot, a, b, c, n, n, n, ua, ub, uc)
    tri(slot, a, c, d, n, n, n, ua, uc, ud)

# A surface of revolution about the local Z axis. profile: Vector2(z, r) points ordered
# from the front tip to the back tip (r = 0 at a tip closes it). Normals are smooth around
# the axis and along the profile except at corners sharper than 50°. inward flips them
# (the inside of a nozzle).
func lathe(slot: String, profile: PackedVector2Array, segments: int, xf := Transform3D.IDENTITY, inward := false) -> void:
    var count := profile.size()
    if count < 2:
        return
    # Normal of each profile segment in (z, r): the tangent turned +90°.
    var seg_normals: Array[Vector2] = []
    var lengths: Array[float] = [0.0]
    for i in range(count - 1):
        var t := profile[i + 1] - profile[i]
        seg_normals.append(Vector2(-t.y, t.x).normalized() if t.length() > 1.0e-9 else Vector2.ZERO)
        lengths.append(lengths[i] + t.length())
    var total := maxf(lengths[count - 1], 1.0e-9)
    var flip := -1.0 if inward else 1.0
    for i in range(count - 1):
        var n_seg := seg_normals[i]
        if n_seg == Vector2.ZERO:
            continue
        var n0 := n_seg
        var n1 := n_seg
        if i > 0 and seg_normals[i - 1] != Vector2.ZERO and seg_normals[i - 1].dot(n_seg) > 0.64:
            n0 = (seg_normals[i - 1] + n_seg).normalized()
        if i < count - 2 and seg_normals[i + 1] != Vector2.ZERO and seg_normals[i + 1].dot(n_seg) > 0.64:
            n1 = (seg_normals[i + 1] + n_seg).normalized()
        var p0 := profile[i]
        var p1 := profile[i + 1]
        var v0 := lengths[i] / total
        var v1 := lengths[i + 1] / total
        for s in range(segments):
            var a0 := TAU * float(s) / float(segments)
            var a1 := TAU * float(s + 1) / float(segments)
            var c0 := Vector2(cos(a0), sin(a0))
            var c1 := Vector2(cos(a1), sin(a1))
            var q00 := xf * Vector3(p0.y * c0.x, p0.y * c0.y, p0.x)
            var q01 := xf * Vector3(p0.y * c1.x, p0.y * c1.y, p0.x)
            var q10 := xf * Vector3(p1.y * c0.x, p1.y * c0.y, p1.x)
            var q11 := xf * Vector3(p1.y * c1.x, p1.y * c1.y, p1.x)
            var m00 := (xf.basis * Vector3(n0.y * c0.x, n0.y * c0.y, n0.x)).normalized() * flip
            var m01 := (xf.basis * Vector3(n0.y * c1.x, n0.y * c1.y, n0.x)).normalized() * flip
            var m10 := (xf.basis * Vector3(n1.y * c0.x, n1.y * c0.y, n1.x)).normalized() * flip
            var m11 := (xf.basis * Vector3(n1.y * c1.x, n1.y * c1.y, n1.x)).normalized() * flip
            var u0 := float(s) / float(segments)
            var u1 := float(s + 1) / float(segments)
            if p0.y > 1.0e-9:
                tri(slot, q00, q01, q11, m00, m01, m11, Vector2(u0, v0), Vector2(u1, v0), Vector2(u1, v1))
            if p1.y > 1.0e-9:
                tri(slot, q00, q11, q10, m00, m11, m10, Vector2(u0, v0), Vector2(u1, v1), Vector2(u0, v1))

# A cylinder along local Z from z0 to z1, closed at both ends unless open.
func cylinder(slot: String, z0: float, z1: float, radius: float, segments: int, xf := Transform3D.IDENTITY, open := false) -> void:
    if open:
        lathe(slot, PackedVector2Array([Vector2(z0, radius), Vector2(z1, radius)]), segments, xf)
    else:
        lathe(slot, PackedVector2Array([Vector2(z0, 0.0), Vector2(z0, radius), Vector2(z1, radius), Vector2(z1, 0.0)]), segments, xf)

# A capsule-like vessel along Z: a cylinder with elliptical end domes of the given depth.
func vessel(slot: String, z0: float, z1: float, radius: float, dome: float, segments: int, xf := Transform3D.IDENTITY) -> void:
    dome = minf(dome, (z1 - z0) * 0.5)
    var profile := PackedVector2Array()
    var steps := 5
    for i in range(steps + 1):
        var a := PI * 0.5 * float(i) / float(steps)
        profile.append(Vector2(z0 + dome * (1.0 - cos(a)), radius * sin(a) if i < steps else radius))
    for i in range(steps + 1):
        var a := PI * 0.5 * float(i) / float(steps)
        profile.append(Vector2(z1 - dome * (1.0 - sin(a)), radius * cos(a) if i > 0 else radius))
    lathe(slot, profile, segments, xf)

# A box of the given size centred on xf's origin. UVs: u across local X (flip_u reverses
# it), v along local Z, both 0..1, on every face.
func box(slot: String, size: Vector3, xf := Transform3D.IDENTITY, flip_u := false) -> void:
    var h := size * 0.5
    var uv := func(p: Vector3) -> Vector2:
        var u := p.x / maxf(size.x, 1.0e-9) + 0.5
        return Vector2(1.0 - u if flip_u else u, p.z / maxf(size.z, 1.0e-9) + 0.5)
    for axis in range(3):
        for sign in [-1.0, 1.0]:
            var n := Vector3.ZERO
            n[axis] = sign
            var u_axis := (axis + 1) % 3
            var v_axis := (axis + 2) % 3
            var corners: Array[Vector3] = []
            for k in [[-1, -1], [1, -1], [1, 1], [-1, 1]]:
                var p := Vector3.ZERO
                p[axis] = sign * h[axis]
                p[u_axis] = float(k[0]) * h[u_axis]
                p[v_axis] = float(k[1]) * h[v_axis]
                corners.append(p)
            var wn := (xf.basis * n).normalized()
            quad(slot, xf * corners[0], xf * corners[1], xf * corners[2], xf * corners[3], wn,
                uv.call(corners[0]), uv.call(corners[1]), uv.call(corners[2]), uv.call(corners[3]))

# A square beam of the given thickness from a to b.
func beam(slot: String, a: Vector3, b: Vector3, thickness: float) -> void:
    var along := b - a
    var length := along.length()
    if length < 1.0e-6:
        return
    var dir := along / length
    var up := Vector3.UP if absf(dir.dot(Vector3.UP)) < 0.95 else Vector3.RIGHT
    var basis := Basis.looking_at(dir, up)
    box(slot, Vector3(thickness, thickness, length), Transform3D(basis, (a + b) * 0.5))

# A torus around the local Z axis at z, major radius big, tube radius small.
func torus(slot: String, z: float, big: float, small: float, xf := Transform3D.IDENTITY, segments := 24, sides := 6) -> void:
    for s in range(segments):
        var a0 := TAU * float(s) / float(segments)
        var a1 := TAU * float(s + 1) / float(segments)
        for k in range(sides):
            var b0 := TAU * float(k) / float(sides)
            var b1 := TAU * float(k + 1) / float(sides)
            var pts: Array[Vector3] = []
            var nrm: Array[Vector3] = []
            for pair in [[a0, b0], [a1, b0], [a1, b1], [a0, b1]]:
                var radial := Vector3(cos(pair[0]), sin(pair[0]), 0.0)
                var n := radial * cos(pair[1]) + Vector3(0.0, 0.0, sin(pair[1]))
                pts.append(xf * (radial * big + n * small + Vector3(0.0, 0.0, z)))
                nrm.append((xf.basis * n).normalized())
            tri(slot, pts[0], pts[1], pts[2], nrm[0], nrm[1], nrm[2])
            tri(slot, pts[0], pts[2], pts[3], nrm[0], nrm[2], nrm[3])

# The surfaces as plain arrays (safe on a worker thread: no engine resources are made).
func commit_arrays() -> Dictionary:
    var surfaces: Array = []
    var slots: Array[String] = []
    for slot in _order:
        var st: SurfaceTool = _tools[slot]
        surfaces.append(st.commit_to_arrays())
        slots.append(slot)
    return {"surfaces": surfaces, "slots": slots, "aabb": _aabb}

# Turns commit_arrays() output into {mesh, slots, aabb} (main thread).
static func to_mesh(arrays: Dictionary) -> Dictionary:
    var mesh := ArrayMesh.new()
    for surface in arrays["surfaces"]:
        mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, surface)
    return {"mesh": mesh, "slots": arrays["slots"], "aabb": arrays["aabb"]}

func commit() -> Dictionary:
    return to_mesh(commit_arrays())
