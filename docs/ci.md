# Continuous integration

DEngine uses GitHub Actions. The workflows live in `.github/workflows/`. `ci.yml`,
`sanitizers.yml` and `checks.yml` run on a push to `main`, `dev` or `sprint/**`, on every pull
request, and by hand (`workflow_dispatch`). `determinism.yml` runs on a push to `main`, `dev` or
`sprint/**` and by hand, not on pull requests. Each workflow has one concurrency group per branch,
so a new run on a branch cancels the run that it replaces (also a second `gh workflow run` on the
same branch). `dev` and `main` are the exception: a new run there waits for the running one to
finish instead of cancelling it, and only the newest waiting run is kept. Merges land on `dev`
faster than a full run takes, so cancelling left `dev` without a finished result.

`ui-m1.yml` qualifies the original static HUD workload separately from the general
matrix. It verifies native Apple M1 (including M1 Virtual) arm64 hardware, builds
`test_ui_draw` with the `macos-dev` preset, and requires exactly one actual passing
`ui_draw.two_thousand_widgets_static_zero_alloc_build_extract` case. The original
2,000 widgets, eight warm-up frames, 1,000 measured frames, zero-allocation guard,
0.5 ms build+Extract and 2.5 ms whole-frame budgets remain in the test. Hardware,
configuration, JUnit and test logs are uploaded as `ui-m1-acceptance`. Relevant UI/ECS
pull requests trigger the workflow; it can also be dispatched by hand. A queued job
or a result on another CPU does not qualify M1 performance.

## Tiers: quick and full

Workers run CI on their `feature/**` branch before review (`docs/agents/WORKER.md`), up to six
branches at a time, so the full matrix (17 build and sanitizer jobs, 6 of them on Windows) kept the
runners queued. `ci.yml` and `sanitizers.yml` therefore have two tiers:

| Tier | When | Jobs |
|---|---|---|
| quick | a run on a `feature/**` branch | windows msvc Debug, linux gcc Dev, headless linux Release, linux clang ASan+UBSan, linux clang TSan |
| full | every other run: `dev`, `main`, `sprint/**`, pull requests | every job in the tables below, the libFuzzer smoke run included |

- A first `plan` job picks the tier and builds the matrix, so the jobs are listed once, in the
  `plan` step, each with `"quick": true` if it is in the quick tier.
- `gh workflow run CI --ref <branch> -f tier=full` (or `quick`) overrides the choice.
- The quick jobs are the ones that caught most branch failures: MSVC warnings, GCC flow warnings
  that only fire in optimized builds, the build without GPU and window modules, Linux-only UB and
  data races. Workers also run `ci-macos-asan` and the `windows-xwin-dev` cross-build on the
  host before they push.
- What the quick tier misses (clang-cl, MSVC Release, Shipping, GCC TSan, macOS jobs, headless Windows,
  the libFuzzer smoke run) is caught by the full run on `dev` after the merge; the orchestrator
  files a fix task for it.
- `checks.yml` is small (three Ubuntu jobs) and always runs in full. `determinism.yml` (five
  platform jobs, two cook jobs and a diff job) has no tiers either.

Every build job runs a preset from `CMakePresets.json`. To reproduce a job locally, run the same
preset on a host of the same OS (see `docs/building.md`):

```sh
cmake --preset <preset>
cmake --build --preset <preset>
ctest --preset <preset> --output-on-failure
```

## Jobs

### `ci.yml`: build and unit tests

The compiler × build type matrix of SPEC §19.3, plus the headless server builds. Each job runs
configure, build and `ctest` for one preset.

| Job | Runner | Preset |
|---|---|---|
| windows clang-cl Debug / Release | `windows-2022` | `windows-clang-cl-debug`, `windows-clang-cl-release` |
| windows msvc Debug / Release | `windows-2022` | `windows-msvc-debug`, `windows-msvc-release` |
| linux clang Debug / Release | `ubuntu-24.04` | `linux-clang-debug`, `linux-clang-release` |
| linux gcc Debug / Dev / Release | `ubuntu-24.04` | `linux-gcc-debug`, `linux-gcc-dev`, `linux-gcc-release` |
| macos apple-clang Debug / Release | `macos-14` | `macos-debug`, `macos-release` |
| windows clang-cl / linux clang / macos apple-clang Shipping | `windows-2022`, `ubuntu-24.04`, `macos-14` | `windows-clang-cl-shipping`, `linux-clang-shipping`, `macos-shipping` |
| headless linux Release | `ubuntu-24.04` | `linux-headless-release` |
| headless windows Release | `windows-2022` | `windows-headless-release` |

The Shipping jobs build the `DE_SHIPPING` configuration (packed assets only, no cooker, no
`dev:/`; SPEC §3) and are in the full tier only. Tests that need source mode or the cooker
skip themselves there; a test that fails only in Shipping is a real failure.

The configure step prints `build/<preset>/de_config_dump.txt`. If a job fails, it uploads the
artifact `logs-<preset>` (kept for 7 days). The artifact holds `Testing/Temporary/`
(`LastTest.log`), `CMakeConfigureLog.yaml` and the config dump.

**Crashing tests (DENG-418).** CTest starts tests with Windows Error Reporting's crash dialog
off, so Windows writes no crash dump for them. A de_test executable that crashes prints the
exception and the crashing thread's stack itself on stderr: module, symbol, offset, and file
and line where the build's PDB has them (`tests/unit/de_test.c`). `--output-on-failure` shows it
in the job log. On Windows it also writes a minidump to `$DE_TEST_DUMP_DIR/<exe>.<pid>.dmp`. The
Test step sets `DE_TEST_DUMP_DIR` to `build/<preset>/test-dumps/`, and `logs-<preset>` uploads
that directory too. Open a dump with the job's build of the same commit (`cdb -z <dmp> -y
<dir of the test's .pdb>`, then `.ecxr; kn`). The dumps hold thread stacks and the memory they
reference, not module images.

The golden-image jobs (S10) are below; self-hosted runners come in S20 and the ABI check in S37.
The cross-architecture determinism jobs are in `determinism.yml` (below).

**D3D12 tests on WARP.** The hosted runners have no GPU, so the D3D12 tests (`gpu_d3d12.*`) run
on WARP, the software adapter. They do not use the runner image's WARP, because it has shader
model 6.2, no enhanced barriers and no mesh shaders. Instead, the Windows jobs that build tests
download a newer WARP (`Microsoft.Direct3D.WARP`, the test-only prebuilt entry `d3d12_warp` in
`third_party/vendor.toml`, DENG-405) at configure time, through the vendor cache. The build copies
`d3d10warp.dll` next to `test_gpu_d3d12.exe`. This WARP has the Base tier (shader model 6.6,
binding tier 3), enhanced barriers and mesh shaders. So the pipeline, draw, mesh and
pipeline-library tests run on CI, and so does the enhanced-barrier path. `d3d12.vendored_warp`
fails if the WARP in the test process is not the vendored one. Its licence forbids
redistribution, so no job uploads it: the `logs-<preset>` artifact holds no binaries. To run the
tests on the runner image's own WARP, configure with `-DDE_D3D12_TEST_WARP=OFF`. The quick tier
has `windows msvc Debug`, so it also runs these tests.

**The Agility runtime (DENG-423).** WARP has shader model 6.6 only on the Agility D3D12 runtime
(`<exe dir>/D3D12/`), which the loader uses only for an executable that exports
`D3D12SDKVersion`/`D3D12SDKPath`. `de_d3d12_agility_deploy()` copies the runtime and adds the
exports to its target (unless a source includes `gpu/d3d12/d3d12_agility_exports.h` already), and
the check `agility_exports_check` (label `check`, Windows) reads the export table of every deployed
executable. The render tests that create D3D12 devices log their runtime and WARP and, on the
vendored WARP, require the Agility runtime and the Base tier (`tests/unit/render/render_d3d12_test.c`).

#### Golden images (`golden` job, full tier only)

DENG-339 adds `test_render_ssr` / `render_ssr.*` to the explicit full-tier lavapipe and
WARP selections: the tracing, mirror reference, exposure, HDR convergence, edge/fallback,
forward transparency and history lifecycle cases must execute on the supported backend.
Native Metal verification runs on the development host.

The golden-image harness (`docs/testing/golden.md`, DENG-329) renders the scenes of
`tests/golden/` and compares them with the references in `tests/golden/refs/` by LDR-FLIP. The
`golden` job is a matrix of three entries. It runs only when the run is in the full tier: its
`if:` repeats the tier choice of the `plan` job (`-f tier=full`, or a run outside `feature/**`).

| Job | Runner | Preset | Backend |
|---|---|---|---|
| golden linux vulkan (lavapipe) | `ubuntu-24.04` | `linux-clang-debug` | Vulkan on Mesa lavapipe (`mesa-vulkan-drivers`, with `vulkan-validationlayers`; `VK_DRIVER_FILES` and `VK_ICD_FILENAMES` name only the lavapipe ICD) |
| golden windows d3d12 (WARP) | `windows-2022` | `windows-clang-cl-debug` | D3D12 on the vendored WARP (`--warp`), with the debug layer |
| golden macos metal (hosted, skips below Base) | `macos-14` | `macos-debug` | Metal; every scene reports SKIPPED, because the runner's paravirtual device is below the Base tier |

- Each job builds only `de_golden`, `test_flip` and `hello_triangle` (and `hello_triangle_warp`
  on Windows), then runs the CTests `flip.*`, `golden.*` and its backend's `example_hello_*`
  (DENG-327). So the Vulkan backend runs on CI here, on lavapipe. The Linux entry also builds
  `test_render_loaders` and runs `render_loaders.*` (DENG-105): asset uploads on the copy queue,
  the hand-over to the graphics queue and byte-exact readback, with the validation layers. It
  also builds `test_render_graph_gpu` (`render_graph_gpu.*`, DENG-414) and `test_render_gpu_scene`
  (`render_gpu_scene.*`, DENG-177: the GPU scene tables uploaded and read back every frame),
  `test_render_vis` (`render_vis.*`, DENG-229: GPU instance culling compared with its CPU
  reference), `test_render_meshlet_cull` (`render_meshlet_cull.*`, DENG-231: GPU meshlet
  culling compared with its CPU reference and the triangles), `test_render_occlusion`
  (`render_occlusion.*`, DENG-233: the HZB and two-phase occlusion culling),
  `test_render_raster_mesh` (`render_raster_mesh.*`, DENG-234: the mesh-shader vis-buffer raster
  compared with a CPU rasterizer), `test_render_raster_compute` (`render_raster_compute.*`,
  DENG-235: the compute-expansion vis-buffer raster, the path these software rasterizers run,
  compared with a CPU rasterizer), `test_render_raster_alpha` (`render_raster_alpha.*`,
  DENG-236: alpha-tested meshlets on both raster paths, a cutout compared with a CPU rasterizer,
  and a cutout occluder in the HZB), `test_render_classify` (`render_classify.*`, DENG-237:
  material classification of synthetic vis buffers compared with a CPU reference), and
  `test_render_picking` (`render_picking.*`, DENG-239: entities and triangles read back from the
  vis buffer under a pixel, on both raster paths), and `test_render_resolve` (`render_resolve.*`,
  DENG-238: the material resolve's positions, UVs, texture LOD, motion vectors and feedback
  against CPU and forward-rendered references), and `test_render_clustered_lights`
  (`render_clustered_lights.*`, DENG-303: the clustered light lists and the deferred lighting pass
  against CPU references: light units, spot cones, ranges, area lights against a numerical form
  factor, emissive proxies, the diffuse / specular split, the cluster sets), and
  `test_render_ibl_exposure` (`render_ibl_exposure.*`, DENG-304: image-based lighting against
  white-furnace and rotation references, the strip copy of a BC6H environment cube, EV100 exposure
  and the output pass), and `test_render_sss` (`render_sss.*`, DENG-306: separable Burley
  subsurface scattering against the same frame with it off: unflagged pixels bitwise, no bleeding
  across a depth edge, a constant world-space radius at two distances). The Windows entry
  also builds those and
  runs their `_d3d12` tests with
  `ctest -V`, so its log shows the D3D12 runtime and the WARP each one ran on (DENG-423). The
  Linux entry also builds `test_gpu_vulkan` and runs `gpu_vulkan.*` (DENG-426): the Vulkan
  backend's own device, resource, command, sync and pipeline tests, with validation and
  synchronization validation. lavapipe has one queue, so it also covers de_gpu queues that share
  a VkQueue; `swapchain_present_resize` skips there (no display).
- The Linux and Windows entries then run `de-golden ... --scene rhi_clear` and require exit 0.
  Without that step a backend that skipped every scene would leave the job green.
- The jobs restore the compiler cache of the build job with the same preset and do not save it.
- On a failure, the artifact `golden-<backend>` (14 days) holds `build/<preset>/golden/` (each
  scene's image; for a failed comparison also the reference and the FLIP heat map, in the layout
  of `tests/golden/refs/`) and `Testing/Temporary/`. It holds PNG files and logs only, never the
  WARP DLL.
- References are never written on CI: `de-golden --update-refs` refuses to run when `CI` is set.
  Take a reviewed image from the artifact instead (`docs/testing/golden.md`).
- The Windows build jobs above also run `golden.d3d12.*` on WARP, since they deploy the vendored
  WARP next to `de-golden` too. Linux build jobs have no Vulkan driver, so `golden.vulkan.*` skips
  there.

### `sanitizers.yml`: sanitizers

| Job | Runner | Preset | `DE_SANITIZE` |
|---|---|---|---|
| linux clang ASan+UBSan | `ubuntu-24.04` | `ci-linux-asan` | `address,undefined` |
| linux clang TSan | `ubuntu-24.04` | `ci-linux-tsan` | `thread` |
| macos apple-clang ASan+UBSan | `macos-14` | `ci-macos-asan` | `address,undefined` |
| linux gcc TSan | `ubuntu-24.04` | `ci-linux-gcc-tsan` | `thread` |

- GCC TSan catches a different class of error from Clang TSan: GCC rejects some atomics under
  `-fsanitize=thread` (`-Wtsan`, an error with `-Werror`), see `src/platform/de_atomic.h`.
- The `ci-*` presets are Debug builds. `DE_SANITIZE` instruments every target, third-party code
  included. With `undefined`, a UB report stops the test (`-fno-sanitize-recover=undefined`).
- The sanitizer options are in the `ci-*` test presets, so `ctest --preset ci-linux-asan` uses
  them locally too:
  - `ASAN_OPTIONS=strict_string_checks=1:detect_stack_use_after_return=1:check_initialization_order=1:strict_init_order=1`
  - `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`
  - `TSAN_OPTIONS=halt_on_error=1`
- A job step checks `build/<preset>/compile_commands.json` for the expected `-fsanitize=` flag
  and fails if it is missing. This makes sure that a sanitizer job cannot pass without the
  sanitizer.
- On Linux, the job runs `sudo sysctl -w vm.mmap_rnd_bits=28` first. The Ubuntu 24.04 kernel
  uses more mmap randomness than the TSan runtime can handle ("unexpected memory mapping"). If
  you see that error on your own Linux machine, run the same command.
- On macOS, the ASan+UBSan job runs locally with `cmake --preset ci-macos-asan`, then the
  build and test commands above.

#### Fuzz smoke (full tier only)

| Job | Runner | Preset |
|---|---|---|
| linux clang libFuzzer smoke (ASan+UBSan) | `ubuntu-24.04` | `ci-linux-asan` with `-DDE_FUZZ_LIBFUZZER=ON` and `-fsanitize=fuzzer-no-link` |

The `sanitize` jobs only replay each fuzz target's corpus (the CTest entries `fuzz.<name>`). This
job fuzzes for real (SPEC §19.1, DENG-216):

- `-fsanitize=fuzzer-no-link` in `CMAKE_C_FLAGS` and `CMAKE_CXX_FLAGS` instruments every library,
  so libFuzzer sees coverage inside the parsers, not only in the fuzz target's own file. The
  configure step fails if the targets did not get libFuzzer (`DE_FUZZ_USES_LIBFUZZER`).
- It lists the targets from CTest (label `fuzz`, without `selftest`: the harness self test has a
  crash input on purpose), builds only those, and runs each for 60 s (`FUZZ_SECONDS`) from its
  cached corpus `fuzz-corpus/<test name>/` (below) plus its checked-in corpus. A target registered
  with `de_add_fuzz_target(... DICT <file>)` also gets its libFuzzer dictionary (`-dict=<file>`,
  taken from the CTest command; DENG-106).
- **Cached corpus** (DENG-395). `fuzz-corpus/` is kept between runs with `actions/cache`, so each
  run starts where the last one stopped and coverage grows:
  - libFuzzer writes the new inputs to `fuzz-corpus/<test name>/`. After a clean run,
    `-merge=1` keeps only the inputs of that directory that add coverage, so the cache stays
    small. A failed merge keeps the corpus unmerged and only warns.
  - A cache entry cannot be overwritten. Each run saves under a new key,
    `fuzz-corpus-<ref name>-<run id>-<attempt>`, and the next run restores the newest entry whose
    key starts with `fuzz-corpus-<ref name>-`, else `fuzz-corpus-`. GitHub lets a run read the
    caches of its own branch, of a pull request's base branch, and of the default branch (`main`),
    so a `feature/**` branch starts empty unless `main` has an entry; `dev` runs start from the
    previous `dev` run.
  - The corpus is saved also when a target finds something: the failing input goes to
    `fuzz-findings/`, not into the corpus. Pull request runs restore but do not save. A cancelled
    run saves nothing.
  - The corpora of targets that no longer exist are dropped before fuzzing.
  - The job summary names the restored cache key and, per target, the corpus size as restored,
    after fuzzing and after the merge.
  - The checked-in seed corpora (`tests/fuzz/<module>/corpus/`) never change in CI. To keep an
    input for good (a regression), commit it as described below.
  - To start from scratch, delete the `fuzz-corpus-*` entries under the repository's Actions →
    Caches, or list them with `gh cache list --key fuzz-corpus-` and delete each with
    `gh cache delete <key>`.
- Limits: `-timeout=10` (seconds per input), `-malloc_limit_mb=256`, `-rss_limit_mb=2048`. ASan
  runs with `detect_leaks=1`.
- A crash, leak, timeout or oversized allocation fails the job. The inputs that caused it and the
  logs are uploaded as `fuzz-findings` (kept for 14 days). The job summary has a table of the
  targets with their run counts and corpus sizes.
- When a run finds a bug: fix the parser, and add the input to the target's corpus as
  `regress_<what>` (`tests/fuzz/asset/corpus/dpak_reader/` has an example). The importer targets
  (`tests/fuzz/import/`) keep theirs in `tests/fuzz/import/regressions/<target>/` instead, replayed
  by the CTest `import_fuzz_regressions.replay.<target>`, so the seed corpora stay the fixtures.
- New fuzz targets need no change here.
- Local equivalent on macOS (Apple Clang has no libFuzzer; use Homebrew LLVM):

  ```sh
  cmake -S . -B build/fuzz -G Ninja -DCMAKE_BUILD_TYPE=Dev \
      -DCMAKE_C_COMPILER=/opt/homebrew/opt/llvm/bin/clang \
      -DCMAKE_CXX_COMPILER=/opt/homebrew/opt/llvm/bin/clang++ \
      -DCMAKE_OSX_SYSROOT=$(xcrun --show-sdk-path) \
      -DCMAKE_C_FLAGS=-fsanitize=fuzzer-no-link -DCMAKE_CXX_FLAGS=-fsanitize=fuzzer-no-link \
      -DDE_SANITIZE=address,undefined -DDE_FUZZ_LIBFUZZER=ON
  cmake --build build/fuzz --target fuzz_<name>
  mkdir -p /tmp/fz && build/fuzz/tests/fuzz/<module>/fuzz_<name> -max_total_time=60 \
      -timeout=10 -malloc_limit_mb=256 /tmp/fz tests/fuzz/<module>/corpus/<name>
  # importer targets: add -dict=tests/fuzz/import/dict/<name>.dict
  ```

  `fuzz_dpak_file` (DENG-395) writes each input to `fuzz_dpak_file.<pid>.dpak` in its build
  directory (`tests/fuzz/asset/`) and deletes it after the input; the environment variable
  `DE_FUZZ_DPAK_FILE_DIR` names another directory. Its seeds are `corpus/dpak_reader/`.

  On macOS, the LLVM 23.1.3 RSS thread can retain libc++ thread state at exit
  (DENG-559). Keep leak detection and the default RSS limit enabled; a suppressed
  report does not qualify a clean fuzz run. The isolated runtime lifetime experiment
  and its positive/negative controls are in `tools/diagnostics/macos-lsan/README.md`.

### `checks.yml`: repository checks

| Job | What it runs | Local equivalent |
|---|---|---|
| header + layering checks | Configures `linux-clang-dev`, builds `de_deterministic_objects` (the exact owning targets from the generated assembly-check list), then runs `ctest -L check`: `header_check`, `layering_check`, `layering_selftest`, `vendor_selftest`, `vendor_prebuilt_selftest`, `deterministic_asm_check`, `check_determinism_src` and its self test, `check_det_registered` and its self test. The assembly check retains its own build step and 120-second timeout; CI prepares cold dependencies before verification. The job uploads the module graph `layers.dot` as an artifact. | `cmake --build --preset dev --target de_deterministic_objects`, then `ctest --preset dev -L check` |
| vendor.py verify | `python tools/vendor/vendor.py verify --all-hosts`: every vendored tree, and the archive of every host of a prebuilt entry (DENG-390). The archives are cached in `~/.cache/dengine-vendor` (key `vendor-all-<hash of third_party/vendor.toml>`), so a warm run downloads nothing. | `python3 tools/vendor/vendor.py verify --all-hosts` |
| clang-format (changed files) | `clang-format --dry-run --Werror` on the C, C++ and Objective-C files that the change adds or modifies, outside `third_party/`. | See below. |

The format job compares against the pull request base. For a push, it compares against the
previous tip of the branch. For a new branch or a manual run, it compares against the merge base
with the default branch. clang-format is pinned (`CLANG_FORMAT_VERSION` in `checks.yml`, now
22.1.4), because different versions format some code differently. To run the same check:

```sh
pip install clang-format==22.1.4
git diff --name-only --diff-filter=ACMR sprint/<sprint> -- '*.c' '*.h' '*.cpp' '*.hpp' '*.inl' \
    '*.m' '*.mm' ':!third_party/**' | xargs clang-format --dry-run --Werror
```

Every C, C++ and Objective-C file outside `third_party/` is formatted, the public headers in
`include/dengine/` and `examples/minimal_game.c` included (DENG-355). Objective-C files (`.m`,
`.mm`) use the `Language: ObjC` section of `.clang-format`, which repeats the house rules of the
`Cpp` section; change both sections together (DENG-408).

### `determinism.yml`: cross-platform determinism

SPEC §19.4 and risk R6: every scenario in `tests/determinism/` must give the same per-tick
checksums on every platform and compiler. The harnesses and the scenarios are described in the
Trama pages `Testing` and `Module: physics`.

| Job | Runner | Preset |
|---|---|---|
| plan | `ubuntu-24.04` | none: holds the platform list |
| determinism macos-arm64-clang | `macos-14` | `macos-release` |
| determinism linux-x86-64-gcc | `ubuntu-24.04` | `linux-gcc-release` |
| determinism linux-x86-64-clang | `ubuntu-24.04` | `linux-clang-release` |
| determinism windows-x86-64-msvc | `windows-2022` | `windows-msvc-release` |
| determinism windows-x86-64-clang-cl | `windows-2022` | `windows-clang-cl-release` |
| determinism cook macos-arm64-clang | `macos-14` | `macos-dev` |
| determinism cook linux-x86-64-gcc | `ubuntu-24.04` | `linux-gcc-dev` |
| determinism diff (all platforms) | `ubuntu-24.04` | none |

- The `plan` step lists the platforms, one JSON line each. `determinism-run` is one matrix job over
  that list and `determinism-diff` reads the same list, so a new platform is one appended line.
  The first line (macOS arm64, where the golden files are made) is the diff reference.
- Each `determinism-run` job builds only the target `de_determinism_tests` (every scenario
  executable) and runs `ctest -L determinism`: worker counts, the checked-in golden files, the
  harness self tests. So a platform that differs from a golden file fails here already.
- Then it uploads what the tests wrote to `build/<preset>/determinism/` as the artifact
  `determinism-<platform>` (kept for 14 days): `<scenario>.checksums` (one value per tick) and,
  for the physics scenarios, `<scenario>.fields` (per-tick hashes of the field groups: positions,
  rotations, velocities, mass, AABBs, other body fields, probe, ECS).
- `determinism-diff` downloads every artifact and compares each file with the reference platform's
  byte for byte. It names the first differing tick and, when both platforms have a `.fields`
  file, the field groups that differ at that tick, and writes a table to the job summary. It runs
  also when a platform job failed, and then fails if that platform's files are missing.
- A new scenario needs no change here: its tests write the files and the diff job globs them.
- **Cooking (DENG-216).** `determinism-cook` runs the `det.cook.*` tests
  (`tests/determinism/cook/`): `de-cook cook` of `tests/data/cook_project` must give the same pack
  and cache entries with a cold and a warm cache, with `--jobs 1` and `--jobs 8`, and from a copy of
  the project at another path. These tests need the cooker, which only Debug and Dev builds have, so
  the cook jobs use Dev presets on macOS arm64 and Linux x86-64 (the Release jobs above skip the
  tests). The `plan` step holds the cook hosts as a second list. Each cook job uploads
  `build/<preset>/determinism/cook_project.linux.dpak` (the pack cooked with `--platform linux`) and
  `cook_project.linux.decache` (path, size and XXH3-64 of each cache entry) as
  `determinism-cook-<platform>`. The step "Compare cooked packs" of `determinism-diff` compares them
  with the first host's files byte for byte and writes the first differing byte offset to the job
  summary. To find the asset that differs, download both artifacts and compare `de-pack list --json`
  of the two packs, then `de-pack extract` the asset from each.
- Local equivalent: `ctest --preset dev -L determinism` (the cook tests included). To check x86-64
  on an arm64 Mac, configure `macos-dev` into its own directory with
  `-DCMAKE_OSX_ARCHITECTURES=x86_64`, build `de_determinism_tests` and run the tests under Rosetta 2
  (`arch -x86_64`).
- Start it on a branch with `gh workflow run Determinism --ref <branch>`.
- The SPEC §5.7 source check (`check_determinism_src`, label `check`) is not a step here: as a
  CTest entry it runs in `checks.yml` and in every `ci.yml` and `sanitizers.yml` job. The same
  holds for `check_det_registered` (every determinism de_test case has a CTest entry).

## Shared setup: `.github/actions/setup-dengine`

Every build job uses this composite action. It does these steps:

1. Installs Python (3.11), and CMake (3.31.6) and Ninja (1.13.2) from PyPI. The same versions are
   used on every OS. The project needs CMake 3.28 or newer, and the action fails early with an
   older version.
2. On Windows, opens the x64 MSVC developer environment (`ilammy/msvc-dev-cmd`). Ninja needs it
   for both `cl` and `clang-cl`.
3. Checks the compiler (`compiler` input: `apple-clang`, `clang`, `gcc`, `clang-cl`, `msvc`).
   If the runner image does not have it, the action installs it: `clang` and `lld` from apt, the
   Clang sanitizer runtimes (`libclang-rt-<n>-dev`) when `sanitizers: "true"`, or LLVM from
   Chocolatey for `clang-cl`.
4. Sets up the compiler cache: ccache on Linux and macOS, sccache on Windows. The cache key is
   the preset name plus the hash of `third_party/vendor.toml` and `CMakePresets.json`. A key
   miss restores the newest cache of the same preset. Objects that a job did not use are
   evicted before the save, so the caches stay small. The action sets
   `CMAKE_<LANG>_COMPILER_LAUNCHER` in the environment, so the presets do not change. Pull
   requests restore the cache but do not save it.

5. Caches the vendor archive directory `~/.cache/dengine-vendor`, where a configure downloads the
   prebuilt binaries of a `kind = "prebuilt"` entry (`cmake/DePrebuilt.cmake`, DENG-390). The
   key is `vendor-host-<runner OS>-<hash of third_party/vendor.toml>`, so each OS keeps only its
   own host's archives, and a new pin falls back to the newest cache of the same OS. The
   directory is created first, so the step works when nothing was downloaded. With
   `cache-save: false` (pull requests) the cache is restored but not saved.

The Windows SDK comes from the runner image. No job caches or uploads SDK files
(CONVENTIONS §6). The compiler cache holds object files only, and the vendor cache holds only the
pinned archives of `third_party/vendor.toml`.

Debug info and the cache: a local Debug or Dev build with `cl` or `clang-cl` uses `/Zi` (one
shared program database per target), which sccache cannot cache with `cl`. Because the action
sets a compiler launcher, `cmake/DeBuildTypes.cmake` switches these builds to `/Z7` (debug info
in each object file), so every Windows job, `windows msvc Debug` included, uses the cache.

## Third-party actions

Every `uses:` is pinned to a full commit SHA, with the release tag in a comment. To update an
action, look up the commit of the new tag (`gh api repos/<owner>/<repo>/commits/<tag> --jq .sha`)
and replace the SHA and the comment in every file that uses the action.

## Adding a job

1. If the job builds the engine, add a preset first. Platform and build-type presets go in
   their family in `CMakePresets.json` (see "Adding a preset" in `docs/building.md`). CI-only
   variants, such as the sanitizer presets, are called `ci-<os>-<what>`. Append them at the end
   of the configure, build and test preset arrays. Add a build preset and a test preset with the
   same name. Put runtime environment (`*_OPTIONS`) in the test preset.
2. Add a matrix `include` entry to the workflow that fits: `ci.yml` for a compiler or
   configuration, `sanitizers.yml` for a sanitizer, `checks.yml` for a repository check. Use
   `./.github/actions/setup-dengine` with `cache-key` set to the preset name.
3. A new repository check should be a CTest entry with the label `check`, so that
   `ctest -L check` runs it locally and in `checks.yml`. A workflow step is only for checks
   that cannot be a test, for example checks that need the network or the git history.
4. Run `actionlint` (`brew install actionlint shellcheck`) before you commit. `act -l` lists the
   jobs.
5. Update this file and the Trama page `CI` (space `DENG`).

## Verifying CI changes

- Lint: `actionlint` must report no errors for `.github/workflows/*.yml`. It also runs
  shellcheck on the `run:` scripts.
- To test that a negative case fails, push a throwaway branch with a deliberate break and open a
  pull request: a use-after-free in a test for ASan, an upward `#include` for
  `layering_check`, or a public header that uses a type it does not include for
  `header_check`. Then close the pull request and delete the branch.

DENG-341 adds `test_render_irradiance_ssgi` / `render_irradiance_ssgi.*` to both explicit
full-tier GPU selections. The suite executes real six-view irradiance bakes, packed-asset
reload, static DDGI composition and half-resolution temporal SSGI on lavapipe and WARP.
Native Metal verification also enables `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1`.
