#!/usr/bin/env python3
"""Build an isolated LLVM 23.1.3 arm64 libFuzzer RSS-lifetime candidate.

Never changes the installed compiler/runtime. This is a diagnostic candidate,
not a replacement for the engine's default fuzz acceptance configuration.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import urllib.request

COMMIT = "0d261d1ca552c95a8f007e061c787ac7132fbcbc"


def run(*args):
    subprocess.run([str(x) for x in args], check=True)


def create_resource_directory(resource, archive, out):
    """Keep Clang's normal runtime selection, replacing only libFuzzer."""
    private = out / "clang-resource"
    private.mkdir()
    resource = resource.resolve()
    for entry in resource.iterdir():
        if entry.name != "lib":
            (private / entry.name).symlink_to(entry.resolve(), target_is_directory=entry.is_dir())
    runtime_dir = private / "lib/darwin"
    runtime_dir.mkdir(parents=True)
    for entry in (resource / "lib/darwin").iterdir():
        target = archive if entry.name == "libclang_rt.fuzzer_osx.a" else entry.resolve()
        (runtime_dir / entry.name).symlink_to(target, target_is_directory=entry.is_dir())
    return private


def verify_controls(compiler, resource, out):
    control = Path(__file__).with_name("fuzzer_control.c").resolve()
    env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:halt_on_error=1",
               UBSAN_OPTIONS="halt_on_error=1", LSAN_OPTIONS="")
    results = {}
    for name, mode, expected in (("empty", 0, 0), ("leak", 1, 1), ("rss", 2, 71)):
        exe = out / f"control-{name}"
        run(compiler.with_name("clang"), "-g", "-std=c11", "-Wall", "-Wextra", "-Wpedantic",
            "-Werror", f"-resource-dir={resource}",
            "-fsanitize=fuzzer,address", f"-DMODE={mode}", control, "-o", exe)
        args = [str(exe), "-runs=0"]
        if mode == 2:
            args.append("-rss_limit_mb=1")
        result = subprocess.run(args, env=env, cwd=out, text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT)
        (out / f"{name}.log").write_text(result.stdout)
        if result.returncode != expected:
            raise RuntimeError(f"{name} control exited {result.returncode}, expected {expected}")
        if mode == 1 and "LeakSanitizer" not in result.stdout:
            raise RuntimeError("real-leak control did not report LeakSanitizer")
        if mode == 2 and "ERROR: libFuzzer: out-of-memory (used:" not in result.stdout:
            raise RuntimeError("RSS control did not report the watchdog limit")
        results[name] = result.returncode
    (out / "controls.json").write_text(json.dumps(results, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-prefix", type=Path, default=Path("/opt/homebrew/opt/llvm"))
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        parser.error("this diagnostic requires native macOS arm64")
    compiler = args.llvm_prefix / "bin/clang++"
    version = subprocess.check_output([str(compiler), "--version"], text=True)
    if "version 23.1.3" not in version:
        parser.error("requires LLVM 23.1.3 to match the pinned driver source")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)
    source = out / "source"
    source.mkdir()
    api = f"https://api.github.com/repos/llvm/llvm-project/contents/compiler-rt/lib/fuzzer?ref={COMMIT}"
    with urllib.request.urlopen(api) as response:
        entries = json.load(response)
    entries = [x for x in entries if x["type"] == "file" and
               (x["name"].endswith((".h", ".def")) or x["name"] == "FuzzerDriver.cpp")]

    def download(entry):
        name = entry["name"]
        url = f"https://raw.githubusercontent.com/llvm/llvm-project/{COMMIT}/compiler-rt/lib/fuzzer/{name}"
        with urllib.request.urlopen(url) as response:
            data = response.read()
        blob = b"blob " + str(len(data)).encode() + b"\0" + data
        if hashlib.sha1(blob).hexdigest() != entry["sha"]:
            raise RuntimeError(f"upstream blob verification failed: {name}")
        (source / name).write_bytes(data)

    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(download, entries))
    patch = Path(__file__).with_name("fuzzer-rss-lifetime.patch")
    run("patch", "--batch", "--forward", "-p1", "-d", source, "-i", patch.resolve())
    resource = Path(subprocess.check_output([str(compiler), "-print-resource-dir"], text=True).strip())
    installed = resource / "lib/darwin/libclang_rt.fuzzer_osx.a"
    archive = out / "libclang_rt.fuzzer_joined_arm64.a"
    run("xcrun", "lipo", installed, "-thin", "arm64", "-output", archive)
    obj = source / "FuzzerDriver.cpp.o"
    run(compiler, "-std=c++17", "-O2", "-g", "-fno-exceptions", "-fno-rtti",
        "-fno-omit-frame-pointer", "-c", source / "FuzzerDriver.cpp", "-o", obj)
    run(args.llvm_prefix / "bin/llvm-ar", "r", archive, obj)
    private = create_resource_directory(resource, archive, out)
    metadata = {"upstream_commit": COMMIT, "compiler": version,
                "original_archive": str(installed),
                "original_sha256": hashlib.sha256(installed.read_bytes()).hexdigest(),
                "patch_sha256": hashlib.sha256(patch.read_bytes()).hexdigest(),
                "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
                "resource_directory": str(private),
                "original_resource_directory": str(resource.resolve()),
                "host": platform.platform(), "architecture": platform.machine()}
    (out / "provenance.json").write_text(json.dumps(metadata, indent=2) + "\n")
    verify_controls(compiler, private, out)
    print(archive)
    print(f"Clang flag: -resource-dir={private}")


if __name__ == "__main__":
    main()
