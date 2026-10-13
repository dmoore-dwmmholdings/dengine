#!/usr/bin/env python3
"""Writes the generated glTF fixtures of tests/unit/import/test_gltf_import.c (DENG-101).

Run it from anywhere: `python3 tests/unit/import/data/gltf/gen_fixtures.py`. The output is
deterministic (sorted JSON keys, fixed data), so a re-run leaves the files unchanged. The Khronos
sample files next to it (Box, BoxTextured, Duck, SimpleSparseAccessor) are not generated; see
KHRONOS_SAMPLES.txt.

Every buffer is an embedded base64 data URI, so each fixture is one self-contained .gltf file.
"""
import base64
import json
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

FLOAT, U8, U16, U32, I8, I16 = 5126, 5121, 5123, 5125, 5120, 5122
FMT = {FLOAT: "f", U8: "B", U16: "H", U32: "I", I8: "b", I16: "h"}
SIZE = {FLOAT: 4, U8: 1, U16: 2, U32: 4, I8: 1, I16: 2}
NCOMP = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}


def png_rgba(w, h, rgba):
    """A tiny uncompressed-filter PNG of one colour."""
    raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def data_uri(mime, data):
    return "data:%s;base64,%s" % (mime, base64.b64encode(data).decode("ascii"))


class Doc:
    def __init__(self):
        self.bin = bytearray()
        self.j = {"asset": {"version": "2.0", "generator": "dengine gen_fixtures.py"},
                  "buffers": [], "bufferViews": [], "accessors": []}

    def view(self, data, stride=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        bv = {"buffer": 0, "byteOffset": len(self.bin), "byteLength": len(data)}
        if stride:
            bv["byteStride"] = stride
        self.bin += data
        self.j["bufferViews"].append(bv)
        return len(self.j["bufferViews"]) - 1

    def acc(self, ctype, typ, values, normalized=False, minmax=False, stride=None):
        """An accessor over its own buffer view; `stride` pads each element (vertex attributes
        must start on 4-byte boundaries, so a VEC3 of shorts needs stride 8)."""
        n = NCOMP[typ]
        elem = SIZE[ctype] * n
        rows = [struct.pack("<%d%s" % (n, FMT[ctype]), *values[i:i + n])
                for i in range(0, len(values), n)]
        data = b"".join(r + b"\0" * ((stride or elem) - elem) for r in rows)
        a = {"bufferView": self.view(data, stride), "componentType": ctype,
             "count": len(values) // n, "type": typ}
        if normalized:
            a["normalized"] = True
        if minmax:
            a["min"] = [min(values[i::n]) for i in range(n)]
            a["max"] = [max(values[i::n]) for i in range(n)]
        self.j["accessors"].append(a)
        return len(self.j["accessors"]) - 1

    def image(self, name, png):
        self.j.setdefault("images", []).append({"name": name, "mimeType": "image/png",
                                                "uri": data_uri("image/png", png)})
        self.j.setdefault("textures", []).append({"source": len(self.j["images"]) - 1})
        return len(self.j["textures"]) - 1

    def write(self, name):
        self.j["buffers"] = [{"byteLength": len(self.bin),
                              "uri": data_uri("application/octet-stream", bytes(self.bin))}]
        with open(os.path.join(HERE, name), "w", newline="\n") as f:
            json.dump(self.j, f, indent=1, sort_keys=True)
            f.write("\n")


QUAD_POS = [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0]
QUAD_UV = [0, 0, 1, 0, 1, 1, 0, 1]
QUAD_IDX = [0, 1, 2, 0, 2, 3]
TRI_POS = [0, 0, 0, 1, 0, 0, 0, 1, 0]
WHITE = png_rgba(2, 2, (255, 255, 255, 255))
NORMAL = png_rgba(2, 2, (128, 128, 255, 255))


def quad_prim(d, material=None, dx=0.0, uv=True, normals=True):
    pos = [v + (dx if i % 3 == 0 else 0) for i, v in enumerate(QUAD_POS)]
    attrs = {"POSITION": d.acc(FLOAT, "VEC3", pos, minmax=True)}
    if normals:
        attrs["NORMAL"] = d.acc(FLOAT, "VEC3", [0, 0, 1] * 4)
    if uv:
        attrs["TEXCOORD_0"] = d.acc(FLOAT, "VEC2", QUAD_UV)
    p = {"attributes": attrs, "indices": d.acc(U16, "SCALAR", QUAD_IDX)}
    if material is not None:
        p["material"] = material
    return p


def scene(d, nodes):
    d.j["scenes"] = [{"nodes": nodes}]
    d.j["scene"] = 0


def two_materials():
    d = Doc()
    d.j["materials"] = [
        {"name": "Red", "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1]}},
        {"name": "Blue", "pbrMetallicRoughness": {"baseColorFactor": [0, 0, 1, 1],
                                                  "metallicFactor": 0.5,
                                                  "roughnessFactor": 0.25}},
    ]
    d.j["meshes"] = [{"name": "Panels", "primitives": [quad_prim(d, 0), quad_prim(d, 1, dx=2)]}]
    d.j["nodes"] = [{"name": "Panel", "mesh": 0}]
    scene(d, [0])
    d.write("two_materials.gltf")


def strips_fans():
    d = Doc()
    strip = [0, 0, 0, 0, 1, 0, 1, 0, 0, 1, 1, 0, 2, 0, 0]
    fan = [0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, -1, 1, 0]
    prims = [
        {"mode": 5, "attributes": {"POSITION": d.acc(FLOAT, "VEC3", strip, minmax=True)}},
        {"mode": 6, "attributes": {"POSITION": d.acc(FLOAT, "VEC3", fan, minmax=True)},
         "indices": d.acc(U8, "SCALAR", [0, 1, 2, 3, 4])},
        {"mode": 0, "attributes": {"POSITION": d.acc(FLOAT, "VEC3", TRI_POS, minmax=True)}},
        {"mode": 1, "attributes": {"POSITION": d.acc(FLOAT, "VEC3", TRI_POS[:6], minmax=True)}},
    ]
    d.j["meshes"] = [{"name": "Strips", "primitives": prims}]
    d.j["nodes"] = [{"name": "S", "mesh": 0}]
    scene(d, [0])
    d.write("strips_fans.gltf")


def normals_tangents():
    d = Doc()
    t = d.image("nrm", NORMAL)
    d.j["materials"] = [{"name": "Bumpy", "normalTexture": {"index": t, "scale": 0.5}}]
    d.j["meshes"] = [{"name": "Q", "primitives": [quad_prim(d, 0, normals=False)]}]
    d.j["nodes"] = [{"name": "Q", "mesh": 0}]
    scene(d, [0])
    d.write("normals_tangents.gltf")


def axis():
    d = Doc()
    tri = [0, 1, 0, 0, 0, 1, 1, 0, 0]
    d.j["meshes"] = [{"name": "Tri", "primitives": [
        {"attributes": {"POSITION": d.acc(FLOAT, "VEC3", tri, minmax=True),
                        "NORMAL": d.acc(FLOAT, "VEC3", [0, 1, 0, 0, 0, 1, 1, 0, 0]),
                        "TANGENT": d.acc(FLOAT, "VEC4", [1, 0, 0, 1, 0, 1, 0, -1, 0, 0, 1, 1])}}]}]
    s45, c45 = math.sin(math.pi / 4), math.cos(math.pi / 4)
    s15, c15 = math.sin(math.pi / 12), math.cos(math.pi / 12)
    # A matrix node: rotation 90 deg about Z, scale 2, translation (1, 0, 0), column-major.
    mat = [0, 2, 0, 0, -2, 0, 0, 0, 0, 0, 2, 0, 1, 0, 0, 1]
    d.j["nodes"] = [
        {"name": "Parent", "translation": [1, 2, 3], "rotation": [0, s45, 0, c45],
         "scale": [1, 2, 3], "children": [1]},
        {"name": "Child", "translation": [0, 1, 0], "rotation": [s15, 0, 0, c15],
         "scale": [2, 2, 2], "mesh": 0, "children": [2]},
        {"name": "Matrix", "matrix": mat},
    ]
    scene(d, [0])
    d.write("axis.gltf")


def dup_names():
    d = Doc()
    t0 = d.image("img", WHITE)
    t1 = d.image("img", png_rgba(1, 1, (10, 20, 30, 255)))
    d.j["materials"] = [
        {"name": "M", "pbrMetallicRoughness": {"baseColorTexture": {"index": t0}}},
        {"name": "M", "pbrMetallicRoughness": {"baseColorTexture": {"index": t1}}},
        {},
    ]
    d.j["meshes"] = [{"name": "Shared", "primitives": [quad_prim(d, 0)]},
                     {"name": "Unused", "primitives": [quad_prim(d, 2)]}]
    names = ["A", "A", None, "A~2", "prefab", "a#b*?", None, "material:M"]
    nodes = []
    for i, n in enumerate(names):
        node = {}
        if n is not None:
            node["name"] = n
        if i < 2:
            node["mesh"] = 0
        nodes.append(node)
    d.j["nodes"] = nodes
    scene(d, list(range(len(nodes))))
    d.write("dup_names.gltf")


def hierarchy():
    d = Doc()
    d.j["materials"] = [{"name": "Metal", "pbrMetallicRoughness": {"metallicFactor": 1.0,
                                                                    "roughnessFactor": 0.3}}]
    d.j["meshes"] = [{"name": "Base", "primitives": [quad_prim(d, 0)]},
                     {"name": "Box", "primitives": [quad_prim(d, 0), quad_prim(d, None, dx=1)]}]
    s45, c45 = math.sin(math.pi / 4), math.cos(math.pi / 4)
    d.j["nodes"] = [
        {"name": "Root", "mesh": 0, "children": [1]},
        {"name": "Arm", "translation": [0, 1, 0], "children": [2]},
        {"name": "Hand", "translation": [0, 0, 1], "rotation": [0, s45, 0, c45],
         "scale": [0.5, 0.5, 0.5], "mesh": 1},
        {"name": "Lamp", "translation": [2, 0, 0]},
        {"name": "NotInScene", "mesh": 0},
    ]
    scene(d, [0, 3])
    d.write("hierarchy.gltf")


def texture_transform():
    d = Doc()
    t = d.image("base", WHITE)
    xf = {"KHR_texture_transform": {"offset": [0.5, 0], "scale": [2, 2]}}
    xf2 = {"KHR_texture_transform": {"offset": [0, 0.25]}}
    d.j["materials"] = [
        {"name": "Tiled", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": t, "extensions": xf}},
         "occlusionTexture": {"index": t, "extensions": xf}},
        {"name": "Mixed", "pbrMetallicRoughness": {
            "baseColorTexture": {"index": t, "extensions": xf}},
         "occlusionTexture": {"index": t, "extensions": xf2}},
    ]
    d.j["meshes"] = [{"name": "A", "primitives": [quad_prim(d, 0)]},
                     {"name": "B", "primitives": [quad_prim(d, 1)]}]
    d.j["nodes"] = [{"name": "A", "mesh": 0}, {"name": "B", "mesh": 1}]
    d.j["extensionsUsed"] = ["KHR_texture_transform"]
    scene(d, [0, 1])
    d.write("texture_transform.gltf")


def quantized():
    d = Doc()
    attrs = {
        "POSITION": d.acc(I16, "VEC3", [0, 0, 0, 100, 0, 0, 0, 100, 0], minmax=True, stride=8),
        "NORMAL": d.acc(I8, "VEC3", [0, 0, 127] * 3, normalized=True, stride=4),
        "TEXCOORD_0": d.acc(U16, "VEC2", [0, 0, 65535, 0, 0, 65535], normalized=True),
    }
    d.j["meshes"] = [{"name": "Q", "primitives": [{"attributes": attrs}]}]
    d.j["nodes"] = [{"name": "Q", "mesh": 0, "scale": [0.01, 0.01, 0.01]}]
    d.j["extensionsUsed"] = ["KHR_mesh_quantization"]
    d.j["extensionsRequired"] = ["KHR_mesh_quantization"]
    scene(d, [0])
    d.write("quantized.gltf")


def compression(ext, name):
    d = Doc()
    d.j["meshes"] = [{"primitives": [{"attributes": {
        "POSITION": d.acc(FLOAT, "VEC3", TRI_POS, minmax=True)}}]}]
    d.j["nodes"] = [{"mesh": 0}]
    d.j["extensionsUsed"] = [ext]
    d.j["extensionsRequired"] = [ext]
    scene(d, [0])
    d.write(name)


def skinned():
    d = Doc()
    attrs = {
        "POSITION": d.acc(FLOAT, "VEC3", TRI_POS, minmax=True),
        "JOINTS_0": d.acc(U8, "VEC4", [0, 1, 0, 0] * 3),
        "WEIGHTS_0": d.acc(FLOAT, "VEC4", [0.75, 0.25, 0, 0] * 3),
    }
    ibm = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1,
           1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, -1, 0, 1]
    d.j["meshes"] = [{"name": "Skinned", "primitives": [{"attributes": attrs}]}]
    d.j["nodes"] = [{"name": "Body", "mesh": 0, "skin": 0},
                    {"name": "Hip", "children": [2]},
                    {"name": "Knee", "translation": [0, 1, 0]}]
    d.j["skins"] = [{"name": "Rig", "joints": [1, 2], "skeleton": 1,
                     "inverseBindMatrices": d.acc(FLOAT, "MAT4", ibm)}]
    times = d.acc(FLOAT, "SCALAR", [0, 1], minmax=True)
    d.j["animations"] = [{"name": "Walk", "samplers": [
        {"input": times, "output": d.acc(FLOAT, "VEC3", [0, 1, 0, 0, 0, 1])},
        {"input": times, "output": d.acc(FLOAT, "VEC4", [0, 0, 0, 1, 0, 0.7071068, 0, 0.7071068]),
         "interpolation": "STEP"}],
        "channels": [{"sampler": 0, "target": {"node": 2, "path": "translation"}},
                     {"sampler": 1, "target": {"node": 1, "path": "rotation"}}]}]
    scene(d, [0, 1])
    d.write("skinned.gltf")


# One file per KHR material extension: one triangle and one material that uses it with
# non-default values. Textures are given where the extension has a slot, so the usage inference
# (srgb / normal / mask) is covered too.
def khr(ext, fields, tex_slots=(), core=None):
    d = Doc()
    t_col = d.image("col", WHITE)
    t_lin = d.image("lin", png_rgba(1, 1, (40, 80, 120, 200)))
    t_nrm = d.image("nrm", NORMAL)
    pick = {"col": t_col, "lin": t_lin, "nrm": t_nrm}
    e = dict(fields)
    for key, which in tex_slots:
        e[key] = {"index": pick[which]}
    mat = {"name": ext, "extensions": {ext: e}}
    if core:
        mat.update(core)
    d.j["materials"] = [mat]
    prim = quad_prim(d, 0)
    d.j["meshes"] = [{"name": "M", "primitives": [prim]}]
    d.j["nodes"] = [{"name": "N", "mesh": 0}]
    d.j["extensionsUsed"] = [ext]
    scene(d, [0])
    d.write("khr_%s.gltf" % ext[len("KHR_materials_"):])


def khr_all():
    khr("KHR_materials_emissive_strength", {"emissiveStrength": 5.0},
        core={"emissiveFactor": [1, 0.5, 0.25]})
    khr("KHR_materials_ior", {"ior": 1.33})
    khr("KHR_materials_specular", {"specularFactor": 0.5, "specularColorFactor": [1, 0.5, 0.25]},
        [("specularTexture", "lin"), ("specularColorTexture", "col")])
    khr("KHR_materials_clearcoat", {"clearcoatFactor": 0.75, "clearcoatRoughnessFactor": 0.125},
        [("clearcoatTexture", "lin"), ("clearcoatRoughnessTexture", "lin"),
         ("clearcoatNormalTexture", "nrm")])
    khr("KHR_materials_sheen", {"sheenColorFactor": [0.5, 0.25, 1], "sheenRoughnessFactor": 0.5},
        [("sheenColorTexture", "col"), ("sheenRoughnessTexture", "lin")])
    khr("KHR_materials_transmission", {"transmissionFactor": 0.9},
        [("transmissionTexture", "lin")])
    khr("KHR_materials_volume", {"thicknessFactor": 2.0, "attenuationDistance": 0.5,
                                 "attenuationColor": [0.5, 0.75, 1]},
        [("thicknessTexture", "lin")])
    khr("KHR_materials_diffuse_transmission",
        {"diffuseTransmissionFactor": 0.25, "diffuseTransmissionColorFactor": [1, 0.5, 0]},
        [("diffuseTransmissionTexture", "lin"), ("diffuseTransmissionColorTexture", "col")])
    khr("KHR_materials_anisotropy", {"anisotropyStrength": 0.6, "anisotropyRotation": 1.5},
        [("anisotropyTexture", "lin")])
    khr("KHR_materials_iridescence", {"iridescenceFactor": 1.0, "iridescenceIor": 1.8,
                                      "iridescenceThicknessMinimum": 200,
                                      "iridescenceThicknessMaximum": 800},
        [("iridescenceTexture", "lin"), ("iridescenceThicknessTexture", "lin")])
    khr("KHR_materials_unlit", {}, core={"pbrMetallicRoughness": {"baseColorFactor": [0, 1, 0, 1]}})


if __name__ == "__main__":
    two_materials()
    strips_fans()
    normals_tangents()
    axis()
    dup_names()
    hierarchy()
    texture_transform()
    quantized()
    compression("KHR_draco_mesh_compression", "draco_required.gltf")
    compression("EXT_meshopt_compression", "meshopt_required.gltf")
    skinned()
    khr_all()
