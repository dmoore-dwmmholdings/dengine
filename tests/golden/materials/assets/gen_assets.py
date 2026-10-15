#!/usr/bin/env python3
"""Generates the materials gallery's assets (DENG-307): procedural meshes (glTF 2.0 with the
buffer embedded), tiny PNG textures, small uncompressed OpenEXR environments, the `.mat.toml`
materials, the `.scene.toml` scenes and assets.toml.

Everything here is procedural and written by this script, so no third-party file is committed
(LICENSES.md). The output is deterministic: the same script writes the same bytes. Run it from
anywhere with the Python 3 standard library only:

    python3 tests/golden/materials/assets/gen_assets.py

and commit what changed. The golden references depend on these files: a change re-bakes them
(docs/testing/golden.md).
"""

import base64
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def out(rel, data, mode="wb"):
    path = os.path.join(HERE, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, mode) as f:
        f.write(data)


# ================================================================================================
# glTF meshes
# ================================================================================================


def gltf(name, positions, normals, tangents, uvs, indices):
    """One mesh `name`, one node of the same name, one primitive without a material."""
    n = len(positions)
    blob = bytearray()

    def floats(rows):
        for r in rows:
            blob.extend(struct.pack("<%df" % len(r), *r))

    floats(positions)
    off_n = len(blob)
    floats(normals)
    off_t = len(blob)
    floats(tangents)
    off_uv = len(blob)
    floats(uvs)
    off_i = len(blob)
    blob.extend(struct.pack("<%dH" % len(indices), *indices))
    while len(blob) % 4:
        blob.append(0)
    lo = [min(p[k] for p in positions) for k in range(3)]
    hi = [max(p[k] for p in positions) for k in range(3)]
    views = [
        {"buffer": 0, "byteOffset": 0, "byteLength": off_n},
        {"buffer": 0, "byteOffset": off_n, "byteLength": off_t - off_n},
        {"buffer": 0, "byteOffset": off_t, "byteLength": off_uv - off_t},
        {"buffer": 0, "byteOffset": off_uv, "byteLength": off_i - off_uv},
        {"buffer": 0, "byteOffset": off_i, "byteLength": len(indices) * 2},
    ]
    acc = [
        {"bufferView": 0, "componentType": 5126, "count": n, "type": "VEC3",
         "min": [round(v, 6) for v in lo], "max": [round(v, 6) for v in hi]},
        {"bufferView": 1, "componentType": 5126, "count": n, "type": "VEC3"},
        {"bufferView": 2, "componentType": 5126, "count": n, "type": "VEC4"},
        {"bufferView": 3, "componentType": 5126, "count": n, "type": "VEC2"},
        {"bufferView": 4, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
    ]
    doc = {
        "asset": {"version": "2.0", "generator": "dengine gen_assets.py (DENG-307)"},
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": name, "mesh": 0}],
        "meshes": [{"name": name, "primitives": [{
            "attributes": {"POSITION": 0, "NORMAL": 1, "TANGENT": 2, "TEXCOORD_0": 3},
            "indices": 4}]}],
        "accessors": acc,
        "bufferViews": views,
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64,"
                     + base64.b64encode(bytes(blob)).decode("ascii")}],
    }
    return (json.dumps(doc, indent=1, sort_keys=True) + "\n").encode("ascii")


def r6(v):
    return [round(x, 6) + 0.0 for x in v]


def mesh_sphere(seg=32, rings=16, r=0.5):
    """A UV sphere of radius `r` (glTF axes: +Y up; the importer converts to the engine's Z up)."""
    P, N, T, UV, I = [], [], [], [], []
    for j in range(rings + 1):
        v = j / rings
        th = v * math.pi
        for i in range(seg + 1):
            u = i / seg
            ph = u * 2.0 * math.pi
            n = (math.sin(th) * math.cos(ph), math.cos(th), -math.sin(th) * math.sin(ph))
            P.append(r6([r * c for c in n]))
            N.append(r6(n))
            T.append(r6([-math.sin(ph), 0.0, -math.cos(ph), 1.0]))
            UV.append(r6([u, v]))
    for j in range(rings):
        for i in range(seg):
            a = j * (seg + 1) + i
            b = a + seg + 1
            I += [a, b, a + 1, a + 1, b, b + 1]
    return gltf("sphere", P, N, T, UV, I)


def mesh_card(n=2):
    """A unit square in glTF's XY plane facing +Z (the engine's -Y after the axis conversion:
    towards the gallery camera), subdivided `n` x `n`, UV over [0, 1]."""
    P, N, T, UV, I = [], [], [], [], []
    for j in range(n + 1):
        for i in range(n + 1):
            u, v = i / n, j / n
            P.append(r6([u - 0.5, 0.5 - v, 0.0]))
            N.append([0.0, 0.0, 1.0])
            T.append([1.0, 0.0, 0.0, 1.0])
            UV.append(r6([u, v]))
    for j in range(n):
        for i in range(n):
            a = j * (n + 1) + i
            b = a + n + 1
            I += [a, b, a + 1, a + 1, b, b + 1]
    return gltf("card", P, N, T, UV, I)


def mesh_box(h=(0.5, 0.5, 0.5)):
    """A box of half extents `h` (glTF axes), flat faces."""
    P, N, T, UV, I = [], [], [], [], []
    faces = [((1, 0, 0), (0, 0, -1)), ((-1, 0, 0), (0, 0, 1)), ((0, 1, 0), (1, 0, 0)),
             ((0, -1, 0), (1, 0, 0)), ((0, 0, 1), (1, 0, 0)), ((0, 0, -1), (-1, 0, 0))]
    for n, t in faces:
        b = [n[1] * t[2] - n[2] * t[1], n[2] * t[0] - n[0] * t[2], n[0] * t[1] - n[1] * t[0]]
        base = len(P)
        for (su, sv) in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [(n[k] + t[k] * su + b[k] * sv) * h[k] for k in range(3)]
            P.append(r6(p))
            N.append([float(c) for c in n])
            T.append([float(t[0]), float(t[1]), float(t[2]), 1.0])
            UV.append([(su + 1) * 0.5, (1 - sv) * 0.5])
        I += [base, base + 1, base + 2, base, base + 2, base + 3]
    return gltf("box", P, N, T, UV, I)


def mesh_cylinder(seg=32, r=0.5, hh=0.5):
    """An open cylinder (a lamp shade) about glTF +Y, radius `r`, half height `hh`, both faces
    drawn by a double-sided material."""
    P, N, T, UV, I = [], [], [], [], []
    for j in range(2):
        y = hh if j == 0 else -hh
        for i in range(seg + 1):
            ph = i / seg * 2.0 * math.pi
            c, s = math.cos(ph), -math.sin(ph)
            P.append(r6([r * c, y, r * s]))
            N.append(r6([c, 0.0, s]))
            T.append(r6([-math.sin(ph), 0.0, -math.cos(ph), 1.0]))
            UV.append(r6([i / seg, float(j)]))
    for i in range(seg):
        a, b = i, i + seg + 1
        I += [a, b, a + 1, a + 1, b, b + 1]
    return gltf("cylinder", P, N, T, UV, I)


# ================================================================================================
# PNG and EXR
# ================================================================================================


def png(w, h, rows, channels):
    """An 8-bit PNG (channels 3: RGB, 4: RGBA); `rows` is a list of byte strings."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ct = 2 if channels == 3 else 6
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, ct, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def half(f):
    return struct.pack("<e", f)


def exr(w, h, pixel):
    """An uncompressed scanline OpenEXR of HALF R, G, B: `pixel(x, y)` gives (r, g, b)."""
    def attr(name, typ, data):
        return name.encode() + b"\0" + typ.encode() + b"\0" + struct.pack("<i", len(data)) + data

    ch = b""
    for c in "BGR":
        ch += c.encode() + b"\0" + struct.pack("<iB3xii", 1, 0, 1, 1)
    ch += b"\0"
    hdr = b"\x76\x2f\x31\x01" + struct.pack("<i", 2)
    hdr += attr("channels", "chlist", ch)
    hdr += attr("compression", "compression", b"\0")
    hdr += attr("dataWindow", "box2i", struct.pack("<iiii", 0, 0, w - 1, h - 1))
    hdr += attr("displayWindow", "box2i", struct.pack("<iiii", 0, 0, w - 1, h - 1))
    hdr += attr("lineOrder", "lineOrder", b"\0")
    hdr += attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    hdr += attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    hdr += attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    hdr += b"\0"
    line = 8 + w * 3 * 2
    table_end = len(hdr) + 8 * h
    body = b""
    table = b""
    for y in range(h):
        table += struct.pack("<Q", table_end + y * line)
        px = [pixel(x, y) for x in range(w)]
        data = b"".join(half(p[2]) for p in px) + b"".join(half(p[1]) for p in px) + \
            b"".join(half(p[0]) for p in px)
        body += struct.pack("<ii", y, len(data)) + data
    return hdr + table + body


def tex_checker():
    """128 x 128: two greys in 16 x 16 texel cells (the backdrop and the floor): large enough that
    the magnified edges stay sharp, which keeps the reference images small."""
    rows = []
    for y in range(128):
        r = bytearray()
        for x in range(128):
            v = 200 if ((x // 16) + (y // 16)) % 2 == 0 else 70
            r += bytes((v, v, v))
        rows.append(r)
    return png(128, 128, rows, 3)


def tex_cutout():
    """32 x 32 RGBA: white, alpha 255 / 0 in 8 x 8 cells (the alpha-mask card)."""
    rows = []
    for y in range(32):
        r = bytearray()
        for x in range(32):
            a = 255 if ((x // 8) + (y // 8)) % 2 == 0 else 0
            r += bytes((255, 255, 255, a))
        rows.append(r)
    return png(32, 32, rows, 4)


def tex_leaf():
    """64 x 64 RGBA: a leaf (an ellipse with a vein), alpha 0 outside (diffuse-transmission card)."""
    rows = []
    for y in range(64):
        r = bytearray()
        for x in range(64):
            u, v = (x + 0.5) / 32.0 - 1.0, (y + 0.5) / 32.0 - 1.0
            inside = (u / 0.55) ** 2 + (v / 0.95) ** 2 <= 1.0
            vein = abs(u) < 0.04 and inside
            g = 140 if not vein else 200
            r += bytes((90 if not vein else 170, g, 40, 255 if inside else 0))
        rows.append(r)
    return png(64, 64, rows, 4)


def tex_bumps():
    """64 x 64 tangent-space normal map: a grid of round bumps (coat normal)."""
    rows = []
    for y in range(64):
        r = bytearray()
        for x in range(64):
            u = ((x + 0.5) % 16) / 8.0 - 1.0
            v = ((y + 0.5) % 16) / 8.0 - 1.0
            d = u * u + v * v
            if d < 0.8:
                nx, ny = u * 0.6, -v * 0.6
            else:
                nx, ny = 0.0, 0.0
            nz = math.sqrt(max(0.0, 1.0 - nx * nx - ny * ny))
            r += bytes((int(round((nx * 0.5 + 0.5) * 255)), int(round((ny * 0.5 + 0.5) * 255)),
                        int(round((nz * 0.5 + 0.5) * 255))))
        rows.append(r)
    return png(64, 64, rows, 3)


def env_sky():
    """64 x 32 equirectangular sky (engine +Z up): a blue zenith to a pale horizon, a brown
    ground, and a small bright sun at azimuth 40 degrees, elevation 35 degrees."""
    sun_az, sun_el = math.radians(40.0), math.radians(35.0)
    sd = (math.cos(sun_el) * math.cos(sun_az), math.cos(sun_el) * math.sin(sun_az),
          math.sin(sun_el))

    def pixel(x, y):
        az = (x + 0.5) / 64.0 * 2.0 * math.pi - math.pi
        el = math.pi * 0.5 - (y + 0.5) / 32.0 * math.pi
        d = (math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el))
        if el >= 0.0:
            t = el / (math.pi * 0.5)
            c = (0.9 - 0.6 * t, 1.0 - 0.4 * t, 1.2 - 0.2 * t)
        else:
            c = (0.25, 0.18, 0.12)
        cs = sum(d[k] * sd[k] for k in range(3))
        if cs > math.cos(math.radians(7.0)):
            c = (40.0, 36.0, 30.0)
        return c

    return exr(64, 32, pixel)


def env_uniform():
    """8 x 4: radiance 1 everywhere (the furnace)."""
    return exr(8, 4, lambda x, y: (1.0, 1.0, 1.0))


# ================================================================================================
# Materials
# ================================================================================================

MATS = {}  # id -> ordered list of (key, value)


def mat(name, **kv):
    """A material; glTF's defaults (de_cooked_material_params_default) apply to missing keys, so
    `metallic` (glTF default 1) is always written: 0 unless given."""
    kv.setdefault("metallic", 0.0)
    MATS[name] = kv
    return "mat/" + name


def toml_value(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, str):
        return '"%s"' % v
    if isinstance(v, (list, tuple)):
        return "[" + ", ".join(toml_value(x) for x in v) + "]"
    if isinstance(v, int):
        return "%d" % v
    return repr(float(round(v, 6)))


def mat_file(name, kv):
    lines = ["# %s (DENG-307 materials gallery; written by gen_assets.py)" % name]
    for k in sorted(kv):
        lines.append("%s = %s" % (k, toml_value(kv[k])))
    return ("\n".join(lines) + "\n").encode("ascii")


GREY = [0.8, 0.8, 0.8]
RED = [0.8, 0.12, 0.08]
GOLD = [1.0, 0.78, 0.34]


def gallery_materials():
    """The gallery rows: (row title, [material ids]); 12 per row."""
    rows = []
    r = []
    for i, ro in enumerate([0.0, 0.2, 0.4, 0.6, 0.8, 1.0]):
        r.append(mat("diel_r%d" % i, base_color=RED, roughness=ro))
    for i, ro in enumerate([0.0, 0.2, 0.4, 0.6, 0.8, 1.0]):
        r.append(mat("metal_r%d" % i, base_color=GOLD, metallic=1.0, roughness=ro))
    rows.append(("metallic x roughness", r))
    r = []
    for i, ior in enumerate([1.0, 1.33, 1.5, 1.8, 2.4]):
        r.append(mat("ior_%d" % i, base_color=[0.05, 0.05, 0.05], roughness=0.15, ior=ior))
    r.append(mat("spec_0", base_color=[0.05, 0.05, 0.05], roughness=0.15, specular=0.0))
    for i, s in enumerate([0.25, 0.5, 1.0, 2.0, 4.0, 8.0]):
        r.append(mat("emit_%d" % i, base_color=[0.1, 0.1, 0.1], roughness=0.6,
                     emissive=[1.0, 0.45, 0.1], emissive_strength=s))
    rows.append(("IOR and specular | emissive strength", r))
    r = [
        mat("coat_r0", base_color=RED, roughness=0.6, clearcoat=1.0, clearcoat_roughness=0.0),
        mat("coat_r1", base_color=RED, roughness=0.6, clearcoat=1.0, clearcoat_roughness=0.25),
        mat("coat_r2", base_color=RED, roughness=0.6, clearcoat=1.0, clearcoat_roughness=0.5),
        mat("coat_half", base_color=RED, roughness=0.6, clearcoat=0.5, clearcoat_roughness=0.0),
        mat("coat_normal", base_color=RED, roughness=0.6, clearcoat=1.0,
            clearcoat_roughness=0.05, clearcoat_normal_tex="tex/bumps"),
        mat("coat_metal", base_color=GOLD, metallic=1.0, roughness=0.5, clearcoat=1.0,
            clearcoat_roughness=0.0),
    ]
    for i, (c, ro) in enumerate([([1.0, 0.1, 0.1], 0.3), ([1.0, 0.1, 0.1], 0.8),
                                 ([0.1, 0.3, 1.0], 0.3), ([0.1, 0.3, 1.0], 0.8),
                                 ([1.0, 1.0, 1.0], 0.5), ([1.0, 0.8, 0.2], 0.5)]):
        r.append(mat("sheen_%d" % i, base_color=[0.08, 0.08, 0.08], roughness=0.9,
                     sheen_color=c, sheen_roughness=ro))
    rows.append(("clearcoat | sheen", r))
    r = []
    for i, (a, rot) in enumerate([(0.0, 0.0), (0.4, 0.0), (0.8, 0.0), (1.0, 0.0), (0.8, 0.785398),
                                  (0.8, 1.570796)]):
        r.append(mat("aniso_%d" % i, base_color=[0.95, 0.95, 0.95], metallic=1.0, roughness=0.4,
                     anisotropy=a, anisotropy_rotation=rot))
    for i, (f, lo, hi, ior) in enumerate([(1.0, 100.0, 200.0, 1.3), (1.0, 200.0, 400.0, 1.3),
                                          (1.0, 400.0, 800.0, 1.3), (1.0, 250.0, 500.0, 1.8),
                                          (0.5, 250.0, 500.0, 1.3), (1.0, 600.0, 1200.0, 1.3)]):
        r.append(mat("irid_%d" % i, base_color=[0.02, 0.02, 0.02], metallic=1.0, roughness=0.2,
                     iridescence=f, iridescence_ior=ior, iridescence_thickness_min=lo,
                     iridescence_thickness_max=hi))
    rows.append(("anisotropy | iridescence", r))
    r = [
        mat("trans_thin", base_color=[1.0, 1.0, 1.0], roughness=0.0, transmission=1.0),
        mat("trans_rough", base_color=[1.0, 1.0, 1.0], roughness=0.3, transmission=1.0),
        mat("trans_vol_red", base_color=[1.0, 1.0, 1.0], roughness=0.0, transmission=1.0,
            thickness=1.0, attenuation_color=[0.9, 0.2, 0.15], attenuation_distance=0.5),
        mat("trans_vol_blue", base_color=[1.0, 1.0, 1.0], roughness=0.05, transmission=1.0,
            thickness=1.0, attenuation_color=[0.2, 0.45, 0.95], attenuation_distance=1.0),
        mat("leaf", base_color=[1.0, 1.0, 1.0], base_color_tex="tex/leaf", roughness=0.7,
            alpha_mode="mask", alpha_cutoff=0.5, double_sided=True, diffuse_transmission=0.6,
            diffuse_transmission_color=[0.6, 1.0, 0.2]),
        mat("skin", base_color=[0.85, 0.6, 0.5], roughness=0.5, subsurface=1.0,
            subsurface_radius=[1.0, 0.35, 0.2]),
        mat("jade", base_color=[0.3, 0.75, 0.45], roughness=0.3, subsurface=1.0,
            subsurface_radius=[0.3, 0.8, 0.4]),
        mat("unlit", base_color=[0.1, 0.8, 0.9], unlit=True),
        mat("mask", base_color=[1.0, 0.85, 0.2], base_color_tex="tex/cutout", roughness=0.5,
            alpha_mode="mask", alpha_cutoff=0.5, double_sided=True),
        mat("blend_30", base_color=[0.2, 0.5, 1.0, 0.3], roughness=0.3, alpha_mode="blend"),
        mat("blend_70", base_color=[0.2, 0.5, 1.0, 0.7], roughness=0.3, alpha_mode="blend"),
        mat("blend_red", base_color=[1.0, 0.15, 0.1, 0.5], roughness=0.1, alpha_mode="blend",
            double_sided=True),
    ]
    rows.append(("transmission, diffuse transmission, subsurface, unlit, alpha", r))
    return rows


# Shapes: which mesh each gallery material is drawn on (default: the sphere).
CARDS = {"mat/leaf", "mat/mask", "mat/blend_30", "mat/blend_70", "mat/blend_red"}


# ================================================================================================
# Scenes
# ================================================================================================


class Scene:
    def __init__(self, title):
        self.title = title
        self.lines = []
        self.next_id = 0x10

    def entity(self, comps, eid=None):
        if eid is None:
            eid = self.next_id
            self.next_id += 1
        self.lines.append("")
        self.lines.append("[[entity]]")
        self.lines.append('id = "0x%016x"' % eid)
        for name, fields in comps:
            self.lines.append("[entity.%s]" % name)
            for k, v in fields:
                self.lines.append("%s = %s" % (k, toml_value(v)))

    def text(self):
        return ("# %s (DENG-307; written by gen_assets.py)\n" % self.title
                + "\n".join(self.lines) + "\n").encode("ascii")


def quat_axis(axis, angle):
    s = math.sin(angle * 0.5)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(angle * 0.5)]


def quat_mul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def xf(pos, rot=(0.0, 0.0, 0.0, 1.0), scale=(1.0, 1.0, 1.0)):
    return ("DeTransform", [("position", r6(pos)), ("rotation", r6(rot)), ("scale", r6(scale))])


def camera(sc, pos, pitch_deg, fov_deg, ev100):
    """The scene's camera, entity id 0x1 (the scene's first root: de_prefab_instantiate returns
    it). It looks along +Y, pitched down by `pitch_deg`. The engine's camera looks down its -Z
    with +Y up, so the rotation takes -Z to +Y first."""
    q = quat_axis((1.0, 0.0, 0.0), math.radians(90.0 - pitch_deg))
    sc.entity([xf(pos, q),
               ("DeCamera", [("projection", "DE_PROJ_PERSPECTIVE"), ("fov_y", math.radians(fov_deg)),
                             ("near_z", 0.1), ("exposure_ev100", ev100)])], eid=0x1)


def mesh_renderer(mesh, material):
    return ("DeMeshRenderer", [("mesh", mesh), ("materials", [material]), ("layer_mask", 1),
                               ("cast_shadows", False)])


def light(sc, kind, pos, rot, color, intensity, **extra):
    fields = [("kind", kind), ("color", color), ("intensity", intensity)]
    for k in ("range", "spot_inner", "spot_outer", "area_size", "source_radius"):
        if k in extra:
            fields.append((k, extra[k]))
    sc.entity([xf(pos, rot), ("DeLight", fields)])


def environment(sc, hdri, intensity):
    sc.entity([xf((0.0, 0.0, 0.0)),
               ("DeEnvironment", [("hdri", hdri), ("intensity", intensity), ("rotation", 0.0)])])


SPHERE = "mesh/sphere#sphere"
CARD = "mesh/card#card"
BOX = "mesh/box#box"
CYL = "mesh/cylinder#cylinder"

# Cards face the camera: the glTF card faces +Z, the engine's -Y after the axis conversion.
FACE = (0.0, 0.0, 0.0, 1.0)


def gallery_grid(sc, rows, spacing=1.15):
    cols = max(len(r) for _, r in rows)
    for j, (_, items) in enumerate(rows):
        z = (len(rows) - 1) * 0.5 * spacing - j * spacing
        for i, m in enumerate(items):
            x = (i - (cols - 1) * 0.5) * spacing
            if m in CARDS:
                sc.entity([xf((x, 0.0, z), FACE, (0.95, 0.95, 0.95)), mesh_renderer(CARD, m)])
            else:
                sc.entity([xf((x, 0.0, z)), mesh_renderer(SPHERE, m)])
    # The backdrop: a checker wall behind the grid (transmission and blending show it).
    w = cols * spacing + 1.0
    h = len(rows) * spacing + 1.0
    sc.entity([xf((0.0, 1.6, 0.0), FACE, (w, 1.0, h)), mesh_renderer(CARD, "mat/backdrop")])


def scene_gallery(rows, lights):
    sc = Scene("materials gallery, " + ("lights" if lights else "image-based lighting"))
    # 12 columns x 5 rows; the camera 9.6 m in front, level.
    camera(sc, (0.0, -24.0, 0.0), 0.0, 15.5, 0.0)
    gallery_grid(sc, rows)
    if not lights:
        environment(sc, "env/sky", 1.0)
        return sc
    # Physical units at EV100 0 (exposure 1 / 1.2, a dim scene, so that Unlit and emissive
    # materials, whose radiance is in nits, show beside the lit ones): sun in lux, point and spot
    # in lumens, rect and disk in nits. Lights point along -Z of their transform.
    down_front = quat_mul(quat_axis((0.0, 0.0, 1.0), math.radians(30.0)),
                          quat_axis((1.0, 0.0, 0.0), math.radians(50.0)))
    light(sc, "DE_LIGHT_DIRECTIONAL", (0.0, -5.0, 6.0), down_front, [1.0, 0.96, 0.9], 2.0)
    light(sc, "DE_LIGHT_POINT", (-4.5, -2.0, 2.0), FACE, [1.0, 0.55, 0.3], 300.0,
          source_radius=0.05)
    spot_rot = quat_axis((1.0, 0.0, 0.0), math.radians(75.0))
    light(sc, "DE_LIGHT_SPOT", (4.0, -3.0, 0.5), spot_rot, [0.4, 0.6, 1.0], 15.0,
          spot_inner=math.radians(15.0), spot_outer=math.radians(30.0))
    rect_rot = quat_axis((1.0, 0.0, 0.0), math.radians(70.0))
    light(sc, "DE_LIGHT_RECT", (-1.5, -2.5, 3.8), rect_rot, [1.0, 1.0, 1.0], 8.0,
          area_size=[2.0, 0.6])
    light(sc, "DE_LIGHT_DISK", (2.5, -2.5, -3.6), quat_axis((1.0, 0.0, 0.0), math.radians(100.0)),
          [1.0, 0.85, 0.7], 25.0, area_size=[0.8, 0.8])
    return sc


def scene_furnace():
    """White spheres in a uniform environment of radiance 1 at EV100 0 (exposure 1 / 1.2): every
    sphere must render 1 / 1.2 of the environment within 2 % (energy conservation)."""
    sc = Scene("furnace")
    camera(sc, (0.0, -6.0, 0.0), 0.0, 30.0, 0.0)
    items = [
        mat("furnace_diel_r0", base_color=[1.0, 1.0, 1.0], roughness=0.0),
        mat("furnace_diel_r5", base_color=[1.0, 1.0, 1.0], roughness=0.5),
        mat("furnace_diel_r1", base_color=[1.0, 1.0, 1.0], roughness=1.0),
        mat("furnace_metal_r2", base_color=[1.0, 1.0, 1.0], metallic=1.0, roughness=0.2),
        mat("furnace_metal_r5", base_color=[1.0, 1.0, 1.0], metallic=1.0, roughness=0.5),
        mat("furnace_metal_r1", base_color=[1.0, 1.0, 1.0], metallic=1.0, roughness=1.0),
    ]
    for k, m in enumerate(items):
        x = ((k % 3) - 1) * 1.2
        z = 0.6 - (k // 3) * 1.2
        sc.entity([xf((x, 0.0, z)), mesh_renderer(SPHERE, m)])
    environment(sc, "env/uniform", 1.0)
    return sc


def scene_khr():
    """Procedural stand-ins for five Khronos glTF sample models (LICENSES.md), lit by the sky and
    a sun: ClearCoatTest (coat x coat roughness over a base), SheenChair (a seat of two boxes in a
    sheen fabric), IridescenceLamp (iridescent spheres), TransmissionTest (transmissive spheres of
    rising roughness and thickness before a checker wall) and AnisotropyBarnLamp (a brushed metal
    shade)."""
    sc = Scene("Khronos sample stand-ins")
    camera(sc, (0.0, -9.5, 1.3), 4.0, 42.0, 0.0)
    environment(sc, "env/sky", 1.0)
    light(sc, "DE_LIGHT_DIRECTIONAL", (0.0, 0.0, 6.0),
          quat_mul(quat_axis((0.0, 0.0, 1.0), math.radians(-35.0)),
                   quat_axis((1.0, 0.0, 0.0), math.radians(40.0))), [1.0, 0.97, 0.92], 1.5)
    # ClearCoatTest: rows coat 0 / 0.5 / 1, columns coat roughness 0 / 0.3 / 0.7.
    for j, c in enumerate([0.0, 0.5, 1.0]):
        for i, cr in enumerate([0.0, 0.3, 0.7]):
            m = mat("khr_coat_%d%d" % (j, i), base_color=[0.1, 0.25, 0.8], roughness=0.7,
                    clearcoat=c, clearcoat_roughness=cr)
            sc.entity([xf((-4.6 + i * 0.7, 0.0, 2.6 - j * 0.7), scale=(0.6, 0.6, 0.6)),
                       mesh_renderer(SPHERE, m)])
    # SheenChair: a seat and a back in a velvet.
    velvet = mat("khr_velvet", base_color=[0.25, 0.04, 0.08], roughness=0.8,
                 sheen_color=[1.0, 0.45, 0.6], sheen_roughness=0.4)
    sc.entity([xf((-1.0, 0.3, 1.25), scale=(1.4, 1.2, 0.3)), mesh_renderer(BOX, velvet)])
    sc.entity([xf((-1.0, 0.85, 1.95), scale=(1.4, 0.25, 1.4)), mesh_renderer(BOX, velvet)])
    for lx in (-1.6, -0.4):
        sc.entity([xf((lx, 0.3, 0.2), scale=(0.12, 0.12, 1.8)), mesh_renderer(BOX, velvet)])
    # IridescenceLamp: three soap-film spheres.
    for i, (lo, hi) in enumerate([(200.0, 400.0), (400.0, 700.0), (600.0, 1000.0)]):
        m = mat("khr_irid_%d" % i, base_color=[0.9, 0.9, 0.9], roughness=0.05, metallic=1.0,
                iridescence=1.0, iridescence_ior=1.33, iridescence_thickness_min=lo,
                iridescence_thickness_max=hi)
        sc.entity([xf((1.4 + i * 0.95, 0.0, 2.2), scale=(0.8, 0.8, 0.8)),
                   mesh_renderer(SPHERE, m)])
    # TransmissionTest: transmissive spheres before a checker wall.
    for i, (ro, th) in enumerate([(0.0, 0.0), (0.15, 0.0), (0.0, 0.6), (0.3, 0.6)]):
        m = mat("khr_trans_%d" % i, base_color=[0.95, 1.0, 0.95], roughness=ro, transmission=1.0,
                thickness=th, attenuation_color=[0.55, 0.9, 0.6], attenuation_distance=0.6)
        sc.entity([xf((-3.9 + i * 1.0, 0.0, 0.2), scale=(0.85, 0.85, 0.85)),
                   mesh_renderer(SPHERE, m)])
    sc.entity([xf((-2.4, 1.2, 0.2), FACE, (4.4, 1.0, 1.4)), mesh_renderer(CARD, "mat/backdrop")])
    # AnisotropyBarnLamp: a brushed-steel shade, and a plain one beside it.
    brushed = mat("khr_brushed", base_color=[0.92, 0.92, 0.92], metallic=1.0, roughness=0.35,
                  anisotropy=0.9, double_sided=True)
    plain = mat("khr_plain_steel", base_color=[0.92, 0.92, 0.92], metallic=1.0, roughness=0.35,
                double_sided=True)
    sc.entity([xf((1.4, 0.0, 0.3), scale=(1.1, 1.1, 1.0)), mesh_renderer(CYL, brushed)])
    sc.entity([xf((3.3, 0.0, 0.3), scale=(1.1, 1.1, 1.0)), mesh_renderer(CYL, plain)])
    # The floor.
    sc.entity([xf((0.0, 1.0, -0.9), quat_axis((1.0, 0.0, 0.0), math.radians(-90.0)),
                  (12.0, 1.0, 8.0)), mesh_renderer(CARD, "mat/floor")])
    return sc


# ================================================================================================
# Main
# ================================================================================================


def main():
    out("meshes/sphere.gltf", mesh_sphere())
    out("meshes/card.gltf", mesh_card())
    out("meshes/box.gltf", mesh_box())
    out("meshes/cylinder.gltf", mesh_cylinder())
    out("textures/checker.png", tex_checker())
    out("textures/cutout.png", tex_cutout())
    out("textures/leaf.png", tex_leaf())
    out("textures/bumps_normal.png", tex_bumps())
    out("env/sky.exr", env_sky())
    out("env/uniform.exr", env_uniform())

    # The backdrop is Unlit: flat cells (what transmission and blending show), no lighting gradient
    # (which would cost the reference images the most bytes).
    mat("backdrop", base_color=[0.6, 0.6, 0.6], base_color_tex="tex/checker", unlit=True)
    mat("floor", base_color=[0.6, 0.6, 0.6], base_color_tex="tex/checker", roughness=0.7)
    rows = gallery_materials()
    scenes = {
        "gallery_ibl": scene_gallery(rows, False),
        "gallery_lights": scene_gallery(rows, True),
        "furnace": scene_furnace(),
        "khr_samples": scene_khr(),
    }
    for name, kv in MATS.items():
        out("materials/%s.mat.toml" % name, mat_file(name, kv))
    for name, sc in scenes.items():
        out("scenes/%s.scene.toml" % name, sc.text())

    m = ["# The materials gallery (DENG-307; written by gen_assets.py: edit the script, not this",
         "# file). Mounted read-only by tests/golden/materials/materials_scenes.c, cooked in-process",
         "# (SOURCE mode) by the engine's importers.",
         "[project]", 'asset_root = "."', "",
         "[defaults.model]", "generate_lods = false", "",
         "[assets]"]
    for n in ("sphere", "card", "box", "cylinder"):
        m.append('"mesh/%s" = { type = "model", source = "meshes/%s.gltf" }' % (n, n))
    m.append('"tex/checker" = { type = "texture", source = "textures/checker.png" }')
    m.append('"tex/cutout" = { type = "texture", source = "textures/cutout.png" }')
    m.append('"tex/leaf" = { type = "texture", source = "textures/leaf.png" }')
    m.append('"tex/bumps" = { type = "texture", source = "textures/bumps_normal.png", '
             'usage = "normal" }')
    m.append('"env/sky" = { type = "texture", source = "env/sky.exr", usage = "environment", '
             'env_size = 32, env_samples = 256, compression = "none" }')
    m.append('"env/uniform" = { type = "texture", source = "env/uniform.exr", '
             'usage = "environment", env_size = 8, env_samples = 64, compression = "none" }')
    for name in sorted(MATS):
        m.append('"mat/%s" = { type = "material", source = "materials/%s.mat.toml" }'
                 % (name, name))
    for name in sorted(scenes):
        m.append('"scenes/%s" = { type = "scene", source = "scenes/%s.scene.toml" }'
                 % (name, name))
    out("assets.toml", ("\n".join(m) + "\n").encode("ascii"))


if __name__ == "__main__":
    main()
