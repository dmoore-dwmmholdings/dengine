"""Own-made CC0 binary fixtures, generated without Aseprite or third-party artwork.

These are parser oracles. They are not Aseprite-export PNG reference qualification.
Run from any directory; output is deterministic and each file is under 50 KiB.
"""
from pathlib import Path
import struct
import zlib


def pack(fmt, *values):
    return struct.pack("<" + fmt, *values)


def string(value):
    data = value.encode("utf-8")
    return pack("H", len(data)) + data


def chunk(kind, data):
    return pack("IH", len(data) + 6, kind) + data


def frame(chunks, ms=100, extended=False):
    data = b"".join(chunks)
    return pack("IHHH2xI", len(data) + 16, 0xF1FA,
                0xFFFF if extended else len(chunks), ms,
                len(chunks) if extended else 0) + data


def layer(name="Body", flags=1, kind=0, level=0, opacity=255, blend=0):
    return chunk(0x2004, pack("6HB3x", flags, kind, level, 0, 0, blend, opacity)
                 + string(name) + (pack("I", 0) if kind == 2 else b""))


def cel(data, depth=32, kind=0, index=0, linked=0, x=0, y=0, z=0, opacity=255, height=1):
    head = pack("HhhBHh5x", index, x, y, opacity, kind, z)
    if kind == 1:
        body = pack("H", linked)
    else:
        body = pack("HH", len(data) // (depth // 8) // height, height)
        body += zlib.compress(data, level=9) if kind == 2 else data
    return chunk(0x2005, head + body)


def palette(colors, first=0):
    return chunk(0x2019, pack("III8x", first + len(colors), first, first + len(colors) - 1)
                 + b"".join(pack("H4B", 0, *color) for color in colors))


def user(text):
    return chunk(0x2020, pack("I", 1) + string(text))


def document(frames, depth=32, width=2, height=1, flags=3):
    body = b"".join(frames)
    header = bytearray(128)
    struct.pack_into("<I5HIH", header, 0, 128 + len(body), 0xA5E0,
                     len(frames), width, height, depth, flags, 100)
    header[26] = 0
    return bytes(header) + body


def main():
    out = Path(__file__).parent
    tags = chunk(0x2018, pack("H8x", 4) + b"".join(
        pack("HHBH10x", 0, 1, i, i) + string(name)
        for i, name in enumerate(["walk", "back", "bounce", "back-bounce"])))
    slices = chunk(0x2022, pack("III", 2, 3, 0) + string("panel")
                   + pack("IiiIIiiIIii", 0, -1, 0, 2, 1, 0, 0, 2, 1, 1, -2)
                   + pack("IiiIIiiIIii", 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0))
    files = {
        "rgba.aseprite": document([
            frame([layer("Group", kind=1, opacity=128), layer(level=1), user("layer-note"),
                   layer("Hidden", flags=0),
                   palette([(0, 0, 0, 0)]), user("sprite-note"),
                   cel(bytes([255, 0, 0, 255, 0, 0, 255, 128]), index=1), user("cel-note"),
                   cel(bytes([0, 255, 0, 255]), index=2), tags,
                   *[user(f"tag-{i}") for i in range(4)], slices, user("slice-note")]),
            frame([cel(b"", kind=1, index=1, linked=0, x=-1, z=2, opacity=200)], ms=200)]),
        "indexed.aseprite": document([
            frame([layer(), chunk(0x0004, pack("HBB3B", 1, 0, 1, 1, 2, 3)),
                   palette([(0, 0, 0, 0), (255, 0, 0, 255), (0, 0, 255, 128)]),
                   cel(bytes([1, 0]), depth=8, kind=2)]),
            frame([palette([(0, 255, 0, 255)], first=1), cel(b"", kind=1, linked=0)])], depth=8),
        "gray.aseprite": document([frame([layer(), cel(bytes([129, 64]), depth=16)])], depth=16, width=1),
        "legacy6.aseprite": document([frame([layer(), chunk(0x000b, pack("HBB6B", 1, 0, 2, 0, 32, 63, 63, 32, 0)),
                                            cel(bytes([1, 0]), depth=8)])], depth=8),
        "legacy8.aseprite": document([frame([layer(), chunk(0x0004, pack("HBB6B", 1, 0, 2, 0, 123, 255, 255, 123, 0)),
                                            cel(bytes([1, 0]), depth=8)])], depth=8),
        "mixed-palette.aseprite": document([
            frame([layer(), chunk(0x0004, pack("HBB6B", 1, 0, 2, 0, 0, 0, 255, 0, 0)),
                   cel(bytes([1, 0]), depth=8)]),
            frame([palette([(0, 255, 0, 255)], first=1), cel(b"", kind=1, linked=0)]),
            frame([chunk(0x0004, pack("HBB3B", 1, 1, 1, 0, 0, 255)),
                   cel(b"", kind=1, linked=0)])], depth=8),
        "palette-packets.aseprite": document([frame([
            layer(), chunk(0x0004, pack("H", 2) + bytes([1, 2, 255, 0, 0, 0, 0, 255, 4, 1, 0, 255, 0])),
            cel(bytes([2, 5]), depth=8)])], depth=8),
        "cycle.aseprite": document([frame([layer(), cel(b"", kind=1, linked=1)]), frame([cel(b"", kind=1, linked=0)])]),
        "forward.aseprite": document([frame([layer(), cel(b"", kind=1, linked=1)]), frame([cel(bytes([11, 22, 33, 255]))])]),
        "missing-link.aseprite": document([frame([layer(), cel(b"", kind=1, linked=1)]), frame([])]),
    }
    red, green, blue = bytes([255, 0, 0, 255]), bytes([0, 255, 0, 255]), bytes([0, 0, 255, 255])
    nested = [frame([
        layer("Background", flags=9), layer("Outer", kind=1, opacity=128),
        layer("Red", level=1), layer("Inner", kind=1, level=1, opacity=128),
        layer("Green", level=2), layer("Hidden parent", kind=1, flags=0),
        layer("Hidden child", level=1), cel(blue * 4, height=2),
        cel(red, index=2), cel(green, index=4), cel(red, index=6, x=1, y=1)]),
        frame([cel(b"", kind=1, linked=0), cel(b"", kind=1, index=2, linked=0, x=-1),
               cel(b"", kind=1, index=4, linked=0, x=1, y=1)])]
    files["nested.aseprite"] = document(nested, height=2)
    files["group-flag.aseprite"] = document(nested, height=2, flags=2)
    files["background-opacity.aseprite"] = document([frame([
        layer("Background", flags=9, opacity=5, blend=1), cel(red)])], width=1)
    files["transparent-rgb.aseprite"] = document([
        frame([layer(), cel(red + bytes(4) + bytes([123, 45, 67, 0]), x=1, y=1)]),
        frame([cel(bytes(12), x=1, y=1)])], width=5, height=3)
    files["z-order.aseprite"] = document([
        frame([layer("Red"), layer("Hidden", flags=0), layer("Blue"),
               cel(red, z=1), cel(blue, index=2)]),
        frame([cel(b"", kind=1, linked=0, z=2), cel(b"", kind=1, index=2, linked=0)]),
        frame([cel(b"", kind=1, linked=0), cel(b"", kind=1, index=2, linked=0, z=-2)]),
        frame([cel(b"", kind=1, linked=0), cel(b"", kind=1, index=2, linked=0, z=-1)])], width=1)
    grouped = [frame([layer("Group", kind=1), layer("Red", level=1), layer("Blue"),
                      cel(red, index=1, z=100), cel(blue, index=2)])]
    files["group-order.aseprite"] = document(grouped, width=1)
    files["legacy-group-order.aseprite"] = document(grouped, width=1, flags=1)
    files["indexed-background.aseprite"] = document([frame([
        layer("Background", flags=9), layer("Foreground"),
        palette([(255, 0, 0, 255), (0, 255, 0, 255)]),
        cel(bytes([0, 0]), depth=8), cel(bytes([0, 1]), depth=8, index=1)])], depth=8)
    files["warnings.aseprite"] = document([frame([
        layer("Multiply", blend=1), layer("Hidden blend", flags=0, blend=2),
        layer("Tiles", kind=2), cel(red)])], width=1)
    files["alpha.aseprite"] = document([frame([
        layer("Red"), layer("Blue"), cel(bytes([255, 0, 0, 128])),
        cel(bytes([0, 0, 255, 128]), index=1)])], width=1)
    files["deep.aseprite"] = document([frame([
        *[layer("G", kind=1, level=i) for i in range(127)],
        layer("Leaf", level=127), cel(red, index=127)])], width=1)
    files["atlas.aseprite"] = document([
        frame([layer(), cel(bytes([0, 0, 0, 0] * 5 + [255, 0, 0, 255, 0, 255, 0, 255]
                                  + [0, 0, 0, 0] * 5), height=3, x=2, y=1)]),
        frame([cel(red + green + blue, height=3, x=5, y=2)], ms=123),
        frame([cel(red + green + blue + bytes([255, 255, 0, 128]), height=2, y=3)]),
        frame([cel(bytes([123, 45, 67, 0] * 4), height=2, x=-1, y=-1)]),
        frame([cel(red + green + blue + red + green + blue, x=1, y=5)]),
        frame([cel(b"", kind=1, linked=0, x=-1, y=-1)], ms=250)], width=8, height=6)
    varied = []
    for i in range(24):
        w, h = i * 5 % 7 + 1, i * 3 % 5 + 1
        pixels = bytes(v for y in range(h) for x in range(w)
                       for v in [i * 11 % 256, x * 23, y * 31, 255])
        varied.append(frame(([layer()] if i == 0 else []) +
                            [cel(pixels, height=h, x=(9 - w) // 2, y=(9 - h) // 2)], ms=i + 1))
    files["atlas-grid.aseprite"] = document(varied, width=9, height=9)
    playback_tags = chunk(0x2018, pack("H8x", 26) + b"".join(
        pack("HHBH10x", 0, 3, direction, repeats) + string(f"d{direction}-r{repeats}")
        for direction in range(4) for repeats in range(5))
        + pack("HHBH10x", 0, 3, 2, 0) + string("walk")
        + b"".join(pack("HHBH10x", 1, 1, direction, 3) + string(f"single{direction}")
                   for direction in range(4))
        + pack("HHBH10x", 1, 1, 3, 0) + string("single-loop"))
    playback_slices = chunk(0x2022, pack("III", 3, 3, 0) + string("origin")
        + pack("IiiIIiiIIii", 0, 1, 1, 6, 4, 1, 1, 4, 2, 2, 2)
        + pack("IiiIIiiIIii", 2, 2, -1, 5, 5, 1, 2, 3, 2, 1, 3)
        + pack("IiiIIiiIIii", 3, 0, 0, 0, 4, 0, 0, 0, 0, 0, 0))
    late_slice = chunk(0x2022, pack("III", 1, 1, 0) + string("late")
        + pack("IiiIIiiII", 1, -5, 8, 3, 2, -1, 0, 4, 2))
    playback = []
    for i, ms in enumerate((7, 13, 29, 41)):
        metadata = [layer("Body"), layer("Hidden", flags=0), playback_tags,
                    playback_slices, late_slice] if i == 0 else []
        playback.append(frame(metadata + [cel(bytes([(i + 1) * 40, 0, 0, 255]), x=i + 1, y=2),
                                         cel(bytes([0, 255, 0, 255]), index=1)], ms=ms))
    files["playback.aseprite"] = document(playback, width=8, height=6)
    # DIDE import settings add metadata to slices that authored no optional fields.
    bare_slices = [chunk(0x2022, pack("III", 2, 0, 0) + string(name)
                        + pack("IiiII", 0, -1, 2, 8, 6)
                        + pack("IiiII", 1, 3, -2, 4, 5))
                   for name in ["bare.with.dots", "long." + "n" * 2048]]
    files["slice-overrides.aseprite"] = document([
        frame([layer(), cel(bytes([123, 45, 67, 255])), *bare_slices]),
        frame([cel(b"", kind=1, linked=0)])], width=8, height=6)
    # Independent little-endian DSCP v2 seed for the shared cooked-format fuzz reader.
    dscp = bytearray(248)
    dscp[:4] = b"DSCP"
    struct.pack_into("<10I", dscp, 4, 2, len(dscp), 1, 1, 0, 96, 160, 184, 4, 4)
    struct.pack_into("<QQII f II", dscp, 48, 1, 17, 4, 4, 100.0, 1, 184)
    struct.pack_into("<4fI2fI4I4f", dscp, 96, .25, .25, .5, .5, 17, .5, .5, 0,
                     1, 1, 2, 2, .02, .02, .5, .5)
    struct.pack_into("<Q4I", dscp, 160, 2, 0, 0, 3, 3)
    struct.pack_into("<QIIiiIIiiIIii", dscp, 184, 3, 0, 3, 0, 0, 4, 4, 1, 1, 2, 2, 1, 1)
    files["geometry-v2.dscp"] = bytes(dscp)
    for name, data in files.items():
        assert len(data) < 50 * 1024
        (out / name).write_bytes(data)


if __name__ == "__main__":
    main()
