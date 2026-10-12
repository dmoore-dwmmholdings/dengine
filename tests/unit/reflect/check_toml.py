#!/usr/bin/env python3
"""check_toml.py: reads the text serializer's golden files with an independent TOML parser.

Run by ctest as `text.check_toml` (DENG-295) with the data directory as its argument. The C
tests in test_ser_text.c compare the writer's output with these files byte for byte, so every
file that Python's tomllib (TOML 1.0) accepts here proves that the writer emits standard TOML.

Checks, for every data/text_*.toml:
  - it is UTF-8, uses '\n' line endings, ends with '\n' and has no trailing spaces;
  - tomllib parses it;
  - the values of the known goldens are what the C tests wrote (floats bit for bit).

Exit codes: 0 ok, 1 a check failed, 77 skipped (Python older than 3.11 has no tomllib).
"""

import math
import pathlib
import struct
import sys

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    print("tomllib is not available (Python 3.11+ needed); skipping")
    sys.exit(77)

failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


def f32(x):
    """x rounded to the nearest f32, as a Python float."""
    return struct.unpack("<f", struct.pack("<f", x))[0]


def f32_bits(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def f64_bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def same_f64(a, b):
    return isinstance(a, float) and f64_bits(a) == f64_bits(b)


def same_f32(a, b):
    """a (parsed as f64) converts to the same f32 bits as b."""
    return isinstance(a, float) and f32_bits(a) == f32_bits(b)


def check_floats(name, got, want, single):
    same = same_f32 if single else same_f64
    check(
        isinstance(got, list) and len(got) == len(want) and all(same(g, w) for g, w in zip(got, want)),
        f"{name}: {got!r} != {want!r}",
    )


def check_transform(doc, where, x, scale_x=1.0):
    check(doc.get("__type") == "TTransform", f"{where}: __type")
    check(doc.get("__version") == 1, f"{where}: __version")
    check_floats(f"{where}.position", doc.get("position"), [x, -2.0, 1000000.25], False)
    check_floats(f"{where}.rotation", doc.get("rotation"),
                 [0.0, 0.0, f32(0.38268343), f32(0.92387953)], True)
    check_floats(f"{where}.scale", doc.get("scale"), [scale_x, 2.5, f32(0.001)], True)


def check_golden_transform(doc):
    check(set(doc) == {"__type", "__version", "position", "rotation", "scale"}, "transform: keys")
    check_transform(doc, "transform", 1.5)


def check_golden_entity(doc):
    check(doc.get("__type") == "TEntityData", "entity: __type")
    t = dict(doc.get("transform", {}))
    t["__type"], t["__version"] = "TTransform", 1
    check_transform(t, "entity.transform", 4.0)
    inv = doc.get("inventory", {})
    check(inv.get("count") == 3, "entity.inventory.count")
    items = inv.get("items", [])
    check(len(items) == 8, "entity.inventory.items has 8 elements")
    check(items[:3] == [{"id": 100, "count": 0}, {"id": 101, "count": 10},
                        {"id": 102, "count": 20}], "entity.inventory.items[0..2]")
    check(all(i == {"id": 0, "count": 0} for i in items[3:]), "entity.inventory.items[3..7]")
    check(doc.get("mode") == "Run", "entity.mode")


def check_all_kinds(doc):
    expect = {
        "__type": "TAllKinds", "__version": 1, "b": True, "i8v": -128, "i16v": -12345,
        "i32v": -2000000000, "i64v": -(2**63), "u8v": 255, "u16v": 65535, "u32v": 4000000000,
        "u64v": "0xffffffffffffffff", "name": "player", "str": 'say "hi"\\\n\tü\u0001',
        "entity": "0x0000000100000002", "mesh": {"id": "0x0000000000000007"},
        "mode": "Negative", "layers": ["Ground", "Air", 256],
        "vec": {"x": 1.0, "y": 2.0, "z": 3.0},
        "items": [{"id": 1, "count": -1}, {"id": 42, "count": 7}],
    }
    for k, v in expect.items():
        check(doc.get(k) == v, f"all_kinds.{k}: {doc.get(k)!r} != {v!r}")
    check(same_f32(doc.get("f32v"), f32(0.1)), "all_kinds.f32v")
    check(same_f64(doc.get("f64v"), 1.0 / 3.0), "all_kinds.f64v")
    check_floats("all_kinds.v2", doc.get("v2"), [1.0, -2.0], True)
    check_floats("all_kinds.v3", doc.get("v3"), [0.5, 0.25, -0.0], True)
    flt_max = struct.unpack("<f", struct.pack("<I", 0x7F7FFFFF))[0]
    flt_min = struct.unpack("<f", struct.pack("<I", 0x00800000))[0]
    denorm = struct.unpack("<f", struct.pack("<I", 0x00000001))[0]
    check_floats("all_kinds.v4", doc.get("v4"), [denorm, flt_max, -flt_max, flt_min], True)
    check(f32_bits(doc.get("v4", [0.0])[0]) == 1, "all_kinds.v4[0] is the smallest f32 denormal")
    check_floats("all_kinds.dv3", doc.get("dv3"), [1e300, -5e-324, 123456789.125], False)
    check_floats("all_kinds.q", doc.get("q"), [0.0, 0.0, f32(0.70710677), f32(0.70710677)], True)
    m4 = [1.0 if i % 5 == 0 else 0.0 for i in range(16)]
    m4[12], m4[13], m4[14] = 10.0, 20.5, -30.0
    check_floats("all_kinds.m4", doc.get("m4"), m4, True)
    check_floats("all_kinds.color", doc.get("color"), [1.0, 0.5, 0.25, 1.0], True)
    s = doc.get("samples", [])
    check(len(s) == 4 and s[0] == math.inf and s[1] == -math.inf and same_f32(s[2], 0.0)
          and same_f32(s[3], -1.5), f"all_kinds.samples: {s!r}")


def check_doc_multi(doc):
    check(set(doc) == {"entity", "item"}, "doc_multi: top-level keys")
    ents = doc.get("entity", [])
    check(len(ents) == 2, "doc_multi: two [[entity]] tables")
    if len(ents) == 2:
        check_transform(ents[0], "doc_multi.entity[0]", 1.0)
        check_transform(ents[1], "doc_multi.entity[1]", -1.0, scale_x=4.0)
    check(doc.get("item") == [{"__type": "TItem", "__version": 1, "id": 9, "count": -9}],
          "doc_multi: [[item]]")


SEMANTIC = {
    "text_golden_transform.toml": check_golden_transform,
    "text_golden_entity.toml": check_golden_entity,
    "text_all_kinds.toml": check_all_kinds,
    "text_doc_multi.toml": check_doc_multi,
}


def main():
    if len(sys.argv) != 2:
        print("usage: check_toml.py <data dir>")
        return 2
    data = pathlib.Path(sys.argv[1])
    files = sorted(data.glob("text_*.toml"))
    check(len(files) >= len(SEMANTIC), f"expected at least {len(SEMANTIC)} files in {data}")
    for path in files:
        raw = path.read_bytes()
        try:
            text = raw.decode("utf-8")
        except UnicodeDecodeError as e:
            failures.append(f"{path.name}: not UTF-8: {e}")
            continue
        check(b"\r" not in raw, f"{path.name}: has a CR")
        check(text.endswith("\n"), f"{path.name}: does not end with a newline")
        for n, line in enumerate(text.split("\n"), 1):
            check(line == line.rstrip(" \t"), f"{path.name}:{n}: trailing space")
        try:
            doc = tomllib.loads(text)
        except tomllib.TOMLDecodeError as e:
            failures.append(f"{path.name}: tomllib rejects it: {e}")
            continue
        if path.name in SEMANTIC:
            SEMANTIC[path.name](doc)
        print(f"ok {path.name}")
    for name in SEMANTIC:
        check((data / name).exists(), f"{name} is missing")
    for f in failures:
        print(f"FAIL {f}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
