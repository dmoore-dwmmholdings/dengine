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

## Glass refraction contribution verification (DENG-462)

The glass fixture omitted `metallic`, whose material-format default is 1. Both capture and
forward shading correctly scale dielectric transmission by `(1 - metallic)`, so its original
transmission was zero despite the authored transmission factor of 1. The glass material now
explicitly sets metallic 0. All other material values, geometry, camera, lighting, render
settings, seed epochs, 32-frame warmup and comparison thresholds remain unchanged.

The original fixture fails RGB nonvacuity. The corrected two-event RT producer has 466 affected
pixels and RGB sum 401.921. Independent RT captures are identical, and RT and Base now differ
in 1,392 decoded RGBA bytes. Disabling only the actual `rt_refraction` setting retains ordinary
screen-space fallback, reproduces Base output exactly, and fails both original limits:
mean FLIP 0.020695 and p99 0.545914. Restoring RT reproduces the accepted capture exactly.
The verifier now requires this additional negative comparison and restoration. The missing
Metal RT reference and the Base reference were captured from the corrected fixture and
visually reviewed; the initial failed RT capture remains archived. Strict Metal validation
and ASan/UBSan pass. This resolves the historical glass qualification above.

## Shadow negative-control coverage (DENG-465)

The original shadow fixture combined a metallic-default floor with an 18 cm square caster.
Correcting only the floor still let shadow removal pass (mean FLIP 0.001250, p99 0.012747):
the caster's shadow had too little screen area for the original global image tolerances.
The fixture now uses an explicit metallic-0 floor and a 1 m square caster at the original
position and height. Camera, sun, sample epochs, 32-frame warmup and thresholds are unchanged.
Only this scene's RT/Base references were recaptured and reviewed.

Disabling actual shadows removes `RT.Shadows` and now fails both original limits, with mean
FLIP 0.005593 and p99 0.147135. Restoring shadows reproduces the accepted image exactly.
The active producer has 75 affected visibility pixels and range [0,1]. Strict Metal and
ASan/UBSan RT verification pass. This resolves the historical shadow negative-control failure.

## Sanitizer RT run (DENG-459, still open)

All seven production RT goldens completed under ASan/UBSan and strict Metal API/shader
validation in 23.03 seconds total, within their unchanged 300-second per-test limits.
Shadows (4.10 s), DI (3.25 s), reflections (3.16 s), GI (3.99 s), refraction (3.00 s), and
foliage (3.05 s) pass. AO completes in 2.42 s but fails its unchanged image comparison:
mean FLIP 0.027015 and p99 0.235291. That failure and the existing reference are preserved
for source-versus-reference investigation; this is not an all-green sanitizer result.
The separate full path-tracer shader-validation hang is not covered by these scene tests.

## Many-light Base stability (DENG-464)

The cluster fill previously retained the first `max_per_cluster` entries from an atomically
compacted light list. Both the capped light set and floating-point summation order therefore
depended on scheduling. A controlled GPU regression permutes the same 24 physical lights
three ways and checks caps 1, 4, and 24: the original shader changes selected records and actual
HDR pixels; the corrected shader produces identical HDR data for every permutation.

Cluster fill now uses its existing output range as a bounded max heap. A stable spatial hash
avoids systematically selecting one coordinate half-space, and complete record bits resolve
key ties. The retained set is sorted before shading, including uncapped clusters. This is a
stable bounded selection policy, not an importance estimate. Complexity is O(N log K), with
no additional buffers, allocations, or public ABI changes; existing caps and overflow counters
are preserved. Two initial fresh captures happened to agree before the source fix, so the
controlled regression, rather than those captures, establishes the order-dependent defect.

Base DI verification now requires the actual deferred-lighting fallback, at least 512 uploaded
light slots, an inactive DI owner and absent DI input textures. A shadow-atlas draw is not a
requirement when the atlas admits none of the local lights. The corrected original scene has
three independent captures with identical RGBA pixels; its previously missing reviewed Metal
Base reference is installed. No scene settings, materials, geometry, light counts, seeds,
warmup or thresholds changed. Clustered-light suites pass in Dev and ASan/UBSan (18 CPU/Metal
checks each, 22 unavailable-backend skips each); strict Metal golden checks cover both DI tiers.

## Sanitizer RT completion (DENG-459)

All seven production RT goldens now pass under ASan/UBSan and strict Metal API/shader
validation on Apple M5 Pro: shadows 5.51 s, DI 10.44 s, AO 2.47 s, reflections 3.14 s,
GI/refraction/foliage also pass; total 31.46 s. Their original 300-second per-test timeouts
are unchanged. This supersedes the intermediate AO failure recorded above and does not
claim to resolve the separate full path-tracer shader-validation hang (DENG-455).

The AO mismatch was caused by a frozen image captured before the existing scalar-channel,
filter-footprint and progressive sampling fixes. As a controlled ablation, substituting only
the capture-era RTAO and four denoiser shaders reproduced the original reference exactly,
including file SHA-256 `8f8efd2d5d70a2a997083b2946702c0ee47adf0d314b484af5d7034dd91b70d1`.
All temporary substitutions were restored before final verification. The current shader
suite passes 29 original AO/denoiser checks, including five independent CPU visibility
probes (worst relative error 0.02482623 against 0.03) and the original denoised AO reference
criterion. Its published AO has 2,064 affected pixels and range [0.298039, 1].

Two independent current RT captures have identical RGBA pixels. The corrected image was
visually reviewed and installed as the RT AO reference; the Base AO reference remains
unchanged and passes. No production code, fixture, seeds, 32-frame warmup, sample counts,
comparison limits or timeouts were changed for this completion.

The complete strict-Metal verifier also passes: 34 real captures, all 14 RT/Base repeat pairs
byte-identical, all three actual-effect negative comparisons rejected, and all three restored
outputs identical. The normal verifier changed no references or thresholds during this run.

## Related reflection acceptance (DENG-452, DENG-453)

The moving-mirror test's fixed middle-row ray intersects the mirror near Y=1.2, beyond the
original colored cube at Y=0. Its reflected-edge strength was exactly zero. Moving that same
cube 3 m along Y puts it beyond the intersection, where the reflected ray can hit it. The fixed
probe, both camera poses, 0.3 m strafe, 512-sample reference, 24-frame history, two-frame motion
window and two-pixel limit are unchanged. The test additionally requires real reflected-hit
coverage. It now measures 158 colored closest-hit pixels, edge strength about 1.8, old edge
32.0003392, reference edge 30 and frame-two edge 29.9626856: displacement error 0.0373144 pixels.

The original reflection/material-AO/RTAO composition check passes on the current renderer
without any fixture, assertion or threshold changes: 42 affected reflected pixels (>10),
material-AO normalized L1 0.004348528 (<0.005), and once-only HDR composition L1 0.0022220508
(<0.01). This validates the existing repairs rather than claiming a new composition algorithm.
The six GI/specular denoiser cases plus composition pass in Dev and ASan/UBSan with Metal
shader validation enabled (7/7 each). A broader Dev reflection run passes 16/17; the separate
rough-sphere accumulated-vs-1024-sample normalized pixel L1 remains 0.062116629 against its
unchanged 0.03 limit. That failing acceptance is preserved for further investigation.
