#!/usr/bin/env python3
"""Makes the FLIP validation data of test_flip (DENG-329). Run once, offline; not part of the build.

    python3 -m venv /tmp/flipvenv && /tmp/flipvenv/bin/pip install flip-evaluator==1.7 numpy
    /tmp/flipvenv/bin/python tests/golden/flip/data/make_reference.py tests/golden/flip/data

It draws the image pairs below (deterministic: integer math and a fixed LCG, no random module),
writes them as 8-bit RGB PNGs, and evaluates each pair with NVIDIA's official FLIP
(`flip_evaluator`, the Python binding of the BSD-3-Clause C++ implementation, https://github.com/
NVlabs/flip) in LDR mode at the listed pixels per degree. reference.txt gets one line per pair:
the PNG names, the PPD, and the official mean, 99th percentile and maximum error. test_flip.c
requires the C implementation (tests/golden/flip/flip.c) to give the same mean within 1e-3.

The official tool is not vendored and nothing in the build calls it.
"""
import math
import os
import struct
import sys
import zlib

import numpy as np
import flip_evaluator as flip

W, H = 96, 64  # not square, so a row/column mix-up shows


def write_png(path, rgb):
    """8-bit RGB PNG, filter 0 on every row (simple, and stb_image reads it)."""
    h, w, _ = rgb.shape
    raw = b"".join(b"\x00" + rgb[y].astype(np.uint8).tobytes() for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def lcg_noise(seed, shape, amp):
    """Integers in [-amp, amp] from a 32-bit LCG (Numerical Recipes constants)."""
    n = int(np.prod(shape))
    out = np.empty(n, dtype=np.int32)
    s = seed & 0xFFFFFFFF
    for i in range(n):
        s = (1664525 * s + 1013904223) & 0xFFFFFFFF
        out[i] = (s >> 16) % (2 * amp + 1) - amp
    return out.reshape(shape)


def clamp8(a):
    return np.clip(a, 0, 255).astype(np.uint8)


def gradient():
    y, x = np.mgrid[0:H, 0:W]
    return clamp8(np.stack([x * 255 // (W - 1), y * 255 // (H - 1), (x + y) * 255 // (W + H - 2)], -1))


def shapes(dx=0, dy=0):
    img = np.full((H, W, 3), (40, 60, 90), dtype=np.int32)
    y, x = np.mgrid[0:H, 0:W]
    disc = (x - 30 - dx) ** 2 + (y - 30 - dy) ** 2 < 18 * 18
    img[disc] = (230, 200, 40)
    box = (x >= 55 + dx) & (x < 85 + dx) & (y >= 10 + dy) & (y < 50 + dy)
    img[box] = (200, 30, 60)
    line = np.abs((x - dx) - 2 * (y - dy) + 10) < 2
    img[line] = (250, 250, 250)
    return clamp8(img)


def checker(cell=8):
    y, x = np.mgrid[0:H, 0:W]
    on = ((x // cell) + (y // cell)) % 2 == 0
    img = np.where(on[..., None], np.array([220, 220, 220]), np.array([30, 30, 30]))
    return clamp8(img)


def box_blur(img, r=1):
    acc = np.zeros(img.shape, dtype=np.int64)
    for oy in range(-r, r + 1):
        for ox in range(-r, r + 1):
            yy = np.clip(np.arange(H) + oy, 0, H - 1)
            xx = np.clip(np.arange(W) + ox, 0, W - 1)
            acc += img[yy][:, xx].astype(np.int64)
    return clamp8(acc // ((2 * r + 1) ** 2))


def patches():
    cols = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 0), (0, 255, 255), (255, 0, 255),
            (128, 128, 128), (255, 255, 255), (0, 0, 0), (200, 120, 60), (60, 120, 200), (120, 200, 60)]
    img = np.zeros((H, W, 3), dtype=np.int32)
    for i, c in enumerate(cols):
        px, py = (i % 6) * 16, (i // 6) * 32
        img[py:py + 32, px:px + 16] = c
    return clamp8(img)


def flat(c=(120, 140, 160)):
    return clamp8(np.broadcast_to(np.array(c), (H, W, 3)))


PAIRS = [
    # name, reference, test, ppd
    ("identical", gradient, gradient, 0.0),
    ("gradient_noise", gradient, lambda: clamp8(gradient().astype(np.int32) + lcg_noise(7, (H, W, 3), 12)), 0.0),
    ("shapes_shift", shapes, lambda: shapes(1, 1), 0.0),
    ("checker_blur", checker, lambda: box_blur(checker()), 0.0),
    ("patches_swap", patches, lambda: patches()[..., [1, 2, 0]], 0.0),
    ("flat_plus8", flat, lambda: flat((128, 148, 168)), 0.0),
    ("shapes_noise_ppd30", shapes, lambda: clamp8(shapes().astype(np.int32) + lcg_noise(3, (H, W, 3), 30)), 30.0),
    ("checker_shift_ppd120", checker, lambda: checker(9), 120.0),
]

DEFAULT_PPD = 0.7 * (3840.0 / 0.7) * math.pi / 180.0


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))
    lines = ["# FLIP validation pairs (DENG-329), written by make_reference.py with flip-evaluator "
             + getattr(flip, "__version__", "1.7") + ".",
             "# name reference.png test.png ppd mean p99 max  (ppd 0 = the default viewing setup, "
             + "%.6f)" % DEFAULT_PPD]
    written = set()
    for name, ref_fn, test_fn, ppd in PAIRS:
        ref, test = ref_fn(), test_fn()
        ref_name = ref_fn.__name__ + ".png" if ref_fn.__name__ != "<lambda>" else name + "_ref.png"
        test_name = name + "_test.png" if name != "identical" else ref_name
        for fname, img in ((ref_name, ref), (test_name, test)):
            if fname not in written:
                write_png(os.path.join(out, fname), img)
                written.add(fname)
        params = {"ppd": float(ppd if ppd > 0 else DEFAULT_PPD)}
        err, mean, _ = flip.evaluate(ref.astype(np.float32) / 255.0, test.astype(np.float32) / 255.0,
                                     "LDR", inputsRGB=True, applyMagma=False, computeMeanError=True,
                                     parameters=params)
        e = np.sort(np.asarray(err, dtype=np.float64).reshape(-1))
        p99 = e[int(math.ceil(0.99 * e.size)) - 1]
        lines.append("%s %s %s %.4f %.9f %.9f %.9f" % (name, ref_name, test_name, ppd, mean, p99, e[-1]))
        print(lines[-1])
    with open(os.path.join(out, "reference.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
