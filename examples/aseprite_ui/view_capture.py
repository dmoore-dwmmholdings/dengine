"""Convert the sample's linear RGBA16F diagnostic capture to an SDR PNG (stdlib only)."""
from pathlib import Path
import argparse
import math
import struct
import zlib


def convert(source, destination):
    if source.stat().st_size > 12 + 2048 * 2048 * 8:
        raise ValueError("capture exceeds the 2048 x 2048 bound")
    data = source.read_bytes()
    width, height, pitch = struct.unpack_from("<III", data)
    if not (0 < width <= 2048 and 0 < height <= 2048 and width * 8 <= pitch <= 16384):
        raise ValueError("invalid capture dimensions or row pitch")
    if len(data) != 12 + pitch * height:
        raise ValueError("incomplete capture")
    rows = bytearray()
    for y in range(height):
        rows.append(0)  # PNG filter: None
        for x in range(width):
            for value in struct.unpack_from("<3e", data, 12 + y * pitch + x * 8):
                if not math.isfinite(value):
                    raise ValueError("capture contains nonfinite RGB")
                value = max(0.0, min(1.0, value))
                srgb = 12.92 * value if value <= 0.0031308 else 1.055 * value ** (1 / 2.4) - 0.055
                rows.append(round(srgb * 255))

    def chunk(kind, payload):
        body = kind + payload
        return struct.pack(">I", len(payload)) + body + struct.pack(">I", zlib.crc32(body))

    destination.write_bytes(b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    convert(args.source, args.destination)
