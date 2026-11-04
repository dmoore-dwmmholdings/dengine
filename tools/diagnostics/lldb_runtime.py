"""Optional Python ABI provider for the observed Windows LLDB dependency.

The checker and diagnostic must launch the debugger with the same environment.
Only that child gets Python 3.10; the caller's compiler/tool Python is untouched.
"""
import os
from pathlib import Path
import sys


PYTHON_EXECUTABLE = "DE_LLDB_PYTHON_EXECUTABLE"


def debugger_environment():
    provider = os.environ.get(PYTHON_EXECUTABLE)
    if sys.platform != "win32" or not provider:
        return None  # Preserve ordinary subprocess inheritance on other hosts.
    executable = Path(provider)
    if not executable.is_absolute() or not executable.is_file():
        raise ValueError(f"{PYTHON_EXECUTABLE} must name an existing absolute Python executable")
    root = executable.parent.resolve()
    for relative in ("python310.dll", "Lib/os.py", "Lib/encodings/__init__.py"):
        if not (root / relative).is_file():
            raise ValueError(f"LLDB Python 3.10 provider is missing {relative}: {root}")
    environment = os.environ.copy()
    previous = environment.get("PATH", "")
    environment["PATH"] = str(root) + (os.pathsep + previous if previous else "")
    environment["PYTHONHOME"] = str(root)
    return environment
