"""Portable evidence-integrity controls; no claim to emulate Windows DLL loading."""
import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest import mock

import windows_lldb_loader as loader


class LoaderEvidence(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="deng723 café space ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.build = self.root / "build"
        self.logs = self.build / "Testing/Temporary"
        self.logs.mkdir(parents=True)
        self.tool = self.root / "tool/lldb.exe"
        self.tool.parent.mkdir()
        self.tool.write_bytes(b"selected debugger")
        self.system = self.root / "Windows"
        (self.system / "System32").mkdir(parents=True)
        self.working = self.build / "tests/unit/reflect"
        self.working.mkdir(parents=True)
        # CMake/CTest evidence is UTF-8, including the intentional Unicode path.
        (self.build / "CMakeCache.txt").write_text(
            f"DE_LLDB_EXECUTABLE:FILEPATH={self.tool}\n", encoding="utf-8")
        (self.logs / "LastTestsFailed.log").write_text("1986:debugger.native_lldb\n", encoding="utf-8")
        (self.logs / "LastTest.log").write_text(
            f"1986/2158 Testing: debugger.native_lldb\n1986/2158 Test: debugger.native_lldb\n"
            f"Command: unchanged command\nDirectory: {self.working}\nOutput:\nfailed\n", encoding="utf-8")
        self.output = self.build / "test-dumps/deng723-loader"

    def collect(self, imports, mutate=None):
        def invoke(command, log, deadline, working=None):
            if command[1] == "--version":
                self.assertEqual(command[0], self.tool)
                self.assertEqual(working, self.working)
                log.write_bytes(b"")
                return {"exit": 3221225781, "exitHex": "0xc0000135"}
            name = Path(command[-1]).name
            log.write_text("File Type: DLL\n" + "".join(f"    {n}\n" for n in imports[name]),
                           encoding="utf-8")
            if mutate:
                mutate()
            return {"exit": 0}
        environment = {"SystemRoot": str(self.system), "PATH": str(self.tool.parent)}
        with mock.patch.dict(os.environ, environment), mock.patch.object(loader, "invoke", invoke), \
                mock.patch.object(loader.shutil, "which", return_value="dumpbin"), \
                contextlib.redirect_stdout(io.StringIO()):
            result = loader.collect(self.build, self.output)
        return result, json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))

    def test_transitive_missing_import_keeps_original_failure_and_real_status(self):
        (self.tool.parent / "liblldb.dll").write_bytes(b"native library")
        (self.system / "System32/KERNEL32.dll").write_bytes(b"OS")
        before = {p: p.read_bytes() for p in self.logs.iterdir()}
        code, report = self.collect({"lldb.exe": ["liblldb.dll", "KERNEL32.dll"],
                                     "liblldb.dll": ["missing_runtime.dll", "api-ms-win-core-x.dll",
                                                     "liblldb.dll"]})
        self.assertEqual(code, 0, report)
        self.assertEqual(report["versionProbe"]["exit"], 3221225781)
        self.assertEqual(len(report["modules"]), 2)  # Cycle bounded; OS not traversed.
        self.assertEqual(report["unlocatedImports"],
                         [{"importer": str(self.tool.parent / "liblldb.dll"),
                           "name": "missing_runtime.dll"}])
        self.assertFalse(report["originalCTestResultReplaced"])
        self.assertFalse(report["dependencyResolutionProven"])
        self.assertTrue(report["originalEvidenceUnchanged"])
        self.assertTrue(report["inspectedModulesUnchanged"])
        self.assertEqual(before, {p: p.read_bytes() for p in self.logs.iterdir()})

    def test_no_matching_failure_refuses_diagnostic_without_touching_logs(self):
        (self.logs / "LastTestsFailed.log").write_text("123:some.other.test\n", encoding="utf-8")
        code, report = self.collect({})
        self.assertEqual(code, 2)
        self.assertEqual(report["status"], "incomplete")
        self.assertNotIn("versionProbe", report)
        self.assertTrue(report["originalEvidenceUnchanged"])

    def test_inspected_debugger_mutation_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "debugger files changed"):
            self.collect({"lldb.exe": []}, lambda: self.tool.write_bytes(b"changed"))
        report = json.loads((self.output / "manifest.json").read_text(encoding="utf-8"))
        self.assertFalse(report["inspectedModulesUnchanged"])

    def test_original_log_mutation_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "original evidence"):
            self.collect({"lldb.exe": []}, lambda: (self.logs / "LastTest.log").write_text(
                "clobbered", encoding="utf-8"))

    def test_prior_capture_cannot_be_overwritten(self):
        self.output.mkdir(parents=True)
        with self.assertRaises(FileExistsError):
            self.collect({})

    def test_cache_must_pin_one_existing_absolute_executable(self):
        for cache in ["", "DE_LLDB_EXECUTABLE:FILEPATH=lldb.exe\n",
                      "DE_LLDB_EXECUTABLE:FILEPATH=DE_LLDB_EXECUTABLE-NOTFOUND\n",
                      f"DE_LLDB_EXECUTABLE:FILEPATH={self.tool}\n" * 2]:
            with self.subTest(cache=cache), self.assertRaises(ValueError):
                loader.selected_lldb(cache)

    def test_dependency_lines_cannot_escape_search_directories(self):
        text = "File: C:/tool/library.dll\n    good.dll\n    ../escape.dll\n    /switch.dll\n    good.dll\n"
        self.assertEqual(loader.dependent_names(text), ["good.dll"])

    def test_version_probe_preserves_nonzero_real_subprocess_status(self):
        log = self.root / "status.log"
        run = loader.invoke([sys.executable, "-c", "import sys;sys.exit(17)"], log,
                            time.monotonic() + 10, self.working)
        self.assertEqual(run["exit"], 17)
        self.assertEqual(run["exitHex"], "0x00000011")
        self.assertEqual(log.read_bytes(), b"")

    def test_timeout_is_incomplete_and_does_not_change_acceptance_timeout(self):
        with mock.patch.object(loader.subprocess, "run", side_effect=subprocess.TimeoutExpired("probe", 10)):
            with self.assertRaises(subprocess.TimeoutExpired):
                loader.invoke([self.tool, "--version"], self.root / "timeout.log", time.monotonic() + 60)


if __name__ == "__main__":
    unittest.main()
