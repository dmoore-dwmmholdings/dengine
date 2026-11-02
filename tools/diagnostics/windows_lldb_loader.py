"""DENG-723: inspect the selected Windows LLDB after its original CTest fails.

Read-only evidence, never a substitute acceptance run or a DLL-search repair.
DUMPBIN reports static/delay imports. Files found in ordinary search directories
are candidates, not proof of loader resolution (API sets, KnownDLLs, SxS and
already-loaded modules can take precedence). No SDK/dependency is installed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time


MAX_MODULES = 64
MAX_SECONDS = 60


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def selected_lldb(cache):
    values = re.findall(r"^DE_LLDB_EXECUTABLE:FILEPATH=(.+)$", cache, re.M)
    if len(values) != 1 or values[0].strip().endswith("-NOTFOUND"):
        raise ValueError("one discovered DE_LLDB_EXECUTABLE is required")
    path = Path(values[0].strip())
    if not path.is_absolute() or not path.is_file():
        raise ValueError("the selected LLDB must be an existing absolute file")
    return path.resolve()


def dependent_names(text):
    # DUMPBIN /DEPENDENTS prints one import basename per indented line. Reject
    # paths, switches and other output rather than treating them as filenames.
    return sorted(set(re.findall(r"^\s+([\w.+-]+\.dll)\s*$", text, re.M | re.I)),
                  key=str.casefold)


def original_working_directory(data):
    # CTest logs contain raw child output, which need not be UTF-8. Select the
    # metadata as bytes, then decode only this test's directory; never reinterpret
    # or rewrite unrelated output to recover its launch location.
    entries = re.findall(rb"^\d+/\d+ Testing: debugger\.native_lldb\r?\n"
                         rb"(?:(?!^\d+/\d+ Testing:).)*?^Directory: ([^\r\n]+)\r?$",
                         data, re.M | re.S)
    if len(entries) != 1:
        raise ValueError("one original LLDB test working directory is required")
    working = Path(entries[0].decode("utf-8").strip())
    if not working.is_absolute() or not working.is_dir():
        raise ValueError("original test working directory is unavailable")
    return working.resolve()


def search_directories(executable, working):
    windows = Path(os.environ["SystemRoot"])
    candidates = [executable.parent, windows / "System32", windows / "System", windows,
                  working, *(Path(p.strip('"')) for p in os.environ.get("PATH", "").split(os.pathsep)
                             if p.strip('"'))]
    # Record absolute existing directories only; cwd is already explicit above.
    return list(dict.fromkeys(p.resolve() for p in candidates if p.is_absolute() and p.is_dir()))


def invoke(command, log, deadline, working=None):
    remaining = deadline - time.monotonic()
    if remaining <= 0:
        raise TimeoutError("diagnostic time budget exhausted")
    with log.open("wb") as output:
        result = subprocess.run(command, stdout=output, stderr=subprocess.STDOUT,
                                timeout=min(10, remaining), check=False, cwd=working)
    return {"command": [str(p) for p in command], "exit": result.returncode,
            "exitHex": f"0x{result.returncode & 0xffffffff:08x}", "log": str(log)}


def collect(build, output):
    output.mkdir(parents=True, exist_ok=False)
    protected = [build / "CMakeCache.txt", *(build / "Testing/Temporary" / name for name in
                 ("LastTest.log", "LastTestsFailed.log"))]
    before = {str(p): digest(p) for p in protected if p.is_file()}
    report = {"diagnosticOnly": True, "originalCTestResultReplaced": False,
              "dependencyResolutionProven": False, "modules": [], "unlocatedImports": [],
              "limits": ["Candidate search omits loader API-set/KnownDLL/SxS redirection.",
                         "Delay imports and runtime-loaded libraries need separate causal proof."]}
    fingerprints = {}
    try:
        failed = protected[2].read_text(encoding="utf-8-sig")
        if not re.search(r"^\d+:debugger\.native_lldb$", failed, re.M):
            raise ValueError("original debugger.native_lldb failure is required")
        executable = selected_lldb(protected[0].read_text(encoding="utf-8-sig"))
        working = original_working_directory(protected[1].read_bytes())
        report["executable"] = str(executable)
        fingerprints[str(executable)] = digest(executable)
        directories = search_directories(executable, working)
        report["candidateDirectories"] = [str(p) for p in directories]
        report["workingDirectory"] = str(working)
        deadline = time.monotonic() + MAX_SECONDS
        # No fixture/debuggee and no changed environment. A version probe can
        # establish the same loader status without repeating the acceptance test.
        report["versionProbe"] = invoke([executable, "--version"], output / "lldb-version.log",
                                         deadline, working)
        dumpbin = shutil.which("dumpbin")
        if not dumpbin:
            raise ValueError("MSVC dumpbin unavailable; no installation attempted")
        report["dumpbin"] = dumpbin
        system = Path(os.environ["SystemRoot"]).resolve()
        pending, seen = [executable], set()
        while pending:
            module = pending.pop(0)
            if module in seen:
                continue
            if len(seen) >= MAX_MODULES:
                raise ValueError("module bound reached; import collection incomplete")
            seen.add(module)
            fingerprints.setdefault(str(module), digest(module))
            log = output / f"imports-{len(seen):02d}.log"
            run = invoke([dumpbin, "/nologo", "/dependents", module], log, deadline)
            if run["exit"] != 0:
                raise ValueError(f"dumpbin failed for {module}; see {log}")
            dependencies = []
            for name in dependent_names(log.read_text(encoding="utf-8", errors="replace")):
                virtual = name.lower().startswith(("api-ms-", "ext-ms-"))
                locations = [p / name for p in directories if (p / name).is_file()]
                dependencies.append({"name": name, "apiSetContract": virtual,
                                     "candidates": [str(p) for p in locations]})
                if not locations and not virtual:
                    report["unlocatedImports"].append({"importer": str(module), "name": name})
                if locations and not virtual:
                    candidate = locations[0].resolve()
                    # Bound traversal to tool/runtime dependencies; system imports
                    # are retained in the report without expanding the entire OS.
                    if system not in candidate.parents and candidate not in seen:
                        pending.append(candidate)
            report["modules"].append({"path": str(module), "sha256": fingerprints[str(module)],
                                      "dumpbin": run, "imports": dependencies})
        report["status"] = "collected; missing import candidates require causal confirmation"
        return 0
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["status"] = "incomplete"
        report["error"] = str(error)
        return 2
    finally:
        report["originalEvidenceSha256"] = before
        report["originalEvidenceUnchanged"] = all(Path(p).is_file() and digest(Path(p)) == h
                                                   for p, h in before.items())
        report["inspectedModulesSha256"] = fingerprints
        report["inspectedModulesUnchanged"] = all(Path(p).is_file() and digest(Path(p)) == h
                                                   for p, h in fingerprints.items())
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(json.dumps(report), flush=True)
        if not report["originalEvidenceUnchanged"] or not report["inspectedModulesUnchanged"]:
            raise RuntimeError("original evidence or inspected debugger files changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-dir", required=True, type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("actual loader collection requires Windows; controls are portable")
    build = args.test_dir.resolve()
    return collect(build, build / "test-dumps/deng723-loader")


if __name__ == "__main__":
    sys.exit(main())
