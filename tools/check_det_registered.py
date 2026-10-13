#!/usr/bin/env python3
"""Every determinism TEST has a CTest entry (DENG-396).

A de_test case in tests/determinism/ that no CTest entry selects never runs, and so passes
silently. This check compares the cases that the determinism sources declare with the CTest
entries of the build, directory by directory (tests/determinism/<dir>/), and fails on:

  unregistered  a case that no CTest entry of its directory selects with --filter=
  stale         a CTest entry whose --filter= selects no case of its directory

Cases. It reads the sources under tests/determinism/ that compile_commands.json compiles (a
directory whose target is not built has no cases) and collects, with comments ignored:
  - TEST(suite, name) and TEST_F(suite, name, fixture): the case `suite.name`;
  - calls of registration macros: a function-like #define in any tests/determinism/ source whose
    body uses TEST or TEST_F (DE_PHYS_DET_REGISTER(name) gives name.workers_0, .workers_1,
    .workers_4), expanded with their arguments; such a macro may call another one.
DE_TEST_CASE and DE_TEST_F_CASE are not registered, so they are not cases. A TEST written
directly in a header is not seen (headers are not translation units). #if, #ifdef, #ifndef,
#elif and #else are evaluated with the -D / /D definitions of the file's compile command and the
file's own #define / #undef lines. A macro that comes from an included header or the compiler is
unknown: a case under a condition that depends on it is optional (not required, and an entry that
selects it is not stale), and the check prints a note.

CTest entries. It reads the CTestTestfile.cmake files of the build's tests/determinism/ tree:
what `ctest -N` lists. Not `ctest --show-only=json-v1`, because that drops the command of a test
whose executable is not built yet, and the `check` label runs on a configured tree with no build
(checks.yml). An entry belongs to the directory of its CTestTestfile.cmake. An entry selects the
cases that its --filter=POS[-NEG] argument matches, with the de_test rules (':'-separated globs
with * and ?; after '-', globs to exclude). An entry with no --filter= (for example the cmake -P
check determinism_physics.registered) selects nothing here.

Exit status: 0 clean, 1 violations, 2 bad input or usage. --self-test runs the checker on planted
trees (CTest check_det_registered_selftest). Standard library only; Python 3.10+.
"""

from __future__ import annotations

import argparse
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
SOURCE_EXTS = (".c", ".h", ".inl", ".cpp", ".cc")

RE_TEST = re.compile(
    r"(?<![A-Za-z0-9_])TEST(_F)?\s*\(\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*[,)]")
RE_DIRECTIVE = re.compile(r"^\s*#\s*([a-z]+)\b(.*)$", re.S)
RE_FUNC_DEFINE = re.compile(r"([A-Za-z_]\w*)\(([^)]*)\)(.*)$", re.S)
RE_OBJ_DEFINE = re.compile(r"([A-Za-z_]\w*)(?:\s+(.*))?$", re.S)


class InputError(Exception):
    pass


# --------------------------------------------------------------------------------------------
# Sources


def strip_comments(text: str) -> str:
    """Blanks comments and the contents of string and character literals; keeps newlines."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and nxt == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(c + re.sub(r"[^\n]", " ", text[i + 1:j - 1]) + (c if j - i > 1 else ""))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def logical_lines(text: str) -> list[tuple[int, str]]:
    """(first line number, text) per logical line: backslash-newline continuations joined."""
    lines, buf, start = [], [], 0
    for no, line in enumerate(text.split("\n"), 1):
        if not buf:
            start = no
        if line.endswith("\\"):
            buf.append(line[:-1])
            continue
        buf.append(line)
        lines.append((start, " ".join(buf)))
        buf = []
    if buf:
        lines.append((start, " ".join(buf)))
    return lines


class Macro:
    def __init__(self, params: list[str], body: str, where: str):
        self.params, self.body, self.where = params, body, where


def collect_macros(files: list[tuple[str, str]]) -> dict[str, Macro]:
    """The registration macros: function-like #defines whose body (directly or through another
    registration macro) uses TEST or TEST_F."""
    defs: dict[str, Macro] = {}
    for rel, code in files:
        for no, line in logical_lines(code):
            m = RE_DIRECTIVE.match(line)
            if not m or m.group(1) != "define":
                continue
            f = RE_FUNC_DEFINE.match(m.group(2).strip())
            if f:
                params = [p.strip() for p in f.group(2).split(",") if p.strip()]
                defs[f.group(1)] = Macro(params, f.group(3), f"{rel}:{no}")
    regs: dict[str, Macro] = {}
    changed = True
    while changed:
        changed = False
        for name, mac in defs.items():
            if name in regs:
                continue
            if RE_TEST.search(mac.body) or any(re.search(rf"\b{re.escape(r)}\s*\(", mac.body)
                                               for r in regs):
                regs[name] = mac
                changed = True
    return regs


def _call_args(text: str, open_paren: int) -> tuple[list[str], int] | None:
    """Splits the arguments of a call whose '(' is at open_paren; returns (args, end index)."""
    depth, args, cur = 0, [], []
    for i in range(open_paren, len(text)):
        c = text[i]
        if c == "(":
            depth += 1
            if depth == 1:
                continue
        elif c == ")":
            depth -= 1
            if depth == 0:
                args.append("".join(cur).strip())
                return [a for a in args if a or len(args) > 1], i + 1
        elif c == "," and depth == 1:
            args.append("".join(cur).strip())
            cur = []
            continue
        cur.append(c)
    return None


def expand_cases(text: str, macros: dict[str, Macro], depth: int = 0) -> list[str]:
    """The case names that `text` registers: TEST / TEST_F, and registration macro calls."""
    if depth > 16:
        raise InputError("registration macros nest more than 16 deep")
    cases = [f"{m.group(2)}.{m.group(3)}" for m in RE_TEST.finditer(text)]
    if not macros:
        return cases
    names = "|".join(re.escape(n) for n in sorted(macros))
    for m in re.finditer(rf"(?<![A-Za-z0-9_])({names})\s*\(", text):
        mac = macros[m.group(1)]
        parsed = _call_args(text, m.end() - 1)
        if parsed is None:
            continue
        args, _ = parsed
        subst = dict(zip(mac.params, args))
        body = re.sub(r"[A-Za-z_]\w*", lambda t: subst.get(t.group(0), t.group(0)), mac.body)
        body = re.sub(r"\s*##\s*", "", body)
        cases += expand_cases(body, macros, depth + 1)
    return cases


# Tri-state truth for #if: True, False, or None (depends on a macro the check cannot know).
def _not3(a):
    return None if a is None else not a


def _and3(a, b):
    if a is False or b is False:
        return False
    return None if a is None or b is None else True


def _or3(a, b):
    if a is True or b is True:
        return True
    return None if a is None or b is None else False


class _BadExpr(Exception):
    pass


class CondEval:
    """Evaluates a #if expression to an int, or None when it depends on an unknown macro (not on
    the command line, not defined or #undef'd in the file). Operators: defined, ! ~ unary - +,
    * / % + - << >> < <= > >= == != & ^ | && || ?: and parentheses. && and || short-circuit, so
    `1 || UNKNOWN` is 1 and `0 && UNKNOWN` is 0."""

    TOKEN = re.compile(r"\s*(?:(0[xX][0-9a-fA-F]+|\d+)[uUlL]*|([A-Za-z_]\w*)"
                       r"|(&&|\|\||==|!=|<=|>=|<<|>>|[!~()<>+\-*/%&|^?:]))")
    LEVELS = [["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="], ["<", "<=", ">", ">="],
              ["<<", ">>"], ["+", "-"], ["*", "/", "%"]]
    BINOPS = {"|": lambda a, b: a | b, "^": lambda a, b: a ^ b, "&": lambda a, b: a & b,
              "==": lambda a, b: int(a == b), "!=": lambda a, b: int(a != b),
              "<": lambda a, b: int(a < b), "<=": lambda a, b: int(a <= b),
              ">": lambda a, b: int(a > b), ">=": lambda a, b: int(a >= b),
              "<<": lambda a, b: a << b if 0 <= b < 64 else None,
              ">>": lambda a, b: a >> b if 0 <= b < 64 else None,
              "+": lambda a, b: a + b, "-": lambda a, b: a - b, "*": lambda a, b: a * b,
              "/": lambda a, b: int(a / b) if b else None,
              "%": lambda a, b: a - b * int(a / b) if b else None}

    def __init__(self, defines: dict[str, str | None], depth: int = 0):
        self.defines, self.depth = defines, depth
        self.toks: list[tuple[str, str]] = []
        self.i = 0

    def eval(self, expr: str) -> int | None:
        try:
            self.toks, self.i = self._lex(expr), 0
            v = self._ternary()
            if self.i != len(self.toks):
                raise _BadExpr()
            return v
        except _BadExpr:
            return None  # an expression this evaluator does not handle: unknown

    def _lex(self, expr: str) -> list[tuple[str, str]]:
        toks, pos, expr = [], 0, expr.strip()
        while pos < len(expr):
            m = self.TOKEN.match(expr, pos)
            if not m:
                raise _BadExpr()
            if m.group(1) is not None:
                toks.append(("num", m.group(1)))
            elif m.group(2) is not None:
                toks.append(("id", m.group(2)))
            else:
                toks.append(("op", m.group(3)))
            pos = m.end()
        return toks

    def _peek(self) -> tuple[str, str] | None:
        return self.toks[self.i] if self.i < len(self.toks) else None

    def _take(self, op: str) -> bool:
        if self._peek() == ("op", op):
            self.i += 1
            return True
        return False

    def _ternary(self) -> int | None:
        c = self._binary(0)
        if not self._take("?"):
            return c
        a = self._ternary()
        if not self._take(":"):
            raise _BadExpr()
        b = self._ternary()
        if c is None:
            return a if a == b else None
        return a if c else b

    def _binary(self, level: int) -> int | None:
        if level == len(self.LEVELS):
            return self._unary()
        v = self._binary(level + 1)
        while True:
            t = self._peek()
            if not t or t[0] != "op" or t[1] not in self.LEVELS[level]:
                return v
            self.i += 1
            r = self._binary(level + 1)
            tv = None if v is None else bool(v)
            tr = None if r is None else bool(r)
            if t[1] == "||":
                o = _or3(tv, tr)
                v = None if o is None else int(o)
            elif t[1] == "&&":
                o = _and3(tv, tr)
                v = None if o is None else int(o)
            elif v is None or r is None:
                v = None
            else:
                v = self.BINOPS[t[1]](v, r)

    def _unary(self) -> int | None:
        t = self._peek()
        if t is None:
            raise _BadExpr()
        self.i += 1
        if t[0] == "op":
            if t[1] == "(":
                v = self._ternary()
                if not self._take(")"):
                    raise _BadExpr()
                return v
            if t[1] in ("!", "~", "-", "+"):
                v = self._unary()
                if v is None:
                    return None
                return {"!": lambda: int(not v), "~": lambda: ~v, "-": lambda: -v,
                        "+": lambda: v}[t[1]]()
            raise _BadExpr()
        if t[0] == "num":
            return int(t[1], 0) if t[1][:2].lower() == "0x" else int(t[1])
        name = t[1]
        if name == "defined":
            paren = self._take("(")
            n = self._peek()
            if not n or n[0] != "id":
                raise _BadExpr()
            self.i += 1
            if paren and not self._take(")"):
                raise _BadExpr()
            if n[1] not in self.defines:
                return None
            return int(self.defines[n[1]] is not None)
        if self._peek() == ("op", "("):
            raise _BadExpr()  # a function-like macro (__has_include, ...): unknown
        if name not in self.defines:
            return None
        value = self.defines[name]
        if value is None:
            return 0  # #undef'd: an undefined identifier is 0
        if not value.strip() or self.depth >= 16:
            return None
        return CondEval(self.defines, self.depth + 1).eval(value)


def cond3(expr: str, defines: dict[str, str | None]):
    v = CondEval(defines).eval(expr)
    return None if v is None else bool(v)


def scan_file(rel: str, code: str, defines: dict[str, str | None],
              macros: dict[str, Macro]) -> list[tuple[str, int, object]]:
    """(case, line, active) for each case of one translation unit; active is True (compiled),
    or None (under a condition the check cannot evaluate). Cases in inactive blocks are left
    out."""
    defines = dict(defines)
    # Each level: [state of the current branch (tri), any earlier branch taken (tri)].
    stack: list[list] = []
    out: list[tuple[str, int, object]] = []

    def active():
        a = True
        for cur, _ in stack:
            a = _and3(a, cur)
        return a

    for no, line in logical_lines(code):
        m = RE_DIRECTIVE.match(line)
        if m:
            d, rest = m.group(1), m.group(2).strip()
            if d in ("if", "ifdef", "ifndef"):
                if d == "if":
                    c = cond3(rest, defines)
                else:
                    name = rest.split()[0] if rest.split() else ""
                    c = None if name not in defines else defines[name] is not None
                    if d == "ifndef":
                        c = _not3(c)
                stack.append([c, c])
            elif d == "elif":
                if not stack:
                    raise InputError(f"{rel}:{no}: #elif without #if")
                taken = stack[-1][1]
                c = cond3(rest, defines)
                stack[-1] = [_and3(_not3(taken), c), _or3(taken, c)]
            elif d == "else":
                if not stack:
                    raise InputError(f"{rel}:{no}: #else without #if")
                taken = stack[-1][1]
                stack[-1] = [_not3(taken), True]
            elif d == "endif":
                if not stack:
                    raise InputError(f"{rel}:{no}: #endif without #if")
                stack.pop()
            elif d in ("define", "undef") and rest:
                a = active()
                if a is False:
                    continue
                if d == "undef":
                    name = rest.split()[0]
                    if a:
                        defines[name] = None
                    else:
                        defines.pop(name, None)
                    continue
                o = RE_OBJ_DEFINE.match(rest)
                if not o or re.match(r"[A-Za-z_]\w*\(", rest):
                    continue  # function-like: a #if that calls it is unknown
                if a:
                    defines[o.group(1)] = (o.group(2) or "").strip()
                else:
                    defines.pop(o.group(1), None)  # defined only under an unknown condition
            continue
        a = active()
        if a is False:
            continue
        for case in expand_cases(line, macros):
            out.append((case, no, a))
    if stack:
        raise InputError(f"{rel}: unterminated #if")
    return out


# --------------------------------------------------------------------------------------------
# compile_commands.json


def _norm(path: str) -> str:
    return os.path.normcase(os.path.normpath(os.path.abspath(path)))


def _unquote(tok: str) -> str:
    if len(tok) >= 2 and tok[0] == tok[-1] and tok[0] in "\"'":
        tok = tok[1:-1]
    return tok.replace('\\"', '"')


RE_MSVC = re.compile(r'(?:^|[\\/\s"])(?:clang-)?cl(?:\.exe)?"?(?:\s|$)', re.I)


def _msvc_style(head: str) -> bool:
    """True when the compiler (or the program after a launcher such as ccache) is cl or
    clang-cl: /D and /U flags, Windows command-line quoting."""
    return bool(RE_MSVC.search(head))


def command_defines(entry: dict) -> dict[str, str | None]:
    """The -D and -U (and, for cl and clang-cl, /D and /U) definitions of one
    compile_commands.json entry."""
    if "arguments" in entry:
        tokens = list(entry["arguments"])
    else:
        command = entry.get("command", "")
        msvc = _msvc_style(command[:400])
        try:
            tokens = [_unquote(t) if msvc else t for t in shlex.split(command, posix=not msvc)]
        except ValueError:
            tokens = command.split()
    flags = (("-D", False), ("-U", True))
    if _msvc_style(" ".join(tokens[:2])):
        flags += (("/D", False), ("/U", True))
    defines: dict[str, str | None] = {}
    i = 0
    while i < len(tokens):
        t = tokens[i]
        for flag, undef in flags:
            if t.startswith(flag):
                body = t[2:]
                if not body and i + 1 < len(tokens):
                    i += 1
                    body = _unquote(tokens[i])
                name, eq, value = body.partition("=")
                if not re.fullmatch(r"[A-Za-z_]\w*", name):
                    break
                if undef:
                    defines[name] = None
                else:
                    defines[name] = value if eq else "1"
                break
        i += 1
    return defines


def compiled_sources(build: str, det_src: str) -> dict[str, list[dict[str, str | None]]]:
    """The tests/determinism sources in compile_commands.json: path -> defines per entry."""
    cc_path = os.path.join(build, "compile_commands.json")
    if not os.path.isfile(cc_path):
        raise InputError(f"{build}: no compile_commands.json (CMAKE_EXPORT_COMPILE_COMMANDS; the "
                         "Ninja and Makefile generators write it)")
    with open(cc_path, "r", encoding="utf-8") as f:
        entries = json.load(f)
    base = _norm(det_src)
    out: dict[str, list[dict[str, str | None]]] = {}
    for e in entries:
        path = e["file"]
        if not os.path.isabs(path):
            path = os.path.join(e.get("directory", ""), path)
        p = _norm(path)
        if p.startswith(base + os.sep):
            out.setdefault(p, []).append(command_defines(e))
    return out


# --------------------------------------------------------------------------------------------
# CTestTestfile.cmake

RE_CMAKE_ARG = re.compile(r'\[(=*)\[(.*?)\]\1\]|"((?:\\.|[^"\\])*)"|([^\s()"]+)', re.S)
RE_CMAKE_CMD = re.compile(r"^\s*(add_test|set_tests_properties)\s*\((.*)\)\s*$")


def _cmake_args(text: str) -> list[str]:
    args = []
    for m in RE_CMAKE_ARG.finditer(text):
        if m.group(2) is not None:
            args.append(m.group(2))
        elif m.group(3) is not None:
            args.append(re.sub(r"\\([^;])", r"\1", m.group(3)))
        else:
            args.append(m.group(4))
    return args


class Entry:
    def __init__(self, name: str, group: str, where: str):
        self.name, self.group, self.where = name, group, where
        self.filters: set[str] = set()


def ctest_entries(det_bin: str) -> list[Entry]:
    """The add_test entries of every CTestTestfile.cmake under det_bin, grouped by the first
    directory below it ("" for det_bin itself)."""
    if not os.path.isfile(os.path.join(det_bin, "CTestTestfile.cmake")):
        raise InputError(f"{det_bin}: no CTestTestfile.cmake (configure with DE_BUILD_TESTS=ON)")
    entries: dict[tuple[str, str], Entry] = {}
    for dirpath, dirnames, filenames in os.walk(det_bin):
        dirnames.sort()
        if "CTestTestfile.cmake" not in filenames:
            continue
        rel = os.path.relpath(dirpath, det_bin)
        group = "" if rel == "." else rel.replace(os.sep, "/").split("/")[0]
        path = os.path.join(dirpath, "CTestTestfile.cmake")
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                m = RE_CMAKE_CMD.match(line)
                if not m or m.group(1) != "add_test":
                    continue
                args = _cmake_args(m.group(2))
                if not args:
                    continue
                key = (group, args[0])
                e = entries.setdefault(key, Entry(args[0], group, path))
                for a in args[1:]:
                    if a.startswith("--filter="):
                        e.filters.add(a[len("--filter="):])
    return sorted(entries.values(), key=lambda e: (e.group, e.name))


def _glob_re(glob: str) -> re.Pattern:
    return re.compile("".join(".*" if c == "*" else "." if c == "?" else re.escape(c)
                              for c in glob) + r"\Z", re.S)


def filter_match(flt: str, name: str) -> bool:
    """de_test's --filter=POS[-NEG] (tests/unit/de_test.c filter_match)."""
    pos, dash, neg = flt.partition("-")
    if pos and not any(_glob_re(g).match(name) for g in pos.split(":") if g):
        return False
    return not (dash and any(_glob_re(g).match(name) for g in neg.split(":") if g))


# --------------------------------------------------------------------------------------------
# The check


def _rel(root: str, path: str) -> str:
    try:
        return os.path.relpath(path, root).replace(os.sep, "/")
    except ValueError:
        return path


def run(root: str, det_src: str, build: str, det_bin: str) -> int:
    try:
        tus = compiled_sources(build, det_src)
        entries = ctest_entries(det_bin)
        all_files: list[tuple[str, str]] = []
        texts: dict[str, str] = {}
        for dirpath, dirnames, filenames in os.walk(det_src):
            dirnames.sort()
            for fn in sorted(filenames):
                if fn.endswith(SOURCE_EXTS):
                    p = os.path.join(dirpath, fn)
                    with open(p, "r", encoding="utf-8", errors="replace") as f:
                        texts[_norm(p)] = strip_comments(f.read())
                    all_files.append((_rel(root, p), texts[_norm(p)]))
        macros = collect_macros(all_files)
        base = _norm(det_src)
        # group -> case -> (where, required)
        cases: dict[str, dict[str, tuple[str, bool]]] = {}
        for p in sorted(tus):
            if p not in texts:
                continue
            rel_det = os.path.relpath(p, base).replace(os.sep, "/")
            group = rel_det.split("/")[0] if "/" in rel_det else ""
            rel = _rel(root, p)
            for defines in tus[p]:
                for case, no, act in scan_file(rel, texts[p], defines, macros):
                    have = cases.setdefault(group, {}).get(case)
                    required = act is True or (have is not None and have[1])
                    where = have[0] if have else f"{rel}:{no}"
                    cases[group][case] = (where, required)
    except InputError as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    errors: list[str] = []
    notes: list[str] = []
    by_group: dict[str, list[Entry]] = {}
    for e in entries:
        by_group.setdefault(e.group, []).append(e)
    n_cases = n_entries = 0
    groups = sorted(set(cases) | {g for g, es in by_group.items() if any(e.filters for e in es)})
    for group in groups:
        mine = cases.get(group, {})
        ents = [e for e in by_group.get(group, []) if e.filters]
        n_cases += len(mine)
        n_entries += len(ents)
        where_dir = f"tests/determinism/{group}" if group else "tests/determinism"
        for case, (where, required) in sorted(mine.items()):
            hit = any(filter_match(f, case) for e in ents for f in e.filters)
            if hit:
                continue
            if required:
                errors.append(f"{where}: error: unregistered: de_test case {case} has no CTest "
                              f"entry: register it in {where_dir}/CMakeLists.txt (de_det_add_tests "
                              f"<ctest-name>={case}, or the directory's own helper such as "
                              "de_phys_det_add_scenarios)")
            else:
                notes.append(f"{where}: {case} is under a condition this check cannot evaluate "
                             "and has no CTest entry (optional)")
        for e in ents:
            for f in sorted(e.filters):
                if not any(filter_match(f, c) for c in mine):
                    errors.append(f"{_rel(root, e.where)}: error: stale: CTest entry {e.name} "
                                  f"(--filter={f}) selects no de_test case of {where_dir}")
    for n in notes:
        print(f"note: {n}")
    for e in errors:
        print(e)
    summary = (f"check_det_registered: {len(groups)} directories, "
               f"{n_cases} cases, {n_entries} CTest entries")
    if errors:
        print(f"{summary}: {len(errors)} violation(s)")
        return 1
    print(f"{summary}: OK")
    return 0


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--root", default=DEFAULT_ROOT, help="repository root")
    ap.add_argument("--build-dir", help="a configured build directory (compile_commands.json)")
    ap.add_argument("--det-source-dir", default=None,
                    help="default: <root>/tests/determinism")
    ap.add_argument("--det-build-dir", default=None,
                    help="default: <build-dir>/tests/determinism")
    ap.add_argument("--self-test", action="store_true", help="run the self test and exit")
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.build_dir:
        ap.error("--build-dir is required")
    det_src = a.det_source_dir or os.path.join(a.root, "tests", "determinism")
    det_bin = a.det_build_dir or os.path.join(a.build_dir, "tests", "determinism")
    return run(a.root, det_src, a.build_dir, det_bin)


# --------------------------------------------------------------------------------------------
# Self test


def _write(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


SELF_HARNESS_H = """/* reg.h: a registration macro, like DE_PHYS_DET_REGISTER. TEST(not, a_case) */
#ifndef REG_H
#define REG_H
#define REG(name)              \\
    TEST(name, workers_0) {}   \\
    TEST(name, workers_4) {}
#define REG2(a, b) REG(a##_##b)
#endif
"""

SELF_A_C = """#include "reg.h"
#if defined(_WIN32) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define LOCAL_ON 1
// TEST(comment, ignored)
static const char *s = "TEST(string, ignored)";
TEST(alpha, one) {}
TEST_F(alpha, two, fx) {}
DE_TEST_CASE(c, alpha, not_registered) {}
REG(scen)
REG2(big, world)
#if HAS_EXTRA
TEST(alpha, extra) {}
#else
TEST(alpha, no_extra) {}
#endif
#undef OFF_FLAG
#if LOCAL_ON && !defined(OFF_FLAG)
TEST(alpha, local) {}
#endif
#ifdef FROM_A_HEADER
TEST(alpha, maybe) {}
#endif
"""

SELF_B_C = """TEST(beta, one) {}
TEST(beta, two) {}
"""

SELF_UNBUILT_C = """TEST(gamma, never_built) {}
"""


def _ctest_file(path: str, entries: list[tuple[str, list[str]]], multi: bool = False) -> None:
    lines = ["# CMake generated Testfile"]
    for name, args in entries:
        quoted = " ".join(f'"{a}"' for a in args)
        if multi:
            lines.append('if(CTEST_CONFIGURATION_TYPE MATCHES "^([Dd][Ee][Bb][Uu][Gg])$")')
            lines.append(f"  add_test([=[{name}]=] {quoted})")
            lines.append('elseif(CTEST_CONFIGURATION_TYPE MATCHES "^([Rr][Ee][Ll])$")')
            lines.append(f"  add_test([=[{name}]=] {quoted})")
            lines.append("else()")
            lines.append(f"  add_test([=[{name}]=] NOT_AVAILABLE)")
            lines.append("endif()")
        else:
            lines.append(f"add_test([=[{name}]=] {quoted})")
        lines.append(f'set_tests_properties([=[{name}]=] PROPERTIES  LABELS "determinism" '
                     'TIMEOUT "300")')
    _write(path, "\n".join(lines) + "\n")


SELF_A_ENTRIES = [
    ("det.alpha.one", ["/b/a/test_a", "--filter=alpha.one"]),
    ("det.alpha.two", ["/b/a/test_a", "--filter=alpha.two"]),
    ("det.scen", ["/b/a/test_a", "--filter=scen.*"]),
    ("det.big_world.w0", ["/b/a/test_a", "--filter=big_world.workers_0"]),
    ("det.big_world.w4", ["/b/a/test_a", "--filter=big_world.workers_4"]),
    ("det.alpha.extra", ["/b/a/test_a", "--filter=alpha.extra"]),
    ("det.alpha.local", ["/b/a/test_a", "--filter=alpha.local"]),
    ("det.a.registered", ["/usr/bin/cmake", "-DDE_EXE=/b/a/test_a", "-P", "x.cmake"]),
]
SELF_B_ENTRIES = [("det.beta", ["/b/b/test_b", "--filter=beta.*-beta.two"]),
                  ("det.beta.two", ["/b/b/test_b", "--filter=beta.two"])]


def _run_checker(args: list[str]) -> tuple[int, str]:
    p = subprocess.run([sys.executable, os.path.abspath(__file__)] + args, capture_output=True,
                       text=True)
    return p.returncode, p.stdout + p.stderr


def self_test() -> int:
    failures: list[str] = []

    def expect(cond: bool, what: str, out: str = "") -> None:
        if not cond:
            failures.append(what + (("\n--- output ---\n" + out) if out else ""))

    tmp = tempfile.mkdtemp(prefix="check_det_reg_")
    try:
        root = os.path.join(tmp, "repo")
        det = os.path.join(root, "tests", "determinism")
        build = os.path.join(tmp, "build")
        det_bin = os.path.join(build, "tests", "determinism")
        _write(os.path.join(det, "a", "reg.h"), SELF_HARNESS_H)
        _write(os.path.join(det, "a", "a.c"), SELF_A_C)
        _write(os.path.join(det, "b", "b.c"), SELF_B_C)
        _write(os.path.join(det, "c", "unbuilt.c"), SELF_UNBUILT_C)
        a_cmd = '/usr/bin/cc -DHAS_EXTRA=1 "-DOUT=\\"/x y\\"" -I/x -o a.o -c a.c'

        def fake_build(a_command: str = a_cmd, a_entries=None, b_entries=None,
                       multi: bool = False) -> None:
            cc = [{"directory": build, "file": os.path.join(det, "a", "a.c"),
                   "command": a_command},
                  {"directory": build, "file": os.path.join(det, "b", "b.c"),
                   "arguments": ["cc", "-c", "b.c"]}]
            _write(os.path.join(build, "compile_commands.json"), json.dumps(cc))
            _ctest_file(os.path.join(det_bin, "CTestTestfile.cmake"), [])
            _ctest_file(os.path.join(det_bin, "a", "CTestTestfile.cmake"),
                        SELF_A_ENTRIES if a_entries is None else a_entries, multi)
            _ctest_file(os.path.join(det_bin, "b", "CTestTestfile.cmake"),
                        SELF_B_ENTRIES if b_entries is None else b_entries)

        args = ["--root", root, "--build-dir", build]

        # 1. Every case registered: TEST, TEST_F, macro calls (nested, ## pasting), #if on a
        # command-line define and on a define of the file, globs and a NEG filter. Comments,
        # strings, DE_TEST_CASE, the #define bodies and unbuilt files are not cases; a case under
        # a header macro is optional.
        fake_build()
        rc, out = _run_checker(args)
        expect(rc == 0 and "2 directories, 11 cases, 9 CTest entries: OK" in out,
               "clean tree must pass", out)
        expect("alpha.maybe is under a condition" in out, "header-macro case is a note", out)
        for bad in ("comment.ignored", "string.ignored", "not_registered", "never_built",
                    "not.a_case", "no_extra"):
            expect(bad not in out, f"{bad} must not be a case", out)

        # 2. The same with multi-config CTestTestfile.cmake blocks.
        fake_build(multi=True)
        rc, out = _run_checker(args)
        expect(rc == 0, "multi-config CTestTestfile.cmake must pass", out)

        # 3. A case left out of the CTest list is reported with its file and line.
        for drop, want in (("det.alpha.two", "a/a.c:9: error: unregistered: de_test case "
                            "alpha.two"),
                           ("det.scen", "a/a.c:11: error: unregistered: de_test case "
                            "scen.workers_0"),
                           ("det.big_world.w4", "a/a.c:12: error: unregistered: de_test case "
                            "big_world.workers_4"),
                           ("det.alpha.extra", "a/a.c:14: error: unregistered: de_test case "
                            "alpha.extra")):
            fake_build(a_entries=[e for e in SELF_A_ENTRIES if e[0] != drop])
            rc, out = _run_checker(args)
            expect(rc == 1 and want in out, f"dropping {drop}: expected '{want}'", out)
        fake_build(b_entries=SELF_B_ENTRIES[:1])
        rc, out = _run_checker(args)
        expect(rc == 1 and "b/b.c:2: error: unregistered: de_test case beta.two" in out,
               "the NEG part of a filter excludes", out)

        # 4. Without -DHAS_EXTRA the #else case is required and the entry for alpha.extra is
        # stale; -UHAS_EXTRA likewise.
        for cmd in ("/usr/bin/cc -c a.c", "/usr/bin/cc -DHAS_EXTRA=0 -c a.c",
                    "cl.exe /nologo /DHAS_EXTRA=0 /c a.c"):
            fake_build(a_command=cmd)
            rc, out = _run_checker(args)
            if "HAS_EXTRA" in cmd:
                expect(rc == 1 and "error: unregistered: de_test case alpha.no_extra" in out
                       and "error: stale: CTest entry det.alpha.extra (--filter=alpha.extra)"
                       in out, f"'{cmd}': #else branch", out)
            else:
                expect(rc == 0 and "alpha.no_extra is under a condition" in out,
                       f"'{cmd}': unknown HAS_EXTRA makes both branches optional", out)

        # 5. An entry for a case in another directory is stale there and leaves the case
        # unregistered in its own.
        fake_build(b_entries=SELF_B_ENTRIES + [("det.misplaced",
                                                ["/b/b/test_b", "--filter=alpha.one"])],
                   a_entries=[e for e in SELF_A_ENTRIES if e[0] != "det.alpha.one"])
        rc, out = _run_checker(args)
        expect(rc == 1 and "stale: CTest entry det.misplaced" in out
               and "unregistered: de_test case alpha.one" in out, "entries are per directory",
               out)

        # 6. Missing inputs are exit 2.
        os.remove(os.path.join(build, "compile_commands.json"))
        rc, out = _run_checker(args)
        expect(rc == 2 and "no compile_commands.json" in out, "no compile_commands.json", out)
        fake_build()
        shutil.rmtree(det_bin)
        rc, out = _run_checker(args)
        expect(rc == 2 and "no CTestTestfile.cmake" in out, "no CTestTestfile.cmake", out)

        # 7. The filter rules match de_test.c.
        for flt, name, want in (("a.*", "a.b", True), ("a.?", "a.bc", False),
                                ("x.y:a.*", "a.b", True), ("-a.b", "a.b", False),
                                ("*-a.b:c.*", "c.d", False), ("*-a.b", "a.c", True),
                                ("a.b", "a.bc", False), ("[a].b", "a.b", False)):
            expect(filter_match(flt, name) == want, f"filter {flt} on {name}: expected {want}")
        # 8. #if expressions.
        d = {"A": "1", "B": "0", "C": "A + 1", "U": None}
        for expr, want in (("A", True), ("B", False), ("C == 2", True), ("!B && A", True),
                           ("defined(A) && !defined U", True), ("U", False),
                           ("A || HEADER", True), ("B && HEADER", False), ("HEADER", None),
                           ("defined(HEADER) || B", None), ("(A ? B : 1)", False),
                           ("__has_include(<x.h>)", None)):
            got = cond3(expr, d)
            expect(got == want, f"#if {expr}: expected {want}, got {got}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    for f in failures:
        print(f"FAIL: {f}")
    print(f"check_det_registered self test: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
