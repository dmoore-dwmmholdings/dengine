"""CC0 deterministic glTF: a two-material torus and a separate UV0/UV1 pyramid.
No external libraries, downloads or image dependencies. Python 3, stdlib only.
"""
import base64
import json
import math
from pathlib import Path
import struct

blob = bytearray()
views, accessors = [], []


def accessor(values, width, indices=False):
    while len(blob) % 4:
        blob.append(0)
    start = len(blob)
    flat = values if indices else [x for row in values for x in row]
    blob.extend(struct.pack("<" + ("I" if indices else "f") * len(flat), *flat))
    views.append({"buffer": 0, "byteOffset": start, "byteLength": len(blob) - start})
    info = {"bufferView": len(views) - 1, "componentType": 5125 if indices else 5126,
            "count": len(values), "type": "SCALAR" if indices else {2: "VEC2", 3: "VEC3"}[width]}
    if not indices and width == 3:
        info["min"] = [min(row[i] for row in values) for i in range(width)]
        info["max"] = [max(row[i] for row in values) for i in range(width)]
    accessors.append(info)
    return len(accessors) - 1


position, normal, uv, triangles = [], [], [], []
for y in range(9):
    v = y / 8
    for x in range(13):
        u = x / 12
        a, b = 2 * math.pi * u, 2 * math.pi * v
        radius = 0.75 + 0.25 * math.cos(b)
        position.append([radius * math.cos(a), 0.25 * math.sin(b), radius * math.sin(a)])
        normal.append([math.cos(b) * math.cos(a), math.sin(b), math.cos(b) * math.sin(a)])
        uv.append([u, v])
for y in range(8):
    for x in range(12):
        a = y * 13 + x
        triangles.extend([a, a + 13, a + 1, a + 1, a + 13, a + 14])
attributes = {"POSITION": accessor(position, 3), "NORMAL": accessor(normal, 3), "TEXCOORD_0": accessor(uv, 2)}
torus = {"name": "Torus", "primitives": [
    {"attributes": attributes, "indices": accessor(triangles[:288], 1, True), "material": 0},
    {"attributes": attributes, "indices": accessor(triangles[288:], 1, True), "material": 1}]}
pyramid = {"name": "Pyramid", "primitives": [{"attributes": {
    "POSITION": accessor([[-0.5, 0, -0.5], [0.5, 0, -0.5], [0.5, 0, 0.5], [-0.5, 0, 0.5], [0, 1.123, 0]], 3),
    "NORMAL": accessor([[0, 1, 0]] * 5, 3),
    "TEXCOORD_0": accessor([[0, 0], [1, 0], [1, 1], [0, 1], [0.5, 0.5]], 2),
    "TEXCOORD_1": accessor([[0.1, 0.1], [0.9, 0.1], [0.9, 0.9], [0.1, 0.9], [0.5, 0.7]], 2)},
    "indices": accessor([0, 2, 1, 0, 3, 2, 0, 1, 4, 1, 2, 4, 2, 3, 4, 3, 0, 4], 1, True), "material": 1}]}
doc = {"asset": {"version": "2.0", "generator": "DEngine CC0 model-inspection fixture"},
       "scene": 0, "scenes": [{"nodes": [0]}],
       "nodes": [{"name": "Root", "children": [1, 2]}, {"name": "Body", "mesh": 0},
                 {"name": "Pivot", "translation": [2, 0, 0], "children": [3]}, {"name": "Lid", "mesh": 1}],
       "meshes": [torus, pyramid],
       "materials": [{"name": "Wood", "pbrMetallicRoughness": {"baseColorFactor": [0.6, 0.3, 0.1, 1]}},
                     {"name": "Metal", "pbrMetallicRoughness": {"metallicFactor": 1, "roughnessFactor": 0.25}}],
       "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," + base64.b64encode(blob).decode()}],
       "bufferViews": views, "accessors": accessors}
Path(__file__).with_name("inspection.gltf").write_text(json.dumps(doc, indent=2) + "\n")
