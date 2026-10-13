#!/usr/bin/env python3
"""SPEC §5.7 source check of the deterministic code (DENG-136).

Reads tools/determinism_modules.toml (the DETERMINISTIC modules and the files passed to
de_mark_deterministic_sources()) and fails on:

  math-include  #include <math.h>, <tgmath.h> or <cmath>: use de_dmath (rule 2)
  libm-call     a call of a libm function (sin, cosf, sqrt, pow, fma, ...): use de_dmath (rule 2)
  simd-approx   approximate reciprocal intrinsics: _mm_rcp_ps, _mm_rsqrt_ss, _mm256_rsqrt_ps,
                ..., NEON / SVE vrecpe*, vrsqrte* and their step functions (rule 3)
  fast-math     -ffast-math, -Ofast, /fp:fast, -funsafe-math-optimizations, an FP contraction
                pragma or flag that turns contraction on, in a source or in the module's
                CMakeLists.txt (rule 1)
  fast-api      the fast-math de_math.h functions (de_v3_*, de_quat_*, de_m4_*, ...), which may
                use FMA or SIMD and differ per CPU (rule 2)
  hashmap-iter  de_hashmap_iter_begin / de_hashmap_iter_next (rule 5)
  rand          rand(), srand(), random(), drand48(): use de_rng (rule 6)

Comments and string literals are ignored (fast-math also looks into strings, for
__attribute__((optimize("fast-math")))). With --build-dir it also checks the build:

  - every translation unit of a listed module (and every listed file) in compile_commands.json is
    compiled without FP contraction: the last -ffp-contract= is "off" (Clang, GCC, and clang-cl
    through /clang:-ffp-contract=off), or MSVC cl has /fp:precise or /fp:strict last; and no
    fast-math flag;
  - the table equals the CMake declarations: every DETERMINISTIC module of de_config_dump.txt is a
    [[module]] with the same directory and every [[module]] that is built is DETERMINISTIC; every
    file of tests/checks/deterministic_objects_<config>.txt (de_mark_deterministic_sources, the
    list deterministic_asm_check disassembles) is a [[source]], and the other way round.

Every finding names the file and line. Exit status: 0 clean, 1 violations, 2 bad table or usage.
--self-test runs the checker on planted violations in a temporary tree (CTest
check_determinism_src_selftest). Standard library only; Python 3.10+ (tomllib from 3.11, else the
minimal TOML reader of tools/check_layers/check_layers.py).
"""

from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_ROOT = os.path.normpath(os.path.join(SCRIPT_DIR, ".."))
DEFAULT_TABLE = os.path.join(SCRIPT_DIR, "determinism_modules.toml")

SOURCE_EXTS = (".c", ".h", ".inl", ".cpp", ".cc", ".m", ".mm")
RULES = ("math-include", "libm-call", "simd-approx", "fast-math", "fast-api", "hashmap-iter",
         "rand")

_LIBM_BASE = (
    "sin cos tan asin acos atan atan2 sinh cosh tanh asinh acosh atanh exp exp2 expm1 log log2 "
    "log10 log1p logb pow sqrt cbrt hypot erf erfc tgamma lgamma fmod remainder remquo fma"
).split()
LIBM_FUNCS = frozenset(f + s for f in _LIBM_BASE for s in ("", "f", "l"))

RE_MATH_INCLUDE = re.compile(r"^[ \t]*#[ \t]*include[ \t]*<(math\.h|tgmath\.h|cmath)>", re.M)
RE_CALL = re.compile(r"(?<![A-Za-z0-9_.>])(?:std::)?([A-Za-z_][A-Za-z0-9_]*)[ \t]*\(")
RE_SIMD_APPROX = re.compile(
    r"\b(_mm(?:256|512)?_(?:mask_|maskz_)?(?:rcp|rsqrt)(?:14|28)?_(?:ps|ss|pd|sd|ph|sh)"
    r"|__builtin_ia32_(?:rcp|rsqrt)\w*"
    r"|v(?:recpe|rsqrte|recps|rsqrts)\w*"
    r"|sv(?:recpe|rsqrte|recps|rsqrts)\w*)\b")
# Fast math inside a source: a pragma, or an optimize attribute (its argument is a string).
RE_FAST_MATH = re.compile(
    r"(#[ \t]*pragma[^\n]*?(?:fast-math|Ofast|STDC[ \t]+FP_CONTRACT[ \t]+ON\b"
    r"|fp_contract[ \t]*\([ \t]*(?:on|fast)[ \t]*\)|fp[ \t]+contract[ \t]*\([ \t]*(?:on|fast)"
    r"[ \t]*\)|float_control[ \t]*\([ \t]*precise[ \t]*,[ \t]*off)"
    r"|optimize[ \t]*\([ \t]*\"[^\"\n]*(?:fast-math|Ofast)[^\"\n]*\")")
RE_FAST_API = re.compile(
    r"\bde_(?:v2|v3|v4|quat|m3|m4|affine|aabb|plane|ray|frustum)_[a-z0-9_]+[ \t]*\(")
RE_HASHMAP_ITER = re.compile(r"\bde_hashmap_iter_(?:begin|next)[ \t]*\(")
RE_RAND = re.compile(r"(?<![A-Za-z0-9_.>])(?:std::)?(rand|srand|random|srandom|drand48|rand_r)"
                     r"[ \t]*\(")
RE_CMAKE_FAST = re.compile(
    r"(-ffast-math|-Ofast\b|[/-]fp:fast|-funsafe-math-optimizations|-ffp-contract=(?:fast|on)\b)")


class TableError(Exception):
    """The table is missing or malformed."""


# --------------------------------------------------------------------------------------------
# Table


def _load_toml(path: str) -> dict:
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as e:
        raise TableError(f"{path}: {e}") from e
    try:
        import tomllib  # Python 3.11+
        return tomllib.loads(data.decode("utf-8"))
    except ImportError:
        pass
    except Exception as e:  # tomllib.TOMLDecodeError
        raise TableError(f"{path}: {e}") from e
    return _load_toml_minimal(path, data.decode("utf-8"))


def _load_toml_minimal(path: str, text: str) -> dict:
    sys.path.insert(0, os.path.join(SCRIPT_DIR, "check_layers"))
    sys.dont_write_bytecode = True  # no __pycache__ in the source tree
    try:
        from check_layers import parse_toml_minimal  # type: ignore
        from check_layers import TableError as LayersTableError  # type: ignore
    finally:
        sys.path.pop(0)
    try:
        return parse_toml_minimal(text, path)
    except LayersTableError as e:
        raise TableError(str(e)) from e


def load_table(path: str) -> tuple[list[dict], list[dict]]:
    raw = _load_toml(path)
    unknown = set(raw) - {"module", "source"}
    if unknown:
        raise TableError(f"{path}: unknown keys {sorted(unknown)}")
    modules = raw.get("module", [])
    sources = raw.get("source", [])
    names, dirs, paths = set(), set(), set()
    for kind, entries, required in (("module", modules, ("name", "dir")),
                                    ("source", sources, ("path", "module"))):
        if not isinstance(entries, list):
            raise TableError(f"{path}: [[{kind}]] must be an array of tables")
        for e in entries:
            for k in required:
                if not isinstance(e.get(k), str) or not e[k]:
                    raise TableError(f"{path}: a [[{kind}]] entry needs a string '{k}': {e}")
            extra = set(e) - set(required) - {"allow", "allow_calls", "why"}
            if extra:
                raise TableError(f"{path}: [[{kind}]] {e[required[0]]}: unknown keys "
                                 f"{sorted(extra)}")
            for k in ("allow", "allow_calls"):
                v = e.setdefault(k, [])
                if not isinstance(v, list) or not all(isinstance(x, str) for x in v):
                    raise TableError(f"{path}: {e[required[0]]}: '{k}' must be a string array")
            bad = [r for r in e["allow"] if r not in RULES]
            if bad:
                raise TableError(f"{path}: {e[required[0]]}: unknown rule ids {bad}")
            if (e["allow"] or e["allow_calls"]) and not e.get("why"):
                raise TableError(f"{path}: {e[required[0]]}: 'allow' needs a 'why'")
            key = e[required[0]]
            if kind == "module":
                if key in names or e["dir"] in dirs:
                    raise TableError(f"{path}: module {key} is listed twice")
                names.add(key)
                dirs.add(e["dir"].rstrip("/"))
                e["dir"] = e["dir"].rstrip("/")
            else:
                if key in paths:
                    raise TableError(f"{path}: source {key} is listed twice")
                paths.add(key)
    return modules, sources


# --------------------------------------------------------------------------------------------
# Source scanning


def strip_c(text: str) -> tuple[str, str]:
    """Returns (code, code_with_strings): comments blanked in both, string and character literal
    contents blanked in the first. Newlines are kept, so line numbers stay."""
    code, keep = [], []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            code.append(" " * (j - i))
            keep.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            blank = re.sub(r"[^\n]", " ", text[i:j])
            code.append(blank)
            keep.append(blank)
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            lit = text[i:j]
            code.append(c + " " * max(0, len(lit) - 2) + (c if len(lit) > 1 else ""))
            keep.append(lit)
            i = j
        else:
            code.append(c)
            keep.append(c)
            i += 1
    return "".join(code), "".join(keep)


def _line_of(text: str, pos: int) -> int:
    return text.count("\n", 0, pos) + 1


def scan_text(rel: str, text: str, allow: set[str], allow_calls: set[str]) -> list[str]:
    code, with_strings = strip_c(text)
    out = []

    def report(rule: str, pos: int, what: str, hint: str, src: str = code) -> None:
        if rule not in allow:
            out.append(f"{rel}:{_line_of(src, pos)}: error: {rule}: {what} ({hint})")

    for m in RE_MATH_INCLUDE.finditer(code):
        report("math-include", m.start(), f"<{m.group(1)}>", "use de_dmath")
    for m in RE_CALL.finditer(code):
        name = m.group(1)
        if name in LIBM_FUNCS and name not in allow_calls:
            report("libm-call", m.start(1), f"{name}()", "use de_dmath")
    for m in RE_SIMD_APPROX.finditer(code):
        report("simd-approx", m.start(), m.group(1), "approximate reciprocals differ per CPU")
    for m in RE_FAST_MATH.finditer(with_strings):
        report("fast-math", m.start(), m.group(1).strip(), "no fast math or FP contraction",
               with_strings)
    for m in RE_FAST_API.finditer(code):
        report("fast-api", m.start(), m.group(0).rstrip("( \t") + "()",
               "fast math may use FMA or SIMD; use de_dmath / de_dvec3")
    for m in RE_HASHMAP_ITER.finditer(code):
        report("hashmap-iter", m.start(), m.group(0).rstrip("( \t") + "()",
               "hash-map order is not deterministic")
    for m in RE_RAND.finditer(code):
        report("rand", m.start(1), f"{m.group(1)}()", "use a de_rng stream")
    return out


def scan_cmake(rel: str, text: str) -> list[str]:
    out = []
    lines = text.split("\n")
    for no, line in enumerate(lines, 1):
        code = line.split("#", 1)[0]
        for m in RE_CMAKE_FAST.finditer(code):
            out.append(f"{rel}:{no}: error: fast-math: {m.group(1)} in a deterministic module's "
                       "build (no fast math or FP contraction)")
    return out


def _read(path: str) -> str:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def scan_tree(root: str, modules: list[dict], sources: list[dict]) -> tuple[list[str], int]:
    errors: list[str] = []
    files = 0
    for mod in modules:
        base = os.path.join(root, mod["dir"])
        if not os.path.isdir(base):
            errors.append(f"{mod['dir']}: error: module {mod['name']}: directory not found")
            continue
        allow, calls = set(mod["allow"]), set(mod["allow_calls"])
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames.sort()
            for name in sorted(filenames):
                path = os.path.join(dirpath, name)
                rel = os.path.relpath(path, root).replace(os.sep, "/")
                if name.endswith(SOURCE_EXTS):
                    files += 1
                    errors += scan_text(rel, _read(path), allow, calls)
                elif name == "CMakeLists.txt" or name.endswith(".cmake"):
                    errors += scan_cmake(rel, _read(path))
    for src in sources:
        path = os.path.join(root, src["path"])
        if not os.path.isfile(path):
            errors.append(f"{src['path']}: error: listed [[source]] not found")
            continue
        files += 1
        errors += scan_text(src["path"], _read(path), set(src["allow"]), set(src["allow_calls"]))
    return errors, files


# --------------------------------------------------------------------------------------------
# Build checks


def _norm(path: str) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def _key(rel: str) -> str:
    """A repository-relative path as a lookup key: '/' separators, the OS's case rule."""
    return os.path.normcase(rel).replace("\\", "/")


def _compiler_kind(tokens: list[str]) -> str:
    for t in tokens:
        base = os.path.basename(t.strip('"')).lower()
        if base.startswith(("ccache", "sccache")):
            continue
        if base in ("clang-cl", "clang-cl.exe"):
            return "clang-cl"
        if base in ("cl", "cl.exe"):
            return "msvc"
        return "gnu"
    return "gnu"


RE_FP_CONTRACT = re.compile(r"-ffp-contract=(\w+)")
RE_MSVC_FP = re.compile(r"(?:^|\s)[/-]fp:(\w+)")
RE_TU_FAST = re.compile(r"(?:^|\s)(?:/clang:)?(-ffast-math|-Ofast|-funsafe-math-optimizations"
                        r"|-fassociative-math|-freciprocal-math|-mrecip|[/-]fp:fast)(?=\s|$)")


def tu_flag_problem(command: str, tokens: list[str]) -> str | None:
    """None when the compile command keeps FP contraction off and has no fast-math flag."""
    fast = RE_TU_FAST.search(command)
    if fast:
        return f"fast-math flag {fast.group(1)}"
    kind = _compiler_kind(tokens)
    contract = RE_FP_CONTRACT.findall(command)
    if kind == "msvc":
        fp = RE_MSVC_FP.findall(command)
        if not fp or fp[-1] not in ("precise", "strict"):
            return "no /fp:precise (MSVC cl)" if not fp else f"/fp:{fp[-1]} last (MSVC cl)"
        return None
    if not contract:
        return ("no /clang:-ffp-contract=off (clang-cl's /fp:precise keeps contraction on)"
                if kind == "clang-cl" else "no -ffp-contract=off")
    if contract[-1] != "off":
        return f"-ffp-contract={contract[-1]} last"
    return None


def _read_kv(path: str) -> dict[str, str]:
    out = {}
    for line in _read(path).splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def _split(v: str) -> list[str]:
    return [x for x in v.split(";") if x]


def check_build(root: str, build: str, config: str | None, modules: list[dict],
                sources: list[dict]) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    notes: list[str] = []

    # ---- What CMake declared ----
    dumps = [os.path.join(build, "de_config_dump.txt")]
    if config:
        dumps.insert(0, os.path.join(build, f"de_config_dump-{config}.txt"))
    dump_path = next((d for d in dumps if os.path.isfile(d)), None)
    if dump_path is None:
        errors.append(f"{build}: error: no de_config_dump.txt (configure the build first)")
        return errors, notes
    dump = _read_kv(dump_path)
    built = set(_split(dump.get("modules", "")))
    det = set(_split(dump.get("deterministic", "")))
    dirs = {}
    for entry in _split(dump.get("layers", "")):
        parts = entry.split(":")
        if len(parts) == 3:
            dirs[parts[0]] = parts[2]
    table_names = {m["name"]: m for m in modules}
    for name in sorted(det):
        if name not in table_names:
            errors.append(f"tools/determinism_modules.toml: error: module {name} "
                          f"({dirs.get(name, '?')}) is DETERMINISTIC in CMake but not listed")
        elif dirs.get(name) and dirs[name] != table_names[name]["dir"]:
            errors.append(f"tools/determinism_modules.toml: error: module {name}: dir "
                          f"{table_names[name]['dir']}, CMake builds it from {dirs[name]}")
    for name in sorted(table_names):
        if name not in built:
            notes.append(f"module {name} is not built in this configuration (flags not checked)")
        elif name not in det:
            errors.append(f"tools/determinism_modules.toml: error: module {name} is listed but "
                          "not DETERMINISTIC in CMake (de_add_module(... DETERMINISTIC))")

    lists = sorted(glob.glob(os.path.join(build, "tests", "checks",
                                          "deterministic_objects_*.txt")))
    if config:
        want = os.path.join(build, "tests", "checks", f"deterministic_objects_{config}.txt")
        lists = [want] if os.path.isfile(want) else lists
    if not lists:
        errors.append(f"{build}: error: no tests/checks/deterministic_objects_<config>.txt "
                      "(written by tests/checks/CMakeLists.txt at generate time)")
    marked = set()
    for lst in lists:
        for line in _read(lst).splitlines():
            parts = line.split(" ", 3)
            if len(parts) >= 3 and parts[0] == "source":
                target, rel = parts[1], parts[2]
                mod = target[3:] if target.startswith("de_") else target
                if mod in dirs:
                    marked.add(f"{dirs[mod]}/{rel}")
                else:
                    errors.append(f"{lst}: error: unknown target {target}")
    table_sources = {s["path"]: s for s in sources}
    for path in sorted(marked - set(table_sources)):
        errors.append(f"tools/determinism_modules.toml: error: {path} is passed to "
                      "de_mark_deterministic_sources() but not listed as a [[source]]")
    for path, s in sorted(table_sources.items()):
        if path not in marked:
            if s["module"] in built:
                errors.append(f"tools/determinism_modules.toml: error: [[source]] {path} is not "
                              "passed to de_mark_deterministic_sources()")
            else:
                notes.append(f"{path}: module {s['module']} is not built (not checked)")

    # ---- How it is compiled ----
    cc_path = os.path.join(build, "compile_commands.json")
    if not os.path.isfile(cc_path):
        errors.append(f"{build}: error: no compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS; "
                      "the Ninja and Makefile generators write it)")
        return errors, notes
    with open(cc_path, "r", encoding="utf-8") as f:
        entries = json.load(f)
    root_n = _norm(root)
    tus: dict[str, list[tuple[str, list[str]]]] = {}
    for e in entries:
        path = e["file"]
        if not os.path.isabs(path):
            path = os.path.join(e.get("directory", ""), path)
        p = _norm(path)
        if not p.startswith(root_n + os.sep):
            continue
        rel = _key(os.path.relpath(p, root_n))
        if "arguments" in e:
            tokens = list(e["arguments"])
            command = " ".join(tokens)
        else:
            command = e.get("command", "")
            try:
                tokens = shlex.split(command, posix=False)
            except ValueError:
                tokens = command.split()
        tus.setdefault(rel, []).append((command, tokens))
    n_tus = 0
    for mod in modules:
        if mod["name"] not in built:
            continue
        prefix = _key(mod["dir"]) + "/"
        mine = sorted(r for r in tus if r.startswith(prefix))
        if not mine:
            errors.append(f"{mod['dir']}: error: module {mod['name']} is built but has no "
                          "translation unit in compile_commands.json")
        for rel in mine:
            for command, tokens in tus[rel]:
                n_tus += 1
                problem = tu_flag_problem(command, tokens)
                if problem:
                    errors.append(f"{rel}: error: fast-math: compiled with FP contraction or fast "
                                  f"math: {problem}")
    for path, s in sorted(table_sources.items()):
        if s["module"] not in built:
            continue
        rel = _key(path)
        if rel not in tus:
            errors.append(f"{path}: error: listed [[source]] has no entry in compile_commands.json")
            continue
        for command, tokens in tus[rel]:
            n_tus += 1
            problem = tu_flag_problem(command, tokens)
            if problem:
                errors.append(f"{path}: error: fast-math: compiled with FP contraction or fast "
                              f"math: {problem}")
    notes.append(f"{n_tus} translation units compiled without FP contraction")
    return errors, notes


# --------------------------------------------------------------------------------------------
# Main


def run(root: str, table: str, build: str | None, config: str | None) -> int:
    try:
        modules, sources = load_table(table)
    except TableError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    errors, files = scan_tree(root, modules, sources)
    notes: list[str] = []
    if build:
        build_errors, notes = check_build(root, build, config, modules, sources)
        errors += build_errors
    for n in notes:
        print(f"note: {n}")
    for e in errors:
        print(e)
    summary = (f"check_determinism_src: {len(modules)} modules, {len(sources)} files, "
               f"{files} sources scanned")
    if errors:
        print(f"{summary}: {len(errors)} violation(s)")
        return 1
    print(f"{summary}: OK")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", default=DEFAULT_ROOT, help="repository root")
    ap.add_argument("--table", default=None, help="the table (default: "
                    "<root>/tools/determinism_modules.toml)")
    ap.add_argument("--build-dir", default=None, help="a configured build directory: also "
                    "check compile_commands.json and the CMake declarations")
    ap.add_argument("--config", default=None, help="build configuration (multi-config "
                    "generators; picks deterministic_objects_<config>.txt)")
    ap.add_argument("--self-test", action="store_true", help="run the self test and exit")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    table = a.table or os.path.join(a.root, "tools", "determinism_modules.toml")
    return run(a.root, table, a.build_dir, a.config or None)


# --------------------------------------------------------------------------------------------
# Self test


def _write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


SELF_TABLE = """
[[module]]
name = "mod"
dir = "src/mod"

[[source]]
path = "src/core/dm.c"
module = "core"
allow = ["math-include"]
allow_calls = ["sqrtf"]
why = "self test"
"""

CLEAN_C = """/* clean.c: mentions sinf(x) and _mm_rsqrt_ps(v) only in comments and strings. */
// sqrt(2), vrsqrteq_f32(v), rand(), de_v3_add(a, b), #include <math.h>
static const char *k = "sinf(x) _mm_rsqrt_ps(v) -ffast-math";
float my_sinf(float x);              /* names that merely contain a libm name */
float g(float x) { return my_sinf(x) + de_dsin(x); }
struct s { float (*sqrt)(float); };
float h(struct s *p, float x) { return p->sqrt(x); }   /* a member, not libm */
"""

PLANTED = {
    "sinf.c": ("float f(float x) {\n    return sinf(x);\n}\n", "src/mod/sinf.c:2: error: libm-call: sinf()"),
    "rsqrt.c": ("#include <xmmintrin.h>\n__m128 g(__m128 v) { return _mm_rsqrt_ps(v); }\n",
                "src/mod/rsqrt.c:2: error: simd-approx: _mm_rsqrt_ps"),
    "neon.c": ("float32x4_t n(float32x4_t v) { return vrsqrteq_f32(vrecpe_f32(v)); }\n",
               "src/mod/neon.c:1: error: simd-approx: vrsqrteq_f32"),
    "inc.c": ("#include <stdio.h>\n#  include <math.h>\n", "src/mod/inc.c:2: error: math-include: <math.h>"),
    "sqrt.c": ("double r(double x) { return sqrt(x); }\n", "src/mod/sqrt.c:1: error: libm-call: sqrt()"),
    "pragma.c": ("#pragma STDC FP_CONTRACT ON\n", "src/mod/pragma.c:1: error: fast-math"),
    "attr.c": ("__attribute__((optimize(\"fast-math\"))) void a(void) {}\n",
               "src/mod/attr.c:1: error: fast-math"),
    "api.c": ("de_vec3 v(de_vec3 a) { return de_v3_normalize(a); }\n",
              "src/mod/api.c:1: error: fast-api: de_v3_normalize()"),
    "map.c": ("void m(de_hashmap *h) { de_hashmap_iter it = de_hashmap_iter_begin(h); }\n",
              "src/mod/map.c:1: error: hashmap-iter: de_hashmap_iter_begin()"),
    "rand.c": ("int r(void) { return rand(); }\n", "src/mod/rand.c:1: error: rand: rand()"),
}


def _run_checker(args: list[str]) -> tuple[int, str]:
    p = subprocess.run([sys.executable, os.path.abspath(__file__)] + args, capture_output=True,
                       text=True)
    return p.returncode, p.stdout + p.stderr


def _fake_build(build: str, root: str, tus: dict[str, str], det: str, marked: list[str]) -> None:
    os.makedirs(os.path.join(build, "tests", "checks"), exist_ok=True)
    _write(os.path.join(build, "de_config_dump.txt"),
           "build_type=Dev\nmodules=core;mod;other\n"
           f"deterministic={det}\n"
           "layers=core:foundation:src/core;mod:gameplay:src/mod;other:gameplay:src/other\n")
    lines = ["build de_mod de_core"] + [f"source de_core {m} /x/{m}.o" for m in marked]
    _write(os.path.join(build, "tests", "checks", "deterministic_objects_Dev.txt"),
           "\n".join(lines) + "\n")
    entries = [{"directory": build, "file": os.path.join(root, rel), "command": cmd}
               for rel, cmd in tus.items()]
    _write(os.path.join(build, "compile_commands.json"), json.dumps(entries))


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str, out: str = "") -> None:
        if not cond:
            failures.append(what + (("\n--- output ---\n" + out) if out else ""))

    tmp = tempfile.mkdtemp(prefix="check_det_src_")
    try:
        root = os.path.join(tmp, "repo")
        table = os.path.join(root, "tools", "determinism_modules.toml")
        _write(table, SELF_TABLE)
        _write(os.path.join(root, "src", "mod", "clean.c"), CLEAN_C)
        _write(os.path.join(root, "src", "mod", "CMakeLists.txt"),
               "de_add_module(NAME mod LAYER gameplay SOURCES clean.c DETERMINISTIC)\n"
               "# a comment that says -ffast-math is fine\n")
        _write(os.path.join(root, "src", "core", "dm.c"),
               "#include <math.h>\nfloat q(float x) { return sqrtf(x); }\n")

        # 1. The clean tree passes: comments, strings, members and longer names are not calls.
        rc, out = _run_checker(["--root", root, "--table", table])
        expect(rc == 0 and ": OK" in out, "clean tree must pass", out)

        # 2. Each planted violation is reported with file, line and rule; the clean file is not.
        for name, (text, want) in PLANTED.items():
            path = os.path.join(root, "src", "mod", name)
            _write(path, text)
            rc, out = _run_checker(["--root", root, "--table", table])
            expect(rc == 1 and want in out and "clean.c" not in out,
                   f"planted {name}: expected exit 1 and '{want}'", out)
            os.remove(path)
        # Both NEON intrinsics of neon.c count.
        _write(os.path.join(root, "src", "mod", "neon.c"), PLANTED["neon.c"][0])
        rc, out = _run_checker(["--root", root, "--table", table])
        expect("vrecpe_f32" in out and "2 violation(s)" in out, "both NEON estimates flagged", out)
        os.remove(os.path.join(root, "src", "mod", "neon.c"))

        # 3. The module's CMakeLists.txt must not turn on fast math.
        cml = os.path.join(root, "src", "mod", "CMakeLists.txt")
        _write(cml, "target_compile_options(de_mod PRIVATE -ffast-math)\n")
        rc, out = _run_checker(["--root", root, "--table", table])
        expect(rc == 1 and "src/mod/CMakeLists.txt:1: error: fast-math: -ffast-math" in out,
               "fast math in CMakeLists.txt", out)
        _write(cml, "de_add_module(NAME mod LAYER gameplay SOURCES clean.c DETERMINISTIC)\n")

        # 4. allow_calls covers sqrtf only: sinf in the same [[source]] is still an error.
        dm = os.path.join(root, "src", "core", "dm.c")
        _write(dm, "#include <math.h>\nfloat q(float x) { return sqrtf(x) + sinf(x); }\n")
        rc, out = _run_checker(["--root", root, "--table", table])
        expect(rc == 1 and "src/core/dm.c:2: error: libm-call: sinf()" in out
               and "sqrtf" not in out, "allow_calls is per function", out)
        _write(dm, "#include <math.h>\nfloat q(float x) { return sqrtf(x); }\n")

        # 5. Build checks: flags in compile_commands.json and the CMake declarations.
        build = os.path.join(tmp, "build")
        good = "/usr/bin/clang -O2 -ffp-contract=off -c x.c"
        tus = {"src/mod/clean.c": good, "src/core/dm.c": good}
        _fake_build(build, root, tus, "mod", ["dm.c"])
        args = ["--root", root, "--table", table, "--build-dir", build]
        rc, out = _run_checker(args)
        expect(rc == 0 and "2 translation units" in out, "good build passes", out)

        cases = [
            ("/usr/bin/clang -O2 -c x.c", "no -ffp-contract=off"),
            ("/usr/bin/gcc -ffp-contract=off -O2 -ffp-contract=fast -c x.c",
             "-ffp-contract=fast last"),
            ("/usr/bin/clang -ffp-contract=off -ffast-math -c x.c", "fast-math flag -ffast-math"),
            ("C:/LLVM/bin/clang-cl.exe /fp:precise /c x.c", "clang-cl's /fp:precise"),
            ("cl.exe /nologo /fp:fast /c x.c", "fast-math flag /fp:fast"),
            ("cl.exe /nologo /c x.c", "no /fp:precise (MSVC cl)"),
        ]
        for cmd, want in cases:
            _fake_build(build, root, {"src/mod/clean.c": cmd, "src/core/dm.c": good}, "mod",
                        ["dm.c"])
            rc, out = _run_checker(args)
            expect(rc == 1 and "src/mod/clean.c: error: fast-math" in out and want in out,
                   f"flags '{cmd}': expected '{want}'", out)
        for cmd in ("cl.exe /nologo /fp:precise /c x.c",
                    "C:/LLVM/bin/clang-cl.exe /clang:-ffp-contract=off /c x.c",
                    "/usr/bin/ccache /usr/bin/gcc -ffp-contract=off -c x.c"):
            _fake_build(build, root, {"src/mod/clean.c": cmd, "src/core/dm.c": good}, "mod",
                        ["dm.c"])
            rc, out = _run_checker(args)
            expect(rc == 0, f"flags '{cmd}' must pass", out)

        # A DETERMINISTIC module that the table lacks, a listed module that is not
        # DETERMINISTIC, a marked file that is not listed, a listed file that is not marked.
        _fake_build(build, root, tus, "mod;other", ["dm.c"])
        rc, out = _run_checker(args)
        expect(rc == 1 and "module other (src/other) is DETERMINISTIC in CMake but not listed"
               in out, "missing module", out)
        _fake_build(build, root, tus, "", ["dm.c"])
        rc, out = _run_checker(args)
        expect(rc == 1 and "module mod is listed but not DETERMINISTIC" in out,
               "module not deterministic", out)
        _fake_build(build, root, tus, "mod", ["dm.c", "extra.c"])
        rc, out = _run_checker(args)
        expect(rc == 1 and "src/core/extra.c is passed to de_mark_deterministic_sources() but "
               "not listed" in out, "unlisted marked source", out)
        _fake_build(build, root, tus, "mod", [])
        rc, out = _run_checker(args)
        expect(rc == 1 and "[[source]] src/core/dm.c is not passed to" in out,
               "listed source not marked", out)

        # 6. A bad table is exit 2.
        _write(table, SELF_TABLE + '\n[[module]]\nname = "x"\ndir = "src/x"\nallow = ["bogus"]\n'
               'why = "t"\n')
        rc, out = _run_checker(["--root", root, "--table", table])
        expect(rc == 2 and "unknown rule ids" in out, "bad table", out)

        # 7. The minimal TOML reader (Python 3.10) reads the real table like tomllib.
        try:
            import tomllib
            real = os.path.join(SCRIPT_DIR, "determinism_modules.toml")
            with open(real, "rb") as f:
                data = f.read()
            expect(tomllib.loads(data.decode("utf-8")) ==
                   _load_toml_minimal(real, data.decode("utf-8")),
                   "minimal TOML reader differs from tomllib on determinism_modules.toml")
        except ImportError:
            pass
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for f in failures:
        print(f"FAIL: {f}")
    print(f"check_determinism_src self test: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
