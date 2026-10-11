# Building DEngine

DEngine builds with CMake presets. Each preset is a platform, a compiler and one of the four
build types from SPEC §3.2. The presets are in `CMakePresets.json` and the toolchain files are in
`cmake/toolchains/`.

## Quick start

```sh
cmake --preset dev            # configure (dev = macos-dev on the macOS build host)
cmake --build --preset dev    # build
ctest --preset dev            # run every test
```

Use the same preset name for the configure, build and test steps. Each preset builds into
`build/<preset>/`.

Some useful variations:

```sh
ctest --preset dev -L unit                      # unit tests only
ctest --preset dev -R <regex>                   # tests whose name matches
cmake --build --preset dev --target de_core     # one target
cmake --preset dev -DDE_SANITIZE=address        # a preset plus one extra cache variable
cmake --list-presets=all                        # the presets this host can use
```

## Prerequisites

Every host needs:

- **CMake 3.28 or newer.**
- **Ninja.** Every preset uses the Ninja generator.
- **Python 3.** The repository checks (`tools/`) use it.
- **Vulkan SDK** (optional). On macOS it enables the Vulkan backend through MoltenVK, for tests
  only. On Linux it is needed for the Vulkan backend.

The other requirements depend on the host:

| Host | You need | Notes |
|---|---|---|
| macOS 14+ (arm64) | Xcode or the Xcode Command Line Tools (`xcode-select --install`); `brew install cmake ninja` | Apple Clang is the compiler. Intel Macs are not supported (DENG-2 Q3). |
| macOS, for the Windows cross build | `brew install llvm lld`, and `xwin` (`cargo install xwin --locked`) | Apple's toolchain has no `clang-cl` or `lld-link`. Homebrew `llvm` gives `clang-cl`, `llvm-lib`, `llvm-rc` and `llvm-mt`. Homebrew `lld` gives `lld-link`. See [Windows from macOS or Linux (xwin)](#windows-from-macos-or-linux-xwin). |
| Linux x86-64 (glibc 2.31+) | `clang`, `lld`, `cmake`, `ninja-build`; `gcc`/`g++` for the GCC presets | Clang is the primary compiler. GCC must also build cleanly (SPEC §3.2). |
| Linux, for the Windows cross build | `clang`, `lld`, `llvm` (for `clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc`, `llvm-mt`), and `xwin` | The toolchain also looks in `/usr/lib/llvm-*/bin`. |
| Windows 10/11 x64 | Visual Studio 2022 (or the Build Tools) with the "Desktop development with C++" workload, its "C++ Clang tools for Windows" component, and the Windows SDK; CMake; Ninja | Run CMake from an **x64 Native Tools / Developer prompt**, because Ninja needs the MSVC environment. |

## Presets

The configure, build and test presets have the same names. A preset is shown only on the host
that can use it (a `condition` on the host system), so `cmake --list-presets` on macOS shows the
`macos-*`, `windows-xwin-*` and `dev` presets.

| Family | Presets | Host | Toolchain | Notes |
|---|---|---|---|---|
| macOS | `macos-debug`, `macos-dev`, `macos-release`, `macos-shipping` | macOS | `macos-arm64.cmake` | Apple Clang, arm64, deployment target 14.0 |
| Linux Clang | `linux-clang-debug`, `linux-clang-dev`, `linux-clang-release`, `linux-clang-shipping` | Linux | `linux-x64-clang.cmake` | primary Linux compiler |
| Linux GCC | `linux-gcc-debug`, `linux-gcc-release` | Linux | `linux-x64-gcc.cmake` | portability check |
| Linux headless | `linux-headless-dev`, `linux-headless-release` | Linux | `linux-x64-clang.cmake` | `DE_HEADLESS=ON` (server) |
| Windows clang-cl | `windows-clang-cl-debug`, `windows-clang-cl-dev`, `windows-clang-cl-release`, `windows-clang-cl-shipping` | Windows | none (`clang-cl` from the VS prompt) | primary Windows compiler |
| Windows MSVC | `windows-msvc-debug`, `windows-msvc-release` | Windows | none (`cl` from the VS prompt) | portability check |
| Windows headless | `windows-headless-release` | Windows | none (`clang-cl`) | `DE_HEADLESS=ON` (server) |
| Windows cross | `windows-xwin-debug`, `windows-xwin-dev` | macOS, Linux | `windows-clang-cl-xwin.cmake` | build only: no test presets, the output is a Windows binary |
| Host alias | `dev` | macOS | `macos-arm64.cmake` | same settings as `macos-dev`, built in `build/dev/` |

`dev` is the host-native Dev preset of the agent build host, which is macOS. On Linux, use
`linux-clang-dev`. On Windows, use `windows-clang-cl-dev`.

### What each build type sets

| Setting | Debug | Dev | Release | Shipping |
|---|---|---|---|---|
| `CMAKE_BUILD_TYPE` | `Debug` | `Dev` | `Release` | `Shipping` |
| Optimized | no | yes | yes | yes |
| Defines (`de_config`) | `DE_DEBUG=1` | `DE_DEV=1`, `DE_PROFILE=1` | `DE_PROFILE=1` (if `DE_ENABLE_PROFILE`) | `DE_SHIPPING=1` |
| `DE_BUILD_EXAMPLES` | `ON` | `ON` | `ON` | `OFF` |
| `DE_BUILD_SHARED` | default (`OFF`) | default (`OFF`) | default (`OFF`) | forced `OFF`: Shipping is a static link (SPEC §3.2) |
| `DE_BUILD_TESTS` | `ON` | `ON` | `ON` | `ON` |

`cmake/DeBuildTypes.cmake` defines the four build types. `cmake/DeOptions.cmake` defines the
options. Every preset uses Ninja, sets `DE_BUILD_TESTS=ON`, and builds into `build/<preset>/`.
The warnings are always `-Wall -Wextra -Wpedantic -Wvla -Werror` (Clang, GCC) or `/W4 /WX`
(MSVC, clang-cl), from `cmake/DeCompiler.cmake`.

### Adding a preset

`CMakePresets.json` is grouped by family: first the hidden bases, then one block per family in
the order of the table above, then `dev`. Add a new family as a new block at the end. Do not
reorder the existing presets. A new preset inherits one OS base (`os-*`), any feature bases
(`headless`), and one build-type base (`cfg-*`), in that order. Its build and test presets
inherit the matching `host-*` base, which repeats the host condition.

## Headless server build

The headless presets set `DE_HEADLESS=ON`. The build then has no renderer, GPU, audio or UI
module (SPEC §4.2): the targets `de_render`, `de_gpu`, `de_gpu_*`, `de_audio` and `de_ui` do not
exist, and C code sees `DE_HEADLESS=1` and no `DE_HAS_RENDER`, `DE_HAS_GPU*`, `DE_HAS_AUDIO` or
`DE_HAS_UI`.

```sh
cmake --preset linux-headless-dev
cmake --build --preset linux-headless-dev
ctest --preset linux-headless-dev
```

On another host, add `-DDE_HEADLESS=ON` to any preset, for example
`cmake --preset macos-dev -B build/macos-headless -DDE_HEADLESS=ON`.

## Cross-compiling

### Windows from macOS or Linux (xwin)

The `windows-xwin-*` presets build the Windows x86-64 code paths (D3D12, Win32, WASAPI) with
`clang-cl`, `lld-link`, `llvm-lib`, `llvm-rc` and `llvm-mt`. The headers and import libraries of
the Windows SDK and the MSVC CRT come from [xwin](https://github.com/Jake-Shadle/xwin).

The Windows SDK and the CRT are licensed by Microsoft. **You** download them and accept the
license. The repository never contains them, and the build never downloads them (CONVENTIONS §6).

1. Install the LLVM tools (see [Prerequisites](#prerequisites)). On macOS:
   `brew install llvm lld`.
2. Install xwin, then download and unpack the SDK and the CRT. The `--accept-license` flag means
   you accept Microsoft's license:

   ```sh
   cargo install xwin --locked
   xwin --accept-license splat --output ~/.xwin
   ```

   Add `--include-debug-libs` if you want the debug CRT (`msvcrtd.lib`) for
   `windows-xwin-debug`. Without it, the toolchain links the release CRT in every config.
3. If the output is not in `~/.xwin`, set `DE_XWIN_DIR`:

   ```sh
   export DE_XWIN_DIR=/path/to/xwin-output
   ```

4. Configure and build:

   ```sh
   cmake --preset windows-xwin-dev
   cmake --build --preset windows-xwin-dev
   file build/windows-xwin-dev/<target>.exe     # PE32+ executable ... x86-64
   ```

The test executables are Windows programs, so there is no `ctest` preset for this family. Run
them on Windows or in CI.

Settings of `cmake/toolchains/windows-clang-cl-xwin.cmake`:

| Variable | Default | Meaning |
|---|---|---|
| `DE_XWIN_DIR` (environment or `-D`) | `~/.xwin` | The xwin output directory. Both the default `splat` layout and the `--use-winsysroot-style` layout work. |
| `DE_LLVM_DIR` (environment or `-D`) | search `PATH`, Homebrew `llvm`/`lld`, `/usr/lib/llvm-*` | An LLVM install whose `bin/` has `clang-cl`. |

If the SDK or a tool is missing, configure stops with a message that names the missing piece and
this section.

### Linux from a sysroot

`cmake/toolchains/linux-sysroot.cmake` cross-compiles to Linux x86-64 with `clang
--target=x86_64-linux-gnu` and `lld`, against a Linux sysroot (SPEC §3.2: Windows host to
Linux). The sysroot must have glibc 2.31 or newer and the development packages the build needs
(for example a Debian 11 or Ubuntu 20.04 root made with `debootstrap`, or the Steam Runtime SDK).

There is no preset for this toolchain. Configure it directly:

```sh
export DE_LINUX_SYSROOT=/path/to/sysroot      # Windows: set DE_LINUX_SYSROOT=C:\sysroots\linux
cmake -S . -B build/linux-sysroot-dev -G Ninja \
      --toolchain cmake/toolchains/linux-sysroot.cmake \
      -DCMAKE_BUILD_TYPE=Dev -DDE_BUILD_EXAMPLES=ON
cmake --build build/linux-sysroot-dev
```

The output is a Linux program. Run the tests on Linux or in CI.

## Options

The options most often set on top of a preset:

| Option | Default | Meaning |
|---|---|---|
| `DE_BUILD_TESTS` | `ON` | Build `tests/`. |
| `DE_BUILD_TOOLS` | `ON` | Build `tools/`. |
| `DE_BUILD_EXAMPLES` | `ON`, `OFF` in Shipping | Build `examples/`. |
| `DE_BUILD_SHARED` | `OFF` | Build `dengine` as a shared library. Forced `OFF` in Shipping. |
| `DE_HEADLESS` | `OFF` | Server build without renderer, GPU, audio or UI. |
| `DE_WITH_<MOD>` | `ON` (`DE_WITH_STEAM`: `OFF`) | Removable modules: `AUDIO`, `NET`, `PHYSICS`, `UI`, `VFX`, `NAV`, `VIDEO`, `STEAM`. |
| `DE_GPU_D3D12`, `DE_GPU_VULKAN`, `DE_GPU_METAL` | per platform | RHI backends. |
| `DE_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX`. Keep it on. |
| `DE_SANITIZE` | empty | `address`, `undefined`, `thread`, or a comma list such as `address,undefined` (`address` and `thread` do not combine; MSVC and clang-cl: `address` only). The `ci-*` presets use it, see `docs/ci.md`. |

`cmake --build --preset <name> --target de_print_config` prints the resolved configuration of a
build directory (from `build/<preset>/de_config_dump.txt`).
