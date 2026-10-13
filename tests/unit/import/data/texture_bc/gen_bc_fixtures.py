#!/usr/bin/env python3
"""Writes the BC compression fixtures (DENG-99) into this directory.

    python3 gen_bc_fixtures.py

Standard library only. The PNG and EXR writers are the DENG-98 ones (../texture/gen_fixtures.py,
imported). Every pattern is deterministic: value noise from a fixed integer hash, no random
module. The images are 64x64, so a BC test encodes 256 blocks per mip 0 and stays fast.

Files:
  photo.png    64x64 RGB 8-bit "photo": a sky-to-ground gradient, multi-octave value noise
               (texture at every scale), soft shading, a few hard edges and fine detail. The
               BC7 and BC1 PSNR fixture.
  mask.png     64x64 grey 8-bit (one channel): noise and a soft disc. The BC4 fixture.
  normal.png   64x64 RGB 8-bit tangent-space normal map of a noise height field ([0, 1]
               encoded). The BC5 fixture.
  cutout.png   64x64 RGBA 8-bit: the photo with alpha from noise and a hard-edged hole. BC3
               and BC7 alpha.
  hdr.exr      64x64 half RGB, ZIP: a sky from 0.05 to 4, a sun disc up to 600, ground in
               shadow from 0.002 to 0.3. The BC6H fixture.
  env.exr      32x16 half RGB, ZIP: an equirectangular (2:1) sky with a sun, for the BC6H
               environment cube (usage = "environment", DENG-100's prefilter).
  odd.png      12x8 RGBA 8-bit blend, alpha included: mips 6x4, 3x2 and 1x1 end inside
               blocks (padding).
"""
import importlib.util
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # importing ../texture/gen_fixtures.py leaves no __pycache__ there

_spec = importlib.util.spec_from_file_location(
    "dengine_texture_fixtures", os.path.join(HERE, "..", "texture", "gen_fixtures.py"))
tex = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(tex)

W = H = 64


def out(name):
    return os.path.join(HERE, name)


def hash2(x, y, seed):
    """Deterministic integer hash of a lattice point to [0, 1)."""
    h = (x * 374761393 + y * 668265263 + seed * 2147483647) & 0xFFFFFFFF
    h = ((h ^ (h >> 13)) * 1274126177) & 0xFFFFFFFF
    h ^= h >> 16
    return (h & 0xFFFFFF) / float(0x1000000)


def value_noise(x, y, scale, seed):
    """Smooth value noise with a lattice of `scale` pixels."""
    fx, fy = x / scale, y / scale
    ix, iy = int(math.floor(fx)), int(math.floor(fy))
    tx, ty = fx - ix, fy - iy
    tx = tx * tx * (3 - 2 * tx)
    ty = ty * ty * (3 - 2 * ty)
    a = hash2(ix, iy, seed)
    b = hash2(ix + 1, iy, seed)
    c = hash2(ix, iy + 1, seed)
    d = hash2(ix + 1, iy + 1, seed)
    return (a * (1 - tx) + b * tx) * (1 - ty) + (c * (1 - tx) + d * tx) * ty


def fbm(x, y, seed, octaves=(16.0, 8.0, 4.0, 2.0), gain=0.5):
    total, amp, norm = 0.0, 1.0, 0.0
    for i, s in enumerate(octaves):
        total += amp * value_noise(x, y, s, seed + i)
        norm += amp
        amp *= gain
    return total / norm


def clamp8(v):
    return max(0, min(255, int(round(v * 255.0))))


def photo_rgb(x, y):
    """Linear-ish display values in [0, 1] of the photo fixture."""
    horizon = 26 + 6 * math.sin(x / 9.0)
    n = fbm(x, y, 11)
    if y < horizon:
        # Sky: blue gradient with soft clouds.
        t = y / horizon
        cloud = max(0.0, fbm(x, y, 31, (16.0, 8.0, 4.0)) - 0.45) * 1.6
        r = 0.35 + 0.25 * t + cloud
        g = 0.55 + 0.2 * t + cloud
        b = 0.85 + 0.1 * t + cloud * 0.8
    else:
        # Ground: earth and grass with texture at every scale, shading from a slope.
        shade = 0.55 + 0.45 * fbm(x, y, 53, (8.0, 4.0, 2.0, 1.0), 0.6)
        r = (0.32 + 0.3 * n) * shade
        g = (0.42 + 0.25 * n) * shade
        b = (0.18 + 0.15 * n) * shade
        # A path with hard edges.
        if abs(x - 20 - (y - 30) * 0.6) < 3:
            r, g, b = 0.6 * shade + 0.1, 0.55 * shade + 0.1, 0.45 * shade + 0.1
    # A dark post with a bright edge (hard vertical edges).
    if 44 <= x < 48 and 12 <= y < 50:
        r, g, b = 0.12, 0.1, 0.08
        if x == 47:
            r, g, b = 0.75, 0.7, 0.6
    return min(r, 1.0), min(g, 1.0), min(b, 1.0)


def photo():
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            r, g, b = photo_rgb(x, y)
            row += bytes((clamp8(r), clamp8(g), clamp8(b)))
        rows.append(bytes(row))
    tex.write_png(out("photo.png"), W, H, 2, 8, rows)


def cutout():
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            r, g, b = photo_rgb(x, y)
            a = fbm(x, y, 71, (16.0, 8.0))
            if (x - 40) ** 2 + (y - 20) ** 2 < 100:
                a = 0.0
            row += bytes((clamp8(r), clamp8(g), clamp8(b), clamp8(a)))
        rows.append(bytes(row))
    tex.write_png(out("cutout.png"), W, H, 6, 8, rows)


def mask():
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            v = 0.15 + 0.6 * fbm(x, y, 97)
            d = math.hypot(x - 32, y - 32)
            v += 0.3 * max(0.0, 1.0 - d / 20.0)
            row.append(clamp8(min(v, 1.0)))
        rows.append(bytes(row))
    tex.write_png(out("mask.png"), W, H, 0, 8, rows)


def normal():
    def height(x, y):
        return 6.0 * fbm(x, y, 131, (16.0, 8.0, 4.0))

    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            dx = height(x + 1, y) - height(x - 1, y)
            dy = height(x, y + 1) - height(x, y - 1)
            nx, ny, nz = -dx, -dy, 2.0
            l = math.sqrt(nx * nx + ny * ny + nz * nz)
            nx, ny, nz = nx / l, ny / l, nz / l
            row += bytes((clamp8(nx * 0.5 + 0.5), clamp8(ny * 0.5 + 0.5), clamp8(nz * 0.5 + 0.5)))
        rows.append(bytes(row))
    tex.write_png(out("normal.png"), W, H, 2, 8, rows)


def hdr_rgb(x, y):
    horizon = 40
    if y < horizon:
        t = 1.0 - y / horizon
        sky = 0.05 + 3.95 * t * t
        cloud = 1.0 + 0.8 * max(0.0, fbm(x, y, 151, (16.0, 8.0, 4.0)) - 0.4)
        r, g, b = 0.6 * sky * cloud, 0.75 * sky * cloud, 1.0 * sky * cloud
        d = math.hypot(x - 44, y - 12)
        if d < 6:
            sun = 600.0 * (1.0 - d / 6.0) ** 2 + 20.0
            r, g, b = r + sun, g + sun * 0.9, b + sun * 0.7
    else:
        n = fbm(x, y, 173, (8.0, 4.0, 2.0))
        v = 0.002 + 0.3 * n * n
        r, g, b = v * 1.1, v, v * 0.7
    return r, g, b


def hdr():
    tex.write_exr(out("hdr.exr"), W, H, {"R": tex.HALF, "G": tex.HALF, "B": tex.HALF},
                  lambda x, y, c: hdr_rgb(x, y)["RGB".index(c)], 3)


def env():
    def px(x, y, c):
        # Scale the 64x64 sky to 32x16: rows 0..15 cover the sky part of hdr_rgb.
        r, g, b = hdr_rgb(x * 2, y * 5 // 2)
        return (r, g, b)["RGB".index(c)]

    tex.write_exr(out("env.exr"), 32, 16, {"R": tex.HALF, "G": tex.HALF, "B": tex.HALF}, px, 3)


def odd():
    # Colour and alpha on one line of RGBA space (a blend along x + y), which a one-subset BC7
    # block holds well: the test looks at padding, not at a hard case for BC.
    rows = []
    for y in range(8):
        row = bytearray()
        for x in range(12):
            t = (x + y) / 18.0
            row += bytes((clamp8(0.15 + 0.7 * t), clamp8(0.25 + 0.5 * t), clamp8(0.8 - 0.6 * t),
                          clamp8(1.0 - 0.45 * t)))
        rows.append(bytes(row))
    tex.write_png(out("odd.png"), 12, 8, 6, 8, rows)


def main():
    photo()
    cutout()
    mask()
    normal()
    hdr()
    env()
    odd()
    return 0


if __name__ == "__main__":
    sys.exit(main())
