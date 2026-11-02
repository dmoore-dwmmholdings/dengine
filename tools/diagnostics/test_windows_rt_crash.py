"""Portable subprocess/evidence controls, not a claim of native CDB execution."""
import json
from pathlib import Path
import sys
import struct
import tempfile
import unittest
from unittest.mock import patch

from windows_rt_crash import CASES, capture_case, complete_minidump, main, select_cases

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
                        f'assert Path.cwd() == Path({str(root)!r}).resolve()\n'
                        'assert os.environ["DE_DENG736_CONTROL"] == "unchanged value"\n'
                        'os.chdir(Path(sys.argv[sys.argv.index("-cf") + 1]).parent)\n'
                        + body.replace('b"MDMPtest"', repr(DUMP)))
        target = root / "unchanged executable.bin"
        target.write_bytes(b"original executable")
        test = {"name": CASES[0], "command": [str(target), "--filter=original.case"],
                "properties": [{"name": "TIMEOUT", "value": timeout},
                               {"name": "WORKING_DIRECTORY", "value": str(root)},
                               {"name": "ENVIRONMENT", "value": ["DE_DENG736_CONTROL=unchanged value"]}]}
        return capture_case([sys.executable, str(fake)], test, root / "capture")

    def test_capture_preserves_command_and_nonzero_debugger_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = self.capture(root, 'from pathlib import Path\n'
                                  'Path("crash.dmp").write_bytes(b"MDMPtest")\n'
                                  'print("DENG736_SECOND_CHANCE\\nExceptionCode: c00000fd")\n'
                                  'raise SystemExit(7)\n')
            self.assertTrue(result["captured"])
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
            self.assertEqual(result["debuggerExit"], 0)
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
                  'print("DENG736_SECOND_CHANCE")\n']
        for body in bodies:
            with self.subTest(body=body), tempfile.TemporaryDirectory() as tmp:
                self.assertFalse(self.capture(Path(tmp), body)["captured"])

    def test_timeout_is_recorded_and_owned_process_is_joined(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.capture(Path(tmp), 'import time\ntime.sleep(30)\n', timeout=.1)
            self.assertTrue(result["timedOut"])
            self.assertFalse(result["captured"])
            self.assertIsInstance(result["debuggerExit"], int)

    def test_executable_mutation_invalidates_capture(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = self.capture(Path(tmp), 'import sys\nfrom pathlib import Path\n'
                                  'Path(sys.argv[-2]).write_bytes(b"changed")\n'
                                  'Path("crash.dmp").write_bytes(b"MDMPtest")\n'
                                  'print("DENG736_SECOND_CHANCE")\n')
            self.assertFalse(result["executableUnchanged"])
            self.assertFalse(result["captured"])

    def test_original_log_mutation_is_a_hard_error(self):
        for mutate in (False, True):
            with self.subTest(mutate=mutate), tempfile.TemporaryDirectory() as tmp:
                root = Path(tmp)
                logs = root / "Testing/Temporary"
                logs.mkdir(parents=True)
                original = logs / "LastTest.log"
                original.write_text("original failed CTest evidence\n")
                (logs / "LastTestsFailed.log").write_text(f"5:{CASES[0]}\n")
                catalog = {"tests": [{"name": CASES[0], "command": [sys.executable]}]}

                def capture(*unused):
                    if mutate:
                        original.write_text("overwritten\n")
                    return {"captured": True}

                with patch("windows_rt_crash.sys.platform", "win32"), \
                     patch("sys.argv", ["capture", "--test-dir", str(root)]), \
                     patch("windows_rt_crash.subprocess.check_output", return_value=json.dumps(catalog)), \
                     patch("windows_rt_crash.find_debugger", return_value=Path(sys.executable)), \
                     patch("windows_rt_crash.capture_case", side_effect=capture), \
                     patch("builtins.print"):
                    if mutate:
                        with self.assertRaisesRegex(RuntimeError, "original CTest evidence changed"):
                            main()
                    else:
                        self.assertEqual(main(), 0)
                report = json.loads((root / "test-dumps/deng736/manifest.json").read_text())
                self.assertFalse(report["originalCTestResultReplaced"])
                self.assertEqual(report["originalLogsUnchanged"], not mutate)


if __name__ == "__main__":
    unittest.main()
