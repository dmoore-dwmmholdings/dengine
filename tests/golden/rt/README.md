# Production RT goldens

These seven procedural scenes and their seven `_base` variants use the original materials
asset loader, real render-world extraction, renderer lighting/forward composition and denoisers.
They are not isolated shader renders. Scene settings are fixed before capture: 128 × 96,
EV100 0, time 0, seed/sample epoch 0–31, exactly 32 frames after asset readiness on a fresh
view. Mean FLIP 0.005 and p99 0.05 are fixed; repeat verification additionally requires
identical decoded RGBA bytes. No adaptive settling or global frame/fence reset is used.

Generate the authored scene/material TOML with `python3 tests/golden/rt/gen_assets.py`.
It references the existing DENG-307 primitive meshes, sky, checker and leaf mask by asset ID;
no duplicated glTF/texture loader or third-party artwork is used. Original asset provenance
is in `../materials/assets/LICENSES.md`; the newly authored scene arrangements/materials are
CC0. The original imported card is XZ with normal −Y; the foliage authoring uses that basis.

Build and verify (from repository root):

```sh
cmake --preset dev
cmake --build --preset dev --target de_golden
python3 tests/golden/rt/verify.py --exe build/dev/tests/golden/de-golden \
  --backend metal --refs tests/golden/refs --out /tmp/rt-golden
```

Explicit local regeneration adds `--capture`. Inspect every image and the actual producer/
consumer diagnostics before accepting new references; keep `results.json` and logs. The
script runs a second process per scene (warm cache versus initial capture), records exact
pixel hashes/differences, disables the actual shadow production setting for one negative
comparison, and restores the ordinary configuration in a separate run. A negative control
that does not fail is a coverage defect, never a reason to tune thresholds.

Reference policy remains `refs/<backend>/<tier>/<scene>.png`. Only actual backend captures
belong there; never copy Metal images into Vulkan/DXR directories or manufacture missing
references. Other backends require their own hardware capture/review. Missing references
on supported hardware are failures. No workflow has been dispatched by this change.

RT variants return 77 before constructing a renderer on non-hardware-RT/API-only devices,
or hosted CI without `DE_RT_GOLDEN_SELF_HOSTED=1`. Base variants retain their normal Base
capability requirement. `DE_RT_GOLDEN_FORCE_UNSUPPORTED=1` exercises the selected RT skip
branch on a capable machine; it does not establish actual unsupported-device execution.
Self-hosted jobs use `rt-nvidia`, `rt-amd`, `rt-radv`, or `rt-m3-plus` runner labels and an
explicit opt-in. Hosted CI needs no RT GPU. The technical testing narrative and measured
qualifications are queued for the project Trama space, rather than duplicated under docs/.

## First Metal capture qualifications

The first fixed-settings pass captured all 14 real images; 11 passed producer diagnostics and
were written as references. Three failed images are retained under `failed-captures/metal/`,
NOT installed as approved references: RT many-light DI, Base many-light fallback, and RT glass.
DI and glass published RGB were zero; Base many-light output was not repeat-identical.
All other pairs were byte-identical, but identical RT/Base GI and glass images do not prove
a rendered effect contribution. GI has nonzero updated atlas data only.

The corrected actual-shadow-disable negative control was NOT detected: mean FLIP 0.000807
and p99 0.005687 pass the fixed 0.005/0.05 limits. This remains a red coverage criterion.
The verifier returns failure for this situation, for a skipped/diagnostic-only negative run,
or for non-identical restoration. No thresholds, warmup, geometry, sample settings or seeds
were retuned after measurement. These observations are queued as deferred bug reports;
source-versus-fixture causes are not established. Vulkan/DXR references are intentionally
absent pending actual hardware captures.

## Many-light DI verification (DENG-461)

The many-light materials omit `metallic`, whose material-format default is 1. Their diffuse
lobe is therefore zero; the original producer check incorrectly measured only that lobe.
The check now reads and combines both published DI RGB textures, excluding alpha. With the
original scene, settings and thresholds, 4,502 pixels carry DI energy (RGB sum 3,690.11).
Two independent captures have identical decoded RGBA bytes. Strict Metal API and shader
validation passes. The reviewed Metal image is now installed as the previously missing
reference; the original failed capture remains archived.

A temporary GPU producer control wrote zero RGB and alpha 1 to both DI textures. It failed
the unchanged nonvacuity requirement and changed 13,506 decoded final-image bytes, confirming
that this scene consumes the lighting signal. The production shader was restored before
final verification. No scene materials, sample counts, warmup or comparison limits changed.
Other first-capture qualifications above remain open.

## Probe GI contribution verification (DENG-463)

The original GI fixture had two independent coverage defects: its box inherited metallic 1
and therefore had no diffuse response, and its camera-following High-quality probe cascades
ended before that receiver. The existing box now uses an explicit metallic-0 material, and
the authored volume's Y extent is 4 instead of 2 so its outer cascade covers the box from the
original camera. Mesh geometry, camera, light, render settings, probe counts, seed epochs,
32-frame warmup and FLIP thresholds are unchanged. Only this scene's RT and Base references
were recaptured and visually reviewed.

The producer proof requires a live cascade containing a point on the receiver, and verifies
that lighting consumes its exact metadata buffer and record, irradiance/distance textures,
and classification buffer. The covered cascade has 2,300 nonzero atlas pixels and RGB sum
3,425.18; inspecting an unrelated nonzero atlas is insufficient. The original Y extent fails
this coverage assertion even with a diffuse receiver.

RT and Base now differ in 313 decoded RGBA bytes; independent captures of each are identical.
Disabling the actual GI production setting changes 471 bytes and fails both original limits:
mean FLIP 0.009310 and p99 0.347733. Restoring GI reproduces the accepted image exactly. The
verifier now requires this GI negative comparison and restoration as well as the existing
shadow control. Strict Metal validation and ASan/UBSan pass for the RT fixture. This resolves
the historical GI contribution qualification above; other open qualifications still apply.
