# Continuous integration

DEngine uses GitHub Actions. There are four workflows in `.github/workflows/`. `ci.yml`,
`sanitizers.yml` and `checks.yml` run on a push to `main`, `dev` or `sprint/**`, on every pull
request, and by hand (`workflow_dispatch`). `determinism.yml` runs on a push to `main`, `dev` or
`sprint/**` and by hand, not on pull requests. Each workflow has one concurrency group per branch,
so a new run on a branch cancels the run that it replaces (also a second `gh workflow run` on the
same branch).

## Tiers: quick and full

Workers run CI on their `feature/**` branch before review (`docs/agents/WORKER.md`), up to six
branches at a time, so the full matrix (17 build and sanitizer jobs, 6 of them on Windows) kept the
runners queued. `ci.yml` and `sanitizers.yml` therefore have two tiers:

| Tier | When | Jobs |
|---|---|---|
| quick | a run on a `feature/**` branch | windows msvc Debug, linux gcc Dev, headless linux Release, linux clang ASan+UBSan, linux clang TSan |
| full | every other run: `dev`, `main`, `sprint/**`, pull requests | every job in the tables below |

- A first `plan` job picks the tier and builds the matrix, so the jobs are listed once, in the
  `plan` step, each with `"quick": true` if it is in the quick tier.
- `gh workflow run CI --ref <branch> -f tier=full` (or `quick`) overrides the choice.
- The quick jobs are the ones that caught most branch failures: MSVC warnings, GCC flow warnings
  that only fire in optimized builds, the build without GPU and window modules, Linux-only UB and
  data races. Workers also run `ci-macos-asan` and the `windows-xwin-dev` cross-build on the
  host before they push.
- What the quick tier misses (clang-cl, MSVC Release, GCC TSan, macOS jobs, headless Windows) is
  caught by the full run on `dev` after the merge; the orchestrator files a fix task for it.
- `checks.yml` is small (three Ubuntu jobs) and always runs in full. `determinism.yml` (five
  platform jobs and a diff job) has no tiers either.

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
| headless linux Release | `ubuntu-24.04` | `linux-headless-release` |
| headless windows Release | `windows-2022` | `windows-headless-release` |

The configure step prints `build/<preset>/de_config_dump.txt`. If a job fails, it uploads the
artifact `logs-<preset>` (kept for 7 days). The artifact holds `Testing/Temporary/`
(`LastTest.log`), `CMakeConfigureLog.yaml` and the config dump.

GPU and golden-image jobs come in S10, self-hosted runners in S20 and the ABI check in S37. The
cross-architecture determinism jobs are in `determinism.yml` (below).

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

### `checks.yml`: repository checks

| Job | What it runs | Local equivalent |
|---|---|---|
| header + layering checks | Configures `linux-clang-dev` and runs `ctest -L check`: `header_check`, `layering_check`, `layering_selftest`, `vendor_selftest`, `deterministic_asm_check` (which builds the deterministic targets itself), `check_determinism_src` and its self test. It uploads the module graph `layers.dot` as an artifact. | `ctest --preset dev -L check` |
| vendor.py verify | `python tools/vendor/vendor.py verify`. The archives are cached in `~/.cache/dengine-vendor`, keyed by the hash of `third_party/vendor.toml`, so a warm run downloads nothing. | `python3 tools/vendor/vendor.py verify` |
| clang-format (changed files) | `clang-format --dry-run --Werror` on the C and C++ files that the change adds or modifies, outside `third_party/`. | See below. |

The format job compares against the pull request base. For a push, it compares against the
previous tip of the branch. For a new branch or a manual run, it compares against the merge base
with the default branch. clang-format is pinned (`CLANG_FORMAT_VERSION` in `checks.yml`, now
22.1.4), because different versions format some code differently. To run the same check:

```sh
pip install clang-format==22.1.4
git diff --name-only --diff-filter=ACMR sprint/<sprint> -- '*.c' '*.h' '*.cpp' '*.hpp' '*.inl' \
    ':!third_party/**' | xargs clang-format --dry-run --Werror
```

Objective-C files (`.m`, `.mm`) are not checked, because `.clang-format` has no ObjC section yet.
Every other C and C++ file outside `third_party/` is formatted, the public headers in
`include/dengine/` and `examples/minimal_game.c` included (DENG-355).

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
- Local equivalent: `ctest --preset dev -L determinism`. To check x86-64 on an arm64 Mac, configure
  `macos-dev` into its own directory with `-DCMAKE_OSX_ARCHITECTURES=x86_64`, build
  `de_determinism_tests` and run the tests under Rosetta 2 (`arch -x86_64`).
- Start it on a branch with `gh workflow run Determinism --ref <branch>`.
- The SPEC §5.7 source check (`check_determinism_src`, label `check`) is not a step here: as a
  CTest entry it runs in `checks.yml` and in every `ci.yml` and `sanitizers.yml` job.

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

The Windows SDK comes from the runner image. No job caches or uploads SDK files
(CONVENTIONS §6). The compiler cache holds object files only.

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
