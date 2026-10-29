"""Deterministic three-joint, transformed skinned-box fixture for actual editor inspection."""
import base64
import json
import math
import pathlib
import struct

blob = bytearray()
views = []
accessors = []


def stream(values, fmt, kind, count, component=5126, **extra):
    while len(blob) % 4:
        blob.append(0)
    start = len(blob)
    blob.extend(struct.pack("<" + fmt * len(values), *values))
    views.append({"buffer": 0, "byteOffset": start, "byteLength": len(blob) - start})
    accessors.append({"bufferView": len(views) - 1, "componentType": component,
                      "count": count, "type": kind, **extra})
    return len(accessors) - 1


positions = [(x, y, z) for z in (-0.25, 0.25) for y in (0, 1) for x in (-0.35, 0.35)]
position = stream([v for p in positions for v in p], "f", "VEC3", 8,
                  min=[-0.35, 0, -0.25], max=[0.35, 1, 0.25])
indices = stream([0, 2, 3, 0, 3, 1, 4, 5, 7, 4, 7, 6, 0, 1, 5, 0, 5, 4,
                  2, 6, 7, 2, 7, 3, 0, 4, 6, 0, 6, 2, 1, 3, 7, 1, 7, 5],
                 "H", "SCALAR", 36, component=5123)
joints = stream([0, 0, 0, 0] * 8, "B", "VEC4", 8, component=5121)
weights = stream([1, 0, 0, 0] * 8, "f", "VEC4", 8)
inverse = []
for x, y in [(0, 0.5), (0, 1.8), (0.6, 1.4)]:
    matrix = [0.0] * 16
    for i in (0, 5, 10, 15):
        matrix[i] = 1
    matrix[12:15] = [0.4 - x, -y, 0]
    inverse.extend(matrix)
bind = stream(inverse, "f", "MAT4", 3)
gltf = {
    "asset": {"version": "2.0", "generator": "DEngine model frame fixture"},
    "scene": 0, "scenes": [{"nodes": [0]}],
    "nodes": [
        {"name": "RigRoot", "translation": [2, 0.5, 0], "scale": [2, 1.2, 0.8],
         "rotation": [0, 0, math.sin(0.2), math.cos(0.2)], "children": [1, 2]},
        {"name": "Body", "mesh": 0, "skin": 0, "translation": [0.4, 0, 0]},
        {"name": "RootJoint", "translation": [0, 0.5, 0], "children": [3, 4]},
        {"name": "TipJoint", "translation": [0, 1.3, 0]},
        {"name": "SideJoint", "translation": [0.6, 0.9, 0]},
    ],
    "meshes": [{"name": "SkinnedBox", "primitives": [{"attributes": {
        "POSITION": position, "JOINTS_0": joints, "WEIGHTS_0": weights},
        "indices": indices, "material": 0}]}],
    "skins": [{"name": "Rig", "joints": [2, 3, 4], "skeleton": 2,
               "inverseBindMatrices": bind}],
    "materials": [{"name": "Blue", "pbrMetallicRoughness": {
        "baseColorFactor": [0.15, 0.5, 0.7, 1], "metallicFactor": 0, "roughnessFactor": 0.7}}],
    "buffers": [{"byteLength": len(blob), "uri": "data:application/octet-stream;base64," +
                 base64.b64encode(blob).decode()}],
    "bufferViews": views, "accessors": accessors,
}
pathlib.Path(__file__).with_name("rig.gltf").write_text(json.dumps(gltf, indent=2) + "\n")
