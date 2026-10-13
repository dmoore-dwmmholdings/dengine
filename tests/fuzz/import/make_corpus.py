#!/usr/bin/env python3
"""Writes the seed corpora of the importer fuzz targets (DENG-106) from the unit-test fixtures.

    python3 tests/fuzz/import/make_corpus.py

Inputs: tests/unit/import/data/{gltf,fbx,texture}/ (DENG-98, DENG-101, DENG-102). Outputs:
tests/fuzz/import/corpus/{gltf,fbx,image,ktx2}/. The corpora are minimised by choice of file:
small fixtures only, one per feature. A glTF fixture with external files (`Box0.bin`, a PNG) is
packed into a self-contained GLB, because the fuzz harness serves no external files; a few
data-URI fixtures are also packed as GLB, so both containers are seeded. Running it twice gives
the same bytes.
"""
import base64
import json
import os
import shutil
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
DATA = os.path.join(ROOT, "tests", "unit", "import", "data")
OUT = os.path.join(HERE, "corpus")

# Self-contained JSON fixtures copied as they are (data URIs only).
GLTF_JSON = [
    "axis.gltf", "hierarchy.gltf", "skinned.gltf", "quantized.gltf", "strips_fans.gltf",
    "two_materials.gltf", "normals_tangents.gltf", "texture_transform.gltf", "dup_names.gltf",
    "khr_clearcoat.gltf", "khr_volume.gltf", "draco_required.gltf",
]
# Fixtures packed into GLB (external files embedded, data URIs decoded into the BIN chunk).
GLTF_GLB = ["Box.gltf", "BoxTextured.gltf", "SimpleSparseAccessor.gltf", "skinned.gltf",
            "khr_sheen.gltf"]
FBX = ["cube_yup_cm.fbx", "cube_zup_m.fbx", "cube_lefthanded.fbx", "ngon_two_mats.fbx",
       "materials.fbx", "skinned.fbx"]
IMAGE = ["checker.png", "npot.png", "palette.png", "gray8.png", "gray_alpha8.png", "rgb16.png",
         "rgba16.png", "cutout.png", "photo.jpg", "float_rgba.exr", "half_rgb.exr"]
KTX2 = ["bc4_zstd.ktx2", "bc7.ktx2", "cube_rgba8.ktx2", "etc1s.ktx2", "rgba16f.ktx2",
        "rgba8_srgb.ktx2", "uastc.ktx2"]


def read(path):
    with open(path, "rb") as f:
        return f.read()


def uri_bytes(base_dir, uri):
    if uri.startswith("data:"):
        return base64.b64decode(uri.split(",", 1)[1])
    return read(os.path.join(base_dir, uri))


def pad4(b, fill):
    return b + fill * ((4 - len(b) % 4) % 4)


def to_glb(path):
    """The glTF at `path` as one GLB: every buffer and image in the BIN chunk."""
    base_dir = os.path.dirname(path)
    doc = json.loads(read(path))
    blob = bytearray()
    offsets = []
    for buf in doc.get("buffers", []):
        data = uri_bytes(base_dir, buf["uri"])
        offsets.append(len(blob))
        blob += pad4(data, b"\0")
    views = doc.setdefault("bufferViews", [])
    for v in views:
        v["byteOffset"] = v.get("byteOffset", 0) + offsets[v.get("buffer", 0)]
        v["buffer"] = 0
    for img in doc.get("images", []):
        if "uri" not in img:
            continue
        data = uri_bytes(base_dir, img.pop("uri"))
        img["bufferView"] = len(views)
        img.setdefault("mimeType", "image/jpeg" if data[:2] == b"\xff\xd8" else "image/png")
        views.append({"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)})
        blob += pad4(data, b"\0")
    doc["buffers"] = [{"byteLength": len(blob)}]
    js = pad4(json.dumps(doc, separators=(",", ":"), sort_keys=True).encode(), b" ")
    chunks = struct.pack("<II", len(js), 0x4E4F534A) + js
    chunks += struct.pack("<II", len(blob), 0x004E4942) + bytes(blob)
    return struct.pack("<III", 0x46546C67, 2, 12 + len(chunks)) + chunks


def png_rgb8(w, h, pixel):
    """A small RGB8 PNG; pixel(x, y) gives (r, g, b)."""
    def chunk(tag, body):
        return struct.pack(">I", len(body)) + tag + body + struct.pack(">I", zlib.crc32(tag + body))
    rows = b"".join(b"\0" + bytes(c for x in range(w) for c in pixel(x, y)) for y in range(h))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


def write(kind, name, data):
    d = os.path.join(OUT, kind)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), "wb") as f:
        f.write(data)


def main():
    for kind in ("gltf", "fbx", "image", "ktx2"):
        shutil.rmtree(os.path.join(OUT, kind), ignore_errors=True)
    for n in GLTF_JSON:
        write("gltf", n, read(os.path.join(DATA, "gltf", n)))
    for n in GLTF_GLB:
        write("gltf", n.replace(".gltf", ".glb"), to_glb(os.path.join(DATA, "gltf", n)))
    for n in FBX:
        write("fbx", n, read(os.path.join(DATA, "fbx", n)))
    for n in IMAGE:
        write("image", n, read(os.path.join(DATA, "texture", n)))
    # A 2:1 image: the equirectangular source of an environment (DENG-100), when the fuzz harness
    # picks usage = "environment" for it.
    write("image", "equirect_16x8.png",
          png_rgb8(16, 8, lambda x, y: (x * 16, y * 32, 255 - x * 16)))
    for n in KTX2:
        write("ktx2", n, read(os.path.join(DATA, "texture", n)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
