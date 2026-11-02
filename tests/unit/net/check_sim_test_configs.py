"""Exercise real test registration/discovery without building engine dependencies."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def run(args: list[str], *, expected_failure: bool = False) -> str:
    result = subprocess.run(args, capture_output=True, text=True)
    output = result.stdout + result.stderr
    if expected_failure:
        assert result.returncode != 0 and "defines no tests" in output, output
    elif result.returncode:
        raise RuntimeError(f"{' '.join(args)}\n{output}")
    return result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path)
    parser.add_argument("--generator", action="append")
    args = parser.parse_args()
    root = args.root.resolve()
    registration = (root / "tests/unit/net/CMakeLists.txt").read_text()
    registration = registration[
        registration.index("# DENG-56:") : registration.index("# DENG-53:")
    ]
    with tempfile.TemporaryDirectory(prefix="de-net-test-configs-") as scratch:
        fixture = Path(scratch)
        (fixture / "registration.cmake").write_text(registration)
        # Ad-hoc signing is only for these temporary Xcode command-line probes.
        (fixture / "CMakeLists.txt").write_text(
            f'''cmake_minimum_required(VERSION 3.28)
# project() runs compiler ABI try_compile before DeBuildTypes is included.
# Apply its launcher-compatible debug format early, including inherited sccache.
if(NOT DEFINED CMAKE_MSVC_DEBUG_INFORMATION_FORMAT)
    foreach(_lang C CXX)
        if(CMAKE_${{_lang}}_COMPILER_LAUNCHER
           OR NOT "$ENV{{CMAKE_${{_lang}}_COMPILER_LAUNCHER}}" STREQUAL "")
            set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT
                "$<$<CONFIG:Debug,Dev,RelWithDebInfo>:Embedded>")
        endif()
    endforeach()
endif()
project(test_configuration_probe C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "-")
set(DE_ROOT_DIR "${{CMAKE_CURRENT_SOURCE_DIR}}")
set(DE_BUILD_TESTS ON)
include([==[{root.as_posix()}/cmake/DeBuildTypes.cmake]==])
add_library(de_build_flags INTERFACE)
add_library(de_test_framework INTERFACE)
add_library(de_test_support INTERFACE)
add_library(de_net INTERFACE)
target_link_libraries(de_net INTERFACE de_config)
if(PROBE_CROSS)
    set(CMAKE_CROSSCOMPILING TRUE)
elseif(CMAKE_GENERATOR STREQUAL "Xcode")
    # Xcode signs after POST_BUILD, but discovery executes during POST_BUILD.
    # Sign only temporary fixture binaries before their first execution.
    set(CMAKE_CROSSCOMPILING_EMULATOR "{Path(sys.executable).as_posix()};${{CMAKE_CURRENT_SOURCE_DIR}}/launch.py")
endif()
enable_testing()
include([==[{root.as_posix()}/cmake/DeTesting.cmake]==])
if(PROBE_EMPTY)
    if(PROBE_UNRESTRICTED)
        de_add_test(NAME empty SOURCES empty.c)
    else()
        de_add_test(NAME empty SOURCES empty.c CONFIGURATIONS Debug Dev)
    endif()
else()
    de_add_test(NAME control_before SOURCES control.c)
    include(registration.cmake)
    de_add_test(NAME control_after SOURCES control.c)
endif()
'''
        )
        (fixture / "control.c").write_text(
            '#include <stdio.h>\n#include <string.h>\n'
            'int main(int argc,char **argv){if(argc==2&&!strcmp(argv[1],"--list"))'
            'puts("control.available");return 0;}\n'
        )
        (fixture / "test_net_sim.c").write_text(
            '#include <stdio.h>\n#include <string.h>\n'
            'int main(int argc,char **argv){if(argc==2&&!strcmp(argv[1],"--list")){\n'
            '#if defined(DE_DEV) || defined(DE_DEBUG)\nputs("net_sim.active");\n'
            '#endif\n}return 0;}\n'
        )
        (fixture / "empty.c").write_text('int main(void){return 0;}\n')
        (fixture / "launch.py").write_text(
            'import subprocess,sys\n'
            'subprocess.run(["/usr/bin/codesign","--force","--sign","-",sys.argv[1]],check=True)\n'
            'raise SystemExit(subprocess.run(sys.argv[1:]).returncode)\n'
        )
        cases = 0
        for generator in args.generator or ["Ninja", "Ninja Multi-Config"]:
            multi = generator != "Ninja"
            for cross in (False, True):
                for config in ("Release", "Debug", "Shipping", "Dev"):
                    directory = fixture / f"{generator}-{cross}-{'' if multi else config}"
                    run([
                        "cmake", "-S", str(fixture), "-B", str(directory), "-G", generator,
                        f"-DCMAKE_BUILD_TYPE={config}", f"-DPROBE_CROSS={'ON' if cross else 'OFF'}",
                    ])
                    before = json.loads(run([
                        "ctest", "--test-dir", str(directory), "-C", config,
                        "--show-only=json-v1",
                    ]))["tests"]
                    active = config in ("Debug", "Dev")
                    if not active:
                        assert not any(t["name"].startswith("net_sim") for t in before), before
                    run(["cmake", "--build", str(directory), "--config", config])
                    tests = json.loads(run([
                        "ctest", "--test-dir", str(directory), "-C", config,
                        "--show-only=json-v1",
                    ]))["tests"]
                    names = sorted(t["name"] for t in tests)
                    expected = (["control_after", "control_before"] if cross else [
                        "control_after.control.available", "control_before.control.available",
                    ])
                    if active:
                        expected.append("net_sim" if cross else "net_sim.active")
                    assert names == sorted(expected), (generator, config, cross, names)
                    run(["ctest", "--test-dir", str(directory), "-C", config,
                         "--output-on-failure"])
                    if generator == "Ninja" and not active:
                        targets = run(["cmake", "--build", str(directory), "--target", "help"])
                        assert not any(line.startswith("test_net_sim:")
                                       for line in targets.splitlines()), targets
                    cases += 1
        # Explicitly eligible empty fixtures and legacy unrestricted callers must still fail.
        for config, unrestricted in (("Debug", False), ("Release", True)):
            directory = fixture / f"empty-{config}"
            run(["cmake", "-S", str(fixture), "-B", str(directory), "-G", "Ninja",
                 f"-DCMAKE_BUILD_TYPE={config}", "-DPROBE_EMPTY=ON",
                 f"-DPROBE_UNRESTRICTED={'ON' if unrestricted else 'OFF'}"])
            run(["cmake", "--build", str(directory)], expected_failure=True)
        print(f"{cases} configuration/registration cases and 2 empty-discovery controls passed")


if __name__ == "__main__":
    main()
