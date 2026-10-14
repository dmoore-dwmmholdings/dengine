#!/usr/bin/env python3
"""Check that every executable deployed with the D3D12 Agility runtime exports it (DENG-423).

The D3D12 loader uses the Agility runtime in <exe dir>/D3D12/ only when the .exe exports
D3D12SDKVersion and D3D12SDKPath (src/gpu/d3d12/d3d12_agility_exports.h). An executable that
gets the DLLs (de_d3d12_agility_deploy) but not the exports silently runs on the OS runtime,
where even CI's vendored WARP lacks shader model 6.6 (DENG-420).

    check_agility.py --manifest <build>/de_d3d12_agility_deployed_<config>.txt [--expect-some]

The manifest lists the executables de_d3d12_agility_deploy() deployed, one path per line
(third_party/d3d12_agility/CMakeLists.txt writes it). The check reads each one's PE export table
and fails (exit 1) listing those without both exports, or not built, or that are not PE files.
It also checks that D3D12/D3D12Core.dll is next to each one (the deploy step ran).
--expect-some also fails on an empty manifest. Stdlib only.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys

REQUIRED = ("D3D12SDKVersion", "D3D12SDKPath")


class PeError(Exception):
    pass


def pe_exports(data: bytes) -> list[str]:
    """The exported names of a PE image (an empty list when it exports nothing)."""

    def u16(off: int) -> int:
        if off + 2 > len(data):
            raise PeError("truncated")
        return struct.unpack_from("<H", data, off)[0]

    def u32(off: int) -> int:
        if off + 4 > len(data):
            raise PeError("truncated")
        return struct.unpack_from("<I", data, off)[0]

    if data[:2] != b"MZ":
        raise PeError("not a PE file (no MZ header)")
    pe = u32(0x3C)
    if data[pe:pe + 4] != b"PE\0\0":
        raise PeError("not a PE file (no PE signature)")
    coff = pe + 4
    sections = u16(coff + 2)
    opt_size = u16(coff + 16)
    opt = coff + 20
    magic = u16(opt)
    if magic == 0x10B:
        dirs = opt + 96
    elif magic == 0x20B:
        dirs = opt + 112
    else:
        raise PeError(f"unknown optional header magic 0x{magic:x}")
    if dirs + 8 > opt + opt_size:
        return []  # no data directories
    exp_rva, exp_size = u32(dirs), u32(dirs + 4)
    if exp_rva == 0 or exp_size == 0:
        return []
    table = opt + opt_size
    spans = []
    for i in range(sections):
        s = table + 40 * i
        vsize, vaddr, rsize, raw = u32(s + 8), u32(s + 12), u32(s + 16), u32(s + 20)
        spans.append((vaddr, max(vsize, rsize), raw))

    def at(rva: int) -> int:
        for vaddr, size, raw in spans:
            if vaddr <= rva < vaddr + size:
                return raw + (rva - vaddr)
        raise PeError(f"RVA 0x{rva:x} is in no section")

    ed = at(exp_rva)
    count, names = u32(ed + 24), u32(ed + 32)
    out = []
    for i in range(count):
        p = at(u32(at(names) + 4 * i))
        end = data.find(b"\0", p)
        if end < 0:
            raise PeError("unterminated export name")
        out.append(data[p:end].decode("ascii", "replace"))
    return out


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--expect-some", action="store_true",
                    help="fail when the manifest lists no executable")
    args = ap.parse_args(argv)
    try:
        with open(args.manifest, encoding="utf-8") as fh:
            exes = [line.strip() for line in fh if line.strip()]
    except OSError as e:
        print(f"error: cannot read the manifest: {e}")
        return 1
    bad = 0
    for exe in exes:
        name = os.path.basename(exe)
        if not os.path.isfile(exe):
            print(f"{name}: error: not built ({exe})")
            bad += 1
            continue
        if not os.path.isfile(os.path.join(os.path.dirname(exe), "D3D12", "D3D12Core.dll")):
            print(f"{name}: error: no D3D12/D3D12Core.dll next to it (the deploy step did not run)")
            bad += 1
        try:
            with open(exe, "rb") as fh:
                names = pe_exports(fh.read())
        except (OSError, PeError) as e:
            print(f"{name}: error: cannot read the exports: {e}")
            bad += 1
            continue
        missing = [n for n in REQUIRED if n not in names]
        if missing:
            print(f"{name}: error: deployed with the Agility runtime but does not export "
                  f"{' and '.join(missing)}: it runs on the OS D3D12 runtime. "
                  "de_d3d12_agility_deploy() adds the exports unless a source of the target "
                  "includes gpu/d3d12/d3d12_agility_exports.h; check that one does define them.")
            bad += 1
        else:
            print(f"{name}: OK ({', '.join(REQUIRED)})")
    if args.expect_some and not exes:
        print(f"error: {args.manifest} lists no executable")
        return 1
    print(f"{len(exes)} executable(s) deployed with the Agility runtime, {bad} error(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
