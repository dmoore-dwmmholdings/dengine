#!/usr/bin/env python3
"""Writes the FBX fixtures of tests/unit/import/test_fbx_import.c (DENG-102).

Run it from anywhere: `python3 tests/unit/import/data/fbx/gen_fixtures.py`. The output is
deterministic (fixed data, no timestamps), so a re-run leaves the files unchanged.

No DCC tool is needed: this script writes FBX 7.4 directly, binary (`Kaydara FBX Binary`, node
records with 32-bit offsets) or ASCII, from one node tree. Each scene is described in engine
coordinates (Z-up, right-handed, metres, model front along -Y; SPEC §5.5, the DENG-2 axis rule)
and converted to the axes and unit that its GlobalSettings declare, the way an exporter writes it:

  - Maya-style Y-up:      right +X, up +Y, front +Z. File (x, y, z) = engine (x, z, -y).
  - 3ds Max-style Z-up:   right +X, up +Z, front -Y. File = engine.
  - UnitScaleFactor is centimetres per file unit (1 = cm, 100 = m).

Files:
  cube_yup_cm.fbx     binary, Maya axes, cm: a 1 x 2 x 4 m box under a translated, rotated node
  cube_zup_m.fbx      ASCII, 3ds Max axes, m: the same box
  cube_ref.gltf       glTF 2.0: the same box (the reference the FBX imports must match)
  cube_wrong_axes.fbx binary: the Z-up data of cube_zup_m.fbx, but GlobalSettings say Y-up
  cube_lefthanded.fbx binary, left-handed axes (right -X, up +Z, front -Y), m: the same box
  ngon_two_mats.fbx   binary: a pentagon, a triangle, a hexagon, a quad and a line face with two
                      materials, no normals (the importer generates them)
  materials.fbx       binary: Phong (embedded and external textures), Lambert (a missing
                      texture) and 3ds Max OpenPBR materials
  skinned.fbx         ASCII, Maya axes, cm: a 5-bone chain skinning a strip (one vertex with 5
                      influences), two animation stacks
  normal.png          the external texture of materials.fbx
"""
import base64
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
KTIME_PER_SECOND = 46186158000

# ---- Values -------------------------------------------------------------------------------------


class Name:
    """An object name: "Name\\x00\\x01Class" in binary, "Class::Name" in ASCII."""

    def __init__(self, name, cls):
        self.name, self.cls = name, cls


class Arr:
    """A typed array property: code is one of d (f64), f (f32), i (i32), l (i64)."""

    def __init__(self, code, values):
        self.code, self.values = code, list(values)


class Raw:
    def __init__(self, data):
        self.data = bytes(data)


class I32(int):
    pass


class I64(int):
    pass


class F64(float):
    pass


def ad(v):
    return Arr("d", v)


def ai(v):
    return Arr("i", v)


def al(v):
    return Arr("l", v)


def af(v):
    return Arr("f", v)


class N:
    """One FBX node: a name, properties, children."""

    def __init__(self, name, *props, children=None):
        self.name, self.props, self.children = name, list(props), list(children or [])


def P(name, ptype, label, flags, *values):
    return N("P", name, ptype, label, flags, *values)


def props70(*ps):
    return N("Properties70", children=list(ps))


# ---- Binary writer (FBX 7400, 32-bit record offsets) --------------------------------------------


def bin_prop(p):
    if isinstance(p, Name):
        s = p.name.encode() + b"\x00\x01" + p.cls.encode()
        return b"S" + struct.pack("<I", len(s)) + s
    if isinstance(p, str):
        s = p.encode()
        return b"S" + struct.pack("<I", len(s)) + s
    if isinstance(p, Raw):
        return b"R" + struct.pack("<I", len(p.data)) + p.data
    if isinstance(p, Arr):
        fmt = {"d": "d", "f": "f", "i": "i", "l": "q"}[p.code]
        data = struct.pack("<%d%s" % (len(p.values), fmt), *p.values)
        return p.code.encode() + struct.pack("<III", len(p.values), 0, len(data)) + data
    if isinstance(p, I64):
        return b"L" + struct.pack("<q", p)
    if isinstance(p, bool):
        return b"C" + struct.pack("<B", 1 if p else 0)
    if isinstance(p, int):
        return b"I" + struct.pack("<i", p)
    if isinstance(p, float):
        return b"D" + struct.pack("<d", p)
    raise TypeError(p)


def bin_node(n, offset):
    name = n.name.encode()
    props = b"".join(bin_prop(p) for p in n.props)
    head = 13 + len(name)
    body = bytearray()
    pos = offset + head + len(props)
    for c in n.children:
        b = bin_node(c, pos)
        body += b
        pos += len(b)
    if n.children:
        body += b"\x00" * 13
    end = offset + head + len(props) + len(body)
    return (struct.pack("<IIIB", end, len(n.props), len(props), len(name)) + name + props +
            bytes(body))


def write_binary(path, nodes):
    out = bytearray(b"Kaydara FBX Binary  \x00\x1a\x00" + struct.pack("<I", 7400))
    for n in nodes:
        out += bin_node(n, len(out))
    out += b"\x00" * 13
    with open(path, "wb") as f:
        f.write(out)


# ---- ASCII writer -------------------------------------------------------------------------------


def num(v):
    if isinstance(v, float):
        r = repr(v)
        return r[:-2] if r.endswith(".0") else r
    return str(int(v))


def ascii_prop(p):
    if isinstance(p, Name):
        return '"%s::%s"' % (p.cls, p.name)
    if isinstance(p, str):
        return '"%s"' % p
    if isinstance(p, Raw):
        return '"%s"' % base64.b64encode(p.data).decode("ascii")
    if isinstance(p, bool):
        return "T" if p else "F"
    return num(p)


def ascii_node(n, depth, out):
    tab = "\t" * depth
    arrays = [p for p in n.props if isinstance(p, Arr)]
    if arrays:
        a = arrays[0]
        out.append("%s%s: *%d {\n" % (tab, n.name, len(a.values)))
        out.append("%s\ta: %s\n" % (tab, ",".join(num(v) for v in a.values)))
        out.append("%s}\n" % tab)
        return
    line = "%s%s: %s" % (tab, n.name, ", ".join(ascii_prop(p) for p in n.props))
    if n.children:
        out.append(line.rstrip() + " {\n")
        for c in n.children:
            ascii_node(c, depth + 1, out)
        out.append("%s}\n" % tab)
    else:
        out.append(line.rstrip() + "\n")


def write_ascii(path, nodes):
    out = ["; FBX 7.4.0 project file\n; Written by dengine gen_fixtures.py (DENG-102)\n"]
    for n in nodes:
        ascii_node(n, 0, out)
    with open(path, "w", newline="\n") as f:
        f.write("".join(out))


# ---- Scene builder ------------------------------------------------------------------------------

MAYA = dict(up=(1, 1), front=(2, 1), right=(0, 1))     # right +X, up +Y, front +Z
MAX = dict(up=(2, 1), front=(1, -1), right=(0, 1))     # right +X, up +Z, front -Y
LEFT = dict(up=(2, 1), front=(1, -1), right=(0, -1))   # left-handed: right -X, up +Z, front -Y


def to_file(axes, v):
    """Engine (x, y, z) to the file axes of `axes`."""
    x, y, z = v
    if axes is MAYA:
        return (x, z, 0.0 - y)  # 0.0 - y: no negative zeros in the files
    if axes is LEFT:
        return (0.0 - x, y, z)
    return (x, y, z)


class Scene:
    def __init__(self, axes, unit_cm, fps_mode=6):
        self.axes, self.unit_cm, self.fps_mode = axes, unit_cm, fps_mode
        self.objects, self.conns = [], []
        self.next_id = 100000

    def uid(self):
        self.next_id += 1
        return I64(self.next_id)

    def oo(self, child, parent):
        self.conns.append(N("C", "OO", I64(child), I64(parent)))

    def op(self, child, parent, prop):
        self.conns.append(N("C", "OP", I64(child), I64(parent), prop))

    def pos(self, v):
        """An engine-space point in metres → file axes and units."""
        k = 100.0 / self.unit_cm
        return tuple(float(c * k) for c in to_file(self.axes, v))

    def dirn(self, v):
        return tuple(float(c) for c in to_file(self.axes, v))

    def geometry(self, name, points, polys, normals=None, uvs=None, face_mats=None):
        """points: engine-space metres; polys: lists of point indices; normals/uvs: per polygon
        vertex (engine directions; glTF-convention uv, v down: written as FBX v up)."""
        gid = self.uid()
        verts = [c for p in points for c in self.pos(p)]
        pvi = []
        for poly in polys:
            pvi += poly[:-1] + [-poly[-1] - 1]
        kids = [N("Vertices", ad(verts)), N("PolygonVertexIndex", ai(pvi)),
                N("GeometryVersion", 124)]
        layer = []
        if normals is not None:
            kids.append(N("LayerElementNormal", 0, children=[
                N("Version", 101), N("Name", ""),
                N("MappingInformationType", "ByPolygonVertex"),
                N("ReferenceInformationType", "Direct"),
                N("Normals", ad([c for n in normals for c in self.dirn(n)]))]))
            layer.append("LayerElementNormal")
        if uvs is not None:
            uniq = []
            index = []
            for uv in uvs:
                fbx_uv = (float(uv[0]), float(1.0 - uv[1]))
                if fbx_uv not in uniq:
                    uniq.append(fbx_uv)
                index.append(uniq.index(fbx_uv))
            kids.append(N("LayerElementUV", 0, children=[
                N("Version", 101), N("Name", "UVMap"),
                N("MappingInformationType", "ByPolygonVertex"),
                N("ReferenceInformationType", "IndexToDirect"),
                N("UV", ad([c for uv in uniq for c in uv])), N("UVIndex", ai(index))]))
            layer.append("LayerElementUV")
        if face_mats is not None:
            kids.append(N("LayerElementMaterial", 0, children=[
                N("Version", 101), N("Name", ""),
                N("MappingInformationType", "ByPolygon"),
                N("ReferenceInformationType", "IndexToDirect"),
                N("Materials", ai(face_mats))]))
            layer.append("LayerElementMaterial")
        kids.append(N("Layer", 0, children=[N("Version", 100)] + [
            N("LayerElement", children=[N("Type", t), N("TypedIndex", 0)]) for t in layer]))
        self.objects.append(N("Geometry", gid, Name(name, "Geometry"), "Mesh", children=kids))
        return gid

    def model(self, name, kind, parent=0, t=(0, 0, 0), r_deg=(0, 0, 0), s=(1, 1, 1)):
        """t in engine metres; r_deg is the Euler XYZ rotation in *file* axes."""
        mid = self.uid()
        ps = [P("Lcl Translation", "Lcl Translation", "", "A", *self.pos(t)),
              P("Lcl Rotation", "Lcl Rotation", "", "A", *[float(x) for x in r_deg]),
              P("Lcl Scaling", "Lcl Scaling", "", "A", *[float(x) for x in s])]
        self.objects.append(N("Model", mid, Name(name, "Model"), kind, children=[
            N("Version", 232), props70(*ps), N("Shading", True), N("Culling", "CullingOff")]))
        self.oo(mid, parent)
        if kind == "LimbNode":
            aid = self.uid()
            self.objects.append(N("NodeAttribute", aid, Name(name, "NodeAttribute"), "LimbNode",
                                  children=[N("TypeFlags", "Skeleton")]))
            self.oo(aid, mid)
        return mid

    def material(self, name, shading, ps):
        mid = self.uid()
        self.objects.append(N("Material", mid, Name(name, "Material"), "", children=[
            N("Version", 102), N("ShadingModel", shading), N("MultiLayer", 0), props70(*ps)]))
        return mid

    def texture(self, name, rel, content=None):
        tid, vid = self.uid(), self.uid()
        self.objects.append(N("Texture", tid, Name(name, "Texture"), "", children=[
            N("Type", "TextureVideoClip"), N("Version", 202),
            N("TextureName", Name(name, "Texture")), N("Media", Name(name, "Video")),
            N("FileName", "C:/art/" + rel), N("RelativeFilename", rel)]))
        vkids = [N("Type", "Clip"), N("UseMipMap", 0), N("Filename", "C:/art/" + rel),
                 N("RelativeFilename", rel)]
        if content is not None:
            vkids.append(N("Content", Raw(content)))
        self.objects.append(N("Video", vid, Name(name, "Video"), "Clip", children=vkids))
        self.oo(vid, tid)
        return tid

    def nodes(self):
        up, front, right = self.axes["up"], self.axes["front"], self.axes["right"]
        settings = props70(
            P("UpAxis", "int", "Integer", "", up[0]), P("UpAxisSign", "int", "Integer", "", up[1]),
            P("FrontAxis", "int", "Integer", "", front[0]),
            P("FrontAxisSign", "int", "Integer", "", front[1]),
            P("CoordAxis", "int", "Integer", "", right[0]),
            P("CoordAxisSign", "int", "Integer", "", right[1]),
            P("OriginalUpAxis", "int", "Integer", "", up[0]),
            P("OriginalUpAxisSign", "int", "Integer", "", up[1]),
            P("UnitScaleFactor", "double", "Number", "", float(self.unit_cm)),
            P("OriginalUnitScaleFactor", "double", "Number", "", float(self.unit_cm)),
            P("TimeMode", "enum", "", "", self.fps_mode),
            P("TimeSpanStart", "KTime", "Time", "", I64(0)),
            P("TimeSpanStop", "KTime", "Time", "", I64(KTIME_PER_SECOND)))
        return [
            N("FBXHeaderExtension", children=[
                N("FBXHeaderVersion", 1003), N("FBXVersion", 7400),
                N("Creator", "dengine gen_fixtures.py")]),
            N("GlobalSettings", children=[N("Version", 1000), settings]),
            N("Objects", children=self.objects),
            N("Connections", children=self.conns)]


# ---- The box ------------------------------------------------------------------------------------

BOX_MIN = (-0.5, -1.0, 0.0)
BOX_MAX = (0.5, 1.0, 4.0)
BOX_T = (2.0, 3.0, 0.5)   # node translation, engine metres
BOX_ANGLE = 30.0           # node rotation about engine +Z (up), degrees


def box():
    """24 points (4 per face), 6 quads, per-corner normals and uvs (glTF convention)."""
    lo, hi = BOX_MIN, BOX_MAX
    faces = [  # (normal, 4 corners CCW seen from outside)
        ((1, 0, 0), [(hi[0], lo[1], lo[2]), (hi[0], hi[1], lo[2]), (hi[0], hi[1], hi[2]),
                     (hi[0], lo[1], hi[2])]),
        ((-1, 0, 0), [(lo[0], hi[1], lo[2]), (lo[0], lo[1], lo[2]), (lo[0], lo[1], hi[2]),
                      (lo[0], hi[1], hi[2])]),
        ((0, 1, 0), [(hi[0], hi[1], lo[2]), (lo[0], hi[1], lo[2]), (lo[0], hi[1], hi[2]),
                     (hi[0], hi[1], hi[2])]),
        ((0, -1, 0), [(lo[0], lo[1], lo[2]), (hi[0], lo[1], lo[2]), (hi[0], lo[1], hi[2]),
                      (lo[0], lo[1], hi[2])]),
        ((0, 0, 1), [(lo[0], lo[1], hi[2]), (hi[0], lo[1], hi[2]), (hi[0], hi[1], hi[2]),
                     (lo[0], hi[1], hi[2])]),
        ((0, 0, -1), [(lo[0], hi[1], lo[2]), (hi[0], hi[1], lo[2]), (hi[0], lo[1], lo[2]),
                      (lo[0], lo[1], lo[2])]),
    ]
    uv4 = [(0.0, 1.0), (1.0, 1.0), (1.0, 0.0), (0.0, 0.0)]
    points, polys, normals, uvs = [], [], [], []
    for n, corners in faces:
        base = len(points)
        points += corners
        polys.append([base, base + 1, base + 2, base + 3])
        normals += [n] * 4
        uvs += uv4
    return points, polys, normals, uvs


def box_fbx(axes, unit_cm, settings_axes=None):
    s = Scene(axes, unit_cm)
    points, polys, normals, uvs = box()
    if axes is LEFT:
        # Mirrored data stays self-consistent (the winding's cross product agrees with the
        # normals) only with every polygon reversed.
        polys = [[q[0], q[3], q[2], q[1]] for q in polys]
        n_by_corner = {c: (normals[i], uvs[i]) for i, c in enumerate(range(len(normals)))}
        normals = [n_by_corner[c][0] for q in polys for c in q]
        uvs = [n_by_corner[c][1] for q in polys for c in q]
    g = s.geometry("Box", points, polys, normals, uvs)
    r = (0.0, BOX_ANGLE, 0.0) if axes is MAYA else (0.0, 0.0, BOX_ANGLE)
    if axes is LEFT:
        r = (0.0, 0.0, -BOX_ANGLE)  # a mirror in X turns the rotation about Z the other way
    m = s.model("Box", "Mesh", t=BOX_T, r_deg=r)
    s.oo(g, m)
    if settings_axes is not None:
        s.axes = settings_axes
    return s.nodes()


def box_gltf(path):
    points, polys, normals, uvs = box()
    gl = lambda v: (v[0], v[2], -v[1])  # engine → glTF
    pos = [c for p in points for c in gl(p)]
    nrm = [c for n in normals for c in gl(n)]
    tex = [c for uv in uvs for c in uv]
    idx = []
    for q in polys:
        idx += [q[0], q[1], q[2], q[0], q[2], q[3]]
    blob = (struct.pack("<%df" % len(pos), *pos) + struct.pack("<%df" % len(nrm), *nrm) +
            struct.pack("<%df" % len(tex), *tex) + struct.pack("<%dH" % len(idx), *idx))
    pmin = [min(pos[i::3]) for i in range(3)]
    pmax = [max(pos[i::3]) for i in range(3)]
    o_n, o_t, o_i = 4 * len(pos), 4 * len(pos) + 4 * len(nrm), 4 * (len(pos) + len(nrm) + len(tex))
    half = math.radians(BOX_ANGLE) / 2
    doc = {
        "asset": {"version": "2.0", "generator": "dengine gen_fixtures.py (DENG-102)"},
        "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," +
                     base64.b64encode(blob).decode("ascii")}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": 4 * len(pos)},
            {"buffer": 0, "byteOffset": o_n, "byteLength": 4 * len(nrm)},
            {"buffer": 0, "byteOffset": o_t, "byteLength": 4 * len(tex)},
            {"buffer": 0, "byteOffset": o_i, "byteLength": 2 * len(idx)}],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": 24, "type": "VEC3",
             "min": pmin, "max": pmax},
            {"bufferView": 1, "componentType": 5126, "count": 24, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": 24, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(idx), "type": "SCALAR"}],
        "meshes": [{"name": "Box", "primitives": [
            {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3}]}],
        "nodes": [{"name": "Box", "mesh": 0, "translation": list(gl(BOX_T)),
                   "rotation": [0.0, math.sin(half), 0.0, math.cos(half)]}],
        "scenes": [{"nodes": [0]}], "scene": 0}
    with open(path, "w", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")


# ---- N-gons and two materials -------------------------------------------------------------------


def ngon_two_mats():
    s = Scene(MAX, 100.0)

    def ring(cx, cy, n):
        return [(cx + math.cos(2 * math.pi * k / n), cy + math.sin(2 * math.pi * k / n), 0.0)
                for k in range(n)]
    points = ring(0, 0, 5) + ring(3, 0, 3) + ring(0, 3, 6) + ring(3, 3, 4)
    points += [(6.0, 0.0, 0.0), (7.0, 0.0, 0.0)]  # the line face
    polys = [list(range(0, 5)), list(range(5, 8)), list(range(8, 14)), list(range(14, 18)),
             [18, 19]]
    g = s.geometry("Shapes", points, polys, face_mats=[0, 0, 1, 1, 0])
    m = s.model("Shapes", "Mesh")
    s.oo(g, m)
    for name, rgb in (("Mat0", (0.8, 0.1, 0.1)), ("Mat1", (0.1, 0.8, 0.1))):
        mat = s.material(name, "lambert", [P("DiffuseColor", "Color", "", "A", *rgb)])
        s.oo(mat, m)
    return s.nodes()


# ---- Materials ----------------------------------------------------------------------------------


def png_rgba(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


CHECKER = png_rgba(2, 2, (200, 100, 50, 255))
NORMAL = png_rgba(2, 2, (128, 128, 255, 255))


def materials():
    s = Scene(MAX, 100.0)
    points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0), (2, 1, 0), (3, 0, 0),
              (3, 1, 0)]
    polys = [[0, 1, 2, 3], [1, 4, 5, 2], [4, 6, 7, 5]]
    uvs = [(0.0, 0.0), (1.0, 0.0), (1.0, 1.0), (0.0, 1.0)] * 3
    g = s.geometry("Panel", points, polys, uvs=uvs, face_mats=[0, 1, 2])
    m = s.model("Panel", "Mesh")
    s.oo(g, m)

    phong = s.material("Phong", "phong", [
        P("DiffuseColor", "Color", "", "A", 0.8, 0.4, 0.2),
        P("DiffuseFactor", "Number", "", "A", 0.5),
        P("SpecularColor", "Color", "", "A", 1.0, 0.5, 0.25),
        P("SpecularFactor", "Number", "", "A", 0.25),
        P("ShininessExponent", "Number", "", "A", 30.0),
        P("EmissiveColor", "Color", "", "A", 1.0, 0.5, 0.0),
        P("EmissiveFactor", "Number", "", "A", 2.0),
        P("TransparencyFactor", "Number", "", "A", 0.25)])
    lambert = s.material("Lambert", "lambert", [
        P("DiffuseColor", "Color", "", "A", 0.2, 0.4, 0.6)])
    pbr = s.material("OpenPBR", "", [
        P("3dsMax|ClassIDa", "int", "Integer", "", -246079949),   # 0xf1551e33
        P("3dsMax|ClassIDb", "int", "Integer", "", 939201335),    # 0x37fb1337
        P("3dsMax|Parameters|base_weight", "Float", "", "A", 0.5),
        P("3dsMax|Parameters|base_color", "RGBA", "Color", "A", 0.6, 0.4, 0.2, 1.0),
        P("3dsMax|Parameters|base_metalness", "Float", "", "A", 0.8),
        P("3dsMax|Parameters|specular_weight", "Float", "", "A", 0.7),
        P("3dsMax|Parameters|specular_color", "RGBA", "Color", "A", 1.0, 0.9, 0.8, 1.0),
        P("3dsMax|Parameters|specular_roughness", "Float", "", "A", 0.3),
        P("3dsMax|Parameters|specular_ior", "Float", "", "A", 1.45),
        P("3dsMax|Parameters|transmission_weight", "Float", "", "A", 0.2),
        P("3dsMax|Parameters|coat_weight", "Float", "", "A", 0.4),
        P("3dsMax|Parameters|coat_roughness", "Float", "", "A", 0.15),
        P("3dsMax|Parameters|fuzz_weight", "Float", "", "A", 0.5),
        P("3dsMax|Parameters|fuzz_color", "RGBA", "Color", "A", 0.2, 0.4, 0.6, 1.0),
        P("3dsMax|Parameters|fuzz_roughness", "Float", "", "A", 0.35),
        P("3dsMax|Parameters|emission_weight", "Float", "", "A", 3.0),
        P("3dsMax|Parameters|emission_color", "RGBA", "Color", "A", 1.0, 0.5, 0.25, 1.0),
        P("3dsMax|Parameters|geometry_opacity", "Float", "", "A", 0.6)])
    for mat in (phong, lambert, pbr):
        s.oo(mat, m)

    # Phong: an embedded diffuse texture and an external normal map next to the .fbx.
    t_diffuse = s.texture("checker", "checker.png", content=CHECKER)
    s.op(t_diffuse, phong, "DiffuseColor")
    t_normal = s.texture("normal", "normal.png")
    s.op(t_normal, phong, "NormalMap")
    # Lambert: a texture whose file does not exist (a warning; the slot stays empty).
    t_missing = s.texture("missing", "missing.png")
    s.op(t_missing, lambert, "DiffuseColor")
    # OpenPBR: the same embedded image as base colour (one texture sub-asset with Phong's).
    t_base = s.texture("checker_pbr", "checker.png", content=CHECKER)
    s.op(t_base, pbr, "3dsMax|Parameters|base_color_map")
    return s.nodes()


# ---- Skin and animation -------------------------------------------------------------------------


def mat_translate(v):
    m = [1.0, 0, 0, 0, 0, 1.0, 0, 0, 0, 0, 1.0, 0, v[0], v[1], v[2], 1.0]
    return [float(x) for x in m]


def anim_curve(s, times, values):
    cid = s.uid()
    n = len(times)
    s.objects.append(N("AnimationCurve", cid, Name("", "AnimCurve"), "", children=[
        N("Default", float(values[0])), N("KeyVer", 4009),
        N("KeyTime", al([int(t * KTIME_PER_SECOND) for t in times])),
        N("KeyValueFloat", af([float(v) for v in values])),
        N("KeyAttrFlags", ai([0x00000104])),           # linear interpolation
        N("KeyAttrDataFloat", af([0.0, 0.0, 0.0, 0.0])),
        N("KeyAttrRefCount", ai([n]))]))
    return cid


def anim_prop(s, layer, model, prop, short, times, xyz):
    """Animates `prop` (e.g. "Lcl Rotation") of `model`: xyz = per-axis value lists (file
    axes and units)."""
    nid = s.uid()
    s.objects.append(N("AnimationCurveNode", nid, Name(short, "AnimCurveNode"), "", children=[
        props70(*[P("d|" + a, "Number", "", "A", float(xyz[i][0]))
                  for i, a in enumerate("XYZ")])]))
    s.oo(nid, layer)
    s.op(nid, model, prop)
    for i, a in enumerate("XYZ"):
        s.op(anim_curve(s, times, xyz[i]), nid, "d|" + a)


def anim_stack(s, name, seconds):
    sid, lid = s.uid(), s.uid()
    stop = I64(int(seconds * KTIME_PER_SECOND))
    s.objects.append(N("AnimationStack", sid, Name(name, "AnimStack"), "", children=[props70(
        P("LocalStart", "KTime", "Time", "", I64(0)), P("LocalStop", "KTime", "Time", "", stop),
        P("ReferenceStart", "KTime", "Time", "", I64(0)),
        P("ReferenceStop", "KTime", "Time", "", stop))]))
    s.objects.append(N("AnimationLayer", lid, Name("BaseLayer", "AnimLayer"), ""))
    s.oo(lid, sid)
    return lid


def skinned():
    s = Scene(MAYA, 1.0)
    # A strip of two quads along engine +Z (file +Y), 1 m per quad, at x in {0, 1}.
    points = [(0, 0, 0), (1, 0, 0), (0, 0, 1), (1, 0, 1), (0, 0, 2), (1, 0, 2)]
    polys = [[0, 1, 3, 2], [2, 3, 5, 4]]
    normals = [(0, -1, 0)] * 8
    g = s.geometry("Body", points, polys, normals=normals)
    body = s.model("Body", "Mesh")
    s.oo(g, body)

    # Bones: Root at the origin, then 4 bones up the chain, each 0.5 m above its parent.
    names = ["Root", "Bone", "B2", "B3", "B4"]
    bones, world = [], []
    parent = 0
    for i, n in enumerate(names):
        t = (0.0, 0.0, 0.0 if i == 0 else 0.5)
        b = s.model(n, "LimbNode", parent=parent, t=t)
        bones.append(b)
        world.append((0.0, 0.0, 0.5 * i))
        parent = b

    skin = s.uid()
    s.objects.append(N("Deformer", skin, Name("Skin", "Deformer"), "Skin", children=[
        N("Version", 101), N("Link_DeformAcuracy", 50.0)]))
    s.oo(skin, g)
    # Per bone: (control point, weight). Vertex 5 has five influences (capped to 4).
    influences = {
        "Root": [(0, 1.0), (1, 1.0), (2, 0.5), (3, 0.5), (5, 0.05)],
        "Bone": [(2, 0.5), (3, 0.5), (4, 1.0), (5, 0.4)],
        "B2": [(5, 0.3)],
        "B3": [(5, 0.15)],
        "B4": [(5, 0.1)],
    }
    for i, n in enumerate(names):
        cid = s.uid()
        idx = [cp for cp, _ in influences[n]]
        wts = [float(w) for _, w in influences[n]]
        s.objects.append(N("Deformer", cid, Name("Cluster " + n, "SubDeformer"), "Cluster",
                           children=[
            N("Version", 100), N("UserData", "", ""), N("Indexes", ai(idx)),
            # Transform: mesh to bone at bind time (the inverse of the bone's bind matrix, the
            # mesh being at the origin); TransformLink: the bone's bind matrix.
            N("Weights", ad(wts)),
            N("Transform", ad(mat_translate([0.0 - c for c in s.pos(world[i])]))),
            N("TransformLink", ad(mat_translate(s.pos(world[i]))))]))
        s.oo(cid, skin)
        s.oo(bones[i], cid)

    # "Wave": 1 s, Bone rotates 0 → 90 degrees about engine X (= file X).
    layer = anim_stack(s, "Wave", 1.0)
    anim_prop(s, layer, bones[1], "Lcl Rotation", "R", [0.0, 1.0],
              [[0.0, 90.0], [0.0, 0.0], [0.0, 0.0]])
    # "Idle": 0.5 s, Root moves 0.5 m along engine -Y (file +Z, in cm).
    layer = anim_stack(s, "Idle", 0.5)
    end = s.pos((0.0, -0.5, 0.0))
    anim_prop(s, layer, bones[0], "Lcl Translation", "T", [0.0, 0.5],
              [[0.0, end[0]], [0.0, end[1]], [0.0, end[2]]])
    return s.nodes()


def main():
    write_binary(os.path.join(HERE, "cube_yup_cm.fbx"), box_fbx(MAYA, 1.0))
    write_ascii(os.path.join(HERE, "cube_zup_m.fbx"), box_fbx(MAX, 100.0))
    write_binary(os.path.join(HERE, "cube_wrong_axes.fbx"), box_fbx(MAX, 100.0, MAYA))
    write_binary(os.path.join(HERE, "cube_lefthanded.fbx"), box_fbx(LEFT, 100.0))
    box_gltf(os.path.join(HERE, "cube_ref.gltf"))
    write_binary(os.path.join(HERE, "ngon_two_mats.fbx"), ngon_two_mats())
    write_binary(os.path.join(HERE, "materials.fbx"), materials())
    write_ascii(os.path.join(HERE, "skinned.fbx"), skinned())
    with open(os.path.join(HERE, "normal.png"), "wb") as f:
        f.write(NORMAL)


if __name__ == "__main__":
    main()
