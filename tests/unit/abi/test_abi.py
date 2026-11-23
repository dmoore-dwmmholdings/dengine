#!/usr/bin/env python3
"""ABI checker tests (DENG-128): tools/abi/abi_dump.py and tools/abi/abi_check.py.

Each fixture directory under fixtures/ holds one fx.h. The base fixture is the baseline, and
every other fixture changes one thing. A test dumps both headers (with the layout probe compiled
and run on this host), runs the checker, and checks the exit code and the classification.

Run one test:  python3 test_abi.py --clang clang TestAbi.test_abi_removed_function
Python 3.10+, stdlib only.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
FIXTURES = HERE / "fixtures"
DUMP = ROOT / "tools" / "abi" / "abi_dump.py"
CHECK = ROOT / "tools" / "abi" / "abi_check.py"

CLANG = "clang"
CC = ""


def run(cmd: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True)


class TestAbi(unittest.TestCase):
    tmp: tempfile.TemporaryDirectory

    @classmethod
    def setUpClass(cls) -> None:
        cls.tmp = tempfile.TemporaryDirectory(prefix="de_abi_test_")

    @classmethod
    def tearDownClass(cls) -> None:
        cls.tmp.cleanup()

    def dump(self, include_dir: Path, header: str, name: str) -> Path:
        out = Path(self.tmp.name) / f"{name}.json"
        cmd = [sys.executable, str(DUMP), "--include-dir", str(include_dir), "--header", header,
               "--clang", CLANG, "--out", str(out), "--work-dir", str(Path(self.tmp.name) / name)]
        if CC:
            cmd += ["--cc", CC]
        r = run(cmd)
        self.assertEqual(r.returncode, 0, f"abi_dump failed:\n{r.stdout}\n{r.stderr}")
        return out

    def fixture(self, name: str) -> Path:
        return self.dump(FIXTURES / name, "fx.h", name)

    def check(self, current: str) -> tuple[int, str, dict]:
        base = self.fixture("base")
        cur = self.fixture(current)
        report = Path(self.tmp.name) / f"{current}.report.json"
        r = run([sys.executable, str(CHECK), str(base), str(cur), "--json", str(report)])
        data = json.loads(report.read_text()) if report.exists() else {}
        return r.returncode, r.stdout + r.stderr, data

    def kinds(self, data: dict, cls: str) -> list[str]:
        return [c["kind"] for c in data.get(cls, [])]

    # --- breaking -----------------------------------------------------------------------------
    def test_abi_removed_function(self) -> None:
        rc, out, data = self.check("removed_function")
        self.assertEqual(rc, 1, out)
        self.assertIn("function_removed", self.kinds(data, "breaking"))
        self.assertIn("fx_destroy", out)

    def test_abi_param_type_change(self) -> None:
        rc, out, data = self.check("param_type_change")
        self.assertEqual(rc, 1, out)
        self.assertIn("function_signature_changed", self.kinds(data, "breaking"))

    def test_abi_struct_reorder(self) -> None:
        rc, out, data = self.check("struct_reorder")
        self.assertEqual(rc, 1, out)
        kinds = self.kinds(data, "breaking")
        self.assertIn("field_offset_changed", kinds)
        self.assertNotIn("record_size_changed", kinds)  # same size: offsets alone must catch it

    def test_abi_enum_value_change(self) -> None:
        rc, out, data = self.check("enum_value_change")
        self.assertEqual(rc, 1, out)
        self.assertIn("enum_value_changed", self.kinds(data, "breaking"))

    def test_abi_handle_change(self) -> None:
        rc, out, data = self.check("handle_change")
        self.assertEqual(rc, 1, out)
        self.assertIn("record_size_changed", self.kinds(data, "breaking"))

    # --- compatible ---------------------------------------------------------------------------
    def test_abi_identical_ok(self) -> None:
        rc, out, data = self.check("base")
        self.assertEqual(rc, 0, out)
        self.assertEqual(data.get("breaking"), [])
        self.assertEqual(data.get("compatible"), [])

    def test_abi_added_function_ok(self) -> None:
        rc, out, data = self.check("added_function")
        self.assertEqual(rc, 0, out)
        self.assertEqual(data.get("breaking"), [], out)
        kinds = self.kinds(data, "compatible")
        self.assertIn("function_added", kinds)
        self.assertIn("record_added", kinds)

    def test_abi_appended_enum_ok(self) -> None:
        rc, out, data = self.check("appended_enum")
        self.assertEqual(rc, 0, out)
        self.assertEqual(data.get("breaking"), [], out)
        self.assertIn("enum_constant_added", self.kinds(data, "compatible"))

    def test_abi_major_bump_allows(self) -> None:
        rc, out, data = self.check("major_bump")
        self.assertEqual(rc, 0, out)
        self.assertIn("field_offset_changed", self.kinds(data, "breaking"))
        self.assertTrue(data.get("allowed_by_major_bump"), out)
        self.assertIn("major version", out.lower())

    # --- dump details -------------------------------------------------------------------------
    def test_abi_dump_fixture_contents(self) -> None:
        d = json.loads(self.fixture("base").read_text())
        self.assertEqual(sorted(d["functions"]), ["fx_create", "fx_destroy"])  # static inline excluded
        self.assertEqual(d["versions"]["DE_VERSION_MAJOR"], 1)
        self.assertEqual(d["versions"]["DE_GAME_API_VERSION"], 1)
        desc = d["records"]["fx_desc"]
        self.assertEqual(desc["size"], 12)
        self.assertEqual([f["name"] for f in desc["fields"]], ["flags", "scale", "mode"])
        self.assertEqual([f["offset"] for f in desc["fields"]], [0, 4, 8])
        anon = d["records"]["fx_anon"]  # anonymous struct named by its typedef
        self.assertEqual([f["name"] for f in anon["fields"]], ["x", "f", "u"])
        self.assertEqual(d["enums"]["fx_mode"]["constants"], {"FX_MODE_A": 0, "FX_MODE_B": 1, "FX_MODE_C": 2})
        self.assertIn("fx_engine", d["typedefs"])

    def test_abi_dump_real_headers(self) -> None:
        out = self.dump(ROOT / "include", "dengine/dengine.h", "real")
        d = json.loads(out.read_text())
        self.assertGreater(len(d["functions"]), 100)
        self.assertGreater(len(d["records"]), 50)
        self.assertIn("de_host_run", d["functions"])
        self.assertIn("DE_GAME_API_VERSION", d["versions"])
        self.assertIn("DE_VERSION_MAJOR", d["versions"])
        for name, rec in d["records"].items():
            self.assertIsNotNone(rec.get("size"), f"{name} has no measured size")
        # The real headers against themselves: no differences.
        r = run([sys.executable, str(CHECK), str(out), str(out)])
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


def main() -> None:
    global CLANG, CC
    ap = argparse.ArgumentParser(add_help=False)
    ap.add_argument("--clang", default="clang")
    ap.add_argument("--cc", default="")
    args, rest = ap.parse_known_args()
    CLANG, CC = args.clang, args.cc
    unittest.main(argv=[sys.argv[0], *rest], verbosity=2)


if __name__ == "__main__":
    main()
