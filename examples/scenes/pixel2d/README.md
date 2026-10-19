# Lit pixel-art reference: dusk ruins

DENG-202 / Milestone 1 reference, side view in world X-Z, camera looking +Y.
The scene is static so image comparisons remain reproducible. It uses the real
SOURCE asset pipeline, scene importer, `de_asset_load_bundle("pixel2d")`, prefab
instantiation, renderer Extract, visibility/material resolve, lighting,
transparency and pixel output. It is a rendering example, not a playable game.

The authored view is 480×270 at 32 pixels/metre. Integer cases output 1920×1080.
The sharp-bilinear mode outputs 2560×1080 with aspect-preserving side bars (the
height-limited fit happens to be 4× at this resolution). Its filtering retains the
known DENG-495 fractional-sharpness defect; this reference does not claim that
bug fixed. The scroll case includes a 1 pixel guard border, a fractional camera
position and residual output scroll. Ordinary sprite transforms remain unchanged
by the renderer's per-view pixel snapping.

Composition: quiet/cool/layered dusk; warm torch and character focus. Two distant
background depths, rising platforms, ground brick rhythm, normal/emissive
character, two point lights, low directional sun, emissive torches, cutout plants,
and smoke/glass at distinct translucent layers/orders. Source texture art uses
fixed 16 colors,8 pixel blocks and 1 pixel details. See [LICENSE.md](LICENSE.md) for
per-file CC0 provenance. All source assets together are under 2 MB.

Regenerate only when intentionally changing the scene:

```sh
python3 examples/scenes/pixel2d/generate.py
cmake --build build/dev --target de_golden -j4
build/dev/tests/golden/de-golden --backend metal --scene 'pixel2d_*' --update-refs
ctest --test-dir build/dev -R 'golden.metal.pixel2d_' --output-on-failure
```

Never update references on CI. The unchanged S10 harness performs actual
offscreen rendering/readback and LDR-FLIP comparison. Per-case mean threshold
is 0.001 and p99 is 0.015. CTests have `golden;pixel2d;gpu` labels.

| Case | Tier | Purpose |
| --- | --- | --- |
| pixel2d_base_int4x | Base | Lighting, cutouts and exact 4× output |
| pixel2d_rt_int4x | RT | Thick sprites in actual RT shadow producer, fixed 32 frames/seed |
| pixel2d_sharp_bilinear | Base | Fractional filtering mode at 2560×1080 |
| pixel2d_subtexel_scroll | Base | Fractional camera, snap and output residual |
| pixel2d_palette_dither | Base | 16 color OKLab palette plus anchored Bayer dither |
| pixel2d_translucent_sort | Base | Glass/smoke overlap with layer/order precedence |

Metal references live in `tests/golden/refs/metal/{base,rt}` and were captured on
Apple M5 Pro. Vulkan and D3D12 are **needs-hw-verify**; no references from those
backends are invented. RT additionally requires hardware ray query. Base shadow
softness/banding and RT numerical defects inherited from prior rendering work
remain visible rather than being hidden by scene-specific shaders.

Negative controls (run without `--update-refs`) select intentionally changed
inputs, never a committed broken renderer:

```sh
DE_PIXEL2D_NEGATIVE=snap build/dev/tests/golden/de-golden --backend metal --scene pixel2d_base_int4x --out /tmp/pixel2d-no-snap
DE_PIXEL2D_NEGATIVE=sort build/dev/tests/golden/de-golden --backend metal --scene pixel2d_translucent_sort --out /tmp/pixel2d-reversed-sort
```

The snap control disables `snap_sprites`. The sort control negates authored
layer/order values, equivalent to reversing the dominant sort-key fields.
Spatial edge/coverage checks also reject blank or flat-color captures before
reference comparison.

Verification on the reference host: all six native captures were visually
reviewed. Negative snapping changed FLIP mean to 0.006452 / p99 0.152312;
reversed sort inputs changed mean to 0.002191 / p99 0.100163, both failing the
original thresholds. The validation-enabled CTest Base case timed out during
pipeline setup at 300.03 seconds (DENG-508); the remaining instrumented batch was
cancelled. Its checked-in environment is preserved. This is a separate result
from direct native image comparisons, not an instrumented-suite pass. Base
horizontal striping is tracked by DENG-506.
