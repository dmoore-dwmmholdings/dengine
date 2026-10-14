#!/usr/bin/env python3
"""Self-test for tools/check_layers/check_layers.py (ctest `layering_selftest`).

Runs the checker on the fixture trees in fixtures/ and checks the exit status and the output.
A fixture with its own layers.toml uses it; the others use the real table, so the fixtures also
pin down the rules that layers.toml encodes. Also checks that the minimal TOML reader (used on
Python 3.10) reads the tables exactly like tomllib, and that the full tree is checked in < 2 s.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL_DIR = os.path.dirname(HERE)
CHECKER = os.path.join(TOOL_DIR, "check_layers.py")
REAL_TABLE = os.path.join(TOOL_DIR, "layers.toml")
FIXTURES = os.path.join(HERE, "fixtures")

# name -> (expected exit status, substrings that must appear, substrings that must not appear)
CASES = {
    "ok": (0, ["21 module edges", ": OK"], ["error"]),
    "upward": (1, [
        "src/core/x.c:3: error: upward include: core (foundation) includes ecs (runtime) "
        "via <dengine/de_ecs.h>",
        "include/dengine/de_base.h:2: error: upward include: core (foundation) includes "
        "render (content)",
    ], []),
    "cycle": (1, [
        "error: cycle between alpha, beta: alpha -> beta -> alpha",
        "alpha -> beta: src/alpha/alpha.c:2",
        "beta -> alpha: src/beta/beta.h:2",
        "0 violation(s), 1 cycle(s)",
    ], ["gamma"]),
    "bridge": (0, [": OK"], ["error"]),
    "sideways": (1, [
        "src/anim/x.c:2: error: forbidden include: anim (gameplay) includes physics (gameplay)",
        "src/physics/y.c:1: error: forbidden include: physics (gameplay) includes audio "
        "(gameplay)",
        "src/input/z.c:2: error: same-band include not allowed: input (gameplay) includes net "
        "(gameplay)",
    ], []),
    "tools": (1, [
        "src/render/r.c:1: error: engine module render includes "
        "\"../../tools/pack/lib/pack_writer.h\" under tools/",
        "src/render/r.c:2: error: restricted include: render (content) includes cook (content)",
        "2 violation(s)",
    ], ["src/asset/a.c"]),
    "umbrella": (1, ["src/host/h.c:1: error: host includes the umbrella header "
                     "<dengine/dengine.h>"], []),
    "unmapped": (1, [
        "src/newmod/n.c: error: file is not in any module",
        "src/world/loose.c: error: file is not in any module",
        "src/core/x.c:1: error: <dengine/de_bogus.h> is not owned by any module",
        "src/core/x.c:2: error: \"newmod/n.h\" resolves to src/newmod/n.h, which is not in any "
        "module",
    ], []),
    "examples": (1, [
        "examples/demo/main.c:7: error: <dengine/de_bogus.h> is not a public header",
        "examples/demo/main.c:8: error: \"gpu/gpu_internal.h\" is engine-internal "
        "(src/gpu/gpu_internal.h)",
        "examples/demo/main.c:9: error: \"../../src/gpu/gpu_internal.h\" resolves to "
        "src/gpu/gpu_internal.h",
        "examples/demo/gone.c: error: [[example_internal]] names a file that does not exist",
        "4 violation(s)",
    ], ["glue.c", "local.h", "main.c:1:", "main.c:3:", "main.c:5:"]),
    "bad_table": (2, ["[[allow]] a -> b: `why` (the reason) is required"], []),
    "bad_forbid": (2, ["[[allow]] anim -> physics contradicts [[forbid]] anim -> physics"], []),
}


def run(args: list[str]) -> tuple[int, str]:
    p = subprocess.run([sys.executable, CHECKER] + args, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True)
    return p.returncode, p.stdout


def table_for(fixture_dir: str) -> str:
    own = os.path.join(fixture_dir, "layers.toml")
    return own if os.path.isfile(own) else REAL_TABLE


def main() -> int:
    failures = []

    def check(name: str, ok: bool, detail: str = ""):
        print(f"{'PASS' if ok else 'FAIL'}  {name}")
        if not ok:
            failures.append(name)
            if detail:
                print("      " + detail.replace("\n", "\n      "))

    present = sorted(d for d in os.listdir(FIXTURES) if os.path.isdir(os.path.join(FIXTURES, d)))
    check("every fixture has a case", present == sorted(CASES),
          f"fixtures {present} vs cases {sorted(CASES)}")

    for name, (want_rc, must, must_not) in sorted(CASES.items()):
        fdir = os.path.join(FIXTURES, name)
        for fallback in (False, True):
            args = ["--root", fdir, "--table", table_for(fdir)]
            if fallback:
                args.append("--toml-fallback")
            rc, out = run(args)
            problems = []
            if rc != want_rc:
                problems.append(f"exit {rc}, expected {want_rc}")
            problems += [f"missing: {s}" for s in must if s not in out]
            problems += [f"unexpected: {s}" for s in must_not if s in out]
            label = f"fixture {name}" + (" (fallback TOML reader)" if fallback else "")
            check(label, not problems, "\n".join(problems) + "\n--- output ---\n" + out)

    # The minimal TOML reader must agree with tomllib on every table we ship or test with.
    try:
        import tomllib  # noqa: F401
        have_tomllib = True
    except ImportError:
        have_tomllib = False
    tables = [REAL_TABLE] + sorted(
        os.path.join(FIXTURES, d, "layers.toml") for d in present
        if os.path.isfile(os.path.join(FIXTURES, d, "layers.toml")))
    if have_tomllib:
        for t in tables:
            _, a = run(["--table", t, "--dump-table"])
            _, b = run(["--table", t, "--dump-table", "--toml-fallback"])
            check(f"fallback TOML reader matches tomllib: {os.path.relpath(t, TOOL_DIR)}",
                  a == b and a.startswith("{"), f"tomllib:\n{a}\nfallback:\n{b}")
    else:
        print("SKIP  fallback-vs-tomllib comparison (no tomllib on this Python)")

    # Extra fallback-reader syntax that layers.toml may grow into.
    with tempfile.NamedTemporaryFile("w", suffix=".toml", delete=False) as f:
        f.write('# c\nkey = "a # not a comment" # comment\nlit = \'x\\y\'\nn = 1_000\n'
                'b = false\narr = [\n  "one", # c\n  "two",\n]\nesc = "q\\"\\u00e9"\n'
                '[t]\nx = []\n[[a.b]]\ny = true\n')
        tmp = f.name
    try:
        _, out = run(["--table", tmp, "--dump-table", "--toml-fallback"])
        import json
        try:
            got = json.loads(out)
        except ValueError:
            got = None
        want = {"key": "a # not a comment", "lit": "x\\y", "n": 1000, "b": False,
                "arr": ["one", "two"], "esc": 'q"\u00e9', "t": {"x": []},
                "a": {"b": [{"y": True}]}}
        check("fallback TOML reader syntax subset", got == want, f"got {got!r}")
    finally:
        os.unlink(tmp)

    # The full repository is checked in under 2 s, interpreter start included (acceptance
    # criterion). Whether it is clean is layering_check's job, so exit 1 is accepted here.
    t0 = time.perf_counter()
    rc, out = run([])
    elapsed = time.perf_counter() - t0
    check(f"full tree checked in < 2 s ({elapsed:.2f} s)", rc in (0, 1) and elapsed < 2.0, out)

    # --graph writes a dot file that names the bands.
    with tempfile.TemporaryDirectory() as d:
        dot = os.path.join(d, "layers.dot")
        rc, out = run(["--graph", dot])
        text = open(dot, encoding="utf-8").read() if os.path.isfile(dot) else ""
        check("--graph writes the module graph",
              rc in (0, 1) and text.startswith("digraph") and "cluster_foundation" in text
              and '"ecs" -> "task"' in text, out)

    print(f"\nlayering_selftest: {len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
