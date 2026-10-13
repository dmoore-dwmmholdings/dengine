#!/usr/bin/env python3
"""Self-test for `kind = "prebuilt"` entries of tools/vendor/vendor.py and for
cmake/DePrebuilt.cmake (DENG-390). Offline: fake per-host archives are built locally and fetched
through file:// URLs into a temporary fake repository and cache.

  prebuilt_selftest.py [--cmake <cmake> [--generator <g>] [--make-program <p>]]

With --cmake, a small CMake project also imports the fake entry at configure time, for the host
this machine builds for, and a hash mismatch must stop that configure. Exit status 0 when every
case passes."""

from __future__ import annotations

import argparse
import io
import os
import pathlib
import stat
import subprocess
import sys
import tarfile
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True  # keep tools/vendor/ free of __pycache__
sys.path.insert(0, HERE)
import selftest  # noqa: E402  (shared helpers: Repo, check, make_tar)
from selftest import Repo, check, vendor  # noqa: E402

REPO_ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
HOSTS = ("linux-x64", "macos-arm64", "windows-x64")

LICENSE = b"MIT License\n\nCopyright (c) demo\n"
HEADER = b"#pragma once\nint demo(void);\n"


def host_files(host: str) -> dict:
    """Archive content for one host: committed part (LICENSE, include/) + binaries + noise."""
    files = {
        "LICENSE": LICENSE,
        "include/demo.h": HEADER,
        "host.txt": host.encode(),
        "share/doc/big.md": b"not extracted\n" * 100,
    }
    if host == "windows-x64":
        files["bin/demo.dll"] = b"MZ dll " + host.encode()
        files["bin/democ.exe"] = b"MZ exe " + host.encode()
    else:
        ext = "dylib" if host.startswith("macos") else "so"
        files["bin/democ"] = b"#!/bin/sh\necho demo\n"
        files[f"lib/libdemo.1.0.{ext}"] = b"\x7fELF lib " + host.encode()
    return files


def make_tar_modes(path: str, files: dict, links: dict) -> str:
    """tar.gz without a top directory (like Slang's Linux archive), with modes and symlinks."""
    with tarfile.open(path, "w:gz") as tf:
        for rel, data in sorted(files.items()):
            info = tarfile.TarInfo(rel)
            info.size = len(data)
            info.mode = 0o755 if rel.startswith(("bin/", "lib/")) else 0o644
            tf.addfile(info, io.BytesIO(data))
        for rel, target in sorted(links.items()):
            info = tarfile.TarInfo(rel)
            info.type = tarfile.SYMTYPE
            info.linkname = target
            tf.addfile(info)
    return vendor.sha256_file(path)


def make_zip_modes(path: str, files: dict, links: dict, top: str) -> str:
    """zip with a top directory, unix modes in external_attr and symlink members."""
    with zipfile.ZipFile(path, "w") as z:
        for rel, data in sorted(files.items()):
            info = zipfile.ZipInfo(f"{top}/{rel}")
            mode = 0o755 if rel.startswith(("bin/", "lib/")) else 0o644
            info.external_attr = (stat.S_IFREG | mode) << 16
            z.writestr(info, data)
        for rel, target in sorted(links.items()):
            info = zipfile.ZipInfo(f"{top}/{rel}")
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(info, target)
    return vendor.sha256_file(path)


def build_archives(base: str) -> dict:
    """{host: (file:// url, sha256, strip, files globs)} for the three fake host archives."""
    out = {}
    lin = os.path.join(base, "demo-1.0-linux-x86_64.tar.gz")
    sha = make_tar_modes(lin, host_files("linux-x64"), {"lib/libdemo.so": "libdemo.1.0.so"})
    out["linux-x64"] = (pathlib.Path(lin).as_uri(), sha, 0,
                        ["host.txt", "bin/democ", "lib/libdemo*"])
    mac = os.path.join(base, "demo-1.0-macos-aarch64.zip")
    sha = make_zip_modes(mac, host_files("macos-arm64"), {"lib/libdemo.dylib": "libdemo.1.0.dylib"},
                         top="demo-1.0-macos")
    out["macos-arm64"] = (pathlib.Path(mac).as_uri(), sha, 1,
                          ["host.txt", "bin/democ", "lib/*.dylib"])
    win = os.path.join(base, "demo-1.0-windows-x86_64.zip")
    sha = make_zip_modes(win, host_files("windows-x64"), {}, top="demo-1.0-windows")
    out["windows-x64"] = (pathlib.Path(win).as_uri(), sha, 1, ["host.txt", "bin/*"])
    return out


def toml_list(items: list) -> str:
    return "[" + ", ".join(f'"{x}"' for x in items) + "]"


def prebuilt_entry(archives: dict, top: dict | None = None, hosts: dict | None = None,
                   name: str = "demo") -> str:
    """A prebuilt [[lib]] entry. The committed part comes from the Linux archive (shared cache
    file). `top` and `hosts` override keys ({key: toml text or None to drop})."""
    url, sha, _, _ = archives["linux-x64"]
    fields = {
        "name": f'"{name}"',
        "kind": '"prebuilt"',
        "version": '"1.0"',
        "url": f'"{url}"',
        "sha256": f'"{sha}"',
        "license": '"MIT"',
        "strip": "0",
        "files": '["include/*.h"]',
    }
    for k, v in (top or {}).items():
        if v is None:
            fields.pop(k, None)
        else:
            fields[k] = v
    text = "[[lib]]\n" + "".join(f"{k} = {v}\n" for k, v in fields.items())
    for host, (hurl, hsha, strip, globs) in archives.items():
        t = {"url": f'"{hurl}"', "sha256": f'"{hsha}"', "strip": str(strip),
             "files": toml_list(globs)}
        for k, v in ((hosts or {}).get(host) or {}).items():
            if v is None:
                t.pop(k, None)
            else:
                t[k] = v
        text += f"\n[lib.hosts.{host}]\n" + "".join(f"{k} = {v}\n" for k, v in t.items())
    return text + "\n"


def plain_entry(base: str) -> str:
    """A normal source entry next to the prebuilt one: it must behave as before."""
    arch = os.path.join(base, "plain-1.0.tar.gz")
    sha = selftest.make_tar(arch, selftest.UPSTREAM)
    return selftest.entry("plain", pathlib.Path(arch).as_uri(), sha)


def test_config(repo: Repo, archives: dict, plain: str) -> None:
    # Both TOML readers see the same entry.
    for fb in (False, True):
        r = repo.run("list", fallback=fb)
        check(r.returncode == 0 and "prebuilt linux-x64" in r.stdout
              and "prebuilt windows-x64" in r.stdout and "plain" in r.stdout,
              f"list shows the hosts ({'fallback' if fb else 'tomllib'} reader) {r.stderr.strip()}")
    cfg = os.path.join(repo.tp, "vendor.toml")
    libs = [vendor.load_config(cfg, force_fallback=fb) for fb in (False, True)]

    def shape(ls):
        return [(x.name, x.kind, x.files, x.strip,
                 {h: (a.url, a.sha256, a.strip, a.files, a.exclude) for h, a in x.hosts.items()})
                for x in ls]
    check(shape(libs[0]) == shape(libs[1]), "tomllib and the fallback reader agree")
    check(libs[0][0].kind == "prebuilt" and libs[0][1].kind == "source"
          and libs[0][1].hosts == {}, "kind defaults to source")

    # Config errors (exit 2).
    cases = {
        "prebuilt without hosts": prebuilt_entry(archives).split("\n[lib.hosts")[0] + "\n",
        "hosts on a source entry": prebuilt_entry(archives, top={"kind": None}),
        "unknown kind": prebuilt_entry(archives, top={"kind": '"binary"'}),
        "bad host name": prebuilt_entry(archives).replace("[lib.hosts.linux-x64]",
                                                          "[lib.hosts.linux-x86]"),
        "host without sha256": prebuilt_entry(archives, hosts={"linux-x64": {"sha256": None}}),
        "host without files": prebuilt_entry(archives, hosts={"macos-arm64": {"files": "[]"}}),
        "host with an unknown key": prebuilt_entry(
            archives, hosts={"linux-x64": {"branch": '"main"'}}),
        "host with a bad sha256": prebuilt_entry(
            archives, hosts={"windows-x64": {"sha256": '"abc"'}}),
        "host with a proprietary SDK url": prebuilt_entry(
            archives, hosts={"windows-x64": {"url": '"https://example.com/steamworks_sdk.zip"'}}),
        "prebuilt entry outside the licence allowlist": prebuilt_entry(
            archives, top={"license": '"Proprietary"'}),
    }
    for what, text in cases.items():
        repo.config(text)
        for fb in (False, True):
            r = repo.run("list", fallback=fb)
            check(r.returncode == 2 and "config error" in r.stderr,
                  f"refuses {what} ({'fallback' if fb else 'tomllib'})")
    repo.config(prebuilt_entry(archives) + plain)


def test_fetch_verify(repo: Repo, archives: dict) -> None:
    d = pathlib.Path(repo.tp, "demo")
    r = repo.run("fetch", "--all", "--host", "linux-x64")
    check(r.returncode == 0, f"fetch --all with a prebuilt entry {r.stderr.strip()}")
    snap = repo.snapshot("demo")
    check(sorted(snap) == ["LICENSE", "include/demo.h"],
          f"fetch commits only the headers and LICENSE {sorted(snap)}")
    check(sorted(repo.snapshot("plain")) == ["LICENSE", "include/demo.h", "src/demo.c",
                                              "src/extra/deep.c"], "the source entry is unchanged")
    cached = sorted(os.listdir(repo.cache))
    check(len([c for c in cached if c.startswith("demo-")]) == 1,
          f"committed part and linux-x64 share one cached archive {cached}")
    check(not any(c.endswith(".zip") for c in cached), "fetch caches only this host's archive")

    r = repo.run("fetch", "demo", "--all-hosts")
    check(r.returncode == 0 and sum(1 for c in os.listdir(repo.cache) if c.startswith("demo-")) == 3,
          f"fetch --all-hosts caches every host archive {r.stderr.strip()}")

    for args in (["--host", "linux-x64"], ["--all-hosts"], ["--offline", "--all-hosts"]):
        r = repo.run("verify", *args)
        check(r.returncode == 0 and "ok   demo [linux-x64]" in r.stdout,
              f"verify {' '.join(args)} {r.stderr.strip()}")
    r = repo.run("verify", "--all-hosts")
    check(all(f"ok   demo [{h}]" in r.stdout for h in HOSTS), "--all-hosts checks every host")
    r = repo.run("verify", "--host", "linux-arm64")
    check(r.returncode == 0 and "skip demo 1.0: no prebuilt archive for host linux-arm64"
          in r.stdout, "verify on a host without an archive skips it with a note")
    r = repo.run("verify", "--host", "linux-x86")
    check(r.returncode == 2, "verify refuses a malformed --host")

    # The committed part is still checked byte for byte.
    h = d / "include" / "demo.h"
    h.write_bytes(h.read_bytes() + b"// edit\n")
    r = repo.run("verify", "--host", "linux-x64")
    check(r.returncode == 1 and "include/demo.h differs" in r.stderr,
          "verify detects an edit in the committed part")
    repo.run("fetch", "demo", "--host", "linux-x64")
    # A prebuilt binary in third_party/ is an unexpected file: binaries are never committed.
    (d / "lib").mkdir()
    (d / "lib" / "libdemo.1.0.so").write_bytes(b"binary")
    r = repo.run("verify", "--host", "linux-x64")
    check(r.returncode == 1 and "unexpected file lib/libdemo.1.0.so" in r.stderr,
          "a committed prebuilt binary fails verify")
    (d / "lib" / "libdemo.1.0.so").unlink()
    (d / "lib").rmdir()


def test_mismatch(repo: Repo, archives: dict, plain: str) -> None:
    bad = "0" * 64
    repo.config(prebuilt_entry(archives, hosts={"windows-x64": {"sha256": f'"{bad}"'}}) + plain)
    r = repo.run("verify", "--host", "linux-x64")
    check(r.returncode == 0, "a bad pin of another host does not fail the host verify")
    r = repo.run("verify", "--all-hosts")
    check(r.returncode == 1 and "demo [windows-x64]: sha256 mismatch" in r.stderr,
          "verify --all-hosts names the host with the sha256 mismatch")
    dest = os.path.join(os.path.dirname(repo.root), "out-bad")
    r = repo.run("prebuilt", "demo", "--host", "windows-x64", "--dest", dest)
    check(r.returncode == 1 and "sha256 mismatch" in r.stderr and "expected " + bad in r.stderr
          and not os.path.exists(dest), "prebuilt fails clearly on a hash mismatch, writes nothing")
    r = repo.run("fetch", "demo", "--all-hosts")
    check(r.returncode == 1 and "sha256 mismatch" in r.stderr, "fetch --all-hosts fails too")

    # A host glob that matches nothing, and a symlink whose target is not selected.
    repo.config(prebuilt_entry(archives, hosts={"linux-x64": {"files": '["nothing/*"]'}}) + plain)
    r = repo.run("verify", "--host", "linux-x64")
    check(r.returncode == 1 and "demo [linux-x64]: `files` glob 'nothing/*' matches nothing"
          in r.stderr, "a host glob that matches nothing fails")
    repo.config(prebuilt_entry(archives, hosts={"linux-x64": {"files": '["lib/libdemo.so"]'}})
                + plain)
    r = repo.run("verify", "--host", "linux-x64")
    check(r.returncode == 1 and "target is not selected" in r.stderr,
          "a selected symlink needs its target")
    repo.config(prebuilt_entry(archives) + plain)


def test_extract(repo: Repo, archives: dict) -> None:
    out = os.path.join(os.path.dirname(repo.root), "out")
    for host in HOSTS:
        dest = os.path.join(out, host)
        r = repo.run("prebuilt", "demo", "--host", host, "--dest", dest)
        check(r.returncode == 0 and "extracted" in r.stdout,
              f"prebuilt extracts {host} {r.stderr.strip()}")
        p = pathlib.Path(dest)
        check((p / "host.txt").read_bytes() == host.encode(), f"{host}: the right archive")
        check(not (p / "share").exists() and not (p / "include").exists(),
              f"{host}: only the selected files")
        check((p / vendor.PREBUILT_STAMP).is_file(), f"{host}: stamp written")
        if host != "windows-x64":
            exe = p / "bin" / "democ"
            if os.name != "nt":
                check(os.access(exe, os.X_OK), f"{host}: executable bit kept")
            ext = "dylib" if host.startswith("macos") else "so"
            link = p / "lib" / f"libdemo.{ext}"
            real = p / "lib" / f"libdemo.1.0.{ext}"
            check(link.read_bytes() == real.read_bytes(), f"{host}: symlink resolves to its target")
            if os.name != "nt":
                check(link.is_symlink(), f"{host}: symlink kept as a symlink")

    # A second run with the same pin does nothing; a missing file or a new pin re-extracts.
    dest = os.path.join(out, "linux-x64")
    stamp = pathlib.Path(dest, vendor.PREBUILT_STAMP)
    before = stamp.stat().st_mtime_ns
    r = repo.run("prebuilt", "demo", "--host", "linux-x64", "--dest", dest, "--offline")
    check(r.returncode == 0 and "up to date" in r.stdout and stamp.stat().st_mtime_ns == before,
          "a second extraction is a no-op")
    pathlib.Path(dest, "bin", "democ").unlink()
    pathlib.Path(dest, "stray.txt").write_text("left over\n")
    r = repo.run("prebuilt", "demo", "--host", "linux-x64", "--dest", dest)
    check(r.returncode == 0 and "extracted" in r.stdout and pathlib.Path(dest, "bin", "democ")
          .exists() and not pathlib.Path(dest, "stray.txt").exists(),
          "a missing file re-extracts the whole directory")

    # A cached archive that no longer matches its pin is refused offline, re-downloaded online.
    ha = vendor.load_config(os.path.join(repo.tp, "vendor.toml"))[0].hosts["macos-arm64"]
    cached = pathlib.Path(repo.cache, ha.archive_name)
    cached.write_bytes(b"corrupt")
    dest = os.path.join(out, "mac-again")
    r = repo.run("prebuilt", "demo", "--host", "macos-arm64", "--dest", dest, "--offline")
    check(r.returncode == 1 and "has sha256" in r.stderr and not os.path.exists(dest),
          "a corrupt cached archive fails offline")
    r = repo.run("prebuilt", "demo", "--host", "macos-arm64", "--dest", dest)
    check(r.returncode == 0 and vendor.sha256_file(str(cached)) == ha.sha256,
          f"a corrupt cached archive is downloaded again {r.stderr.strip()}")

    r = repo.run("prebuilt", "demo", "--host", "linux-arm64", "--dest", os.path.join(out, "x"))
    check(r.returncode == vendor.EXIT_NO_HOST and "no prebuilt archive for host linux-arm64"
          in r.stderr, "a host the entry does not list is exit 3")
    r = repo.run("prebuilt", "plain", "--dest", os.path.join(out, "x"))
    check(r.returncode == 2 and "not a prebuilt entry" in r.stderr, "a source entry is refused")


FIXTURE = """cmake_minimum_required(VERSION 3.28)
project(de_prebuilt_fixture LANGUAGES NONE)
list(APPEND CMAKE_MODULE_PATH "{module_dir}")
include(DePrebuilt)

# A host the entry does not list: OPTIONAL makes that a soft miss.
de_prebuilt_import(demo HOST linux-arm64 ROOT "{root}" OPTIONAL)
if(DE_PREBUILT_DEMO_FOUND)
    message(FATAL_ERROR "linux-arm64 must not be found")
endif()

# The target platform's archive (default HOST), extracted into <build>/prebuilt/demo.
de_prebuilt_target_host(host)
if(NOT host STREQUAL "{expect_host}")
    message(FATAL_ERROR "target host '${{host}}', expected '{expect_host}'")
endif()
de_prebuilt_import(demo ROOT "{root}" {optional})
if(DE_PREBUILT_DEMO_FOUND)
    file(READ "${{DE_PREBUILT_DEMO_DIR}}/host.txt" got)
    if(NOT got STREQUAL "${{host}}" OR NOT DE_PREBUILT_DEMO_HOST STREQUAL "${{host}}"
       OR NOT DE_PREBUILT_DEMO_DIR STREQUAL "${{CMAKE_BINARY_DIR}}/prebuilt/demo")
        message(FATAL_ERROR "wrong import: '${{got}}' from ${{DE_PREBUILT_DEMO_DIR}}")
    endif()
    message(STATUS "fixture: imported ${{host}}")
endif()

# An explicit host and directory.
de_prebuilt_import(demo HOST windows-x64 DEST "${{CMAKE_BINARY_DIR}}/win" ROOT "{root}")
if(NOT EXISTS "${{CMAKE_BINARY_DIR}}/win/bin/demo.dll")
    message(FATAL_ERROR "windows-x64 files missing")
endif()
"""


def test_cmake(base: str, repo: Repo, archives: dict, plain: str, args) -> None:
    host = vendor.current_host()
    known = host in HOSTS
    proj = pathlib.Path(base, "cmake-src")
    proj.mkdir()
    module_dir = pathlib.Path(REPO_ROOT, "cmake").as_posix()

    def configure(root: str, build: str) -> subprocess.CompletedProcess:
        (proj / "CMakeLists.txt").write_text(FIXTURE.format(
            module_dir=module_dir, root=pathlib.Path(root).as_posix(),
            expect_host=host if known else "", optional="" if known else "OPTIONAL"))
        cmd = [args.cmake, "-S", str(proj), "-B", os.path.join(base, build)]
        if args.generator:
            cmd += ["-G", args.generator]
        if args.make_program:
            cmd += [f"-DCMAKE_MAKE_PROGRAM={args.make_program}"]
        env = dict(selftest.GIT_ENV, DE_VENDOR_CACHE=repo.cache)
        return subprocess.run(cmd, capture_output=True, text=True, env=env)

    if not known:
        print(f"note: this machine ({host}) is not one of {HOSTS}; the default-host import is "
              "checked as OPTIONAL")
    r = configure(repo.root, "cmake-build")
    out = r.stdout + r.stderr
    check(r.returncode == 0, "CMake configure imports the prebuilt entry"
          + ("" if r.returncode == 0 else "\n" + out))
    if known:
        check(f"fixture: imported {host}" in out
              and pathlib.Path(base, "cmake-build", "prebuilt", "demo", "host.txt").is_file(),
              f"the configure extracted {host} into <build>/prebuilt/demo")
    r = configure(repo.root, "cmake-build")
    check(r.returncode == 0 and "is up to date" in r.stdout, "a re-configure extracts nothing")

    # A hash mismatch stops the configure with a clear message.
    bad = Repo(os.path.join(base, "bad"))
    hosts = {h: {"sha256": '"' + "0" * 64 + '"'} for h in HOSTS}
    bad.config(prebuilt_entry(archives, hosts=hosts) + plain)
    r = configure(bad.root, "cmake-bad")
    out = r.stdout + r.stderr
    check(r.returncode != 0 and "sha256 mismatch" in out and "de_prebuilt_import(demo)" in out,
          "a hash mismatch fails the configure clearly")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cmake", help="also test cmake/DePrebuilt.cmake with this cmake")
    ap.add_argument("--generator", default="")
    ap.add_argument("--make-program", default="")
    args = ap.parse_args()

    with tempfile.TemporaryDirectory(prefix="de-vendor-prebuilt-") as base:
        gitconfig = pathlib.Path(base, "gitconfig")
        gitconfig.write_text("")
        selftest.GIT_ENV["GIT_CONFIG_GLOBAL"] = str(gitconfig)
        archives = build_archives(base)
        plain = plain_entry(base)
        repo = Repo(os.path.join(base, "main"))
        repo.config(prebuilt_entry(archives) + plain)

        test_config(repo, archives, plain)
        test_fetch_verify(repo, archives)
        test_mismatch(repo, archives, plain)
        test_extract(repo, archives)
        if args.cmake:
            test_cmake(base, repo, archives, plain, args)
        else:
            print("note: --cmake not given; cmake/DePrebuilt.cmake not tested")

    if selftest.FAILS:
        print(f"\n{len(selftest.FAILS)} case(s) failed", file=sys.stderr)
        return 1
    print("\nvendor prebuilt selftest: all cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
