"""Validate scoped debugger ABI setup without changing the caller's tools."""
import os
from pathlib import Path
import tempfile
import unittest
from unittest import mock

import lldb_runtime as runtime


class DebuggerRuntime(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="lldb ABI café space ")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        (self.root / "Lib/encodings").mkdir(parents=True)
        for name in ["python.exe", "python310.dll", "Lib/os.py", "Lib/encodings/__init__.py"]:
            (self.root / name).write_bytes(b"fixture, never executed")
        self.provider = self.root / "python.exe"

    def test_default_windows_launch_keeps_inheritance(self):
        with mock.patch.object(runtime.sys, "platform", "win32"), \
                mock.patch.dict(os.environ, {runtime.PYTHON_EXECUTABLE: ""}):
            self.assertIsNone(runtime.debugger_environment())

    def test_other_hosts_keep_inheritance_even_with_a_provider(self):
        with mock.patch.object(runtime.sys, "platform", "darwin"), \
                mock.patch.dict(os.environ, {runtime.PYTHON_EXECUTABLE: str(self.provider)}):
            self.assertIsNone(runtime.debugger_environment())

    def test_only_debugger_child_gets_matching_dll_and_stdlib(self):
        base = {runtime.PYTHON_EXECUTABLE: str(self.provider), "PATH": "compiler-path",
                "PYTHONHOME": "tool-python311", "UNCHANGED": "sentinel"}
        with mock.patch.object(runtime.sys, "platform", "win32"), mock.patch.dict(os.environ, base):
            before = dict(os.environ)
            child = runtime.debugger_environment()
            self.assertEqual(child["PATH"], str(self.root) + os.pathsep + "compiler-path")
            self.assertEqual(child["PYTHONHOME"], str(self.root))
            self.assertEqual(child["UNCHANGED"], "sentinel")
            self.assertEqual(dict(os.environ), before)

    def test_empty_parent_path_has_no_empty_search_component(self):
        with mock.patch.object(runtime.sys, "platform", "win32"), \
                mock.patch.dict(os.environ, {runtime.PYTHON_EXECUTABLE: str(self.provider), "PATH": ""}):
            self.assertEqual(runtime.debugger_environment()["PATH"], str(self.root))

    def test_missing_or_relative_provider_is_not_a_fallback(self):
        for path in ["python.exe", str(self.root / "missing.exe")]:
            with self.subTest(path=path), mock.patch.object(runtime.sys, "platform", "win32"), \
                    mock.patch.dict(os.environ, {runtime.PYTHON_EXECUTABLE: path}):
                with self.assertRaises(ValueError):
                    runtime.debugger_environment()

    def test_required_abi_dll_and_stdlib_are_checked(self):
        for name in ["python310.dll", "Lib/os.py", "Lib/encodings/__init__.py"]:
            path = self.root / name
            path.unlink()
            with self.subTest(name=name), mock.patch.object(runtime.sys, "platform", "win32"), \
                    mock.patch.dict(os.environ, {runtime.PYTHON_EXECUTABLE: str(self.provider)}):
                with self.assertRaisesRegex(ValueError, name.replace(".", r"\.")):
                    runtime.debugger_environment()
            path.write_bytes(b"fixture, never executed")


if __name__ == "__main__":
    unittest.main()
