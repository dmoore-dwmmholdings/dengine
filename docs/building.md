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

Run the repository checks before you ask for a review:

```sh
ctest --preset dev -L check
```

| CTest entry | What it checks |
|---|---|
| `header_check` | Builds the target `de_header_check`: every `include/dengine/*.h` compiles on its own as C11 and as C++17. It is also part of the default build. |
| `layering_check` | `tools/check_layers/check_layers.py`: every `#include` in `src/`, `tools/cook/lib/` and `include/dengine/` follows the module layers in `tools/check_layers/layers.toml` (SPEC §4.1). It writes the module graph to `build/<preset>/tools/check_layers/layers.dot`. |
| `layering_selftest` | The layering checker on its own fixture trees. |
| `vendor_selftest` | `tools/vendor/vendor.py` on a local fake archive: fetch, excludes, patches and verify. No network. It needs `git` on `PATH` (`git apply` applies the patches). |
| `deterministic_asm_check` | Builds the deterministic targets, then disassembles every object of a `DETERMINISTIC` module and every file passed to `de_mark_deterministic_sources()`, and fails on an FMA or approximate-reciprocal instruction (SPEC §5.7). It needs `llvm-objdump` or `objdump` at configure time; without one it is reported as skipped. It runs serially, because it drives the build. |
| `check_determinism_src` | `tools/check_determinism_src.py` over the deterministic code listed in `tools/determinism_modules.toml`: no `<math.h>` or libm calls (use `de_dmath`), no approximate-reciprocal intrinsics, no fast-math pragmas or flags, no fast `de_v3_*`-style math, no hash-map iteration, no `rand()` (SPEC §5.7). It also checks `compile_commands.json` (every translation unit of those modules is compiled with `-ffp-contract=off`, `/clang:-ffp-contract=off` or `/fp:precise`) and that the table equals the `DETERMINISTIC` modules and `de_mark_deterministic_sources()` files of the build. A new deterministic module or file needs an entry in the table. |
| `check_determinism_src_selftest` | The source checker on planted violations in a temporary tree. |
| `check_det_registered` | `tools/check_det_registered.py`: every de_test case (`TEST`, `TEST_F`, and registration macros such as `DE_PHYS_DET_REGISTER`) of a `tests/determinism/` source that the build compiles has a CTest entry in its directory whose `--filter=` selects it, and every such entry selects a case. A case left out of `de_det_add_tests()` would never run. It reads `compile_commands.json` (and evaluates `#if` blocks with the file's `-D` definitions) and the `CTestTestfile.cmake` files, so it needs no build. |
| `check_det_registered_selftest` | The registration checker on planted trees: a left-out entry, a stale entry, `#if` blocks, macro expansion. |

CI runs the same entries (`docs/ci.md`).

Some useful variations:

```sh
ctest --preset dev -L unit                      # unit tests only
ctest --preset dev -L check                     # the repository checks (see below)
ctest --preset dev -R <regex>                   # tests whose name matches
cmake --build --preset dev --target de_core     # one target
cmake --preset dev -DDE_SANITIZE=address        # a preset plus one extra cache variable
cmake --list-presets=all                        # the presets this host can use
```

## Prerequisites

Every host needs:

- **CMake 3.28 or newer.**
- **Ninja.** Every preset uses the Ninja generator.
- **Python 3.10 or newer.** Configure stops without it: the repository checks in `tools/`
  (`check_layers`, `vendor`) are CTest entries and need it (`find_package(Python3 3.10
  REQUIRED)`). Python 3.11 or newer is better: the tools then read TOML with `tomllib` instead of
  their own minimal reader. CI uses 3.11.
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
| Linux GCC | `linux-gcc-debug`, `linux-gcc-dev`, `linux-gcc-release` | Linux | `linux-x64-gcc.cmake` | portability check |
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

Debug information with MSVC and clang-cl: Debug and Dev use `/Zi` (one `.pdb` per target that
every compile writes into). When a compiler launcher is set (`CMAKE_C_COMPILER_LAUNCHER` or
`CMAKE_CXX_COMPILER_LAUNCHER`, as a cache variable or an environment variable), they use `/Z7`
instead: the debug info goes into each `.obj`, so a compiler cache such as sccache can cache the
compile. The linker writes the `.pdb` in both cases. To choose yourself, pass
`-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=<value>` (`cmake/DeBuildTypes.cmake`).

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

The toolchain restricts pkg-config searches to the sysroot and clears host
`PKG_CONFIG_PATH` entries and pkg-config's automatic host `CMAKE_PREFIX_PATH` lookup.
On Windows it also disables automatic package-prefix rewriting: Linux `.pc` files
keep their declared prefixes before pkg-config applies the target sysroot.
Target `.pc` files belong under the sysroot's normal
`usr/lib/<triple>/pkgconfig`, `usr/lib/pkgconfig`, `usr/lib64/pkgconfig`, or
`usr/share/pkgconfig` directories (the corresponding `lib` and `usr/local` directories
are included too).

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
| `DE_GPU_D3D12`, `DE_GPU_VULKAN`, `DE_GPU_METAL` | per platform; `DE_GPU_VULKAN` ON everywhere | RHI backends. Vulkan needs no SDK to build (vendored headers; libvulkan is opened at run time, `DE_VULKAN_LIBRARY` overrides which library). |
| `DE_WARNINGS_AS_ERRORS` | `ON` | `-Werror` / `/WX`. Keep it on. |
| `DE_ENABLE_PROFILE` | `ON` | Define `DE_PROFILE=1` (profiler zones) in Release. Debug never defines it, Dev always does, Shipping never does. |
| `DE_TRACY` | `ON` | Use the vendored Tracy client (`third_party/tracy/`) as the profiler backend of the builds that define `DE_PROFILE`. `OFF` keeps the profiling calls but links no backend (`de_prof_init` returns `DE_ERR_UNSUPPORTED`) and no C++. Debug and Shipping never build Tracy. |
| `DE_FUZZ_LIBFUZZER` | `ON` | Build the fuzz targets (`tests/fuzz/`) with libFuzzer (`-fsanitize=fuzzer,address`) when the compiler supports it: upstream or Homebrew Clang (not Apple Clang, not clang-cl), and not with `thread` in `DE_SANITIZE`. Otherwise, or when `OFF`, each target links a standalone driver that only replays its corpus. Either way `ctest -L fuzz` replays the corpora. The cache variable `DE_HAVE_LIBFUZZER` shows which build you got. |
| `DE_SANITIZE` | empty | `address`, `undefined`, `thread`, or a comma list such as `address,undefined` (`address` and `thread` do not combine; MSVC and clang-cl: `address` only). The `ci-*` presets use it, see `docs/ci.md`. |

`cmake --build --preset <name> --target de_print_config` prints the resolved configuration of a
build directory (from `build/<preset>/de_config_dump.txt`).

## A game in a separate repository

Pin the engine source as a submodule (for example `engine/`). There is currently no installed
`find_package` SDK; source integration uses the engine's CMake helpers:

```cmake
cmake_minimum_required(VERSION 3.28)
project(my_game LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(DE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(DE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(DE_BUILD_TOOLS ON CACHE BOOL "" FORCE) # source asset cooking in Debug/Dev
add_subdirectory(engine)
de_add_game(NAME my_game SOURCES game.c GAME_TOML game.toml)
```

Configure with Ninja and `-DCMAKE_BUILD_TYPE=Dev`, then build `my_game` and `dengine_exe`.
`game.c` exports `DE_GAME_EXPORT const de_game_api *de_game_get_api(void)` with
`api_version = DE_GAME_API_VERSION`, `init`, `register_types`, `register_systems` and optional
shutdown/reload callbacks. Public headers are under `<dengine/...>`; a game does not need `src/` includes.
`de_add_game` copies `game.toml` beside the game binary. It does not copy the manifest or sources.
Run the built host with `--game <path-to-game-library> --dev-dir <game-source-directory>` and load
`dev:/assets.toml` explicitly in `init`. `run_my_game` also starts the host, but does not supply
`--dev-dir`; stage your data next to the binary and use `game:/`, or mount its source directory.
On macOS the library is normally `build/my_game/libmy_game.dylib`, and the host is
`build/engine/src/host/dengine`. Windows static-engine builds and Shipping build a game executable
instead of a dynamic game library. Shipping uses cooked `.dpak` files beside that executable;
`--dev-dir` is ignored there.

The host provides the window, tasks, ECS world/types, input, assets and text. For a rendered game,
create a device with `de_gpu_device_create` and assign `e->gpu`; create a renderer with that device,
`e->window`, `e->assets` and `e->types`, assign `e->renderer`, then call `de_renderer_attach`.
These non-NULL assignments transfer final teardown ownership to the host. Assign immediately
when creation succeeds so failed `init` also unwinds them. Optional UI follows the same rule
(`de_ui_create`, assign `e->ui`, `de_ui_attach`). The host creates none of these optional objects
automatically. If you destroy an assigned object yourself, obey its dependency requirements and
set the corresponding field to NULL. In particular, assets must unload before their renderer.
Do not destroy a renderer in ordinary game shutdown while `e->assets` still owns render assets.
The host waits for the final render frame, calls game shutdown, then releases its modules and
only afterward unloads the game library.

Register render/game component types before loading scene assets. Use a manifest containing your
models/materials/textures, font and scene; poll `de_asset_status` until READY, then instantiate
with `de_asset_scene`/`de_model_prefab` and `de_prefab_instantiate` on the main thread outside a
running ECS schedule. A 3D scene needs a transformed `DeCamera`, mesh renderers and lighting.
`de_component_find(world, "DeCamera")` and the other public ECS calls avoid private render IDs.
Add your movement/camera systems and read `de_key_down`/`de_key_pressed`, or load/push action maps
on `e->input`; the host already pumps input and runs Update/FixedUpdate/Extract.
For a text HUD, load a font, call `de_text_attach`, build a public `de_text_layout`, obtain its
quads with `de_text_layout_quads`, and submit `de_text_draw` during a main-thread game system.
Keep game state and load handles alive until shutdown, release game-owned loads/layouts there,
and implement reload callbacks when enabling development hot reload. See the public-only
`tests/unit/host/fixtures/owned_game.c` for module creation and host-owned cleanup.

## Engine and IDE workspace

The engine and [DIDE](https://github.com/dmoore-dwmmholdings/dide) remain separate repositories.
Keep them together as `engine/` and `dide/` under one workspace folder, with both on `main`.
From `engine/`, build the development host and the IDE example:

```sh
cmake --preset dev
cmake --build --preset dev
```

From `dide/`, run `DIDE_DENGINE_DIR=../engine cargo xtask check --no-audit` to check the IDE
against this engine checkout, and `pnpm tauri dev` to start it. Open
`engine/examples/flappy/Flappy.dproj` in DIDE; its build presets select the host platform.
The editor IPC protocol is vendored into the engine, so engine builds do not require DIDE.

The incoming editor service shares viewport textures through D3D12 on Windows. Its macOS
service supports IPC and scene editing but does not advertise `views.sharedTexture`; a real
engine viewport on macOS still requires the Metal/IOSurface bridge (DENG-556).

The October 2026 integration builds natively on macOS, including the Flappy executable and
the embedded-frontend IDE. UI unit tests, focused engine integration checks, Rust workspace
linting, and the IDE dependency license checks pass. The wider suites are not green: Metal
golden tests timed out, engine performance budgets failed, and IDE tests expose additional
platform and mock-service issues. The real-engine IPC conformance run reports 12 passed,
8 failed, and 2 skipped; remaining failures include snapshot diffs, subscription limits,
disconnect handling, round-trip timing, and shutdown. Run the suite from `dide/` with:

```sh
cargo run --locked -p dide-conformance -- --json -- ../engine/build/dev/examples/flappy/flappy/flappy
```

The protocol archive is pinned to a private DIDE commit. To verify it with an authenticated
GitHub CLI session, from `engine/`:

```sh
mkdir -p build/vendor-cache
gh api repos/dmoore-dwmmholdings/dide/tarball/93a5ab93bed58b09b0b722e975edb7a2ec3fc972 \
  > build/vendor-cache/dide_protocol-202de0d26765dbe0-93a5ab93bed58b09b0b722e975edb7a2ec3fc972
python3 tools/vendor/vendor.py --cache build/vendor-cache verify dide_protocol --offline
```
