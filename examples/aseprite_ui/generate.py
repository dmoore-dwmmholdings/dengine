"""Own CC0 diagnostic artwork. Deterministic Aseprite RGBA source; no external tool needed."""
from pathlib import Path
import argparse
import struct


def pack(fmt, *args):
    return struct.pack("<" + fmt, *args)


def chunk(kind, data):
    return pack("IH", len(data) + 6, kind) + data


def string(text):
    data = text.encode("utf-8")
    return pack("H", len(data)) + data


def source():
    # Nine regions have distinct colors. Moving a stretch boundary changes pixels visibly.
    colors = [(220, 70, 50), (230, 170, 40), (80, 190, 80),
              (170, 70, 190), (35, 70, 100), (40, 170, 190),
              (220, 100, 150), (100, 120, 210), (190, 190, 190)]
    rgba = bytearray()
    for y in range(20):
        for x in range(20):
            col = 0 if x < 4 else 2 if x >= 16 else 1
            row = 0 if y < 4 else 2 if y >= 16 else 1
            rgba.extend((*colors[row * 3 + col], 255))
    layer = chunk(0x2004, pack("6HB3x", 1, 0, 0, 0, 0, 0, 255) + string("Panel"))
    cel = chunk(0x2005, pack("HhhBHh5xHH", 0, 0, 0, 255, 0, 0, 20, 20) + rgba)
    slices = chunk(0x2022, pack("III", 1, 3, 0) + string("panel")
                   + pack("IiiIIiiIIii", 0, 0, 0, 20, 20, 4, 4, 12, 12, 10, 10))
    chunks = layer + cel + slices
    frame = pack("IHHH2xI", len(chunks) + 16, 0xF1FA, 3, 100, 0) + chunks
    header = bytearray(128)
    struct.pack_into("<I5HIH", header, 0, 128 + len(frame), 0xA5E0, 1, 20, 20, 32, 3, 100)
    return bytes(header) + frame


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    output = Path(__file__).with_name("panel.aseprite")
    data = source()
    if args.check:
        if output.read_bytes() != data:
            raise SystemExit("panel.aseprite differs; run generate.py")
    else:
        output.write_bytes(data)
