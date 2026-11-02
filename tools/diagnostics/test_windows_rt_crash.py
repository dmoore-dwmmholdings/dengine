"""Portable subprocess/evidence controls, not a claim of native CDB execution."""
import json
from pathlib import Path
import subprocess
import sys
import struct
import tempfile
import unittest
from unittest.mock import patch

from windows_rt_crash import CASES, capture_case, complete_minidump, debugger_commands, main, select_cases

DUMP = struct.pack("<4sIIIIIQ", b"MDMP", 0xa793, 1, 32, 0, 0, 0)
DUMP += struct.pack("<III", 10, 4, 44) + b"test"


class WindowsCrashCaptureTest(unittest.TestCase):
    def test_only_original_failed_registered_cases_are_selected(self):
        catalog = {"tests": [{"name": name, "command": ["engine.exe", "--filter=x"]}
                             for name in CASES]}
        self.assertEqual(select_cases(catalog, "5:unrelated\n"), [])
        self.assertEqual([r["name"] for r in select_cases(catalog, f"7:{CASES[1]}\n")],
                         [CASES[1]])
        with self.assertRaises(ValueError):
            select_cases({"tests": []}, f"7:{CASES[1]}\n")

    def capture(self, root, body, timeout=3):
        fake = root / "fake debugger.py"
        fake.write_text('import os, sys\nfrom pathlib import Path\n'
                        f'expected = Path({str(root)!r}).resolve()\n'
                        'actual = Path.cwd().resolve()\n'
                        'assert actual == expected, (str(actual), str(expected))\n'
                        'assert os.environ["DE_DENG736_CONTROL"] == "unchanged value"\n'
                        'os.chdir(Path(sys.argv[sys.argv.index("-cf") + 1]).parent)\n'
                        + body.replace('b"MDMPtest"', repr(DUMP)))
        target = root / "unchanged executable.bin"
        target.write_bytes(b"original executable")
        test = {"name": CASES[0], "command": [str(target), "--filter=original.case"],
                "properties": [{"name": "TIMEOUT", "value": timeout},
                               {"name": "WORKING_DIRECTORY", "value": str(root)},
                               {"name": "ENVIRONMENT", "value": ["DE_DENG736_CONTROL=unchanged value"]}]}
        result = capture_case([sys.executable, str(fake)], test, root / "capture")
        self.console = (root / "capture/console.log").read_text(errors="replace")
        return result

    def test_debugger_callback_preserves_windows_dump_path(self):
        commands = debugger_commands(r"..\..\..\test-dumps\deng736\case-0\crash.dmp")
        callbacks = commands.splitlines()[:-1]
        self.assertEqual(len(callbacks), 7)
        for callback in callbacks:
            self.assertNotIn("\\", callback)
            self.assertIn(".dump /m /o ../../../test-dumps/deng736/case-0/crash.dmp; q", callback)
            self.assertTrue(callback.startswith('sxd -c2 "'))
        self.assertEqual(commands.splitlines()[-1], "g")

    def test_capture_preserves_command_and_nonzero_debugger_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = self.capture(root, 'from pathlib import Path\n'
                                  'Path("crash.dmp").write_bytes(b"MDMPtest")\n'
                                  'print("DENG736_SECOND_CHANCE\\nExceptionCode: c00000fd")\n'
                                  'raise SystemExit(7)\n')
            self.assertTrue(result["captured"], self.console)
            self.assertTrue(result["diagnosticOnly"])
            self.assertEqual(result["debuggerExit"], 7)
            self.assertEqual(result["exceptionCodes"], ["c00000fd"])
            self.assertEqual(result["debuggerCommand"][-2:], result["originalCommand"])
            saved = json.loads((root / "capture/result.json").read_text())
            self.assertEqual(saved, result)
            with self.assertRaises(FileExistsError):
                capture_case([], {}, root / "capture")

    def test_successful_debugger_exit_is_not_capture_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.capture(Path(tmp), 'print("target completed without a crash")\n')
            self.assertEqual(result["debuggerExit"], 0, self.console)
            self.assertFalse(result["captured"])

    def test_incomplete_dump_directory_or_payload_is_not_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "crash.dmp"
            for content in (b"", b"MDMPtest", DUMP[:32], DUMP[:-1], DUMP):
                path.write_bytes(content)
                self.assertEqual(complete_minidump(path), content == DUMP)

    def test_marker_and_valid_dump_are_both_required(self):
        bodies = ['print("DENG736_SECOND_CHANCE")\n',
                  'from pathlib import Path\nPath("crash.dmp").write_bytes(b"MDMPtest")\n',
                  'from pathlib import Path\nPath("crash.dmp").write_bytes(b"bad")\n'
                  'print("DENG736_SECOND_CHANCE")\n',
                  'from pathlib import Path\nPath("crash.dmp").write_bytes(b"MDMPtest")\n'
                  'print("DENG736_SECOND_CHANCE")\n',
                  'from pathlib import Path\nPath("crash.dmp").write_bytes(b"MDMPtest")\n'
                  'print(\'0:000> sxd -c2 ".echo DENG736_SECOND_CHANCE; .exr -1" av\')\n'
                  'print("ExceptionCode: c00000fd")\n']
        for body in bodies:
            with self.subTest(body=body), tempfile.TemporaryDirectory() as tmp:
                self.assertFalse(self.capture(Path(tmp), body)["captured"])

    def test_timeout_is_recorded_and_owned_process_is_joined(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.capture(Path(tmp), 'import time\ntime.sleep(30)\n', timeout=.1)
            self.assertTrue(result["timedOut"], self.console)
            self.assertFalse(result["captured"])
            self.assertIsInstance(result["debuggerExit"], int)

    def test_executable_mutation_invalidates_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.capture(Path(tmp), 'import sys\nfrom pathlib import Path\n'
                                  'Path(sys.argv[-2]).write_bytes(b"changed")\n'
                                  'Path("crash.dmp").write_bytes(b"MDMPtest")\n'
                                  'print("DENG736_SECOND_CHANCE")\n')
            self.assertFalse(result["executableUnchanged"], self.console)
            self.assertFalse(result["captured"])

    def test_original_log_mutation_is_a_hard_error(self):
        for mutate in (None, "log", "catalog"):
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                logs = root / "Testing/Temporary"
                logs.mkdir(parents=True)
                original = logs / "LastTest.log"
                original.write_text("original failed CTest evidence\n")
                (logs / "LastTestsFailed.log").write_text(f"5:{CASES[0]}\n")
                catalog = {"tests": [{"name": CASES[0], "command": [sys.executable]}]}
                snapshot = root / "catalog.json"
                snapshot.write_text(json.dumps(catalog))

                def capture(*unused):
                    if mutate == "log":
                        original.write_text("overwritten\n")
                    if mutate == "catalog":
                        snapshot.write_text("overwritten\n")
                    return {"captured": True}

                with patch("windows_rt_crash.sys.platform", "win32"), \
                     patch("sys.argv", ["capture", "--test-dir", str(root), "--catalog", str(snapshot)]), \
                     patch("windows_rt_crash.subprocess.check_output",
                           side_effect=AssertionError("post-failure discovery is forbidden")), \
                     patch("windows_rt_crash.find_debugger", return_value=Path(sys.executable)), \
                     patch("windows_rt_crash.capture_case", side_effect=capture), \
                     patch("builtins.print"):
                    if mutate:
                        kind = "evidence" if mutate == "log" else "catalog"
                        with self.assertRaisesRegex(RuntimeError, f"original CTest {kind} changed"):
                            main()
                    else:
                        self.assertEqual(main(), 0)
                report = json.loads((root / "test-dumps/deng736/manifest.json").read_text())
                self.assertFalse(report["originalCTestResultReplaced"])
                self.assertEqual(report["originalLogsUnchanged"], mutate != "log")
                self.assertEqual(report["catalogUnchanged"], mutate != "catalog")

    def test_missing_catalog_preserves_logs_and_cannot_launch_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            logs = root / "Testing/Temporary"
            logs.mkdir(parents=True)
            original = logs / "LastTest.log"
            original.write_bytes(b"original failed CTest evidence\r\n")
            failures = logs / "LastTestsFailed.log"
            failures.write_text(f"5:{CASES[0]}\n")
            before = [path.read_bytes() for path in (original, failures)]
            with patch("windows_rt_crash.sys.platform", "win32"), \
                 patch("sys.argv", ["capture", "--test-dir", str(root),
                                    "--catalog", str(root / "missing-catalog.json")]), \
                 patch("windows_rt_crash.subprocess.check_output",
                       side_effect=AssertionError("post-failure discovery is forbidden")), \
                 patch("windows_rt_crash.find_debugger",
                       side_effect=AssertionError("missing catalog cannot launch capture")), \
                 patch("builtins.print"):
                self.assertEqual(main(), 2)
            self.assertEqual([path.read_bytes() for path in (original, failures)], before)
            report = json.loads((root / "test-dumps/deng736/manifest.json").read_text())
            self.assertEqual(report["status"], "capture error")
            self.assertTrue(report["originalLogsUnchanged"])
            self.assertIsNone(report["catalogUnchanged"])
            self.assertFalse(report["originalCTestResultReplaced"])

    def test_real_failed_ctest_logs_survive_catalog_consumption(self):
        with tempfile.TemporaryDirectory(prefix="deng736 catalog ") as tmp:
            root = Path(tmp)
            build = root / "build"
            (root / "CMakeLists.txt").write_text(
                'cmake_minimum_required(VERSION 3.28)\nproject(CaptureControl NONE)\n'
                'enable_testing()\n'
                f'add_test(NAME {CASES[0]} COMMAND [==[{sys.executable}]==] '
                '-c [==[raise SystemExit(1)]==])\n'
                f'set_tests_properties({CASES[0]} PROPERTIES TIMEOUT 300)\n')
            subprocess.run(["cmake", "-G", "Ninja", "-S", str(root), "-B", str(build)],
                           check=True, capture_output=True)
            snapshot = root / "original-catalog.json"
            catalog = subprocess.check_output(["ctest", "--test-dir", str(build),
                                               "--show-only=json-v1"])
            snapshot.write_bytes(catalog)
            original = subprocess.run(["ctest", "--test-dir", str(build), "--output-on-failure"],
                                      capture_output=True)
            self.assertEqual(original.returncode, 8, original.stdout.decode(errors="replace"))
            paths = [build / "Testing/Temporary" / name
                     for name in ("LastTest.log", "LastTestsFailed.log")]
            before = [path.read_bytes() for path in paths]
            self.assertIn(b"Failed", before[0])
            self.assertIn(CASES[0].encode(), before[1])
            observed = []

            def capture(debugger, test, output):
                observed.append(test)
                return {"captured": True}

            with patch("windows_rt_crash.sys.platform", "win32"), \
                 patch("sys.argv", ["capture", "--test-dir", str(build),
                                    "--catalog", str(snapshot)]), \
                 patch("windows_rt_crash.subprocess.check_output",
                       side_effect=AssertionError("post-failure discovery is forbidden")), \
                 patch("windows_rt_crash.find_debugger", return_value=Path(sys.executable)), \
                 patch("windows_rt_crash.capture_case", side_effect=capture), \
                 patch("builtins.print"):
                self.assertEqual(main(), 0)
            self.assertEqual(observed, json.loads(catalog)["tests"])
            self.assertEqual([path.read_bytes() for path in paths], before)
            self.assertEqual(snapshot.read_bytes(), catalog)
            report = json.loads((build / "test-dumps/deng736/manifest.json").read_text())
            self.assertTrue(report["originalLogsUnchanged"])
            self.assertTrue(report["catalogUnchanged"])
            self.assertFalse(report["originalCTestResultReplaced"])


if __name__ == "__main__":
    unittest.main()
