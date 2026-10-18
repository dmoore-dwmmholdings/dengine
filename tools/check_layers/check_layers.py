#!/usr/bin/env python3
"""Include-graph layering check for DEngine (SPEC §4.1, §4.2).

Scans the engine sources for #include lines, maps every file and every include target to a
module with the table in layers.toml, and fails when:

  - a module includes a module in a higher band (upward include);
  - a module includes a module in its own band without an [[allow]] entry;
  - a module edge matches a [[forbid]] entry (physics, audio and anim never meet);
  - a module includes an `explicit_only` module (the cooker library) without an [[allow]] entry;
  - engine code includes a file under tools/ (other than the cooker library through an allow);
  - any file includes the umbrella header dengine.h (it is for games only);
  - a scanned file, a public header or an include target does not map to a module;
  - the module graph has a cycle (Tarjan SCC; members of a [[group]] count as one node);
  - a file under examples/ includes anything of the engine but public headers (DENG-327): an
    example is written against include/dengine/ only, like a game. Files listed in an
    [[example_internal]] entry (with the reason) are exempt; an entry whose file is gone fails.

Every finding names the file and line. Exit status: 0 clean, 1 violations, 2 bad table or usage.

Scanned: src/**/*.{c,h,m,mm,cpp,cc,inl}, tools/cook/lib/** (same extensions) and
include/dengine/*.h; examples/** (same extensions) for the public-headers rule only. Standard library only; Python 3.10+ (tomllib from 3.11, else a minimal
parser for the subset of TOML that layers.toml uses).
"""

from __future__ import annotations

import argparse
import fnmatch
import os
import posixpath
import re
import sys
import time

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, "..", ".."))
DEFAULT_TABLE = os.path.join(SCRIPT_DIR, "layers.toml")

SOURCE_EXTS = (".c", ".h", ".m", ".mm", ".cpp", ".cc", ".inl")
PUBLIC_DIR = "include/dengine"
UMBRELLA = "dengine.h"
COOK_LIB_DIR = "tools/cook/lib"
EXAMPLES_DIR = "examples"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
PUBLIC_HEADER_RE = re.compile(r"^(de_[A-Za-z0-9_]+\.h|dengine\.h)$")


class TableError(Exception):
    """layers.toml is missing, malformed or inconsistent."""


# --------------------------------------------------------------------------------------------
# Minimal TOML reader (Python 3.10 has no tomllib). Supports what layers.toml uses: comments,
# [table], [[array.of.tables]], key = string | integer | boolean | array (may span lines).


def _toml_strip_comment(line: str) -> str:
    out = []
    quote = None
    i = 0
    while i < len(line):
        c = line[i]
        if quote:
            out.append(c)
            if c == "\\" and quote == '"' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
            out.append(c)
        elif c == "#":
            break
        else:
            out.append(c)
        i += 1
    return "".join(out).strip()


def _toml_brackets_balanced(text: str) -> bool:
    depth = 0
    quote = None
    i = 0
    while i < len(text):
        c = text[i]
        if quote:
            if c == "\\" and quote == '"':
                i += 1
            elif c == quote:
                quote = None
        elif c in "\"'":
            quote = c
        elif c == "[":
            depth += 1
        elif c == "]":
            depth -= 1
        i += 1
    return depth <= 0


_ESCAPES = {'"': '"', "\\": "\\", "n": "\n", "t": "\t", "r": "\r", "b": "\b", "f": "\f"}


def _toml_value(text: str, pos: int, where: str):
    """Parse one value starting at text[pos]; return (value, new_pos)."""
    while pos < len(text) and text[pos] in " \t\r\n":
        pos += 1
    if pos >= len(text):
        raise TableError(f"{where}: missing value")
    c = text[pos]
    if c == '"':
        out = []
        pos += 1
        while pos < len(text) and text[pos] != '"':
            if text[pos] == "\\":
                pos += 1
                e = text[pos] if pos < len(text) else ""
                if e in _ESCAPES:
                    out.append(_ESCAPES[e])
                elif e in "uU":
                    n = 4 if e == "u" else 8
                    out.append(chr(int(text[pos + 1 : pos + 1 + n], 16)))
                    pos += n
                else:
                    raise TableError(f"{where}: bad escape \\{e}")
            else:
                out.append(text[pos])
            pos += 1
        if pos >= len(text):
            raise TableError(f"{where}: unterminated string")
        return "".join(out), pos + 1
    if c == "'":
        end = text.find("'", pos + 1)
        if end < 0:
            raise TableError(f"{where}: unterminated string")
        return text[pos + 1 : end], end + 1
    if c == "[":
        items = []
        pos += 1
        while True:
            while pos < len(text) and text[pos] in " \t\r\n,":
                pos += 1
            if pos >= len(text):
                raise TableError(f"{where}: unterminated array")
            if text[pos] == "]":
                return items, pos + 1
            value, pos = _toml_value(text, pos, where)
            items.append(value)
    m = re.compile(r"true|false|[+-]?[0-9][0-9_]*").match(text, pos)
    if not m:
        raise TableError(f"{where}: unsupported value {text[pos:pos + 20]!r}")
    word = m.group(0)
    if word == "true":
        return True, m.end()
    if word == "false":
        return False, m.end()
    return int(word.replace("_", "")), m.end()


def _toml_container(root: dict, dotted: str, array: bool, where: str) -> dict:
    keys = [k.strip().strip('"') for k in dotted.split(".")]
    node = root
    for k in keys[:-1]:
        node = node.setdefault(k, {})
        if isinstance(node, list):
            node = node[-1]
    last = keys[-1]
    if array:
        lst = node.setdefault(last, [])
        if not isinstance(lst, list):
            raise TableError(f"{where}: [[{dotted}]] redefines a key")
        lst.append({})
        return lst[-1]
    if last in node:
        raise TableError(f"{where}: table [{dotted}] defined twice")
    node[last] = {}
    return node[last]


def parse_toml_minimal(text: str, name: str = "<toml>") -> dict:
    root: dict = {}
    current = root
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        lineno = i + 1
        line = _toml_strip_comment(lines[i])
        i += 1
        if not line:
            continue
        where = f"{name}:{lineno}"
        if line.startswith("[["):
            if not line.endswith("]]"):
                raise TableError(f"{where}: bad table header")
            current = _toml_container(root, line[2:-2], True, where)
            continue
        if line.startswith("["):
            if not line.endswith("]"):
                raise TableError(f"{where}: bad table header")
            current = _toml_container(root, line[1:-1], False, where)
            continue
        key, eq, rest = line.partition("=")
        key = key.strip().strip('"')
        if not eq or not re.fullmatch(r"[A-Za-z0-9_-]+", key):
            raise TableError(f"{where}: expected key = value")
        # Arrays may continue on following lines until the brackets balance.
        while not _toml_brackets_balanced(rest) and i < len(lines):
            rest += "\n" + _toml_strip_comment(lines[i])
            i += 1
        value, end = _toml_value(rest, 0, where)
        if rest[end:].strip():
            raise TableError(f"{where}: unexpected text after value: {rest[end:].strip()!r}")
        if key in current:
            raise TableError(f"{where}: key {key!r} defined twice")
        current[key] = value
    return root


def load_toml(path: str, force_fallback: bool = False) -> dict:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise TableError(f"cannot read {path}: {e}") from e
    if not force_fallback:
        try:
            import tomllib  # Python 3.11+
        except ImportError:
            tomllib = None
        if tomllib is not None:
            try:
                return tomllib.loads(data.decode("utf-8"))
            except tomllib.TOMLDecodeError as e:
                raise TableError(f"{path}: {e}") from e
    return parse_toml_minimal(data.decode("utf-8"), path)


# --------------------------------------------------------------------------------------------
# Layer table


def _segments(path: str) -> list[str]:
    return [s for s in path.split("/") if s]


def _patterns_overlap(a: str, b: str) -> bool:
    return a == b or fnmatch.fnmatchcase(a, b) or fnmatch.fnmatchcase(b, a)


class ModuleEntry:
    def __init__(self, raw: dict, index: int, bands: list[str]):
        where = f"[[module]] #{index + 1}"
        self.name = raw.get("name")
        if not isinstance(self.name, str) or not self.name:
            raise TableError(f"{where}: `name` must be a non-empty string")
        where = f"[[module]] {self.name!r}"
        self.band = raw.get("band")
        if self.band not in bands:
            raise TableError(f"{where}: `band` must be one of {bands} (got {self.band!r})")
        self.paths = raw.get("paths", [])
        self.headers = raw.get("headers", [])
        self.explicit_only = raw.get("explicit_only", False)
        for key, val in (("paths", self.paths), ("headers", self.headers)):
            if not isinstance(val, list) or not all(isinstance(v, str) for v in val):
                raise TableError(f"{where}: `{key}` must be a list of strings")
        if not isinstance(self.explicit_only, bool):
            raise TableError(f"{where}: `explicit_only` must be true or false")
        unknown = set(raw) - {"name", "band", "paths", "headers", "explicit_only"}
        if unknown:
            raise TableError(f"{where}: unknown keys {sorted(unknown)}")
        self.wildcards = self.name.count("*")
        for p in self.paths:
            if p.count("*") != self.wildcards:
                raise TableError(
                    f"{where}: path {p!r} must have as many `*` as the name ({self.wildcards})")
        if self.wildcards and self.headers:
            raise TableError(f"{where}: a wildcard module cannot own headers")
        self.path_segs = [_segments(p) for p in self.paths]

    def match(self, segs: list[str]):
        """Score and module id if the file path `segs` lies inside one of this entry's dirs."""
        best = None
        for pat in self.path_segs:
            if len(segs) <= len(pat):
                continue
            captured = []
            for want, got in zip(pat, segs):
                if want == "*":
                    captured.append(got)
                elif want != got:
                    break
            else:
                name = self.name
                for c in captured:
                    name = name.replace("*", c, 1)
                # Longer paths win; at the same length a literal path beats a wildcard.
                score = (len(pat), -len(captured))
                if best is None or score > best[0]:
                    best = (score, name)
        return best


class Table:
    def __init__(self, data: dict, path: str):
        self.path = path
        self.bands = data.get("bands")
        if (not isinstance(self.bands, list) or not self.bands
                or not all(isinstance(b, str) for b in self.bands)
                or len(set(self.bands)) != len(self.bands)):
            raise TableError("`bands` must be a non-empty list of unique strings")
        self.band_rank = {b: i for i, b in enumerate(self.bands)}
        self.include_roots = data.get("include_roots", ["src"])
        if not isinstance(self.include_roots, list):
            raise TableError("`include_roots` must be a list of strings")

        self.modules = [ModuleEntry(m, i, self.bands) for i, m in enumerate(data.get("module", []))]
        if not self.modules:
            raise TableError("no [[module]] entries")
        self.by_name: dict[str, ModuleEntry] = {}
        for m in self.modules:
            if m.name in self.by_name:
                raise TableError(f"module {m.name!r} is declared twice")
            self.by_name[m.name] = m

        self.header_owner: dict[str, str] = {}
        for m in self.modules:
            for h in m.headers:
                if h in self.header_owner:
                    raise TableError(f"header {h!r} is owned by {self.header_owner[h]!r} "
                                     f"and {m.name!r}")
                self.header_owner[h] = m.name

        # First directory names under each include root that belong to a module, e.g. for
        # "src": core, platform, gpu, world, ... An include "core/x.h" must then map to a module.
        self.root_dirs: dict[str, set[str]] = {}
        for root in self.include_roots:
            rsegs = _segments(root)
            dirs = set()
            for m in self.modules:
                for pat in m.path_segs:
                    n = len(rsegs)
                    if len(pat) > n and pat[:n] == rsegs and pat[n] != "*":
                        dirs.add(pat[n])
            self.root_dirs[root] = dirs

        self.groups: list[list[str]] = []
        self.group_of: dict[str, str] = {}
        for i, g in enumerate(data.get("group", [])):
            members = g.get("members")
            if not isinstance(members, list) or len(members) < 2:
                raise TableError(f"[[group]] #{i + 1}: `members` needs at least two modules")
            if not isinstance(g.get("why"), str) or not g["why"].strip():
                raise TableError(f"[[group]] {members}: `why` (the reason) is required")
            for mem in members:
                if mem not in self.by_name or self.by_name[mem].wildcards:
                    raise TableError(f"[[group]] {members}: {mem!r} is not a declared module")
                if mem in self.group_of:
                    raise TableError(f"[[group]]: {mem!r} is in two groups")
            if len({self.by_name[mem].band for mem in members}) != 1:
                raise TableError(f"[[group]] {members}: members must be in the same band")
            node = "+".join(members)
            for mem in members:
                self.group_of[mem] = node
            self.groups.append(list(members))

        self.allows: list[dict] = []
        for i, a in enumerate(data.get("allow", [])):
            where = f"[[allow]] #{i + 1}"
            frm, to, why = a.get("from"), a.get("to"), a.get("why")
            if not isinstance(frm, str) or not isinstance(to, str):
                raise TableError(f"{where}: `from` and `to` must be strings")
            where = f"[[allow]] {frm} -> {to}"
            if not isinstance(why, str) or not why.strip():
                raise TableError(f"{where}: `why` (the reason) is required")
            unknown = set(a) - {"from", "to", "why"}
            if unknown:
                raise TableError(f"{where}: unknown keys {sorted(unknown)}")
            fe, te = self._entry_for_pattern(frm), self._entry_for_pattern(to)
            if fe is None:
                raise TableError(f"{where}: `from` matches no declared module")
            if te is None:
                raise TableError(f"{where}: `to` matches no declared module")
            restricted_lower = (te.explicit_only and
                                self.band_rank[te.band] < self.band_rank[fe.band])
            if fe.band != te.band and not restricted_lower:
                raise TableError(f"{where}: allow edges are for modules in the same band "
                                 f"({fe.band} vs {te.band}); lower bands are always allowed "
                                 "and higher bands never are")
            self.allows.append(a)

        # [[example_internal]]: files under examples/ that may include engine internals.
        self.example_internal: dict[str, str] = {}
        for i, e in enumerate(data.get("example_internal", [])):
            f, why = e.get("file"), e.get("why")
            where = f"[[example_internal]] #{i + 1}"
            if not isinstance(f, str) or not f.startswith(EXAMPLES_DIR + "/"):
                raise TableError(f"{where}: `file` must be a path under {EXAMPLES_DIR}/")
            if not isinstance(why, str) or not why.strip():
                raise TableError(f"[[example_internal]] {f}: `why` (the reason) is required")
            unknown = set(e) - {"file", "why"}
            if unknown:
                raise TableError(f"[[example_internal]] {f}: unknown keys {sorted(unknown)}")
            self.example_internal[f] = why

        self.forbids: list[dict] = []
        for i, f in enumerate(data.get("forbid", [])):
            frm, to, why = f.get("from"), f.get("to"), f.get("why")
            if not isinstance(frm, str) or not isinstance(to, str):
                raise TableError(f"[[forbid]] #{i + 1}: `from` and `to` must be strings")
            where = f"[[forbid]] {frm} -> {to}"
            if not isinstance(why, str) or not why.strip():
                raise TableError(f"{where}: `why` (the reason) is required")
            if self._entry_for_pattern(frm) is None or self._entry_for_pattern(to) is None:
                raise TableError(f"{where}: `from` or `to` matches no declared module")
            for a in self.allows:
                if _patterns_overlap(a["from"], frm) and _patterns_overlap(a["to"], to):
                    raise TableError(f"[[allow]] {a['from']} -> {a['to']} contradicts {where}: "
                                     f"{why}")
            self.forbids.append(f)

    def _entry_for_pattern(self, pattern: str):
        for m in self.modules:
            if m.name == pattern or fnmatch.fnmatchcase(pattern, m.name) \
                    or fnmatch.fnmatchcase(m.name, pattern):
                return m
        return None

    def entry(self, module: str) -> ModuleEntry:
        if module in self.by_name:
            return self.by_name[module]
        for m in self.modules:
            if m.wildcards and fnmatch.fnmatchcase(module, m.name):
                return m
        raise KeyError(module)

    def band(self, module: str) -> str:
        return self.entry(module).band

    def node(self, module: str) -> str:
        return self.group_of.get(module, module)

    def module_of_path(self, path: str):
        segs = _segments(path)
        best = None
        for m in self.modules:
            r = m.match(segs)
            if r and (best is None or r[0] > best[0]):
                best = r
        return best[1] if best else None

    def module_of_header(self, header: str):
        if header in self.header_owner:
            return self.header_owner[header]
        m = re.fullmatch(r"de_([A-Za-z0-9_]+)\.h", header)
        if m and m.group(1) in self.by_name and not self.by_name[m.group(1)].wildcards:
            return m.group(1)
        return None

    def forbidden(self, frm: str, to: str):
        for f in self.forbids:
            if fnmatch.fnmatchcase(frm, f["from"]) and fnmatch.fnmatchcase(to, f["to"]):
                return f
        return None

    def allowed(self, frm: str, to: str) -> bool:
        if self.node(frm) == self.node(to):
            return True
        for a in self.allows:
            if fnmatch.fnmatchcase(frm, a["from"]) and fnmatch.fnmatchcase(to, a["to"]):
                return True
        return False


# --------------------------------------------------------------------------------------------
# Scanning


def collect_files(root: str) -> list[str]:
    files = []
    for top in ("src", COOK_LIB_DIR):
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in filenames:
                if fn.endswith(SOURCE_EXTS):
                    full = os.path.join(dirpath, fn)
                    files.append(os.path.relpath(full, root).replace(os.sep, "/"))
    pub = os.path.join(root, PUBLIC_DIR)
    if os.path.isdir(pub):
        for fn in os.listdir(pub):
            if fn.endswith(".h") and fn != UMBRELLA:
                files.append(f"{PUBLIC_DIR}/{fn}")
    return sorted(set(files))


def collect_example_files(root: str) -> list[str]:
    files = []
    base = os.path.join(root, EXAMPLES_DIR)
    if os.path.isdir(base):
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for fn in filenames:
                if fn.endswith(SOURCE_EXTS):
                    full = os.path.join(dirpath, fn)
                    files.append(os.path.relpath(full, root).replace(os.sep, "/"))
    return sorted(files)


def scan_includes(path: str):
    """Yield (line, delimiter, target) for each #include outside /* */ and // comments."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
    except OSError:
        return
    in_block = False
    for lineno, line in enumerate(lines, 1):
        text = line
        if in_block:
            end = text.find("*/")
            if end < 0:
                continue
            text = text[end + 2 :]
            in_block = False
        # Drop complete /* ... */ spans, then note an unterminated one.
        while True:
            start = text.find("/*")
            if start < 0:
                break
            end = text.find("*/", start + 2)
            if end < 0:
                text = text[:start]
                in_block = True
                break
            text = text[:start] + " " + text[end + 2 :]
        m = INCLUDE_RE.match(text)
        if m:
            yield lineno, m.group(1), m.group(2).strip()


class Resolved:
    """What an include target is: a module, the umbrella, a tool file, or an error."""

    def __init__(self, kind: str, value: str = ""):
        self.kind = kind  # module | umbrella | tool | unknown_header | unmapped
        self.value = value


class Checker:
    def __init__(self, root: str, table: Table):
        self.root = root
        self.table = table

    def _exists(self, rel: str) -> bool:
        return os.path.isfile(os.path.join(self.root, rel))

    def _header(self, name: str) -> Resolved:
        if name == UMBRELLA:
            return Resolved("umbrella", name)
        owner = self.table.module_of_header(name)
        return Resolved("module", owner) if owner else Resolved("unknown_header", name)

    def _classify_path(self, rel: str):
        if rel.startswith(PUBLIC_DIR + "/") and rel.count("/") == 2:
            return self._header(rel.rsplit("/", 1)[1])
        mod = self.table.module_of_path(rel)
        if mod:
            return Resolved("module", mod)
        if rel.startswith("tools/"):
            return Resolved("tool", rel)
        if rel.startswith("src/"):
            return Resolved("unmapped", rel)
        return None  # third_party, generated files, ...

    def resolve(self, includer: str, delim: str, target: str):
        if target.startswith("/") or re.match(r"^[A-Za-z]:", target):
            return None
        if target.startswith("dengine/"):
            return self._header(target[len("dengine/") :])
        segs = target.split("/")
        if delim == '"':
            cand = posixpath.normpath(posixpath.join(posixpath.dirname(includer), target))
            if not cand.startswith("../"):
                if self._exists(cand) or ".." in segs:
                    return self._classify_path(cand)
            if includer.startswith(PUBLIC_DIR + "/") and PUBLIC_HEADER_RE.match(target):
                return self._header(target)  # sibling public header
        for root in self.table.include_roots:
            rel = posixpath.normpath(posixpath.join(root, target))
            if len(segs) > 1 and segs[0] in self.table.root_dirs.get(root, ()):
                return self._classify_path(rel) or Resolved("unmapped", rel)
            if self._exists(rel):
                return self._classify_path(rel)
        if target.startswith("tools/"):
            return self._classify_path(posixpath.normpath(target))
        return None

    def example_include_error(self, includer: str, delim: str, target: str):
        """Error message for an include in an example file, or None if it is allowed."""
        inc = f"<{target}>" if delim == "<" else f'"{target}"'
        why = "examples include only public headers (<dengine/de_*.h>)"
        if target.startswith("/") or re.match(r"^[A-Za-z]:", target):
            return None
        if target.startswith("dengine/"):
            name = target[len("dengine/"):]
            if name == UMBRELLA or (self.table.module_of_header(name)
                                    and self._exists(f"{PUBLIC_DIR}/{name}")):
                return None
            return f"{inc} is not a public header in {PUBLIC_DIR}/"
        if delim == '"':
            cand = posixpath.normpath(posixpath.join(posixpath.dirname(includer), target))
            if not cand.startswith("../") and self._exists(cand):
                if cand.startswith(EXAMPLES_DIR + "/"):
                    return None  # the example's own files (scanned too)
                return f"{inc} resolves to {cand}; {why}"
        segs = target.split("/")
        for root in self.table.include_roots:
            rel = posixpath.normpath(posixpath.join(root, target))
            if self._exists(rel) or (len(segs) > 1
                                     and segs[0] in self.table.root_dirs.get(root, ())):
                return f"{inc} is engine-internal ({rel}); {why}"
        return None  # the C library, the OS, third-party headers

    def check_examples(self, violations: list) -> tuple[list[str], int]:
        files = collect_example_files(self.root)
        n_includes = 0
        for rel in files:
            if rel in self.table.example_internal:
                continue
            for line, delim, target in scan_includes(os.path.join(self.root, rel)):
                n_includes += 1
                msg = self.example_include_error(rel, delim, target)
                if msg:
                    violations.append((rel, line, msg))
        if os.path.isdir(os.path.join(self.root, EXAMPLES_DIR)):
            for f in sorted(self.table.example_internal):
                if not self._exists(f):
                    violations.append((f, 0, "[[example_internal]] names a file that does not "
                                       "exist; remove the entry from tools/check_layers/layers.toml"))
        return files, n_includes

    def file_module(self, rel: str):
        if rel.startswith(PUBLIC_DIR + "/"):
            return self.table.module_of_header(rel.rsplit("/", 1)[1])
        return self.table.module_of_path(rel)

    def run(self):
        t = self.table
        violations: list[tuple[str, int, str]] = []
        edges: dict[tuple[str, str], list[tuple[str, int]]] = {}
        illegal: set[tuple[str, str]] = set()
        files = collect_files(self.root)
        n_includes = 0
        self.modules_seen: set[str] = set()
        for rel in files:
            mod = self.file_module(rel)
            if mod is not None:
                self.modules_seen.add(mod)
            if mod is None:
                what = "public header has no owner" if rel.startswith(PUBLIC_DIR) else \
                    "file is not in any module"
                violations.append((rel, 0, f"{what}; add it to tools/check_layers/layers.toml"))
                continue
            for line, delim, target in scan_includes(os.path.join(self.root, rel)):
                n_includes += 1
                inc = f"<{target}>" if delim == "<" else f'"{target}"'
                r = self.resolve(rel, delim, target)
                if r is None:
                    continue
                if r.kind == "umbrella":
                    violations.append((rel, line, f"{mod} includes the umbrella header {inc}; "
                                       "dengine.h is for games, include the de_*.h you need"))
                    continue
                if r.kind == "unknown_header":
                    violations.append((rel, line, f"{inc} is not owned by any module; add it "
                                       "to `headers` in tools/check_layers/layers.toml"))
                    continue
                if r.kind == "unmapped":
                    violations.append((rel, line, f"{inc} resolves to {r.value}, which is not in "
                                       "any module in tools/check_layers/layers.toml"))
                    continue
                if r.kind == "tool":
                    violations.append((rel, line, f"engine module {mod} includes {inc} under "
                                       "tools/; engine code must not include tools"))
                    continue
                dep = r.value
                if dep == mod:
                    continue
                edges.setdefault((mod, dep), []).append((rel, line))
                msg = self.check_edge(mod, dep, inc)
                if msg:
                    illegal.add((mod, dep))
                    violations.append((rel, line, msg))

        ex_files, ex_includes = self.check_examples(violations)
        files = files + ex_files
        n_includes += ex_includes

        cycles = find_cycles(t, edges)
        return files, n_includes, edges, illegal, violations, cycles

    def check_edge(self, mod: str, dep: str, inc: str):
        """Error message for the module edge mod -> dep (seen as `inc`), or None if legal."""
        t = self.table
        if t.node(mod) == t.node(dep):
            return None
        mb, db = t.band(mod), t.band(dep)
        edge = f"{mod} ({mb}) includes {dep} ({db}) via {inc}"
        forbid = t.forbidden(mod, dep)
        if forbid:
            return f"forbidden include: {edge}; {forbid['why']}"
        if t.band_rank[db] > t.band_rank[mb]:
            return f"upward include: {edge}; a module may include only lower bands"
        if t.entry(dep).explicit_only and not t.allowed(mod, dep):
            return (f"restricted include: {edge}; only modules with an [[allow]] edge may "
                    f"include {dep} (engine code must not include tools/)")
        if db == mb and not t.allowed(mod, dep):
            return (f"same-band include not allowed: {edge}; if the edge is intended, add an "
                    "[[allow]] entry with a reason to tools/check_layers/layers.toml")
        return None


# --------------------------------------------------------------------------------------------
# Cycles (Tarjan SCC over group-collapsed nodes)


def tarjan_scc(nodes: list[str], succ: dict[str, list[str]]) -> list[list[str]]:
    index: dict[str, int] = {}
    low: dict[str, int] = {}
    on_stack: set[str] = set()
    stack: list[str] = []
    out: list[list[str]] = []
    counter = 0
    for start in nodes:
        if start in index:
            continue
        work = [(start, 0)]
        while work:
            v, i = work.pop()
            if i == 0:
                index[v] = low[v] = counter
                counter += 1
                stack.append(v)
                on_stack.add(v)
            nexts = succ.get(v, [])
            recurse = False
            while i < len(nexts):
                w = nexts[i]
                i += 1
                if w not in index:
                    work.append((v, i))
                    work.append((w, 0))
                    recurse = True
                    break
                if w in on_stack:
                    low[v] = min(low[v], index[w])
            if recurse:
                continue
            if low[v] == index[v]:
                comp = []
                while True:
                    w = stack.pop()
                    on_stack.discard(w)
                    comp.append(w)
                    if w == v:
                        break
                out.append(sorted(comp))
            if work:
                parent = work[-1][0]
                low[parent] = min(low[parent], low[v])
    return out


def find_cycles(table: Table, edges: dict):
    """Return one cycle (list of (node_from, node_to, example module edge)) per non-trivial SCC."""
    node_edges: dict[tuple[str, str], tuple[str, str]] = {}
    for (a, b) in sorted(edges):
        na, nb = table.node(a), table.node(b)
        if na != nb:
            node_edges.setdefault((na, nb), (a, b))
    succ: dict[str, list[str]] = {}
    nodes = set()
    for (na, nb) in node_edges:
        succ.setdefault(na, []).append(nb)
        nodes.update((na, nb))
    for v in succ:
        succ[v].sort()
    cycles = []
    for comp in tarjan_scc(sorted(nodes), succ):
        if len(comp) < 2:
            continue
        members = set(comp)
        start = comp[0]
        # Shortest path start -> ... -> start inside the component (BFS).
        prev = {start: None}
        queue = [start]
        found = None
        while queue and found is None:
            v = queue.pop(0)
            for w in succ.get(v, []):
                if w not in members:
                    continue
                if w == start:
                    found = v
                    break
                if w not in prev:
                    prev[w] = v
                    queue.append(w)
        path = [start]
        v = found
        while v is not None and v != start:
            path.append(v)
            v = prev[v]
        path = [start] + list(reversed(path[1:])) + [start]
        cycle = []
        for x, y in zip(path, path[1:]):
            cycle.append((x, y, node_edges[(x, y)]))
        cycles.append((comp, cycle))
    return cycles


# --------------------------------------------------------------------------------------------
# Graph output


def write_dot(path: str, table: Table, modules_seen: set, edges: dict, illegal: set,
              cycles: list):
    cycle_edges = set()
    for _, cyc in cycles:
        for _, _, me in cyc:
            cycle_edges.add(me)
    modules = sorted(modules_seen | {m for e in edges for m in e})
    by_band: dict[str, list[str]] = {b: [] for b in table.bands}
    for m in modules:
        by_band[table.band(m)].append(m)
    out = ["digraph dengine_layers {",
           "  rankdir=BT;",
           '  node [shape=box, fontname="Helvetica", fontsize=10];',
           '  edge [color="#888888", arrowsize=0.6];']
    for b in table.bands:
        out.append(f'  subgraph "cluster_{b}" {{')
        out.append(f'    label="{b}"; style=rounded; color="#bbbbbb";')
        for m in by_band[b]:
            out.append(f'    "{m}";')
        out.append("  }")
    for (a, b) in sorted(edges):
        attrs = [f'label="{len(edges[(a, b)])}"', "fontsize=8"]
        if (a, b) in illegal or (a, b) in cycle_edges:
            attrs.append('color="#d62728"')
            attrs.append("penwidth=2")
        elif table.node(a) == table.node(b):
            attrs.append("style=dashed")
        out.append(f'  "{a}" -> "{b}" [{", ".join(attrs)}];')
    out.append("}")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


# --------------------------------------------------------------------------------------------


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="DEngine include-graph layering check (SPEC §4.2).")
    ap.add_argument("--root", default=DEFAULT_ROOT, help="repository root (default: %(default)s)")
    ap.add_argument("--table", default=DEFAULT_TABLE, help="layer table (default: %(default)s)")
    ap.add_argument("--graph", metavar="OUT.dot", help="write the module graph as Graphviz dot")
    ap.add_argument("--list-edges", action="store_true", help="print every module edge")
    ap.add_argument("--toml-fallback", action="store_true",
                    help="use the built-in TOML reader even if tomllib exists (self-test)")
    ap.add_argument("--dump-table", action="store_true",
                    help="print the parsed table as JSON and exit (self-test)")
    args = ap.parse_args(argv)

    t0 = time.perf_counter()
    try:
        data = load_toml(args.table, args.toml_fallback)
        if args.dump_table:
            import json
            print(json.dumps(data, sort_keys=True, indent=1))
            return 0
        table = Table(data, args.table)
    except TableError as e:
        print(f"check_layers: error: {e}", file=sys.stderr)
        return 2
    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        print(f"check_layers: error: root {root} is not a directory", file=sys.stderr)
        return 2

    checker = Checker(root, table)
    files, n_inc, edges, illegal, violations, cycles = checker.run()

    if args.list_edges:
        for (a, b) in sorted(edges):
            mark = "  ILLEGAL" if (a, b) in illegal else ""
            print(f"edge {a} -> {b} ({len(edges[(a, b)])}){mark}")

    for rel, line, msg in sorted(violations):
        loc = f"{rel}:{line}" if line else rel
        print(f"{loc}: error: {msg}")
    for comp, cycle in cycles:
        chain = " -> ".join([cycle[0][0]] + [y for _, y, _ in cycle])
        print(f"error: cycle between {', '.join(comp)}: {chain}")
        for _, _, (a, b) in cycle:
            rel, line = edges[(a, b)][0]
            print(f"  {a} -> {b}: {rel}:{line}")

    if args.graph:
        write_dot(args.graph, table, checker.modules_seen, edges, illegal, cycles)

    elapsed = time.perf_counter() - t0
    n_bad = len(violations) + len(cycles)
    status = "OK" if n_bad == 0 else \
        f"FAILED: {len(violations)} violation(s), {len(cycles)} cycle(s)"
    print(f"check_layers: {len(files)} files, {n_inc} includes, {len(edges)} module edges, "
          f"{elapsed:.2f}s: {status}")
    return 1 if n_bad else 0


if __name__ == "__main__":
    sys.exit(main())
