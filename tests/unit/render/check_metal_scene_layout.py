#!/usr/bin/env python3
"""Keep checked-in native Metal adapters' structured-buffer stride in sync with Slang.

The existing GPU-scene unit checks Slang against C. This closes the second ABI boundary:
the native Metal adapters embed generated Slang, and a stale natural struct silently reads
the wrong view when its slot is nonzero.
"""
import argparse
from pathlib import Path
import re


def check(root: Path, generated: Path) -> None:
    shared = (root / "shaders/scene/scene_types.slang").read_text()
    metal = generated.read_text()
    bodies = dict(re.findall(r"\bstruct\s+(\w+)\s*\{([^}]+)\}", metal))
    body = re.search(r"\bstruct\s+DeSceneConstants\s*\{([^}]+)\}", shared).group(1)
    expected = [
        (name, kind, int(count or 1))
        for kind, name, count in re.findall(
            r"public\s+(float4|float|uint)\s+(\w+)\s*(?:\[(\d+)\])?\s*;", body
        )
    ]
    assert expected, "shared scene constants were not found"
    arrays = {}
    for name, body in bodies.items():
        match = re.fullmatch(
            r"\s*array<packed_float4,\s*int\((\d+)\)>\s+\w+\s*;\s*", body
        )
        if match:
            arrays[name] = ("float4", int(match.group(1)))
    for prefix in ("DeSceneConstants_", "DeSceneConstants_natural_"):
        names = [name for name in bodies if re.fullmatch(prefix + r"\d+", name)]
        assert len(names) == 1, f"expected one {prefix} struct, got {names}"
        fields = []
        for declaration in bodies[names[0]].split(";"):
            declaration = declaration.strip()
            if not declaration:
                continue
            array = re.fullmatch(r"array<float4,\s*int\((\d+)\)>\s+(\w+)", declaration)
            if array:
                kind, count, name = "float4", int(array.group(1)), array.group(2)
            else:
                match = re.fullmatch(r"(\w+)\s+(\w+)", declaration)
                assert match, f"unrecognized generated declaration: {declaration}"
                kind, name = match.groups()
                kind, count = arrays.get(kind, (kind.removeprefix("packed_"), 1))
            fields.append((re.sub(r"_\d+$", "", name), kind, count))
        assert fields == expected, (
            f"{generated}: {names[0]} is stale: expected {expected}, got {fields}. "
            "Regenerate this adapter with its checked-in script and the pinned Slang library."
        )
    size = sum((16 if kind == "float4" else 4) * count for _, kind, count in expected)
    print(f"{generated.name}: shared scene layout matches: {len(expected)} fields, {size} bytes")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[3])
    parser.add_argument("--generated", type=Path)
    args = parser.parse_args()
    generated = [args.generated] if args.generated else [
        path for path in args.root.glob("shaders/**/*.generated")
        if re.search(r"\bstruct\s+DeSceneConstants_\d+", path.read_text())
    ]
    assert generated, "no generated native scene-layout consumers were found"
    for path in generated:
        check(args.root, path)
