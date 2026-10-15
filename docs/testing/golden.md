# Golden-image tests

The golden-image harness renders small scenes on each GPU backend and compares them with stored
reference images (SPEC §19.2). It lives in `tests/golden/` (DENG-329, S10). Every later rendering
sprint adds scenes to it.

## How it works

1. `de-golden` (`tests/golden/harness/`) creates a device for `--backend`: Metal, Vulkan or D3D12.
2. For each selected scene and each tier of the scene that the device has, it creates an offscreen
   target (no window, no swapchain), records the scene into one command list, submits it and
   reads the target back.
3. It compares the image with `tests/golden/refs/<backend>/<tier>/<scene>.png` by LDR-FLIP. The
   scene passes when the mean FLIP error is at most `threshold_mean` and the 99th percentile is at
   most `threshold_p99` (`tests/golden/scenes.toml`).
4. Any engine log record of level ERROR or above during the scene fails it. In Debug and Dev
   builds that includes the API validation (Vulkan validation layer, D3D12 debug layer; the
   CTests set `MTL_DEBUG_LAYER=1` and `MTL_SHADER_VALIDATION=1` for Metal).

Targets are RGBA8 (`format = "rgba8"`, the bytes are taken as sRGB values) or RGBA16F
(`"rgba16f"`, tone-mapped with the ACES fit and the sRGB curve before the comparison). The
default size is 512 x 512.

## Running it

```sh
ctest --preset dev -L golden                       # FLIP tests + every scene on every backend
ctest --preset dev -R 'golden\.metal\.'            # one backend
build/dev/tests/golden/de-golden --backend metal --scene 'rhi_*' --out /tmp/golden
build/dev/tests/golden/de-golden --list            # the scenes (also checks scenes.toml)
```

| Option | Meaning |
|---|---|
| `--backend metal\|vulkan\|d3d12\|default` | The backend. `default` is the platform's default. |
| `--warp` | D3D12 only: run on WARP, the software adapter. The D3D12 CTests use it when the vendored WARP is deployed (DENG-405, `third_party/README.md` rule 10). |
| `--adapter N` | The N-th adapter (1-based) instead of the best one. |
| `--scene <glob>` | Scenes to run (`*` and `?`). Default: all. |
| `--tier base\|rt` | Only this tier. Default: every tier the scene lists. |
| `--out <dir>` | Writes `<dir>/<backend>/<tier>/<scene>.png` for every scene. For a failed comparison it also writes `<scene>.ref.png` (the reference) and `<scene>.flip.png` (the FLIP heat map: black is no difference, yellow is the largest). |
| `--update-refs` | Writes the images as the new references instead of comparing. It refuses to run when the environment variable `CI` is set. |
| `--refs <dir>`, `--config <file>` | Another reference directory or scene list (the harness's own tests use them). |

Exit status: 0 passed, 1 failed, 2 usage or configuration error, 77 everything was skipped.

A scene reports **SKIPPED** with the reason when the backend has no device on this machine, the
device is below the scene's tier, the device lacks a cap in `requires` (for example `missing cap
mesh_shaders`), the build cannot cook shaders (no tools or no Slang), or the host cannot cook the
backend's shader target (DXIL without DXC).

CTest names: `golden.<backend>.<scene>` for each backend the build has (labels `golden;gpu`), plus
`golden.config` (the scene list and `scenes.toml` match) and `golden.update_refs_refused_on_ci`.
The FLIP unit tests are `flip.<test>` (labels `unit;golden`). The CTests write their images to
`<build>/golden/`.

## Adding a scene

1. Write the scene in a file under `tests/golden/scenes/`. A new file goes in
   `DE_GOLDEN_SCENE_SOURCES` in `tests/golden/CMakeLists.txt`.

   ```c
   #include "golden.h"

   DE_GOLDEN_SCENE(my_scene) {
       de_golden_shader vs, ps;
       if (!de_golden_get_shader(ctx, "my.slang", "vs_main", &vs) || ...)
           return;
       de_gpu_pipeline p = de_golden_pipeline(ctx, &desc);
       de_golden_begin_render(ctx, (const float[4]){0, 0, 0, 1}, (de_gpu_texture){0});
       de_gpu_cmd_set_pipeline(ctx->cmd, p);
       ...
       de_golden_end_render(ctx);
   }
   ```

   `tests/golden/harness/golden.h` has the API and the rules: fixed settings only, record into
   `ctx->cmd`, use the helpers for buffers, textures, samplers and pipelines (the harness destroys
   them), and use GPU_ONLY buffers (`de_golden_storage_buffer`) for data that shaders read.
   `tests/golden/scenes/rhi_basic.c` has examples of each.
2. Put the shaders in `tests/golden/shaders/`. They are cooked in-process for the backend when a
   scene first asks for them.
3. Add a `[scene.my_scene]` table to `tests/golden/scenes.toml`. `de-golden` refuses a scene
   without a table and a table without a scene. Re-run CMake: it reads the table names for the
   CTests.
4. Make the references: `de-golden --backend <b> --scene my_scene --update-refs` on a machine of
   each backend, or take the image from the CI artifact (below). Look at every image before you
   commit it.

### `scenes.toml` keys

| Key | Default | Meaning |
|---|---|---|
| `threshold_mean` | required | Largest mean FLIP error that passes. |
| `threshold_p99` | required | Largest 99th-percentile FLIP error that passes. |
| `backends` | all three | The backends the scene is for; others report SKIPPED. |
| `tiers` | `["base"]` | `"base"` and/or `"rt"`. Each tier has its own references. |
| `requires` | none | Caps the device must have: `mesh_shaders`, `ray_query`, `rt_pipelines`, `hardware_rt`, `async_compute`. |
| `size` | `512` | `N` or `[width, height]`. |
| `format` | `"rgba8"` | `"rgba8"` or `"rgba16f"`. |
| `reference` | the scene | Use another scene's references (the self test). |
| `expect_fail` | `false` | The harness self test: the scene passes only when the comparison fails and the heat map was written. |
| `description` | none | Free text. |

### Choosing thresholds

References are per backend, so the machine that made a reference reproduces it exactly (FLIP 0).
The thresholds are for other devices and drivers of the same backend, which can differ by about
one code value in interpolation, filtering and edge rasterization. LDR-FLIP rates a one-value
difference on flat color at about 0.02 and an 8/255 difference at about 0.14. The `rhi_*` scenes
use a mean of 0.005 and a p99 of 0.02 to 0.05. The self test `rhi_selftest_perturbed`
(`rhi_triangle` with every vertex color off by 8/255) gives a mean of 0.065 and a p99 of 0.23, so
it fails, as it must.

## References

- Path: `tests/golden/refs/<backend>/<tier>/<scene>.png`. `<backend>` is `metal`, `vulkan` or
  `d3d12`; `<tier>` is `base` or `rt` (CONVENTIONS §5: per backend × tier).
- They are RGB PNGs (alpha is not stored and FLIP does not compare it), kept in git without LFS.
- **Size budget:** at most 128 KB per image and 512 KB per `<backend>/<tier>` directory. The
  `rhi_*` scenes use about 175 KB per backend. Prefer flat colors and few edges; a soft gradient
  over the whole image costs the most. A scene that needs more than the budget uses a smaller
  `size`.
- Which machine makes them:
  - Metal: an Apple silicon Mac (the build host, an M5 Pro). The macOS hosted runners' Metal
    device is below the Base tier, so CI skips the Metal scenes; self-hosted Apple GPU runners
    come in S20.
  - Vulkan: Mesa lavapipe on the CI job `golden linux vulkan (lavapipe)` (Ubuntu 24.04).
  - D3D12: the vendored WARP (`Microsoft.Direct3D.WARP`, DENG-405) on the CI job
    `golden windows d3d12 (WARP)` and in the Windows build jobs.
- **Never in CI.** `--update-refs` refuses to run when `CI` is set. To take a CI image as a
  reference: let the job fail, download its artifact `golden-<backend>`, check the image, and copy
  `<backend>/<tier>/<scene>.png` into `tests/golden/refs/` (same layout).
- When a change alters a scene on purpose, update the references of every backend in the same
  change, and say why in the commit message.

## The materials gallery (DENG-307)

`tests/golden/materials/` holds one of the three Milestone 1 reference scenes (SPEC §23). S20
(DENG-278) reuses it, so its path and scene names stay. Its scenes run the engine as a game does:
the asset system in SOURCE mode over `materials/assets/`, which cooks the glTF meshes, PNG and EXR
textures, `.mat.toml` materials and `.scene.toml` scenes on load, then the renderer, an ECS world
and `de_prefab_instantiate`.

| Scene | What |
|---|---|
| `materials_gallery_ibl` | every material layer, one row each, lit by a sky environment |
| `materials_gallery_lights` | the same gallery lit by a sun, a point, a spot, a rect and a disk light |
| `materials_furnace` | white spheres in a uniform environment; C checks every sphere interior is within 2 % of the radiance × exposure |
| `materials_khr_samples` | procedural stand-ins for five Khronos sample models (`assets/LICENSES.md`) |

- `assets/gen_assets.py` writes every asset (Python 3 standard library). Edit the script, run it,
  re-bake the references.
- The cook cache is per backend in the build tree (`tests/golden/materials_cache/`).
- A scene runs frames until at most 1 % of its pixels change between frames, three frames in a
  row, and compares the per-pixel median of those three. This damps the flicker of textured
  surfaces under Metal shader validation (DENG-430).
- Determinism: a fixed EV100 on each scene's `DeCamera`, `output.basic` as the output stage, no
  temporal effects.
- A layer that renders wrong gets a follow-up issue against its owning task, and its references
  are re-baked when the fix lands. Thresholds are not loosened for it. The references hold known
  gaps, listed in `materials_scenes.c`.

## LDR-FLIP

`tests/golden/flip/` is a C implementation of LDR-FLIP (Andersson et al., HPG 2020), written from
the published algorithm: the color pipeline in YyCxCz with the contrast sensitivity filters, the
Hunt-adjusted HyAB color difference, edge and point feature detection, and the pixels per degree
of a viewing setup (default: 0.7 m wide 3840-pixel monitor at 0.7 m, about 67 PPD).

`flip.reference_pairs` checks it against NVIDIA's official implementation: the image pairs in
`tests/golden/flip/data/` and their error values in `reference.txt` were made once, offline, by
`tests/golden/flip/data/make_reference.py` with `flip-evaluator` 1.7 (BSD-3-Clause; not vendored,
not used by the build). The mean error must match within 1e-3; it matches within about 1e-5.

## CI

`.github/workflows/ci.yml`, job `golden` (full tier only; `docs/ci.md`):

| Job | Runner | Backend |
|---|---|---|
| golden linux vulkan (lavapipe) | `ubuntu-24.04`, `linux-clang-debug` | Vulkan on Mesa lavapipe, with the validation layers |
| golden windows d3d12 (WARP) | `windows-2022`, `windows-clang-cl-debug` | D3D12 on the vendored WARP, with the debug layer |
| golden macos metal (hosted, skips below Base) | `macos-14`, `macos-debug` | Metal: every scene SKIPPED (the runner's device is below the Base tier) |

Each job runs the FLIP tests, the golden CTests and its backend's `example_hello_*` CTests. The
Linux and Windows jobs then require one real render (`de-golden ... --scene rhi_clear` must exit
0), so a backend that skips everything fails the job. On a failure the artifact `golden-<backend>`
(14 days) holds the images, references and heat maps, and the CTest logs.

The Windows build jobs of the matrix also run `golden.d3d12.*` on WARP, because every Windows job
that builds tests deploys the vendored WARP.
