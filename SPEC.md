# DEngine — Technical Specification

**Version:** 0.1 (draft, from interview round 1)
**Date:** 2026-10-10
**Owner:** Dawson / DWMM Holdings, LLC
**License:** MIT (engine source). Vendored dependencies keep their own licenses (§22).

Companion files: `include/dengine/*.h` (API sketches), `examples/assets.toml` (manifest example).
Status words: **MUST** = required for the module to be accepted. **SHOULD** = default plan, may change with a recorded reason. **v1** = in the first full release. **Deferred** = designed for now, built later.

---

## 1. Vision and scope

DEngine is an open-source, C11 game engine for desktop platforms. It renders 2D and 3D in one world with a GPU-driven, physically based pipeline, adds hardware raytracing where the GPU supports it, and falls back to a raster pipeline where it does not.

### 1.1 Goals
- Native 2D and 3D in one unified world. 2D is a camera and content choice, not a separate engine.
- Highest practical visual quality: PBR materials, hybrid raytracing, a reference path tracer.
- Native backends per platform: D3D12 (Windows), Vulkan (Linux / Steam Deck), Metal (macOS).
- A declarative asset system: assets and bundles declared in TOML, one call to start a load, one call to query status.
- Deterministic simulation across platforms, so lockstep, rollback and replays work.
- A usable product for other developers: stable C API, documentation, examples, MIT license.

### 1.2 Non-goals (v1)
- Mobile, web, and console targets. The RHI must not block a later console backend, but no work is planned.
- Hardware older than the baseline in §3.3.
- A visual scripting system.
- The editor. It is the separate DEngine IDE (Disegno spec **DIDE**, §20) and is **deferred** until all runtime systems are built and tested.

---

## 2. Decision summary

| Area | Decision |
|---|---|
| Language | C11. Clang primary on every target; MSVC and GCC kept green in CI. |
| Build | CMake. Vendored, vetted dependencies. |
| Targets | Windows 10 22H2+ (x86-64), Linux incl. Steam Deck (x86-64), macOS 14+ (Apple Silicon). |
| Graphics | D3D12 (Windows), Vulkan 1.3 (Linux), Metal 3 (macOS). Shaders in Slang. |
| GPU baseline | Bindless required. Mesh shaders used if present, compute meshlet fallback otherwise. |
| Raster | Visibility buffer, GPU-driven culling, render graph. |
| Raytracing | Hybrid. RT shadows, AO, reflections, GI where supported; raster fallbacks otherwise. Full path-tracing mode. |
| Denoise / upscale | Own denoiser. FSR on D3D12/Vulkan, MetalFX on macOS, own TAA everywhere. |
| Materials | glTF metallic-roughness + clearcoat, sheen, transmission, volume, subsurface, anisotropy, iridescence. |
| Output | HDR and SDR. Pluggable tonemappers. |
| Coordinates | Z-up, right-handed, meters. Double-precision world positions. |
| 2D | Unified world, fully lit and raytraced. First-class pixel art. |
| Object model | Archetype ECS. Systems scheduled by a task graph. |
| Memory | Custom allocators throughout; allocator passed explicitly. |
| Engine shape | Engine host + game DLL (dev, hot reload) and static link (ship). |
| Main loop | Game chooses fixed-step + interpolation or variable step. |
| Scripting | C primary. Stable C binding layer so a scripting module can be added later. |
| Errors | Result codes + thread-local error context. |
| Serialization | Reflection-driven; text in dev, binary in ship. |
| Assets | TOML manifest; bundles + single assets; polling status + ECS events; source in dev, cooked in ship; zstd pack archives; hot reload of all types; ref counting + streaming. |
| Platform layer | SDL3 (window, events, gamepad, sensors). Not SDL audio, not SDL_GPU. |
| Physics | Own. 3D + 2D rigid bodies, character controller, deterministic. |
| Audio | Own, including OS backends. |
| Animation | Skeletal + skinning, blend trees, state machines, IK, 2D sprite animation (§21.5). |
| Input | Action mapping + rebinding, haptics + gyro, input recording/replay. |
| UI | Immediate-mode debug UI + retained in-game UI. |
| Text | MSDF + bitmap fonts, complex shaping, localization. |
| Networking | Transport + all three models: server-authoritative, rollback, lockstep. |
| World systems | Particles/VFX, sky/atmosphere/volumetrics, terrain + foliage, decals, water. |
| Gameplay support | Navigation, tilemaps (LDtk/Tiled), Aseprite import, video playback. |
| Services | Steamworks, headless dedicated server, mod support, crash reporting. |
| Tooling | Tracy, stats overlay, GPU debug markers, ECS inspector. |
| Testing | Unit, golden-image per backend, determinism, CI on all targets. |
| First milestone | Renderer-led (§23). |

---

## 3. Platforms, toolchain, build

### 3.1 Targets

| Platform | Arch | Graphics | Window / input | Audio backend |
|---|---|---|---|---|
| Windows 10 22H2+ / 11 | x86-64 | D3D12 (Agility SDK) | SDL3 | WASAPI |
| Linux (glibc 2.31+), SteamOS | x86-64 | Vulkan 1.3 | SDL3 (Wayland + X11) | PipeWire, ALSA fallback |
| macOS 14+ | arm64 | Metal 3 | SDL3 | Core Audio (AudioUnit) |
| Headless server | x86-64 Linux, Windows | none | none | none |

Stretch (not v1): Windows on ARM, Linux arm64. Intel Macs are out of scope (open question Q3).

### 3.2 Toolchain
- **Compilers.** Clang is the primary compiler on every target: `clang-cl` on Windows, Apple Clang (Xcode) on macOS, `clang` on Linux. MSVC and GCC MUST build cleanly in CI so the code stays portable.
- **C standard.** C11 (`-std=c11`, `/std:c11`). Allowed: `_Static_assert`, `_Alignas`, `_Generic`, `<stdatomic.h>`. Not allowed: VLAs, `<threads.h>` (inconsistent support; the engine has its own thread layer).
- **Warnings.** `-Wall -Wextra -Wpedantic -Werror` in CI. `/W4 /WX` on MSVC.
- **Floating point.** No `-ffast-math`. `-ffp-contract=off` for every translation unit in a deterministic module (§5.7).
- **Build system.** CMake 3.28+. Presets for each platform × {Debug, Dev, Release, Shipping}.
  - *Debug*: no optimization, all asserts.
  - *Dev*: optimized, asserts, hot reload, source-asset import, Tracy, debug UI.
  - *Release*: optimized, cooked assets, Tracy optional.
  - *Shipping*: optimized, cooked assets, no dev tooling, static link.
- **Cross-compilation.**
  - Linux host → Windows: `clang-cl` + `lld-link` with a downloaded Windows SDK / CRT (e.g. via `xwin`; the user accepts Microsoft's license).
  - Windows host → Linux: `clang` + a Linux sysroot.
  - macOS builds run on macOS hosts (local or CI). Cross-compiling to macOS from non-Apple hardware is not supported.
  - Toolchain files ship in `cmake/toolchains/`.

### 3.3 GPU baseline

Two tiers per platform. Exact model list is validated in milestone 1 (risk R5).

| Tier | Windows (D3D12) | Linux (Vulkan) | macOS (Metal) |
|---|---|---|---|
| **Base** (raster + fallbacks) | SM 6.6, Resource Binding Tier 3. Approx. GTX 10 / RX 5000 / Arc. | Vulkan 1.3, descriptor indexing, buffer device address, sync2, dynamic rendering, timeline semaphores. | Metal 3, Apple GPU family 7 (M1), argument buffers tier 2. |
| **RT** | DXR 1.1 (inline ray queries). Approx. RTX 20 / RX 6000 / Arc. | `VK_KHR_acceleration_structure`, `VK_KHR_ray_query`; `VK_KHR_ray_tracing_pipeline` for the path tracer. | Metal ray tracing. Hardware-accelerated on M3+; M1/M2 run the API at lower tiers. |
| **Mesh shaders** | D3D12 mesh shaders | `VK_EXT_mesh_shader` | Metal mesh shaders |

Steam Deck (RDNA2) supports ray queries but defaults to the Base quality tier for performance.

### 3.4 Repository layout

```
dengine/
  CMakeLists.txt
  cmake/                   toolchains, helpers
  include/dengine/         public headers (de_*.h)
  src/
    core/ platform/ task/ reflect/ ecs/ asset/
    gpu/{d3d12,vulkan,metal}/
    render/ anim/ physics/ audio/ input/ net/ ui/ text/
    world/{vfx,sky,terrain,water,decal}/
    gameplay/{nav,tilemap,video}/
    services/{steam,crash,mods}/
  shaders/                 Slang sources
  tools/
    cook/                  de-cook (asset cooker CLI + library)
    pack/                  de-pack
    editor/                placeholder; the IDE lives in its own repo (§20)
  third_party/             vendored, pinned, one LICENSE per directory
  tests/{unit,golden,determinism,fuzz}/
  examples/
  docs/
```

---

## 4. Architecture

### 4.1 Layers

```
 Game (DLL or static)                Editor service → DIDE (deferred, separate process)
 ─────────────────────────────────────────────────────────────────────────
 App host: main loop, game API, hot reload, cvars, services
 ─────────────────────────────────────────────────────────────────────────
 Gameplay: anim · physics · audio · input · net · ui · text · nav · vfx · world
 ─────────────────────────────────────────────────────────────────────────
 Renderer (render world, render graph, passes)  │  Asset system (manifest, loader, streaming)
 GPU RHI (d3d12 · vulkan · metal)               │  Cooker library (dev builds + de-cook)
 ─────────────────────────────────────────────────────────────────────────
 ECS · reflection/serialization · task graph
 ─────────────────────────────────────────────────────────────────────────
 Core: types, memory, errors/log, math, containers, hashing, strings
 Platform: SDL3 wrapper, threads/atomics, files, async IO, file watch, time
```

### 4.2 Rules
- A module depends only on modules below it. No cycles. CI checks this with an include-graph script.
- Every module is created with an explicit context and allocator. No hidden globals, except one optional `de_engine*` convenience pointer for the game.
- Every module that can be removed (audio, net, physics, ui, vfx, nav, video, steam) is a CMake option. A headless server build removes the renderer, GPU, audio and UI.
- C++ dependencies are allowed only behind a C interface inside one module and must be listed in §22 (open question Q1).

---

## 5. Core

### 5.1 API conventions
- Prefix `de_` for functions and types, `DE_` for macros and enum values.
- Handles are typed structs around integers (`typedef struct { uint32_t id; } de_texture;`). Zero is always invalid. Handles carry a generation, so a stale handle is detected, never dereferenced.
- Functions that can fail return `de_result`. Outputs go through pointer parameters.
- Every create/destroy function takes a `*_desc` struct with zero-initialized defaults (`(de_x_desc){0}` is always valid).
- `DE_API` marks exported symbols. The ABI is plain C; no struct passed by value across the ABI changes layout within a major version.
- Public headers compile as C11 and as C++17 (`extern "C"`).
- ECS component types use PascalCase with a `De` prefix (`DeCamera`, `DeRigidBody`) so components are distinguishable from plain structs. Game components follow the same rule with their own prefix.

### 5.2 Errors and logging
- `de_result` enum: `DE_OK`, then categories (invalid argument, out of memory, I/O, not found, unsupported, device lost, timeout, …).
- **Error context:** on failure, the function sets a thread-local `de_error` record: result, message, file, line, function, and an optional cause chain. `de_error_last()` reads it; `de_error_clear()` resets it.
- **Asserts:** `DE_ASSERT` (Debug/Dev), `DE_VERIFY` (always evaluated), `DE_PANIC` (fatal, triggers the crash handler).
- **Logging:** levels Trace, Debug, Info, Warn, Error, Fatal. Categories are registered (`render`, `asset`, …) and each has its own level. Sinks: console, file, debug UI console, Tracy. Logging is lock-free per thread and drained by one thread.

### 5.3 Memory
- `de_allocator`: `{ alloc, realloc, free, user }` function-pointer struct. Every subsystem's create function takes one.
- Provided allocators:
  - **Virtual arena:** reserves address space, commits on demand, reset in O(1).
  - **Frame arena:** one per in-flight frame, reset at frame start. Used for all per-frame scratch.
  - **Pool:** fixed-size blocks, used by ECS chunks, physics bodies, audio voices.
  - **TLSF:** general-purpose, O(1), used as the default heap.
  - **Thread scratch:** per-thread stack-like scratch, nested with save/restore markers.
- No `malloc` in per-frame paths. CI runs a frame-loop test with a counting allocator and fails on unexpected heap traffic.
- Debug builds tag every allocation with a category and report to Tracy and the stats overlay.

### 5.4 Math
- Own library. SIMD paths for SSE4.2/AVX2 (x86-64) and NEON (arm64), plus a scalar reference.
- Types: `de_vec2/3/4`, `de_dvec3`, `de_quat`, `de_mat3/4`, `de_affine` (3×4), `de_aabb`, `de_plane`, `de_ray`, `de_frustum`.
- Storage is column-major. Vectors are column vectors (`v' = M · v`).
- Projection: reverse-Z, infinite far plane. Per-API clip-space differences are handled in the RHI.
- **Two libraries in one:** `de_math` (fast, may use approximations, used by rendering) and `de_dmath` (deterministic: strict IEEE, own transcendental functions, no approximate reciprocal instructions, used by simulation). See §5.7.

### 5.5 Coordinates and units
- Z-up, right-handed. +X right, +Y forward, +Z up.
- 1 unit = 1 meter. Angles in radians. Mass in kg. Time in seconds.
- **Large worlds:** world-space positions are `de_dvec3` (double). Rotation and scale are float. Rendering is camera-relative: positions are converted to float relative to the camera on the CPU before upload. Physics stores double positions and solves in float relative to each island's origin.
- Importers convert from source conventions (glTF Y-up, FBX per-file axis settings) to engine conventions at cook time.

### 5.6 Containers and utilities
Dynamic array, hash map (open addressing, deterministic iteration order not guaranteed → never iterate a hash map in simulation code), string views, string interning (`de_name`, 64-bit hashed), 64-bit hashing (xxh3), ring buffers, SPSC/MPMC lock-free queues, bitsets, sorting (stable sort available for simulation).

### 5.7 Determinism rules
Applies to modules marked deterministic: ECS simulation schedules, physics, animation (simulation side), input replay, netcode, navigation queries used in sim.
1. Compile with `-ffp-contract=off`, no fast-math. MSVC: `/fp:precise`.
2. Use `de_dmath` only. No `<math.h>` transcendentals (results differ between libm implementations).
3. No approximate SIMD instructions (`rcpps`, `rsqrtps`, NEON `frecpe`/`frsqrte`).
4. Denormal mode set identically on every worker thread at startup (flush-to-zero on, x86 MXCSR and arm64 FPCR).
5. No iteration over hash maps, pointer-ordered containers, or thread-completion order. Entities iterate in archetype/row order; parallel work splits into fixed ranges and reduces in fixed order.
6. Random numbers only from `de_rng` streams seeded by the simulation.
7. Every deterministic module exposes a world checksum function used by tests (§19) and netcode desync detection (§16).

### 5.8 Console variables
`de_cvar`: typed (bool, int, float, string, enum) named values registered by any module. Settable from the debug console, command line, and a `config.toml`. Flags: read-only, dev-only, saved, replicated (server → clients), cheat.

---

## 6. Platform layer

- **SDL3** provides windows, display modes, HDR display info, events, keyboard, mouse, gamepads (including haptics, adaptive triggers, gyro), sensors, clipboard, file dialogs and cursor control.
- **Engine-owned, per OS:**
  - Threads, mutexes, condition variables, atomics, thread affinity and names.
  - File I/O and async I/O. v1 uses dedicated I/O threads with blocking reads. Later: IoRing (Windows), io_uring (Linux), dispatch_io (macOS).
  - File watching (dev only): `ReadDirectoryChangesW`, inotify, FSEvents.
  - High-resolution time, sleep, frame pacing.
  - Dynamic library loading (game DLL hot reload).
  - Crash handling (§18.5).
- **Graphics surface creation:** HWND (from SDL properties) for D3D12, `SDL_Vulkan_CreateSurface` for Vulkan, `SDL_Metal_CreateView` → `CAMetalLayer` for Metal.
- The platform layer exposes engine types only. No SDL type appears in a public engine header.

---

## 7. ECS

Archetype ECS, similar in design to Bevy and flecs.

### 7.1 Data model
- **Entity:** 64-bit ID = 32-bit index + 32-bit generation.
- **Component:** registered type with reflection data (§9). Tag components have zero size. Components are POD; resources they own are handles.
- **Archetype:** unique set of component types. Stores entities in tables with one column per component (SoA). Tables grow in fixed-size chunks from a pool.
- **Change detection:** each column row stores `added` and `changed` ticks. Queries can filter on them.
- **Resources:** typed singletons stored in the world (time, input state, asset system pointer, physics world, …).
- **Events:** typed, double-buffered queues. An event is readable for two frames, then dropped.
- **Relationships:** `ChildOf` / `Children` for hierarchies. Transform propagation is a built-in system.
- **Commands:** structural changes (spawn, despawn, add, remove) from inside systems go to per-thread command buffers, applied in deterministic order at sync points.

### 7.2 Queries
- Declared with reads, writes, optional, with, without, added, changed.
- Cached. A query matches archetypes once and updates when new archetypes appear.
- Iteration hands out contiguous column slices (chunks) for SIMD-friendly loops; `de_query_par_each` splits chunks into fixed ranges for parallel execution.

### 7.3 Systems and schedules
- A system = function + declared access (component reads/writes, resource reads/writes, event readers/writers).
- Schedules (fixed order): `Startup`, `First`, `PreUpdate`, `FixedUpdate` (0..N times per frame), `Update`, `PostUpdate`, `Extract` (copy to render world), `Last`.
- Within a schedule, the task graph scheduler (§8) builds a DAG from access conflicts plus explicit `before`/`after` ordering, then runs non-conflicting systems in parallel.
- Systems can be marked `main_thread` (platform calls) or `deterministic` (checked against §5.7 rules in Debug builds: e.g. no use of the frame arena's nondeterministic data).

### 7.4 Snapshots
- `de_world_snapshot` copies all component columns flagged `snapshot` into one contiguous buffer. Restore is a memcpy per column plus entity-index rebuild.
- Used by rollback netcode (§16), replays, editor play-in-editor (§20), and determinism tests.

---

## 8. Task graph

- One worker thread per logical core minus one (main thread also runs work). Workers pinned only on request.
- Work-stealing deques per worker. Global injection queue for external submits.
- **Tasks** are function + data + dependency counter. **Task graphs** are built once per schedule and reused until a system is added or removed.
- `de_task_parallel_for(range, grain, fn)` splits into fixed chunks, so outputs are deterministic when the function is.
- **Main-thread tasks** go into a separate queue drained only by the main thread.
- **Long-running tasks** (asset cooking, shader compiles, navmesh builds) go to a low-priority pool so they never stall a frame.
- **Frame pipelining:** simulation of frame N+1 runs while the render thread records frame N (Bevy-style pipelined rendering). The `Extract` schedule is the only point where both worlds are touched.

---

## 9. Reflection and serialization

### 9.1 Reflection
- Types are registered with macros that produce a `de_type_info`: name, size, alignment, fields (name, type, offset, flags, attributes such as range, units, tooltip), and optional hooks (construct, destruct, migrate).
- Field flags: `serialize`, `replicate`, `snapshot`, `editor_hidden`, `readonly`, `asset_ref`.
- The editor, serialization, netcode replication, ECS inspector, and scripting bindings all read the same reflection data. Nothing is described twice.

### 9.2 Formats
- **Text (dev):** scenes, prefabs, save games in a readable, diff-friendly format (TOML-compatible; open question Q9). Entities referenced by stable 64-bit scene IDs, assets by asset ID.
- **Binary (ship):** same data, versioned, little-endian, schema hash per type.
- **Versioning:** each type has a version. On load, older versions run their `migrate` hook. Unknown fields are skipped with a warning.
- **Save games:** a save is a filtered world serialization (components flagged `serialize` on entities tagged `Persistent`) plus game-defined blobs. Saves can route through Steam Cloud (§21.1).

---

## 10. Application host and main loop

### 10.1 Game attachment
- **Dev builds:** `dengine` host executable loads `game.dll` / `libgame.so` / `libgame.dylib` and calls `de_game_get_api()` to get a `de_game_api` table (`init`, `shutdown`, `register_types`, `register_systems`, `on_reload`, `on_unload`).
- **Shipping:** the same game code links statically. The host's `main` calls the API directly. No code differences in the game.
- **Hot reload:** the host watches the game library. On change: finish the frame, call `on_unload`, unregister systems, copy and load the new library, call `register_types` (reflection compares layouts; changed components run migration), `register_systems`, `on_reload`. All game state lives in the ECS world, so no state is lost.

### 10.2 Loop
Per frame:
1. Pump platform events, update input (§15).
2. `First`, `PreUpdate`.
3. **Timing mode** (chosen per project in `game.toml`, changeable at runtime):
   - *Fixed:* accumulate real time, run `FixedUpdate` at the fixed rate (default 60 Hz, max catch-up steps configurable), compute interpolation alpha.
   - *Variable:* run `FixedUpdate` once with the frame delta.
4. `Update`, `PostUpdate` (transform propagation, animation pose finalize, audio listener update).
5. `Extract` into the render world. Render thread starts recording.
6. `Last`. Frame arena swap. Frame pacing (vsync, frame cap, or VRR).

Deterministic simulation (physics, netcode) always runs in `FixedUpdate`, whichever mode is chosen.

---

## 11. Asset system

### 11.1 Concepts
- **Asset ID:** 64-bit hash of a stable string path (`"player/mesh"`). Sub-assets use `#` (`"player/model#Body"`). IDs never depend on file location on disk.
- **Typed handles:** `de_texture`, `de_mesh`, `de_material`, `de_skeleton`, `de_anim_clip`, `de_sound`, `de_font`, `de_shader`, `de_prefab`, `de_tilemap`, `de_navmesh`, `de_video`, `de_blob`. A handle points to a slot, not memory, so hot reload and streaming swap data behind it.
- **Bundle:** named group of assets that load and unload together. Bundles can depend on other bundles.
- **Load handle:** returned by every load call. Holds a reference until released.

### 11.2 Manifest (TOML)
One root `assets.toml` per project, with `include = [...]` for splitting. Full example in `examples/assets.toml`.

```toml
[project]
asset_root = "assets"
include    = ["characters.toml"]

[defaults.texture]
compression = "auto"     # bc7 color, bc5 normals, bc4 masks, bc6h hdr
mips = true

[assets]
"player/model"  = { type = "model",   source = "models/player.glb" }
"player/albedo" = { type = "texture", source = "textures/player_albedo.png", srgb = true }
"music/title"   = { type = "sound",   source = "audio/title.ogg", stream = true }
"ui/font"       = { type = "font",    source = "fonts/Inter.ttf", mode = "msdf", charset = "latin-ext" }

[bundles.common]
assets   = ["ui/*"]
resident = true          # never evicted

[bundles.level_01]
assets   = ["level01/*", "player/*"]
depends  = ["common"]
priority = "high"
```

- Asset types are registered by modules (renderer registers texture/mesh/material/shader, audio registers sound, …). Games can register their own types with an importer and a runtime loader.
- Globs match asset IDs, not files.
- **Locale overrides:** `[assets."ui/logo".locale] ja = "textures/logo_ja.png"` (§17.3).
- Validation (`de-cook --check`) reports unknown types, missing sources, cycles, and unmatched globs.

### 11.3 Runtime API (the "hook" and status function)

```c
de_load_handle h = de_asset_load_bundle(assets, "level_01", DE_LOAD_DEFAULT);   // start loading
de_load_info info;
de_load_state s = de_asset_status(assets, h, &info);                            // poll
// s: PENDING, LOADING, READY, PARTIAL (some failed), FAILED, CANCELLED
// info: assets_total/ready/failed, bytes_total/done, progress 0..1, first error
de_asset_release(assets, h);                                                     // drop reference
```

- `de_asset_load(assets, id, flags)` loads one asset (plus its dependencies).
- `de_asset_cancel` cancels outstanding work for a handle.
- **ECS events:** `de_asset_event { handle, asset_id, kind }` with kinds `LOADED`, `FAILED`, `RELOADED`, `EVICTED`. Systems read them like any event.
- **Blocking:** `de_asset_wait(assets, h, timeout)` exists for tools and tests, MUST NOT be used in the frame loop (asserts in Dev).
- **Priorities:** critical, high, normal, low, background. Streaming requests are normal or lower.

### 11.4 Pipeline
```
request → resolve deps → [I/O threads] read (pack chunk or cached blob)
        → [task graph] zstd decompress → type loader (parse, fix up)
        → [copy queue] GPU upload via staging ring → publish to slot → event
```
- Staging ring size is configurable (default 128 MB). Uploads are batched per frame.
- Dependencies (model → materials → textures) load in parallel; the parent is `READY` only when all children are.

### 11.5 Dev vs shipping
- **Dev:** the runtime loads source files through the cooker library in-process. Results are cached in `.decache/` keyed by hash(source bytes, import settings, importer version, platform). A warm cache loads as fast as shipping.
- **Shipping:** only cooked data from pack archives. The cooker is not linked.
- `de-cook` CLI cooks a project for a platform and produces packs. It is deterministic: same inputs → byte-identical outputs.

### 11.6 Importers and cooked formats

| Source | Cooked result |
|---|---|
| glTF 2.0 (`.gltf/.glb`), FBX (ufbx) | Model → meshes (meshlets of ≤64 vertices / ≤124 triangles, quantized attributes, LOD chain), materials, skeletons, animation clips (compressed curves), prefab with node hierarchy. |
| PNG, JPG, EXR, KTX2 | BC1/3/4/5/6H/7 with full mip chain in upload-ready layout. KTX2 Basis/UASTC transcoded at cook time. HDR environments → prefiltered cubemaps + SH. |
| WAV, OGG, FLAC | Short sounds: PCM or ADPCM. Long sounds: Vorbis or Opus stream (open question Q11). |
| TTF/OTF | Font file + MSDF atlas pages (pre-baked charsets) + metrics. Bitmap fonts: BMFont/AngelCode or pixel atlases. |
| `.slang` | DXIL, SPIR-V, metallib per backend, per permutation, plus reflection data. |
| `.aseprite` | Sprite atlas + animation clips from tags + slices. |
| LDtk, Tiled (`.tmj/.tmx`) | Tilemaps + entity spawns + collision layers. |
| Video (AV1 in WebM/MKV) | Passed through, re-muxed for streaming (§21.4). |
| TOML scenes/prefabs | Binary scenes. |
| String tables | Binary per-locale tables (§17.3). |

All BC formats are supported on every target GPU, including Apple Silicon, so desktop builds use one texture format family.

### 11.7 Pack archives (`.dpak`)
- Header: magic, format version, platform, build ID, content hash.
- Table of contents sorted by asset ID: offset, compressed and raw sizes, type, chunk table, dependency list.
- Data stored in 64 KiB chunks, each zstd-compressed (optional per-type zstd dictionaries), aligned for direct GPU upload.
- Multiple packs mount in priority order. A higher pack overrides an asset ID (patches, DLC, mods §21.3).

### 11.8 Lifetime and streaming
- Each asset slot is reference-counted by load handles and by other assets. Zero refs → eviction candidate (LRU), evicted when over budget or on explicit flush.
- **Memory budgets** per category (GPU textures, GPU meshes, audio, CPU). Exceeding a budget evicts non-resident assets first, then streams lower mips/LODs.
- **Texture streaming:** the material resolve pass writes the needed mip per texture into a GPU feedback buffer (cheap with a visibility buffer). The residency manager reads it back (1–2 frames late), loads or drops mips.
- **Mesh streaming:** LODs selected by projected screen error; distant LODs resident, close LODs streamed.
- **World streaming:** large worlds are split into cells (§12.13). Cells are bundles loaded by distance from streaming sources (camera, players).

### 11.9 Hot reload
Dev builds watch every source path referenced by the manifest, plus the manifest itself. On change: recook → load → swap behind the handle at the next frame boundary → `RELOADED` event → dependents update (materials rebind textures, pipelines rebuild for shaders). If the new version fails, the old version stays and the error goes to the log and the debug UI.

---

## 12. Renderer

### 12.1 GPU abstraction (RHI, `de_gpu`)
Thin, explicit, bindless. Same API on D3D12, Vulkan, Metal.
- Objects: device, queues (graphics, async compute, copy), buffers, textures, samplers, graphics/mesh/compute/RT pipelines, acceleration structures (BLAS/TLAS), command lists, timeline fences, swapchains, query heaps.
- **Bindless:** one global descriptor heap/table for textures, buffers, samplers. Shaders index by `uint32`. Per-draw data goes through root/push constants (≤128 bytes) or GPU buffers.
- **Memory:** device memory heaps with an engine sub-allocator (TLSF). Transient render-graph resources are aliased.
- **Barriers:** explicit, issued by the render graph. D3D12 enhanced barriers (legacy fallback), Vulkan sync2, Metal fences/events with untracked resources.
- **Capabilities query:** `de_gpu_caps` reports RT support (and whether it is hardware-accelerated), mesh shaders, wave size, HDR output, memory sizes. The renderer picks tiers from it.
- **Debug:** object names, markers (§18.3), validation layers in Debug, GPU crash breadcrumbs (§18.5).

### 12.2 Shaders
- All shaders written in **Slang**. One source set → DXIL, SPIR-V, MSL/metallib.
- Compiled at cook time (dev builds compile on demand with cache and hot reload).
- Material and BSDF code lives in shared Slang modules used by the raster resolve, RT hit shading, and the path tracer, so all three agree by construction.
- Permutations through Slang specialization/generics. Permutation count per material model tracked in the stats overlay.

### 12.3 Render graph
- Passes declare reads/writes of textures/buffers, queue type, and an execute callback.
- Per frame: build → cull unused passes → schedule (async compute where it helps) → allocate and alias transient resources → insert barriers → record (in parallel across passes) → submit.
- Compiled graphs are cached while the pass set is unchanged.
- Every pass gets a GPU timer and a debug marker automatically.

### 12.4 Render world
- `Extract` copies render-relevant components (transforms converted to camera-relative float, mesh/material handles, lights, sprites, decals, particles emitters, UI draw lists) into a separate render world.
- **Views:** one per camera (main, split-screen, render-to-texture, shadow views, editor viewports later). Each view has its own render graph instance and quality settings.
- **GPU scene:** instance, material, and light data live in persistent GPU buffers updated incrementally (only changed entries upload).

### 12.5 Frame pipeline (per view)

1. **GPU culling.** Instances → meshlets. Two-phase occlusion culling against a hierarchical depth buffer (last frame's visible set, then re-test). Frustum, backface-cone, and small-triangle culling at meshlet level.
2. **Visibility buffer raster.** Mesh shader path, or compute expansion + indirect draws where mesh shaders are missing. Writes depth + 32-bit packed (instance, triangle) ID. Alpha-tested geometry (foliage, sprites) runs a separate pass.
3. **Material classification.** Compute pass bins screen tiles by material model.
4. **Material resolve.** Per material model, compute: reconstruct attributes from the triangle ID, evaluate the material, write a thin G-buffer (normal, roughness, base color, metal, motion vectors, material flags) used by lighting, RT and denoisers. Also writes texture-streaming feedback (§11.8).
5. **Lighting.** Clustered light lists (froxels). Direct lighting with shadows (RT or shadow maps). IBL.
6. **RT effects** (if tier allows): AO, reflections, GI, then denoise (§12.9).
7. **Transparency and transmission.** Forward pass using the same clustered lights. Transmission uses RT refraction on RT tier, screen-space refraction otherwise.
8. **Volumetrics, sky, water, particles** (§12.13).
9. **Post.** Temporal upscaling/AA, depth of field, motion blur, bloom, auto-exposure, color grading (LUT), tonemapping, film grain, vignette (each toggleable).
10. **UI composite and output** (HDR or SDR, §12.11).

### 12.6 Materials (PBR)
- Base model: glTF 2.0 metallic-roughness.
- Supported layers (mapped to the matching glTF `KHR_materials_*` extensions on import): emissive strength, IOR, specular, clearcoat, sheen, transmission, volume (thickness, attenuation), diffuse transmission, anisotropy, iridescence, plus subsurface scattering.
- Subsurface: screen-space separable SSS (Burley profile) in raster; random-walk SSS in the path tracer.
- **Material models** are Slang modules implementing an interface (`evaluate`, `sample`, `pdf`, `resolve_gbuffer`). Built-in: Standard (all layers above, features toggled by flags), Unlit, Sprite, Terrain, Water, Hair (open question Q12). Games can add models.
- Material instances are parameter blocks in a bindless buffer. Changing parameters never rebuilds pipelines.
- Energy conservation: multi-scattering compensation for GGX, energy-preserving layering for clearcoat and sheen.

### 12.7 Lights and shadows
- Lights: directional (sun/moon), point, spot, rect and disk area lights, emissive meshes, environment (HDR IBL, sky).
- **RT tier:** ray-traced shadows with soft penumbras from light size; many-light sampling via ReSTIR DI (open question Q13).
- **Base tier:** cascaded shadow maps for directional lights, atlas-packed shadow maps for local lights, PCSS soft shadows, contact shadows (screen-space).
- Light units are physical (lux, lumens, candela, nits). Exposure is physical (EV100).

### 12.8 Raytracing (hybrid)
- **Acceleration structures:** one BLAS per mesh LOD (built at load, compacted). Skinned and deforming meshes: compute skinning → BLAS refit each frame, rebuild every N frames. One TLAS per frame. Sprites and alpha-tested geometry use any-hit/opacity tests (opacity micromaps where supported).
- **Ray queries** (inline) are the default for hybrid effects. RT pipelines are used by the path tracer.
- **Effects per tier:**

| Effect | RT tier | Base tier fallback |
|---|---|---|
| Shadows | RT shadows | CSM + atlas, PCSS, contact shadows |
| AO | RTAO | GTAO |
| Reflections | RT reflections (rough + mirror) | SSR + reflection probes |
| GI | RT GI (ReSTIR GI or RT-updated irradiance probes, Q13) | Baked/static irradiance probes + screen-space GI |
| Transmission | RT refraction | Screen-space refraction |

- Quality presets (Low → Ultra, plus Custom) per effect. Auto-detect picks a preset from `de_gpu_caps` and a short GPU benchmark on first run.

### 12.9 Denoising and upscaling
- **Own denoiser:** spatio-temporal (SVGF/ReBLUR-style) per signal (shadow, AO, diffuse GI, specular). Uses thin G-buffer, motion vectors, and history validation.
- **Upscaling:**
  - D3D12 / Vulkan: AMD FidelityFX FSR (MIT).
  - macOS: Apple MetalFX temporal upscaler (FSR's SDK does not ship a Metal backend).
  - All backends: own TAA/TAAU as fallback and for quality comparison.
- Internal resolution scale is independent per view. Dynamic resolution scaling to hold a frame-time target (optional).

### 12.10 Path-tracing mode
- Progressive, unbiased, accumulates while the camera is still. Same scene, same materials, same lights.
- Purposes: reference images for golden tests, look development, offline screenshots, validation of raster/RT approximations.
- Features: multiple importance sampling, next-event estimation, Russian roulette, spectral-free RGB, volumes (participating media for transmission/volume layers and fog), random-walk SSS, configurable bounce count, optional denoise of the final accumulation.
- Requires the RT tier.

### 12.11 HDR, color, tonemapping
- Internal rendering in scene-referred linear Rec.709/sRGB primaries, half-float.
- **Output:** HDR10 (PQ, Rec.2020) or scRGB on Windows, `CAMetalLayer` EDR on macOS, HDR via Vulkan swapchain color spaces / Wayland where supported. SDR otherwise.
- **Tonemappers are pluggable:** `de_tonemapper` interface (Slang module + parameter block). Built-in: AgX, ACES (fitted), Khronos PBR Neutral, Reinhard, None. Games can register their own.
- HDR calibration settings: paper white, peak brightness, UI brightness.

### 12.12 2D in the unified world
- **Sprites** are entities with `Sprite` (texture or atlas region, pivot, size in meters, flip, color, material model `Sprite`). Optional normal, roughness/metal, emissive and height maps. They go through the same visibility-buffer, lighting and RT path as meshes.
- **2D plane conventions:** side view uses the X-Z plane (camera looks along +Y); top-down uses the X-Y plane (camera looks along −Z). 2D physics and navigation use the same mapping (§13.3).
- **RT for sprites:** sprites are thin alpha-tested quads in the BLAS. A per-sprite `thickness` lets shadows from grazing lights work; optional proxy extrusion for strong side lighting.
- **Sorting:** opaque and alpha-tested sprites use depth. Translucent sprites sort by `layer`, then `order`, then depth.
- **Tilemaps:** chunked meshes generated from tilemap data (§21.4), lit like sprites.
- **Pixel-art mode (first class):**
  - Render at a low internal resolution (e.g. 480×270), upscale by the largest integer factor that fits, letterbox the remainder (or optional fractional "sharp bilinear").
  - Camera snaps to texel grid; sub-texel smooth scrolling via an offset in the final upscale.
  - Sprite position snapping, nearest sampling, no mips for pixel textures, palette and ordered-dither post hooks.
  - Lighting and RT effects run at internal resolution, so lighting stays pixel-aligned.

### 12.13 World rendering systems

| System | v1 scope |
|---|---|
| **Particles / VFX** | GPU-simulated emitters (spawn, update, sort, render) as compute; sprite, ribbon/trail, mesh, and light particles; depth and SDF collisions; lit through clustered lights; parameters exposed via reflection. Authoring as data assets (node graph deferred with the editor). |
| **Sky and atmosphere** | Physically based sky (precomputed transmittance/multi-scatter LUTs), sun/moon, time of day; sky feeds IBL and RT miss shading. |
| **Volumetrics** | Froxel volumetric fog lit by clustered lights and shadows; local fog volumes; volumetric clouds (raymarched, weather map). |
| **Terrain** | Heightmap terrain with GPU-driven LOD (clipmap or quadtree), splat material layers with height blending, holes, runtime sculpt API for the editor later; double-precision streaming cells. |
| **Foliage** | GPU-instanced placement from density maps, wind animation, LOD/impostors, alpha-tested with RT any-hit. |
| **Decals** | Deferred decals writing into the thin G-buffer (base color, normal, roughness), clustered for culling. |
| **Water** | Ocean (FFT waves, LOD grid), rivers/lakes (flow maps), screen-space or RT reflections/refraction, underwater fog, buoyancy queries for physics. |
| **Large worlds** | World partitioned into streaming cells; camera-relative rendering; origin per cell for GPU data; far LOD proxies. |

---

## 13. Physics (own)

### 13.1 3D
- **Shapes:** sphere, capsule, box, cylinder, convex hull, triangle mesh (static), heightfield, compound.
- **Broadphase:** dynamic AABB tree, separate trees for static and dynamic bodies.
- **Narrowphase:** SAT for box/box and polygon features, GJK/EPA for general convex, specialized sphere/capsule paths, persistent contact manifolds.
- **Solver:** sub-stepped soft-step solver (TGS-style), warm starting, restitution, friction (Coulomb, anisotropic optional), sleeping by islands.
- **CCD:** speculative contacts; optional swept tests for bullets.
- **Joints:** fixed, hinge, ball, slider, distance, cone-twist, 6-DOF with limits and motors.
- **Queries:** raycast, shape cast, overlap, closest point; filtered by layer masks.
- **Character controller:** kinematic capsule, slopes, steps, ground snapping, moving platforms, pushing bodies.
- **Large worlds:** body positions stored in double, solver works in float relative to island origin.
- **Integration:** physics runs in `FixedUpdate`. Bodies are ECS components; the physics world is a resource. Interpolated transforms for rendering in fixed-step mode.

### 13.2 Determinism
- Follows §5.7. Bodies, contacts, and constraints are processed in stable ID order.
- Parallelism: island-level and graph-colored constraint batches, colored deterministically.
- Same inputs → identical checksums on every platform and arch (tested, §19.4).

### 13.3 2D
- Separate module with the same design: circle, capsule, box, convex polygon (≤8 verts), chain/segment shapes; same solver structure; joints (revolute, prismatic, distance, weld, wheel, mouse), queries, 2D character controller.
- Operates in the 2D plane mapping of §12.12.

### 13.4 Deferred
Ragdolls built from joints are v1. Cloth, soft bodies, vehicles, and destruction are open questions (Q14).

---

## 14. Audio (own)

- **Backends:** WASAPI (shared, event-driven; exclusive optional), PipeWire (ALSA fallback), Core Audio (AudioUnit). Device hot-plug and default-device switching.
- **Mixer:** float32, configurable sample rate (48 kHz default), fixed block size. Runs on a dedicated real-time thread. No allocation or locks on the audio thread; commands arrive through a lock-free queue, sample-accurately scheduled.
- **Graph:** voices → buses → master. Effects per bus: EQ, low/high-pass, compressor/limiter, reverb (algorithmic; convolution optional), delay. Ducking/sidechain.
- **Sources:** in-memory PCM/ADPCM, streamed Vorbis/Opus, procedural callbacks.
- **3D:** distance attenuation curves, cone attenuation, Doppler, stereo/surround panning; HRTF binaural option for headphones; occlusion/obstruction via physics raycasts; reverb zones.
- **Large worlds:** listener-relative positions computed in double before conversion.
- **API:** play/stop/fade, parameters, voice priority and virtualization (inaudible voices tracked but not mixed), snapshots/mix states.

---

## 15. Input

- SDL3 events → engine input state (keyboard, mouse, gamepad, gyro/accelerometer, text input).
- **Action maps:** named actions (button, 1D axis, 2D axis) with bindings, composites (WASD → 2D axis), modifiers (deadzone, invert, scale, response curve), interactions (press, hold, tap, double-tap). Context stacks (gameplay, menu, vehicle) enable/disable map sets.
- **Rebinding:** runtime API to listen for the next input, conflict detection, save to user settings.
- **Haptics:** rumble, trigger rumble, adaptive triggers (DualSense), HD haptics where SDL3 exposes them.
- **Gyro:** gyro aiming with calibration and "gyro ratcheting" button.
- **Steam Input / Steam Deck:** via Steamworks (§21.1) when present, SDL3 otherwise; glyphs for prompts.
- **Recording/replay:** the resolved action stream is recorded per fixed tick. Replays feed the same stream back, used for determinism tests, bug reproduction, and rollback netcode.

---

## 16. Networking

### 16.1 Transport
- UDP with connection handshake (challenge tokens), reliable-ordered, reliable-unordered, and unreliable channels, fragmentation and reassembly, congestion control, MTU discovery.
- Encryption and authentication with a vetted C crypto library (§22). Optional Steam Networking Sockets backend (§21.1).
- Network condition simulator (latency, jitter, loss, duplication) in Dev builds.

### 16.2 Replication models
The game picks one per session. All use the fixed simulation tick.

1. **Server-authoritative** (action/shooter): component-level replication from reflection flags, delta-compressed snapshots against acked baselines, interest management (spatial + custom relevance), client prediction and reconciliation for owned entities, entity interpolation for others, lag compensation (server-side rewind for hit tests), RPCs.
2. **Rollback** (fighting/small sessions): input exchange, prediction of remote inputs, world snapshot/restore (§7.4) and re-simulation up to N frames, input delay setting, desync detection via checksums.
3. **Deterministic lockstep** (RTS/large unit counts): input-only exchange, turn scheduling, checksum-based desync detection with optional state dump for diffing.

### 16.3 Server builds
- Headless dedicated server build (no GPU, audio, window). Linux x86-64 primary, Windows supported.
- Server tick and client tick rates configurable independently.
- Matchmaking, lobbies, and NAT traversal: via Steamworks when available; own relay/lobby service is out of scope (Q15).

---

## 17. UI and text

### 17.1 Debug UI
- Immediate mode. Used for stats, ECS inspector, console, cvars, asset browser and profiler shortcuts. The editor does not use it (§20).
- Candidate: Dear ImGui via the cimgui C API, compiled only in Debug/Dev builds (open question Q1). Rendered through the engine's 2D path.

### 17.2 In-game UI
- Retained. UI nodes are ECS entities (serializable, inspectable, editable later in the editor).
- **Layout:** flexbox subset + grid, anchors, safe areas, DPI scaling.
- **Style:** style sheets as assets (TOML), states (hover, focus, pressed, disabled), transitions/tweens, themes.
- **Widgets:** text, image (9-slice), button, toggle, slider, progress bar, text input (IME), dropdown, scroll view, list/virtualized list, tabs, tooltips.
- **Navigation:** mouse, keyboard, gamepad (spatial focus navigation), action-map driven.
- **Data binding:** bind widget properties to component fields via reflection paths.
- **World-space UI:** panels rendered into the 3D world (MSDF text stays crisp), occluded and lit optionally.
- **Pixel-art UI:** integer-scaled, snapped, bitmap fonts.

### 17.3 Text and localization
- **Fonts:** MSDF for scalable UI and world text; bitmap fonts for pixel art. Fallback font chains (Latin → CJK → emoji).
- **Shaping:** complex scripts (Arabic, Hebrew, Indic, Thai), ligatures, kerning, bidirectional text (UAX #9), line breaking (UAX #14). Library choice: open question Q2.
- **Dynamic glyphs:** pre-baked charsets ship in packs; missing glyphs are generated at runtime into atlas pages (needs a runtime MSDF generator, Q2).
- **Localization:** string tables keyed by ID, per-locale binary tables, placeholders and plural rules (CLDR), locale-specific number/date formatting (basic), per-locale asset overrides (§11.2), runtime language switch, pseudo-localization in Dev.

---

## 18. Tooling, profiling, debugging

### 18.1 Tracy
- `DE_PROFILE_ZONE`, `DE_PROFILE_FRAME`, `DE_PROFILE_PLOT`, lock and memory instrumentation. GPU zones on all three backends. Compiled out in Shipping.

### 18.2 Stats overlay
- Frame time (CPU sim, CPU render, GPU), per-pass GPU times from the render graph, memory by category and budget, instance/meshlet/triangle counts, ray counts, asset streaming queues and residency, audio voices, physics bodies/contacts/islands, network bandwidth/RTT/loss.

### 18.3 GPU debug markers
- Render-graph passes and named regions emit markers: PIX events (D3D12), `VK_EXT_debug_utils` (Vulkan), debug groups (Metal). All GPU objects are named. Captures work in PIX, RenderDoc, Nsight, Radeon GPU Profiler, Xcode.

### 18.4 ECS inspector
- Entity list with filters, hierarchy view, component editing via reflection, archetype/table browser, system list with timings and access sets, event monitor.

### 18.5 Crash reporting
- CPU crashes: Windows minidumps (`MiniDumpWriteDump`), Linux/macOS signal handlers writing a backtrace + minidump-compatible report.
- GPU crashes: D3D12 DRED, Vulkan `VK_EXT_device_fault` (+ vendor tools optional), Metal command buffer error reporting; render-graph breadcrumbs written before each pass.
- Report bundle: dump, log tail, cvars, GPU caps, build ID. Upload hook is game-provided (engine ships no backend).

### 18.6 Console
- Command console in debug UI: cvars, registered commands, log view, history, autocomplete.

---

## 19. Testing and CI

### 19.1 Unit tests
In-tree minimal test framework. Every module has unit tests. Fuzz targets (libFuzzer) for every parser: TOML manifest, glTF/FBX import, pack reader, net packets, save files.

### 19.2 Golden-image tests
- Scenes rendered on each backend at fixed settings; compared with stored references using the FLIP perceptual metric and per-scene thresholds.
- References per backend × tier. The path tracer's converged image is a cross-backend sanity reference.
- Covers: materials gallery (every layer), lights, shadows, each RT effect, fallbacks, 2D/pixel-art scenes, tonemappers, HDR encode.

### 19.3 CI matrix
- Build: Windows (clang-cl, MSVC), Linux (clang, GCC), macOS (Apple Clang) × Debug/Release. Headless server build.
- Unit tests on all. Sanitizer jobs: ASan, UBSan, TSan.
- GPU tests: hosted runners lack GPUs, so non-RT golden tests use WARP (D3D12) and lavapipe (Vulkan) where they support the needed features; RT and full golden tests run on self-hosted runners: Windows (NVIDIA and AMD), Linux (AMD/RADV), macOS (M3 or later).
- Include-graph check (§4.2), header C/C++ compile check, ABI check on release branches.

### 19.4 Determinism tests
Recorded input scenarios (physics stacks, ragdolls, character movement, netcode rollback sessions) run on Windows x86-64, Linux x86-64, macOS arm64. Per-tick world checksums must match exactly.

### 19.5 Performance tests
Fixed camera paths in benchmark scenes on the self-hosted GPU runners; frame-time regressions above a threshold fail the build.

---

## 20. Editor (deferred)

**Build only after every runtime system in this spec is implemented and tested.** The editor is the DEngine IDE, specified in Disegno spec **DIDE**. It is a Tauri + Monaco desktop app (macOS and Windows) in its own repo that combines the code IDE and the scene/content editors. It replaces the earlier `dengine-editor` plan. It uses only the public engine API; it adds no special paths to the runtime.

### 20.1 Runtime requirements now (so the editor needs no engine changes later)
- Complete reflection for every component, asset type, and setting (§9).
- World snapshot/restore (§7.4) for play-in-editor.
- Multiple views and render-to-texture viewports (§12.4).
- Picking: read back the visibility buffer (instance, triangle) under the cursor.
- Asset hot reload and in-process cooking (§11.9).
- Command buffer for structural changes (§7.1) usable as the basis for undo/redo.

### 20.2 Engine side
- The engine runs in its own process; a crash or debugger break never takes the IDE down.
- **Editor service** (Dev builds only): serves reflection, entity/component state, picking and asset status, and applies edit commands through the command buffer, over versioned local IPC (named pipe on Windows, Unix socket on macOS).
- **Shared-texture viewports:** each editor view renders into a GPU texture shared with the IDE process (IOSurface on Metal, DXGI shared NT handle on D3D12). The IDE presents it in a native view and forwards input over IPC.

### 20.3 Features
Planned in DIDE, in three phases: code IDE; engine integration (viewport, play-in-editor, scene view, inspector, undo/redo); content editors (asset browser, material, prefab, terrain, foliage, particle and animation editors, tilemap viewer, project settings).

---

## 21. Gameplay support, animation, services

### 21.1 Steamworks
Optional module. Uses the Steamworks flat C API (`steam_api_flat.h`). The SDK is proprietary and is **not vendored**; developers download it and point CMake at it. Features: achievements, stats, cloud saves, rich presence, Steam Input, Steam Networking Sockets (transport backend for §16), lobbies/matchmaking.

### 21.2 Navigation
- 3D: navmesh generation from physics/static geometry (voxelization-based, tiled for streaming and runtime rebuild), path queries, string pulling, off-mesh links, dynamic obstacles, crowd steering (local avoidance).
- 2D: grid and polygon navmesh on the 2D plane, A* / JPS.
- Deterministic query mode for simulation use (§5.7).
- Library choice: own vs vendored Recast/Detour (C++, zlib) → open question Q1.

### 21.3 Mod support
- Mods are packs (§11.7) mounted above the base game, plus optional manifest additions.
- Asset overrides by ID; new assets and bundles; load order and conflict report.
- Code mods require the scripting layer (§2, Q5). Sandboxed WASM is the planned direction, so native code is never loaded from mods.
- Steam Workshop hookup through §21.1.

### 21.4 Tilemaps, Aseprite, video
- **Tilemaps:** LDtk and Tiled import, layers, auto-tiles (from source data), tile animation, collision shapes → 2D physics, entity spawns → prefabs.
- **Aseprite:** direct `.aseprite` import: layers flattened (or kept), tags → animation clips, slices → 9-slice/pivot data, frame durations.
- **Video:** AV1 decode via dav1d (BSD-2), Opus audio, WebM/MKV container. Output to a texture (full-screen cutscenes, in-world screens), audio through the engine mixer, A/V sync to the audio clock. Royalty-free codecs only in the engine (H.264/HEVC via OS decoders is open question Q16).

### 21.5 Animation
- **Skeletal:** skeletons and clips from glTF/FBX; clips stored as compressed, quantized curves. Sampling on the task graph; skinning on the GPU (compute), whose output also feeds BLAS refits (§12.8).
- **Blend trees:** 1D and 2D blend spaces, additive layers, per-bone masks, sync groups (foot phase).
- **State machines:** states, transitions with conditions on float/bool/trigger parameters, transition blending, interruptions, sub-state machines, events and markers on clips.
- **Root motion:** extracted per clip, applied to the entity transform or to the character controller (§13.1).
- **IK:** two-bone, FABRIK/CCD chains, look-at, foot placement using physics raycasts. Runs after blending, before skinning.
- **2D:** sprite-sheet animation from Aseprite tags (§21.4) or atlas frame lists; ping-pong, loop, events.
- **Determinism:** animators flagged deterministic sample in `FixedUpdate` with `de_dmath` (needed when animation drives gameplay, e.g. hitboxes in rollback games).
- **Authoring:** animation graphs are data assets (TOML) until the editor's graph editor exists (§20.3).

---

## 22. Third-party dependencies

All vendored and pinned in `third_party/`, each with its license file. ✔ = planned, ? = candidate pending open question.

| Library | Use | Language | License | |
|---|---|---|---|---|
| SDL3 | platform layer | C | zlib | ✔ |
| Joe-Kuo Sobol direction data (SciPy snapshot) | path-tracer sample matrices; data only | — | BSD-3-Clause | ✔ |
| Slang | shader compiler (tools, dev hot reload) | C++ (C API) | Apache-2.0 w/ LLVM exception | ✔ |
| D3D12 Agility SDK, DXC runtime libs | D3D12 | — | Microsoft redistributable | ✔ |
| Vulkan headers, VMA (optional) | Vulkan | C / C++ | Apache-2.0 / MIT | ✔ |
| AMD FidelityFX FSR | upscaling (D3D12/Vulkan) | HLSL/C++ | MIT | ✔ |
| MetalFX | upscaling (macOS) | system framework | — | ✔ |
| cgltf | glTF import | C | MIT | ✔ |
| ufbx | FBX import | C | MIT / public domain | ✔ |
| stb_image, stb_vorbis | PNG/JPG decode, Vorbis | C | MIT / public domain | ✔ |
| tinyexr or own | EXR import | C++ | BSD-3 | ? |
| libktx / Basis Universal | KTX2 import, transcoding | C/C++ | Apache-2.0 | ✔ (tools) |
| bc7enc_rdo or similar | BC compression | C/C++ | MIT / public domain | ✔ (tools) |
| meshoptimizer | meshlets, LOD, quantization | C++ (C API) | MIT | ✔ (tools) |
| dr_wav, dr_flac | WAV/FLAC | C | MIT / public domain | ✔ |
| libopus | Opus streams | C | BSD-3 | ? (Q11) |
| tomlc17 (or similar) | TOML parsing | C | MIT | ✔ |
| zstd | compression | C | BSD-3 | ✔ |
| xxHash | hashing | C | BSD-2 | ✔ |
| msdfgen | MSDF generation | C++ | MIT | ? (Q2) |
| HarfBuzz / kb_text_shape | shaping | C++ / C | MIT | ? (Q2) |
| SheenBidi | bidi | C | Apache-2.0 | ? (Q2) |
| Dear ImGui + cimgui | debug UI | C++ (C API) | MIT | ? (Q1) |
| Recast/Detour | navmesh | C++ | zlib | ? (Q1) |
| Monocypher | network crypto | C | BSD-2 / CC0 | ✔ |
| dav1d | AV1 decode | C | BSD-2 | ✔ |
| Tracy | profiling | C++ (C API) | BSD-3 | ✔ (Dev) |
| Steamworks SDK | services | C++ (flat C API) | proprietary, not vendored | ✔ (optional) |

---

## 23. Milestone 1 — renderer-led

Goal: a complete visual pipeline on all three backends before gameplay systems.

**In scope**
- Core (memory, errors/log, math, containers, cvars), platform (SDL3), task graph, minimal ECS (entities, components, queries, schedules, extract), reflection (enough for renderer types).
- Asset system with TOML manifest, bundles, load/status API, ECS events, dev cooking + cache, packs, hot reload for textures, models, materials, shaders.
- RHI on D3D12, Vulkan, Metal. Render graph. GPU scene.
- Visibility buffer with mesh-shader and compute paths, two-phase occlusion culling.
- Full PBR material set, all light types, base-tier shadows/AO/reflections/GI.
- RT tier: shadows, AO, reflections, GI, own denoiser. Path-tracing mode.
- FSR / MetalFX / own TAA. HDR + SDR output, pluggable tonemappers.
- 2D sprites in the unified world, pixel-art mode.
- Skinned meshes (compute skinning + BLAS refit) as a renderer feature; animation playback only (full animation system is later).
- Tracy, stats overlay, GPU markers. Golden-image tests on all backends.

**Exit criteria**
- Three reference scenes (large PBR interior/exterior, material gallery, lit pixel-art 2D scene) render on all three backends within golden thresholds, at Base and RT tiers.
- Steam Deck holds 30 fps at Base tier on the exterior scene at native resolution with upscaling.
- No per-frame heap allocations (§5.3 test passes).

Later milestones (physics, audio, input, animation, UI/text, networking, world systems, services, editor) are planned in the next document revision.

---

## 24. Risks

| ID | Risk | Mitigation |
|---|---|---|
| R1 | **Scope.** Three native backends, hybrid RT, a path tracer, own physics, own audio, three netcode models, world systems and an editor is a multi-year, multi-person scope. | Strict milestone gating; every module optional; keep the editor deferred; consider vendoring where "own" is not a differentiator. |
| R2 | Slang's Metal backend may lag D3D12/Vulkan for RT and mesh shaders. | Validate in the first weeks of M1; allow hand-written MSL for specific passes. |
| R3 | Upscalers differ per platform (FSR vs MetalFX), so image quality differs. | Own TAA/TAAU on all backends as a common baseline; golden tests per backend. |
| R4 | macOS 14 baseline lacks Metal residency sets (macOS 15+). | Feature-detect; use heap residency (`useHeap`) on 14. |
| R5 | Base-tier GPUs (Pascal, RDNA1) and SM 6.6 / bindless support vary by driver. | Build a caps-probe tool early; publish a tested hardware list. |
| R6 | Cross-platform float determinism (x86-64 vs arm64) is fragile. | §5.7 rules, determinism CI from the first physics commit, checksum per tick. |
| R7 | RT on M1/M2 and Steam Deck is slow. | Tier auto-detection defaults those devices to Base tier. |
| R8 | Full-featured shaping and runtime MSDF generation pull in C++ dependencies. | Q2; isolate behind C interfaces. |
| R9 | Large-world doubles add cost to transforms, physics and audio. | Doubles only for world position; camera/island-relative floats everywhere else. |

---

## 25. Open questions (interview round 2)

| ID | Question |
|---|---|
| Q1 | May the runtime use C++ libraries behind C interfaces (Dear ImGui, Recast/Detour, HarfBuzz, msdfgen), or must the runtime be pure C (tools may still use C++)? |
| Q2 | Text stack: HarfBuzz + SheenBidi + msdfgen, a pure-C alternative, or own? Runtime glyph generation required (CJK, user input)? |
| Q3 | Intel Macs (x86-64 + AMD GPUs): supported or not? Windows on ARM / Linux arm64 timing? |
| Q4 | Performance targets per tier: resolution and fps (e.g. Deck 30/60, RT Ultra 4K60 with upscaling)? |
| Q5 | Scripting direction: Lua/LuaJIT, WASM (also enables mod code), or C only until demand appears? |
| Q6 | Team size and timeline. Solo? Shapes how much is "own" vs vendored (R1). |
| Q7 | Documentation: generated API reference, tutorials, sample games? Which sample game ships with v1? |
| Q8 | Versioning and ABI policy: semantic versioning, ABI stability guarantees per major version? |
| Q9 | Text scene format: TOML, or a dedicated format better suited to large nested entity data? |
| Q10 | Max world size and streaming cell size for large worlds? Any multiplayer large-world use (server-side streaming)? |
| Q11 | Streaming audio codec: Vorbis only, Opus only, or both? |
| Q12 | Extra material models: hair, eye, cloth, car paint? |
| Q13 | RT GI approach: ReSTIR GI, RT-updated irradiance probes (DDGI-style), or both? ReSTIR DI for many lights? |
| Q14 | Physics extras beyond ragdolls: vehicles, cloth, soft bodies, destruction? |
| Q15 | Networking without Steam: own relay/lobby service, or Steam-only for matchmaking? Max players per session? |
| Q16 | Video: allow H.264/HEVC through OS decoders (Media Foundation, VideoToolbox, VA-API) in addition to AV1? |
| Q17 | Accessibility features in engine scope (subtitles system, colorblind filters, remapping UI, screen reader hooks)? |
| Q18 | Particle authoring before the editor exists: data files only, or a standalone particle tool? |
| Q19 | Asset manifest: should bundles support platform/quality variants (e.g. low-res textures for Deck)? |
| Q20 | Should the cooker run as a background service (shared cache across machines / CI cache server)? |

---

## 26. API sketch index

All headers compile with `-std=c11 -Wall -Wextra -Wpedantic -Werror` (GCC, Clang) and as C++17. They are sketches: names and shapes are proposals, not frozen ABI.

| Header | Covers |
|---|---|
| `dengine.h` | Umbrella include |
| `de_base.h` | Types, handles, `de_result`, error context, asserts, logging |
| `de_memory.h` | Allocator interface, arenas, frame/scratch, pools, TLSF |
| `de_math.h` | Vectors, matrices, transforms (double position), fast + deterministic math, RNG |
| `de_platform.h` | Windows, events, time, threads, files, async I/O, file watch, dylibs |
| `de_task.h` | Work-stealing scheduler, parallel-for, task graphs |
| `de_reflect.h` | Type registry, field metadata, serialization, path access |
| `de_ecs.h` | World, entities, components, resources, events, queries, commands, systems, snapshots |
| `de_asset.h` | Manifest, bundle/asset load hook, status, ECS events, custom types, streaming |
| `de_gpu.h` | Bindless RHI: resources, pipelines, acceleration structures, commands, sync, swapchain |
| `de_render.h` | Renderer, settings/tiers, views, render graph + hooks, tonemappers, render/2D/world components |
| `de_anim.h` | Animators, parameters, IK, sprite animation |
| `de_physics.h` | 3D + 2D bodies, shapes, joints, queries, character controllers, contact events |
| `de_audio.h` | Devices, buses, effects, voices, 3D sources, snapshots |
| `de_input.h` | Devices, action maps, rebinding, haptics, gyro, record/replay, input packing for netcode |
| `de_net.h` | Sessions, peers, replication, RPCs, lag compensation, rollback/lockstep input |
| `de_text.h` | Fonts, shaping/layout, localization |
| `de_ui.h` | Retained UI components, events, debug UI |
| `de_gameplay.h` | Navigation, tilemaps, video |
| `de_services.h` | Steamworks, mods |
| `de_debug.h` | Profiling macros, cvars, console commands, stats panels, crash handler |
| `de_game.h` | Host, `de_engine`, timing modes, game API for DLL/static linking |

Examples: `examples/assets.toml` (full manifest), `examples/minimal_game.c` (game module using the load hook, status polling, a deterministic system).
