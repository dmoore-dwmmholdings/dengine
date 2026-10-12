# DEngine conventions (orchestrator and workers)

When this file and SPEC.md disagree, SPEC.md wins. Decisions on DENG-2 win over both.

Project documentation lives in the Trama space `DENG`. `docs/agents/DOCS.md` has the rules: read it for context before you work, and update it as part of every issue.

## 1. Git

| Item | Rule |
|---|---|
| `main` | Moves only when a sprint closes: fast-forward to the green `dev` commit the exit checks ran on, tagged `sxx-done` (LOOP.md §2.7). Nothing is committed to `main` directly. |
| `dev` | The one integration branch. Every feature branch is cut from `dev` and merged back into it by the orchestrator. Tasks of several sprints run at once (LOOP.md §1.1). The `sprint/…` branches of S01–S04 are historical. |
| Feature branch | `feature/<KEY>-<slug>`, cut from `dev`. One issue per branch. The worker pushes it to `origin` to run CI before review (WORKER.md Step 4). |
| Worktrees | `.worktrees/<KEY>/` (gitignored). The orchestrator creates and removes them. |
| Commits | Commit at the **end of each issue**. Subject: `<KEY>: <summary>`. End with the trailer `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. |
| Merges | Only the orchestrator merges, one at a time, and verifies after each merge. |
| Never | Force-push a shared branch (`main`, `dev`), rewrite shared history, commit build output, or commit vendored SDKs that are not allowed (see §6). A worker may `--force-with-lease` its own feature branch after a rebase. |

## 2. Andare

- Statuses: `To Do` → `In Progress` (the worker sets it) → `In Review` (the worker sets it) → `Done` (the orchestrator sets it after a green merge).
- Required comments on every issue: the worker's **🚧 Started** comment (branch, worktree, plan), its **✅ Ready for merge** or **❌ Could not complete** comment, and the orchestrator's **✅ Merged** comment.
- Labels:
  - `needs-hw-verify`: an acceptance criterion cannot be checked on the build host. The report says what has to run, and where.
  - `agent-failed`: a worker failed once. The orchestrator retries once.
  - `needs-human`: escalated with `request_decision`. Agents leave it alone until a human answers.
  - `follow-up`: filed by the orchestrator from a worker's Follow-ups list.
- Parallelism: tasks are linked with `blocked_by`, inside a sprint and across sprints. A task may start when all its blockers are Done and its **Owns paths** do not overlap a running task's (LOOP.md §2.3). The wave number in the description and in the SPRINT PLAN only guide the order.
- Planning shorthand in issue descriptions: `Branch: feature/<KEY>-<slug>`. Here `<KEY>` means the issue's own key. Descriptions use real DENG keys. If you still meet an outline id (`T3`, `S05 T10`), the **Outline ids** list in that sprint's SPRINT PLAN (v2) (epic description) maps it to a key. Corrections that were first posted as comments have been folded into the descriptions; a later comment still wins over the description if they disagree.

## 3. Code (from SPEC §3.2, §4.2, §5.1)

- C11 only (`-std=c11`, `/std:c11`). Allowed: `_Static_assert`, `_Alignas`, `_Generic`, `<stdatomic.h>`. **Forbidden:** VLAs, `<threads.h>`, `-ffast-math`.
- Warnings: `-Wall -Wextra -Wpedantic -Werror` (Clang/GCC) and `/W4 /WX` (MSVC). A warning is a build failure.
- Public headers are in `include/dengine/de_*.h`. Each one compiles as C11 and as C++17 (`DE_EXTERN_C_BEGIN/END`). No SDL type, and no other third-party type, appears in a public header.
- Naming: functions and types use the prefix `de_`, macros and enum values `DE_`, ECS components `De` + PascalCase (`DeCamera`). Internal-only symbols use `de__` or `static`.
- Handles are typed structs around `u32`, using `DE_HANDLE`. Zero is invalid. Each handle carries a generation, so a stale handle is detected.
- Functions that can fail return `de_result` and give their outputs through pointer parameters. On failure, they set the error context with `DE_FAIL(...)`.
- Every create function takes a `*_desc` struct, and `(desc){0}` must be valid. Every subsystem takes an explicit `de_allocator *`. There are no hidden globals.
- No `malloc`/`free` in per-frame paths. Use the frame arena or thread scratch. The zero-alloc frame test (from S06) enforces this.
- Layering (§4.1): a module includes only modules below it. `tools/check_layers/check_layers.py` (table: `tools/check_layers/layers.toml`) enforces this from S01 onward.
- **Deterministic modules** (simulation ECS schedules, physics, sim-side animation, input replay, netcode, sim navigation): every SPEC §5.7 rule applies. Use `de_dmath` only. Build with `-ffp-contract=off` (`de_add_module(... DETERMINISTIC)` sets it). Do not iterate hash maps. Iterate in stable order. Add a checksum test.
- C++ third-party code is allowed only behind a C interface inside one module (DENG-2 Q1). The `.cpp` shims stay inside that module's `src/` directory.
- Style: `.clang-format` (from S01). 4-space indent, 100 columns. Comments explain *why*. Public API comments are Doxygen-style `/** */`.

## 4. Layout and CMake

```
include/dengine/de_<mod>.h      public API (one owner task per sprint)
src/<mod>/                      implementation + CMakeLists.txt (calls de_add_module)
src/gpu/{vulkan,d3d12,metal}/   RHI backends
shaders/<area>/*.slang          shaders
tools/<tool>/                   CLIs (de-cook, de-pack, caps, editor…)
third_party/<lib>/              vendored, pinned, LICENSE inside
tests/unit/<mod>/  tests/fuzz/<mod>/  tests/golden/<scene>/  tests/determinism/<scenario>/
(Trama) Module: <mod>           one page per module, updated by any task that changes it (DOCS.md)
```

**Canonical directories.** Some issues were written in parallel and use different names for the same place. This table decides:

| Thing | Canonical path | Aliases you may see in issues |
|---|---|---|
| App host / `de_engine` / main loop / game API | `src/host/` | `src/engine/`, `src/app/` |
| Cross-module bridges in the app host (anim↔physics, audio occlusion raycasts) | `src/host/bridges/` | `src/app/bridges/` |
| Host-level debug panels (audio/physics/net stats) | `src/host/debug/` | `src/app/debug/` |
| Math (fast + deterministic) | `src/core/math/` | `src/math/` |
| Cooker library / importers | `tools/cook/lib/`, `tools/cook/importers/` | |
| Pack format (runtime reader) / writer | `src/asset/dpak/` / `tools/pack/lib/` | |
| World streaming cells | `src/world/streaming/` | |
| Save games / C binding layer | `src/services/save/` / `src/services/bind/` | |
| 2D physics | `src/physics/2d/` (same deterministic module) | |
| Editor | `tools/editor/` | |

If an issue names an alias, use the canonical path and note that in your report. If an earlier sprint actually built something somewhere else, the code already in the repo wins. Follow it.

- S01 registers every module target in advance, so later tasks **do not edit the root `CMakeLists.txt`**. They add sources in `src/<mod>/CMakeLists.txt` and tests with `de_add_test()`.
- Removable modules (audio, net, physics, ui, vfx, nav, video, steam) sit behind `DE_WITH_<MOD>` options. Code that uses them must build with them switched off.

## 5. Tests

- Each task adds unit tests for what it builds. `ctest` must pass in full on the host preset. Do not disable or skip a test to get to green.
- Every parser gets a fuzz target (TOML manifest, glTF/FBX, images, pack reader, net packets, save files).
- Rendering tasks add golden scenes through the S10 harness when it exists. References are stored per backend × tier.
- Deterministic modules add a checksum scenario under `tests/determinism/`.

## 6. Third-party code

- Vendor only the libraries listed in SPEC §22 (or added on DENG-2). Use `tools/vendor/vendor.py`: pinned version, sha256 checksum, `LICENSE` in the directory.
- **Never vendor or download** the Steamworks SDK, the Windows SDK/CRT, or any other proprietary SDK. CMake finds them through a path that the user sets.

## 7. Verification matrix (the build host is macOS arm64)

| Code | How a worker verifies it | Label if it cannot run |
|---|---|---|
| Portable C (core, ECS, assets, physics, net logic…) | Build and run all tests natively on macOS. | none |
| Metal, Core Audio, FSEvents, macOS platform | Build and run natively. GPU tests run on the host GPU. | Metal RT effects on M1/M2 run in software tiers. Say so. |
| Vulkan base features | Build against the Vulkan SDK. Run the basic tests through MoltenVK where it supports the feature. | `needs-hw-verify` for mesh shaders, RT, and any Vulkan 1.3 feature MoltenVK lacks |
| D3D12, WASAPI, Win32 | Cross-compile with the `windows-*` preset (clang-cl + xwin), if xwin is installed. Otherwise compile-check what you can. | always `needs-hw-verify` |
| Linux-only (PipeWire/ALSA, inotify, X11/Wayland) | Cross-compile with the Linux preset if a sysroot exists. Platform-independent logic is unit-tested on macOS. | always `needs-hw-verify` |
| Perf targets (Steam Deck, frame-time budgets) | Write the benchmark and the procedure. Run what the host can run. | `needs-hw-verify` |

If an `origin` remote with CI is configured, CI runs the full matrix after every push from a sprint merge. CI results that clear a `needs-hw-verify` item are recorded on the issue by whoever checks them.
