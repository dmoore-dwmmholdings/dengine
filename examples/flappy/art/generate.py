"""Generates Flappy's pixel-art textures (DENG-648). Run from anywhere:

    python examples/flappy/art/generate.py

Every PNG is drawn here from the palette below (sRGB), at 1 texel per stage pixel, so the
sprites line up with the 320x400 stage at 32 pixels per metre. Standard library only.
"""

import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

SKY_TOP = (0x7F, 0xC4, 0xE6)
SKY_LOW = (0xC9, 0xEB, 0xF2)
INK = (0x17, 0x1C, 0x26)
RAIL = (0x2B, 0x36, 0x4A)
PIPE = (0x29, 0x9E, 0x8A)
PIPE_LIGHT = (0x5C, 0xC9, 0xB0)
PIPE_DARK = (0x1C, 0x7F, 0x70)
CAP = (0x1C, 0x80, 0x70)
GOLD = (0xFC, 0xC4, 0x33)
GOLD_DARK = (0xE0, 0x9E, 0x1F)
WING = (0xF0, 0x9E, 0x1F)
BEAK = (0xF2, 0x6B, 0x29)
WHITE = (0xFF, 0xFF, 0xFF)
SAND = (0xE3, 0xD1, 0x99)
STRIPE = (0xCC, 0xB8, 0x80)
GRASS = (0x6B, 0xB8, 0x4F)
GRASS_DARK = (0x4E, 0x96, 0x3A)


def png(name, w, h, pixel):
    """Writes `name`.png; pixel(x, y) gives (r, g, b) or (r, g, b, a), y down."""
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            p = pixel(x, y)
            row += bytes(p if len(p) == 4 else (*p, 255))
        rows.append(bytes(row))
    raw = zlib.compress(b"".join(rows), 9)

    def chunk(kind, data):
        c = struct.pack(">I", len(data)) + kind + data
        return c + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    data = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    data += chunk(b"IDAT", raw) + chunk(b"IEND", b"")
    with open(os.path.join(HERE, name + ".png"), "wb") as f:
        f.write(data)


def lerp(a, b, t):
    return tuple(round(a[i] + (b[i] - a[i]) * t) for i in range(3))


# Sky: a vertical gradient in 8-pixel bands (stretched over the stage).
png("sky", 4, 64, lambda x, y: lerp(SKY_TOP, SKY_LOW, (y // 8) / 7))

# Top rail.
png("rail", 4, 4, lambda x, y: RAIL if y < 3 else INK)


# Ground: grass lip, then sand with diagonal stripes; 320x50 (the stage's width).
def ground(x, y):
    if y < 3:
        return GRASS
    if y < 6:
        return GRASS_DARK if (x // 4) % 2 else GRASS
    return STRIPE if ((x + y) // 6) % 4 == 0 else SAND


png("ground", 320, 50, ground)


# Pipe body: shaded across, stretched down. 42 wide.
def pipe(x, y):
    if x < 2 or x > 39:
        return INK
    if x < 8:
        return PIPE_LIGHT
    if x > 32:
        return PIPE_DARK
    return PIPE


png("pipe", 42, 4, pipe)


# Pipe cap: 48x14 with an ink border.
def cap(x, y):
    if x < 2 or x > 45 or y < 2 or y > 11:
        return INK
    if x < 9:
        return PIPE_LIGHT
    if x > 38:
        return PIPE_DARK
    return CAP


png("cap", 48, 14, cap)

# Bird: 24x20, facing right.
BIRD = [
    "........kkkkkk..........",
    "......kkyyyyyykk........",
    ".....kyyyyyywwwwk.......",
    "....kyyyyyywwwwwwk......",
    "...kyyyyyyywwwwkwk......",
    "..kyyyyyyyywwwwkwk......",
    ".kooookyyyywwwwwwk......",
    "kooooooykyyywwwwk.......",
    "kooooooykyyyykkkkkk.....",
    "kooooooykyyykbbbbbbk....",
    ".kooookyyyykbkkkkkkk....",
    "..kkkkyyyyyykbbbbbk.....",
    "...kyyyyyyyyykkkkk......",
    "...kdyyyyyyyyyk.........",
    "....kddyyyyyyk..........",
    ".....kkdddddk...........",
    ".......kkkkk............",
    "........................",
    "........................",
    "........................",
]
COLORS = {"k": INK, "y": GOLD, "d": GOLD_DARK, "w": WHITE, "o": WING, "b": BEAK}


def bird(x, y):
    c = BIRD[y][x] if y < len(BIRD) and x < len(BIRD[y]) else "."
    return (*COLORS[c], 255) if c in COLORS else (0, 0, 0, 0)


png("bird", 24, 20, bird)


# Cloud: 32x12, rounded.
def cloud(x, y):
    bumps = [(9, 6, 6), (17, 5, 6), (24, 7, 5)]
    inside = any((x - cx) ** 2 + (y - cy) ** 2 <= r * r for cx, cy, r in bumps) or (4 <= x <= 28 and 7 <= y <= 11)
    return (255, 255, 255, 230) if inside else (0, 0, 0, 0)


png("cloud", 32, 12, cloud)
print("wrote", sorted(f for f in os.listdir(HERE) if f.endswith(".png")))
