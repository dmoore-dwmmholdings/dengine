#!/usr/bin/env python3
"""Writes the texture importer fixtures (DENG-98) into this directory.

    python3 gen_fixtures.py [--basisu <path to the basisu tool>]

Every file is at most 64x64 and is written by hand-rolled encoders (PNG, EXR, KTX2), except
the JPEG (Pillow) and the two Basis KTX2 files (the `basisu` tool of Basis Universal 2_50, the
version pinned in third_party/vendor.toml; without --basisu they are left as they are). The
pixel patterns are deterministic, so the tests can compute what to expect.

Files:
  rgba8.png         16x16 RGBA 8-bit gradient, alpha 255 except the top-left 4x4 (alpha 0)
  gray8.png         8x8 grey 8-bit ramp
  gray_alpha8.png   8x8 grey + alpha 8-bit
  rgb16.png         8x8 RGB 16-bit gradient
  rgba16.png        8x8 RGBA 16-bit
  palette.png       8x8 palette (4 colours)
  npot.png          5x3 RGB 8-bit
  checker.png       2x2 RGB checker of 0 and 255
  normal.png        32x32 RGB normal map of a bump (tangent space, [0,1] encoded)
  cutout.png        64x64 RGBA: alpha is a disc with soft edges (alpha-test coverage)
  proxy_*.png      8x8 RGBA: donut, disconnected islands, and over-budget isolated pixels
  photo.jpg         16x16 RGB JPEG (quality 95)
  half_rgb.exr      8x8 half RGB, no compression
  float_rgba.exr    8x8 float RGBA, ZIP compression (zlib, 16-line blocks)
  bc7.ktx2          16x16 BC7 sRGB, 5 levels (16 to 1), payload bytes are a counter pattern
  bc4_zstd.ktx2     8x8 BC4, 4 levels, zstd supercompression
  rgba8_srgb.ktx2   4x4 R8G8B8A8_SRGB, one level
  rgba16f.ktx2      4x4 R16G16B16A16_SFLOAT, one level, zstd
  cube_rgba8.ktx2   4x4 cube map (6 faces) R8G8B8A8_UNORM, one level
  uastc.ktx2        16x16 UASTC LDR 4x4 (zstd), from rgba8.png (basisu)
  etc1s.ktx2        16x16 ETC1S (BasisLZ), from rgba8.png (basisu)
"""
import argparse
import os
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))


def out(name):
    return os.path.join(HERE, name)


# ---- PNG ------------------------------------------------------------------------------------

def png_chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))


def write_png(name, w, h, color_type, bit_depth, rows, palette=None):
    """rows: list of bytes, one per row, without the filter byte."""
    ihdr = struct.pack(">IIBBBBB", w, h, bit_depth, color_type, 0, 0, 0)
    raw = b"".join(b"\x00" + r for r in rows)
    data = b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", ihdr)
    if palette is not None:
        data += png_chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    data += png_chunk(b"IDAT", zlib.compress(raw, 9)) + png_chunk(b"IEND", b"")
    open(out(name), "wb").write(data)


def pngs():
    # rgba8.png: r = x * 17, g = y * 17, b = 128, a = 0 in the top-left 4x4 else 255
    rows = []
    for y in range(16):
        r = bytearray()
        for x in range(16):
            r += bytes([x * 17, y * 17, 128, 0 if (x < 4 and y < 4) else 255])
        rows.append(bytes(r))
    write_png("rgba8.png", 16, 16, 6, 8, rows)
    write_png("gray8.png", 8, 8, 0, 8, [bytes([(x + y * 8) * 4 for x in range(8)]) for y in range(8)])
    write_png("gray_alpha8.png", 8, 8, 4, 8,
              [b"".join(bytes([x * 32, y * 32]) for x in range(8)) for y in range(8)])
    write_png("rgb16.png", 8, 8, 2, 16,
              [b"".join(struct.pack(">HHH", x * 9362, y * 9362, 65535) for x in range(8))
               for y in range(8)])
    write_png("rgba16.png", 8, 8, 6, 16,
              [b"".join(struct.pack(">HHHH", x * 9362, 0, y * 9362, 32768) for x in range(8))
               for y in range(8)])
    pal = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]
    write_png("palette.png", 8, 8, 3, 8, [bytes([(x + y) % 4 for x in range(8)]) for y in range(8)],
              palette=pal)
    write_png("npot.png", 5, 3, 2, 8,
              [b"".join(bytes([x * 60, y * 120, 7]) for x in range(5)) for y in range(3)])
    write_png("checker.png", 2, 2, 2, 8,
              [bytes([0, 0, 0, 255, 255, 255]), bytes([255, 255, 255, 0, 0, 0])])
    # normal.png: a smooth bump, n = normalize(-dh/dx, -dh/dy, 1), stored as n * 0.5 + 0.5.
    import math
    rows = []
    for y in range(32):
        r = bytearray()
        for x in range(32):
            dx, dy = (x + 0.5 - 16) / 6.0, (y + 0.5 - 16) / 6.0
            g = math.exp(-(dx * dx + dy * dy))
            nx, ny, nz = 2 * dx * g, 2 * dy * g, 1.0
            l = math.sqrt(nx * nx + ny * ny + nz * nz)
            r += bytes([round((v / l * 0.5 + 0.5) * 255) for v in (nx, ny, nz)])
        rows.append(bytes(r))
    write_png("normal.png", 32, 32, 2, 8, rows)
    # cutout.png: alpha = clamp((20 - distance to centre) / 4 + 0.5), white colour.
    rows = []
    for y in range(64):
        r = bytearray()
        for x in range(64):
            d = math.hypot(x + 0.5 - 32, y + 0.5 - 32)
            a = min(1.0, max(0.0, (20.0 - d) / 4.0 + 0.5))
            r += bytes([255, 255, 255, round(a * 255)])
        rows.append(bytes(r))
    write_png("cutout.png", 64, 64, 6, 8, rows)
    proxy_topology()


def proxy_topology():
    for kind in ("donut", "islands", "budget"):
        rows = []
        for y in range(8):
            row = bytearray()
            for x in range(8):
                if kind == "donut":
                    solid = 1 <= x < 7 and 1 <= y < 7 and not (3 <= x < 5 and 3 <= y < 5)
                elif kind == "islands":
                    solid = (1 <= x < 3 and 1 <= y < 3) or (5 <= x < 7 and 4 <= y < 7)
                else:
                    solid = x % 2 == 0 and y % 2 == 0
                row += bytes([255, 255, 255, 255 if solid else 0])
            rows.append(bytes(row))
        write_png("proxy_" + kind + ".png", 8, 8, 6, 8, rows)


def jpeg():
    from PIL import Image
    img = Image.new("RGB", (16, 16))
    img.putdata([(x * 16, y * 16, 96) for y in range(16) for x in range(16)])
    img.save(out("photo.jpg"), quality=95, subsampling=0, optimize=False)


# ---- EXR ------------------------------------------------------------------------------------

HALF, FLOAT = 1, 2


def exr_attr(name, typ, data):
    return name.encode() + b"\0" + typ.encode() + b"\0" + struct.pack("<i", len(data)) + data


def zip_encode(raw):
    # OpenEXR ZIP: split even and odd bytes, delta-encode, then zlib.
    t = bytes(raw[0::2]) + bytes(raw[1::2])
    d = bytearray(t)
    for i in range(len(t) - 1, 0, -1):
        d[i] = (t[i] - t[i - 1] + 128 + 256) & 0xFF
    return zlib.compress(bytes(d), 9)


def write_exr(name, w, h, channels, pixel, compression):
    """channels: names in any order; pixel(x, y, c) -> float; compression 0 none, 3 zip."""
    names = sorted(channels)
    chlist = b""
    for n in names:
        pixel_type = channels[n]
        chlist += n.encode() + b"\0" + struct.pack("<iB3xii", pixel_type, 0, 1, 1)
    chlist += b"\0"
    hdr = b"\x76\x2f\x31\x01" + struct.pack("<I", 2)
    hdr += exr_attr("channels", "chlist", chlist)
    hdr += exr_attr("compression", "compression", bytes([compression]))
    box = struct.pack("<iiii", 0, 0, w - 1, h - 1)
    hdr += exr_attr("dataWindow", "box2i", box)
    hdr += exr_attr("displayWindow", "box2i", box)
    hdr += exr_attr("lineOrder", "lineOrder", b"\0")
    hdr += exr_attr("pixelAspectRatio", "float", struct.pack("<f", 1.0))
    hdr += exr_attr("screenWindowCenter", "v2f", struct.pack("<ff", 0.0, 0.0))
    hdr += exr_attr("screenWindowWidth", "float", struct.pack("<f", 1.0))
    hdr += b"\0"
    lines = 16 if compression == 3 else 1
    blocks = []
    for y0 in range(0, h, lines):
        raw = b""
        for y in range(y0, min(h, y0 + lines)):
            for n in names:
                fmt = "<e" if channels[n] == HALF else "<f"
                raw += b"".join(struct.pack(fmt, pixel(x, y, n)) for x in range(w))
        data = raw
        if compression == 3:
            z = zip_encode(raw)
            data = z if len(z) < len(raw) else raw
        blocks.append(struct.pack("<ii", y0, len(data)) + data)
    table_size = 8 * len(blocks)
    offs, pos = [], len(hdr) + table_size
    for b in blocks:
        offs.append(pos)
        pos += len(b)
    blob = hdr + b"".join(struct.pack("<Q", o) for o in offs) + b"".join(blocks)
    open(out(name), "wb").write(blob)


def exrs():
    # half_rgb.exr: R = x / 4, G = y / 4, B = 2.5 (HDR above 1)
    write_exr("half_rgb.exr", 8, 8, {"R": HALF, "G": HALF, "B": HALF},
              lambda x, y, c: {"R": x / 4.0, "G": y / 4.0, "B": 2.5}[c], 0)
    # float_rgba.exr: R = x + 0.25, G = -y, B = 1000, A = 0.5
    write_exr("float_rgba.exr", 8, 8, {"R": FLOAT, "G": FLOAT, "B": FLOAT, "A": FLOAT},
              lambda x, y, c: {"R": x + 0.25, "G": -float(y), "B": 1000.0, "A": 0.5}[c], 3)


# ---- KTX2 -----------------------------------------------------------------------------------

KTX2_ID = b"\xabKTX 20\xbb\r\n\x1a\n"


def dfd(model, transfer, block_dim, bytes_plane0, samples):
    """samples: list of (bit_offset, bit_length, channel_type)."""
    block_size = 24 + 16 * len(samples)
    words = struct.pack("<II", 0, 2 | (block_size << 16))
    words += bytes([model, 1, transfer, 0])
    words += bytes([d - 1 for d in block_dim] + [0] * (4 - len(block_dim)))
    words += bytes([bytes_plane0, 0, 0, 0, 0, 0, 0, 0])
    for off, length, ch in samples:
        words += struct.pack("<I", off | ((length - 1) << 16) | (ch << 24))
        words += b"\0\0\0\0" + struct.pack("<II", 0, 0xFFFFFFFF)
    return struct.pack("<I", 4 + len(words)) + words


def write_ktx2(name, vk, w, h, layers, faces, levels, dfd_bytes, level_data, scheme=0):
    """level_data: list (level 0 first) of the uncompressed bytes of each level."""
    n = len(level_data)
    index_end = 80 + 24 * n
    dfd_off = index_end
    pos = dfd_off + len(dfd_bytes)
    stored = []
    for d in level_data:
        if scheme == 2:
            import compression.zstd as zstd
            stored.append(zstd.compress(d, level=19))
        else:
            stored.append(d)
    # Levels are stored smallest first, each 16-byte aligned (the spec asks for lcm(4, block)).
    offsets = [0] * n
    body = b""
    for i in reversed(range(n)):
        pad = (-(pos + len(body))) % 16
        body += b"\0" * pad
        offsets[i] = pos + len(body)
        body += stored[i]
    hdr = KTX2_ID + struct.pack("<9I", vk, 1, w, h, 0, layers, faces, levels, scheme)
    hdr += struct.pack("<IIIIQQ", dfd_off, len(dfd_bytes), 0, 0, 0, 0)
    for i in range(n):
        hdr += struct.pack("<QQQ", offsets[i], len(stored[i]), len(level_data[i]))
    open(out(name), "wb").write(hdr + dfd_bytes + body)


def ktx2s():
    # bc7.ktx2: VK_FORMAT_BC7_SRGB_BLOCK (146), 16x16, 5 levels; byte i of level m = (i * 7 + m) & 255.
    levels = []
    for m in range(5):
        s = max(1, 16 >> m)
        nb = ((s + 3) // 4) ** 2
        levels.append(bytes(((i * 7 + m) & 255) for i in range(nb * 16)))
    write_ktx2("bc7.ktx2", 146, 16, 16, 0, 1, 5, dfd(134, 2, [4, 4], 16, [(0, 128, 0)]), levels)
    # bc4_zstd.ktx2: VK_FORMAT_BC4_UNORM_BLOCK (139), 8x8, 4 levels, zstd.
    levels = []
    for m in range(4):
        s = max(1, 8 >> m)
        nb = ((s + 3) // 4) ** 2
        levels.append(bytes(((i * 13 + 5 * m) & 255) for i in range(nb * 8)))
    write_ktx2("bc4_zstd.ktx2", 139, 8, 8, 0, 1, 4, dfd(131, 1, [4, 4], 8, [(0, 64, 0)]), levels,
               scheme=2)
    rgba_samples = [(0, 8, 0), (8, 8, 1), (16, 8, 2), (24, 8, 15)]
    # rgba8_srgb.ktx2: VK_FORMAT_R8G8B8A8_SRGB (43), 4x4: r = x * 85, g = y * 85, b = 0, a = 255.
    px = b"".join(bytes([x * 85, y * 85, 0, 255]) for y in range(4) for x in range(4))
    write_ktx2("rgba8_srgb.ktx2", 43, 4, 4, 0, 1, 1, dfd(1, 2, [1, 1], 4, rgba_samples), [px])
    # rgba16f.ktx2: VK_FORMAT_R16G16B16A16_SFLOAT (97), 4x4, zstd: (x, y, 4, 1).
    px = b"".join(struct.pack("<eeee", float(x), float(y), 4.0, 1.0)
                  for y in range(4) for x in range(4))
    write_ktx2("rgba16f.ktx2", 97, 4, 4, 0, 1, 1,
               dfd(1, 1, [1, 1], 8, [(0, 16, 0), (16, 16, 1), (32, 16, 2), (48, 16, 15)]), [px],
               scheme=2)
    # cube_rgba8.ktx2: VK_FORMAT_R8G8B8A8_UNORM (37), 4x4 x 6 faces: face f filled with (f * 40).
    px = b"".join(bytes([f * 40, f * 40, f * 40, 255]) * 16 for f in range(6))
    write_ktx2("cube_rgba8.ktx2", 37, 4, 4, 0, 6, 1, dfd(1, 1, [1, 1], 4, rgba_samples), [px])


def basis(tool):
    src = out("rgba8.png")
    for mode, name in (("-uastc", "uastc.ktx2"), ("-etc1s", "etc1s.ktx2")):
        subprocess.run([tool, mode, "-ktx2", src, "-output_file", out(name)], check=True,
                       stdout=subprocess.DEVNULL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--basisu", help="path to the basisu tool (Basis Universal 2_50)")
    args = ap.parse_args()
    pngs()
    jpeg()
    exrs()
    ktx2s()
    if args.basisu:
        basis(args.basisu)
    return 0


if __name__ == "__main__":
    sys.exit(main())
