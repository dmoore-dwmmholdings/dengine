#!/usr/bin/env python3
"""abi_dump.py: describe the public C API and ABI of DEngine as JSON (DENG-128).

Reads the public headers through clang's JSON AST (`-Xclang -ast-dump=json`) and records:
  - every exported function (declared with DE_API: a visibility or dllimport/dllexport attribute)
    and exported variable, with its full type as spelled;
  - every public struct and union, with its fields and field types, and with size, alignment and
    field offsets MEASURED on the target by a generated probe (layout_probe.c.in) that is
    compiled and run here, so the numbers are real for this platform;
  - every enum with its constants and values (and its size, measured by the probe);
  - every typedef (handles, callbacks, opaque types) with its underlying type;
  - DE_VERSION_MAJOR/MINOR/PATCH and DE_GAME_API_VERSION (from `clang -E -dM`).
Only declarations whose file is under --include-dir count; system headers are ignored.

  abi_dump.py --include-dir include --header dengine/dengine.h --out abi.json
  abi_dump.py ... --emit-probe probe.c     # write the probe source only (CMake compile check)
  abi_dump.py ... --compile-only           # compile the probe but do not run it (cross)

Compare two dumps with abi_check.py. Python 3.10+, stdlib only. No libabigail.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
from pathlib import Path

FORMAT = 1
HERE = Path(__file__).resolve().parent
TEMPLATE = HERE / "layout_probe.c.in"
VERSION_MACROS = ("DE_VERSION_MAJOR", "DE_VERSION_MINOR", "DE_VERSION_PATCH", "DE_GAME_API_VERSION")
EXPORT_ATTRS = {"VisibilityAttr", "DLLImportAttr", "DLLExportAttr"}
PROBE_CFLAGS = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wvla", "-Werror"]
_ANON = re.compile(r"\((?:anonymous|unnamed)[^()]*\)")


class DumpError(Exception):
    pass


def norm_type(t: str) -> str:
    """A type string without the file paths clang puts in anonymous type names."""
    return _ANON.sub("(anonymous)", t)


def host_platform() -> str:
    osname = {"darwin": "macos", "win32": "windows"}.get(sys.platform, sys.platform)
    if osname.startswith("linux"):
        osname = "linux"
    mach = platform.machine().lower()
    mach = {"aarch64": "arm64", "amd64": "x86-64", "x86_64": "x86-64"}.get(mach, mach)
    return f"{osname}-{mach}"


def run(cmd: list[str], what: str) -> subprocess.CompletedProcess:
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except OSError as e:
        raise DumpError(f"{what}: cannot run {cmd[0]}: {e}") from e
    if r.returncode != 0:
        raise DumpError(f"{what} failed ({r.returncode}): {' '.join(cmd)}\n{r.stderr.strip()}")
    return r


# --- AST ------------------------------------------------------------------------------------------

class FileTracker:
    """clang's JSON dumper omits "file" when it equals the previously printed location's file, so
    the file of a declaration is only known by walking every location in document order."""

    def __init__(self) -> None:
        self.current = ""

    def walk(self, node: object) -> None:
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "includedFrom":
                    continue  # names the includer, not the current file
                if k == "file" and isinstance(v, str):
                    self.current = v
                else:
                    self.walk(v)
        elif isinstance(node, list):
            for v in node:
                self.walk(v)

    def decl_file(self, decl: dict) -> str:
        """Advance through the decl and return the file of its own location."""
        if "loc" in decl:
            self.walk(decl["loc"])
        f = self.current
        for k, v in decl.items():
            if k != "loc":
                self.walk(v)
        return f


def owned_tag_id(node: dict) -> str | None:
    for c in node.get("inner", []):
        if "ownedTagDecl" in c:
            return c["ownedTagDecl"].get("id")
        found = owned_tag_id(c)
        if found:
            return found
    return None


def const_value(node: dict) -> int | None:
    for c in node.get("inner", []):
        if "value" in c and c.get("kind") == "ConstantExpr":
            try:
                return int(c["value"], 0)
            except ValueError:
                return None
    return None


class Collector:
    def __init__(self, include_dir: Path) -> None:
        self.include_dir = include_dir
        self.functions: dict[str, dict] = {}
        self.variables: dict[str, dict] = {}
        self.records: dict[str, dict] = {}
        self.opaque: set[str] = set()
        self.enums: dict[str, dict] = {}
        self.typedefs: dict[str, str] = {}
        self.anon_records: dict[str, tuple[dict, str]] = {}  # id -> (record, header) awaiting a typedef
        self.anon_enums: dict[str, tuple[dict, str]] = {}
        self._header = ""

    def public_header(self, f: str) -> str | None:
        if not f:
            return None
        try:
            rel = Path(os.path.realpath(f)).relative_to(self.include_dir)
        except ValueError:
            return None
        return rel.as_posix()

    def fields(self, rec: dict, prefix: str = "") -> list[dict]:
        out: list[dict] = []
        anon: dict | None = None
        for c in rec.get("inner", []):
            kind = c.get("kind")
            if kind == "RecordDecl":
                if c.get("name"):
                    if c.get("completeDefinition"):
                        self.add_record(c, c["name"], self._header)  # C: nested tags are file scope
                else:
                    anon = c
            elif kind == "EnumDecl" and c.get("name"):
                self.add_enum(c, c["name"], self._header)
            elif kind == "FieldDecl":
                name = c.get("name")
                ftype = norm_type(c.get("type", {}).get("qualType", ""))
                if not name:
                    if anon is not None:  # anonymous struct/union member: its fields are ours
                        out.extend(self.fields(anon, prefix))
                    anon = None
                    continue
                entry: dict = {"name": prefix + name, "type": ftype}
                if c.get("isBitfield"):
                    entry["bits"] = const_value(c)
                out.append(entry)
                if anon is not None and "(anonymous)" in ftype:
                    sub = prefix + name + ("[0]." if "[" in ftype else ".")
                    out.extend(self.fields(anon, sub))
                anon = None
        return out

    def add_record(self, node: dict, name: str, header: str) -> None:
        if name in self.records:
            return
        tag = node.get("tagUsed", "struct")
        spelling = f"{tag} {node['name']}" if node.get("name") else name
        self.records[name] = {"kind": tag, "header": header, "spelling": spelling,
                              "size": None, "align": None, "fields": self.fields(node)}
        self.opaque.discard(name)

    def add_enum(self, node: dict, name: str, header: str, anonymous: bool = False) -> None:
        if name in self.enums:
            return
        consts: dict[str, int | None] = {}
        nxt: int | None = 0
        for c in node.get("inner", []):
            if c.get("kind") != "EnumConstantDecl":
                continue
            v = const_value(c)
            if v is None:
                v = nxt
            consts[c["name"]] = v
            nxt = None if v is None else v + 1
        spelling = None if anonymous else (f"enum {node['name']}" if node.get("name") else name)
        self.enums[name] = {"header": header, "spelling": spelling, "size": None,
                            "constants": consts}

    def collect(self, ast: dict) -> None:
        tracker = FileTracker()
        for n in ast.get("inner", []):
            f = tracker.decl_file(n)
            if n.get("isImplicit"):
                continue
            header = self.public_header(f)
            if header is None:
                continue
            self._header = header
            kind = n.get("kind")
            name = n.get("name", "")
            if kind == "FunctionDecl":
                if n.get("storageClass") == "static" or name in self.functions:
                    continue
                if any(c.get("kind") in EXPORT_ATTRS for c in n.get("inner", [])):
                    self.functions[name] = {"type": norm_type(n["type"]["qualType"]),
                                            "header": header}
            elif kind == "VarDecl":
                if n.get("storageClass") == "static":
                    continue
                if any(c.get("kind") in EXPORT_ATTRS for c in n.get("inner", [])):
                    self.variables[name] = {"type": norm_type(n["type"]["qualType"]),
                                            "header": header}
            elif kind == "RecordDecl":
                if not n.get("completeDefinition"):
                    if name and name not in self.records:
                        self.opaque.add(name)
                elif name:
                    self.add_record(n, name, header)
                else:
                    self.anon_records[n["id"]] = (n, header)
            elif kind == "EnumDecl":
                if name:
                    self.add_enum(n, name, header)
                else:
                    self.anon_enums[n["id"]] = (n, header)
            elif kind == "TypedefDecl":
                self.typedefs[name] = norm_type(n["type"]["qualType"])
                tid = owned_tag_id(n)
                if tid in self.anon_records:
                    self.add_record(self.anon_records.pop(tid)[0], name, header)
                elif tid in self.anon_enums:
                    self.add_enum(self.anon_enums.pop(tid)[0], name, header)
        for n, hdr in self.anon_enums.values():  # `enum { DE_X = 1 };` with no typedef
            consts = [c["name"] for c in n.get("inner", []) if c.get("kind") == "EnumConstantDecl"]
            if consts:
                self.add_enum(n, f"(anonymous:{consts[0]})", hdr, anonymous=True)


# --- probe ----------------------------------------------------------------------------------------

def probe_source(col: Collector, header: str) -> str:
    lines: list[str] = []
    for name in sorted(col.records):
        r = col.records[name]
        sp = r["spelling"]
        lines.append(f'    DE_ABI_RECORD("{name}", {sp});')
        for f in r["fields"]:
            if "bits" not in f:
                lines.append(f'    DE_ABI_FIELD("{name}", "{f["name"]}", {sp}, {f["name"]});')
    for name in sorted(col.enums):
        e = col.enums[name]
        if e["spelling"]:
            lines.append(f'    DE_ABI_ENUM_SIZE("{name}", {e["spelling"]});')
        for c in e["constants"]:
            lines.append(f'    DE_ABI_ENUM_CONST("{c}", {c});')
    text = TEMPLATE.read_text()
    return text.replace("@DE_ABI_INCLUDE@", f"<{header}>").replace("@DE_ABI_BODY@", "\n".join(lines))


def apply_probe(col: Collector, output: str) -> None:
    consts = {c: e for e in col.enums.values() for c in e["constants"]}
    for line in output.splitlines():
        p = line.split("\t")
        if p[0] == "R" and len(p) == 4:
            col.records[p[1]]["size"], col.records[p[1]]["align"] = int(p[2]), int(p[3])
        elif p[0] == "F" and len(p) == 4:
            for f in col.records[p[1]]["fields"]:
                if f["name"] == p[2]:
                    f["offset"] = int(p[3])
        elif p[0] == "S" and len(p) == 3:
            col.enums[p[1]]["size"] = int(p[2])
        elif p[0] == "E" and len(p) == 3 and p[1] in consts:
            consts[p[1]]["constants"][p[1]] = int(p[2])


# --- main -----------------------------------------------------------------------------------------

def versions(args: argparse.Namespace, header_abs: Path, extra: list[str]) -> dict:
    r = run([args.clang, "-x", "c", "-std=c11", "-E", "-dM", "-I", str(args.include_dir), *extra,
             str(header_abs)], "version macros (clang -E -dM)")
    out: dict = {}
    for m in re.finditer(r"^#define (\w+) (.*)$", r.stdout, re.M):
        if m.group(1) in VERSION_MACROS:
            v = m.group(2).strip().strip("()").rstrip("uUlL")
            try:
                out[m.group(1)] = int(v, 0)
            except ValueError:
                out[m.group(1)] = m.group(2).strip()
    return out


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--include-dir", type=Path, required=True, help="public include root")
    ap.add_argument("--header", default="dengine/dengine.h", help="umbrella header, relative to it")
    ap.add_argument("--clang", default=os.environ.get("DE_ABI_CLANG", "clang"), help="clang for the AST")
    ap.add_argument("--cc", default="", help="compiler for the probe (default: --clang)")
    ap.add_argument("--target", default="", help="clang --target triple for AST and probe")
    ap.add_argument("-D", dest="defines", action="append", default=[], help="extra -D for both")
    ap.add_argument("--platform", default="", help="platform name in the dump (default: host)")
    ap.add_argument("--out", type=Path, help="write the JSON dump here (default: stdout)")
    ap.add_argument("--work-dir", type=Path, help="keep the probe source and binary here")
    ap.add_argument("--emit-probe", type=Path, help="write the probe source here and stop")
    ap.add_argument("--compile-only", action="store_true", help="compile the probe, do not run it")
    args = ap.parse_args(argv)

    args.include_dir = Path(os.path.realpath(args.include_dir))
    header_abs = args.include_dir / args.header
    if not header_abs.is_file():
        print(f"abi_dump: no header {header_abs}", file=sys.stderr)
        return 2
    extra = ([f"--target={args.target}"] if args.target else []) + [f"-D{d}" for d in args.defines]

    try:
        r = run([args.clang, "-x", "c", "-std=c11", "-fsyntax-only", "-fno-color-diagnostics",
                 "-Xclang", "-ast-dump=json", "-I", str(args.include_dir), *extra, str(header_abs)],
                "clang AST dump")
        col = Collector(args.include_dir)
        col.collect(json.loads(r.stdout))
        src = probe_source(col, args.header)
        if args.emit_probe:
            args.emit_probe.parent.mkdir(parents=True, exist_ok=True)
            if not args.emit_probe.exists() or args.emit_probe.read_text() != src:
                args.emit_probe.write_text(src)
            return 0

        with tempfile.TemporaryDirectory(prefix="de_abi_") as tmp:
            work = args.work_dir or Path(tmp)
            work.mkdir(parents=True, exist_ok=True)
            probe_c = work / "layout_probe.c"
            probe_exe = work / ("layout_probe.exe" if os.name == "nt" or "windows" in args.target
                                else "layout_probe")
            probe_c.write_text(src)
            run([args.cc or args.clang, *PROBE_CFLAGS, "-I", str(args.include_dir), *extra,
                 str(probe_c), "-o", str(probe_exe)], "probe compile")
            if not args.compile_only:
                apply_probe(col, run([str(probe_exe)], "probe run").stdout)
        vers = versions(args, header_abs, extra)
    except DumpError as e:
        print(f"abi_dump: {e}", file=sys.stderr)
        return 1

    for rec in col.records.values():
        del rec["spelling"]
    for en in col.enums.values():
        del en["spelling"]
    dump = {
        "format": FORMAT,
        "platform": args.platform or host_platform(),
        "measured": not args.compile_only,
        "header": args.header,
        "versions": vers,
        "functions": col.functions,
        "variables": col.variables,
        "records": col.records,
        "opaque": sorted(col.opaque),
        "enums": col.enums,
        "typedefs": col.typedefs,
    }
    text = json.dumps(dump, indent=1, sort_keys=True) + "\n"
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(text)
        print(f"abi_dump: {len(col.functions)} functions, {len(col.records)} records, "
              f"{len(col.enums)} enums, {len(col.typedefs)} typedefs -> {args.out}")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
