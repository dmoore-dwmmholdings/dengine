#!/usr/bin/env python3
"""Self-test for tools/check_agility/check_agility.py (ctest `agility_exports_selftest`, DENG-423).

Writes small synthetic PE images (PE32+ and PE32, with and without an export table) into a
temporary directory laid out like a build tree, with and without D3D12/D3D12Core.dll beside them,
runs the checker on manifests that list them and checks the exit status and the messages. The
case "deploy without exports" must fail: that is the bug DENG-420 found.
"""

from __future__ import annotations

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = os.path.join(os.path.dirname(HERE), "check_agility.py")
BOTH = ["D3D12SDKPath", "D3D12SDKVersion"]


def make_pe(exports: list[str] | None, pe32: bool = False) -> bytes:
    """A minimal PE image: one section holding the export directory (none when exports is None)."""
    sect_rva, sect_raw = 0x1000, 0x200
    body = b""
    exp_rva = exp_size = 0
    if exports is not None:
        names = sorted(exports)
        names_rva = sect_rva + 40
        strings_rva = names_rva + 4 * len(names)
        strings = b""
        ptrs = b""
        for n in names:
            ptrs += struct.pack("<I", strings_rva + len(strings))
            strings += n.encode("ascii") + b"\0"
        directory = struct.pack("<IIHHIIIIIII", 0, 0, 0, 0, 0, 1, len(names), len(names), 0,
                                names_rva, 0)
        body = directory + ptrs + strings
        exp_rva, exp_size = sect_rva, len(body)
    body = body.ljust(0x200, b"\0")
    data_dirs = struct.pack("<II", exp_rva, exp_size) + b"\0" * (15 * 8)
    if pe32:
        opt = struct.pack("<H", 0x10B) + b"\0" * 94 + data_dirs  # dirs at 96
    else:
        opt = struct.pack("<H", 0x20B) + b"\0" * 110 + data_dirs  # dirs at 112
    coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, len(opt), 0x22)
    section = struct.pack("<8sIIIIIIHHI", b".edata", len(body), sect_rva, len(body), sect_raw, 0,
                          0, 0, 0, 0x40000040)
    dos = b"MZ" + b"\0" * 58 + struct.pack("<I", 0x40)
    head = dos + b"PE\0\0" + coff + opt + section
    return head.ljust(sect_raw, b"\0") + body


def write(path: str, data: bytes) -> str:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as fh:
        fh.write(data)
    return path


def deploy(exe_dir: str) -> None:
    write(os.path.join(exe_dir, "D3D12", "D3D12Core.dll"), b"MZ")


def run(manifest_lines: list[str], tmp: str, *extra: str) -> tuple[int, str]:
    manifest = os.path.join(tmp, "manifest.txt")
    with open(manifest, "w", encoding="utf-8") as fh:
        fh.write("".join(line + "\n" for line in manifest_lines))
    p = subprocess.run([sys.executable, CHECKER, "--manifest", manifest, *extra],
                       capture_output=True, text=True, check=False)
    return p.returncode, p.stdout + p.stderr


def main() -> int:
    failures = 0
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a")
        b = os.path.join(tmp, "b")
        deploy(a)
        deploy(b)
        good = write(os.path.join(a, "good.exe"), make_pe(BOTH))
        good32 = write(os.path.join(a, "good32.exe"), make_pe(BOTH + ["main"], pe32=True))
        bad = write(os.path.join(a, "bad.exe"), make_pe(["main"]))
        half = write(os.path.join(a, "half.exe"), make_pe(["D3D12SDKVersion"]))
        none = write(os.path.join(a, "noexports.exe"), make_pe(None))
        undeployed = write(os.path.join(b, "sub", "undeployed.exe"), make_pe(BOTH))
        notpe = write(os.path.join(a, "notpe.exe"), b"#!/bin/sh\n")
        missing = os.path.join(a, "missing.exe")

        cases = [
            ("exports present", [good, good32], (), 0, ["good.exe: OK", "good32.exe: OK"]),
            ("deploy without exports (DENG-420)", [good, bad], (), 1,
             ["bad.exe: error: deployed with the Agility runtime but does not export "
              "D3D12SDKVersion and D3D12SDKPath", "good.exe: OK"]),
            ("one export missing", [half], (), 1, ["does not export D3D12SDKPath"]),
            ("no export table", [none], (), 1, ["does not export D3D12SDKVersion and D3D12SDKPath"]),
            ("no D3D12/ next to it", [undeployed], (), 1,
             ["no D3D12/D3D12Core.dll next to it"]),
            ("not built", [missing], (), 1, ["missing.exe: error: not built"]),
            ("not a PE file", [notpe], (), 1, ["cannot read the exports: not a PE file"]),
            ("empty manifest", [], (), 0, ["0 executable(s)"]),
            ("empty manifest, --expect-some", [], ("--expect-some",), 1, ["lists no executable"]),
        ]
        for name, lines, extra, want_rc, want in cases:
            rc, out = run(lines, tmp, *extra)
            missing_text = [w for w in want if w not in out]
            if rc != want_rc or missing_text:
                failures += 1
                print(f"FAIL {name}: exit {rc} (want {want_rc}), missing {missing_text}\n{out}")
            else:
                print(f"ok   {name}")
        rc = subprocess.run([sys.executable, CHECKER, "--manifest", os.path.join(tmp, "nope.txt")],
                            capture_output=True, text=True, check=False).returncode
        if rc != 1:
            failures += 1
            print("FAIL a missing manifest must fail")
        else:
            print("ok   missing manifest")
    print("PASS" if failures == 0 else f"{failures} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
