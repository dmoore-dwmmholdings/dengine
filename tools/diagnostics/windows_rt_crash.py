"""DENG-736: collect evidence after original Windows CTests fail; never retry a gate.

CDB runs the unchanged discovered command under its own bounded process. Its exit
code is debugger status, never a replacement test result. The original CTest logs
are fingerprinted and must remain unchanged. No debugger or SDK is installed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys


CASES = (
    "render_rt_lazy.rt_lazy.path_sources_are_lazy_and_reused_by_hybrid_DI",
    "render_rt_lazy.rt_lazy.path_source_partial_failure_is_cleaned_and_cached",
    "render_rt_lazy.rt_lazy.hybrid_DI_failure_retains_working_path_sources",
)
MARKER = "DENG736_SECOND_CHANCE"


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def complete_minidump(path):
    if not path.is_file():
        return False
    size = path.stat().st_size
    with path.open("rb") as stream:
        header = stream.read(32)
        if len(header) != 32 or header[:4] != b"MDMP":
            return False
        count, directory = struct.unpack_from("<II", header, 8)
        if not count or directory < 32 or directory > size or count > (size - directory) // 12:
            return False
        stream.seek(directory)
        for _ in range(count):
            _, length, offset = struct.unpack("<III", stream.read(12))
            if offset > size or length > size - offset:
                return False
    return True


def select_cases(catalog, failures):
    failed = {line.partition(":")[2] for line in failures.splitlines()
              if line.partition(":")[0].isdigit()}
    registered = {test["name"]: test for test in catalog["tests"]}
    selected = []
    for name in CASES:
        if name in failed:
            test = registered.get(name)
            if not test or not test.get("command"):
                raise ValueError(f"failed test is not runnable in current catalog: {name}")
            selected.append(test)
    return selected


def debugger_commands(dump_path):
    # Backslashes inside sxd's quoted callback are escape characters. Win32 also
    # accepts forward-slash separators, so keep the filename out of that grammar.
    dump_path = dump_path.replace("\\", "/")
    # Only change break status, not exception handling status (-h is not used).
    # https://learn.microsoft.com/en-us/windows-hardware/drivers/debuggercmds/sx--sxd--sxe--sxi--sxn--sxr--sx---set-exceptions-
    capture = (f".echo {MARKER}; .exr -1; .ecxr; kn; lm; "
               f".dump /m /o {dump_path}; q")
    events = ("av", "sov", "ii", "0xc0000409", "0x40000015", "eh", "*")
    return "\n".join(f'sxd -c2 "{capture}" {event}' for event in events) + "\ng\n"


def capture_case(debugger, test, output):
    output.mkdir(parents=True, exist_ok=False)  # Never replace an earlier capture.
    command_file = output / "commands.txt"
    target = list(test["command"])
    executable = Path(target[0])
    before = digest(executable)
    properties = {p["name"]: p["value"] for p in test.get("properties", [])}
    if not isinstance(properties.get("WORKING_DIRECTORY"), str):
        raise ValueError("discovered working directory is required for faithful capture")
    working = Path(properties["WORKING_DIRECTORY"])
    original_timeout = float(properties.get("TIMEOUT", 60))
    if not 0 < original_timeout <= 3600:
        raise ValueError(f"invalid discovered timeout for {test['name']}: {original_timeout}")
    timeout = min(original_timeout, 60.0)  # Diagnostic cap; original CTest limit is unchanged.
    dump_path = os.path.relpath(output / "crash.dmp", working)
    # These known CTests and output directories share the build root. A relative
    # name keeps debugger callbacks free of nested quoting, even if that root
    # contains spaces. Refuse an unsupported layout instead of misparsing it.
    if re.search(r'[\s";]', dump_path):
        raise ValueError(f"unsupported debugger dump path: {dump_path}")
    command_file.write_text(debugger_commands(dump_path), encoding="ascii")
    environment = os.environ.copy()
    if properties.get("ENVIRONMENT_MODIFICATION"):
        raise ValueError("environment modifications need explicit diagnostic support")
    for assignment in properties.get("ENVIRONMENT", []):
        key, separator, value = assignment.partition("=")
        if not key or not separator:
            raise ValueError(f"invalid discovered environment assignment: {assignment}")
        environment[key] = value
    command = [*debugger, "-hd", "-G", "-lines", "-y", str(executable.parent),
               "-cf", str(command_file), *target]
    result = {"name": test["name"], "originalCommand": target,
              "debuggerCommand": command, "timeoutSeconds": timeout,
              "originalTimeoutSeconds": original_timeout, "workingDirectory": str(working),
              "diagnosticOnly": True, "executableSha256": before}
    with (output / "console.log").open("wb") as log:
        process = subprocess.Popen(command, cwd=working, env=environment,
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            result["debuggerExit"] = process.wait(timeout=timeout)
            result["timedOut"] = False
        except subprocess.TimeoutExpired:
            # CDB's default kill-on-exit also terminates its debuggee. Only this
            # invocation's owned debugger process is killed; no name-based kill.
            process.kill()
            result["debuggerExit"] = process.wait()
            result["timedOut"] = True
    text = (output / "console.log").read_text(encoding="utf-8", errors="replace")
    dump = output / "crash.dmp"
    # CDB echoes the registration command too; only the executed .echo is evidence.
    result["secondChanceObserved"] = any(line.strip() == MARKER for line in text.splitlines())
    result["exceptionCodes"] = re.findall(r"ExceptionCode:\s*([0-9a-fA-F]{8})", text)
    result["minidumpCaptured"] = complete_minidump(dump)
    result["executableUnchanged"] = digest(executable) == before
    result["captured"] = (not result["timedOut"] and result["secondChanceObserved"]
                          and bool(result["exceptionCodes"])
                          and result["minidumpCaptured"] and result["executableUnchanged"])
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def find_debugger(explicit=None):
    candidates = [explicit, shutil.which("cdb")]
    kits = os.environ.get("ProgramFiles(x86)")
    if kits:
        candidates.append(str(Path(kits) / "Windows Kits/10/Debuggers/x64/cdb.exe"))
    return next((Path(p).resolve() for p in candidates if p and Path(p).is_file()), None)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--test-dir", type=Path, required=True)
    parser.add_argument("--catalog", type=Path, required=True,
                        help="CTest JSON catalog saved before the original test execution")
    parser.add_argument("--cdb", type=Path)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("native CDB capture requires Windows; portable control tests are separate")
    build = args.test_dir.resolve()
    output = build / "test-dumps/deng736"
    output.mkdir(parents=True, exist_ok=False)
    report = {"diagnosticOnly": True, "originalCTestResultReplaced": False, "cases": []}
    protected = [build / "Testing/Temporary" / name
                 for name in ("LastTest.log", "LastTestsFailed.log")]
    before = {str(p): digest(p) for p in protected if p.is_file()}
    catalog_path = args.catalog.resolve()
    catalog_before = None
    try:
        failed = protected[1].read_text() if protected[1].is_file() else ""
        # Even CTest's --show-only rewrites LastTest.log. Never discover here:
        # the original failed-run evidence must survive byte for byte.
        catalog_data = catalog_path.read_bytes()
        catalog_before = hashlib.sha256(catalog_data).hexdigest()
        report["catalogPath"] = str(catalog_path)
        report["catalogSha256"] = catalog_before
        catalog = json.loads(catalog_data.decode("utf-8-sig"))
        selected = select_cases(catalog, failed)
        report["selected"] = [test["name"] for test in selected]
        if not selected:
            report["status"] = "no matching original failures"
            return 0
        debugger = find_debugger(args.cdb)
        if debugger is None:
            report["status"] = "CDB unavailable; capture incomplete, no SDK installation attempted"
            return 2
        report["debugger"] = str(debugger)
        for index, test in enumerate(selected):
            report["cases"].append(capture_case([str(debugger)], test, output / f"case-{index}"))
        report["status"] = "captured" if all(r["captured"] for r in report["cases"]) else "incomplete"
        return 0 if report["status"] == "captured" else 1
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        report["status"] = "capture error"
        report["error"] = str(error)
        return 2
    finally:
        report["originalLogsSha256"] = before
        report["originalLogsUnchanged"] = all(p.is_file() and digest(p) == before[str(p)]
                                              for p in protected if str(p) in before)
        report["catalogUnchanged"] = None
        if catalog_before is not None:
            report["catalogUnchanged"] = (catalog_path.is_file() and
                                          digest(catalog_path) == catalog_before)
        (output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
        print(json.dumps(report), flush=True)
        if not report["originalLogsUnchanged"]:
            raise RuntimeError("original CTest evidence changed during diagnostic capture")
        if report["catalogUnchanged"] is False:
            raise RuntimeError("original CTest catalog changed during diagnostic capture")


if __name__ == "__main__":
    sys.exit(main())
