# Path-tracer references

These scenes run the actual renderer and native RT pipeline at 4096 samples, seed 0, eight bounces, a fixed camera, exposure 1 and final denoise off. `r.pt.golden` enforces fixed sampling strategy/seed and disables optional final denoising. Both scenes assert actual primary geometry, distinct material identities and varying world normals.

`pathtracer_cornell` authors a bounded colored room using the original S14 card mesh by reference. Imported cards face engine -Y; scene transforms include the uniform conversion to the intended +Z authoring basis. `pathtracer_gallery_subset` uses the original S14 `scenes/furnace` assets unchanged (six dielectric/metal spheres with varied roughness in a uniform environment). No original gallery assets or loader are copied.

Capture reviewed local references with the existing harness `build/dev/tests/golden/de-golden --backend metal --scene pathtracer_cornell --update-refs`, and likewise for `pathtracer_gallery_subset`. CI refuses reference updates. Normal CTest registration compares per-backend/tier references; the checked-in captures are actual Metal images. Vulkan/DXR captures require suitable native RT hardware.

For an explicit cross-backend comparison against those Metal images, run `python3 tests/golden/pathtracer/compare.py --exe <de-golden> --backend vulkan --out <directory>` (or d3d12). The script stages clearly attributed Metal baselines temporarily and preserves their hashes, target backend and actual outcomes. It does not create fake Vulkan/DXR references. Exit 77 remains unsupported, never success. The manual `Path tracer hardware references` workflow wires this to installed Linux/Windows self-hosted runners labeled `rt`; no workflow is dispatched by this feature.

Scene FLIP thresholds are fixed in `tests/golden/scenes.toml` (mean .02, p99 .10). Reference files remain unfiltered. The separate final-denoise 16/4096-spp numerical test retains its original .05 mean criterion; a red result is tracked separately and does not justify changing references or thresholds.


Native pipeline validation (DENG-455): the native Metal adapter has separate
surface and media entry points backed by the same integrator implementation.
The surface pipeline excludes the medium/SSS event loop. The media pipeline is
created only when media are actually selected, then cached and reused across
views and toggles. A failed media creation is cached independently and leaves
the surface pipeline intact. Shared Slang material/transport estimators, shader validation and sampling are unchanged.

The unchanged original Cornell control timed out at 300.04 seconds while creating
the first instrumented RT pipeline, before any sample. A native stack sample
locates the wait inside Metal/AGX linked-intersection shader compilation.
With the entry-point split and normal inline traversal, the original strict
4096-sample golden comparisons pass in 237.83 seconds (Cornell) and 72.32 seconds
(gallery), both below the original 300-second per-test timeout. API and shader
validation are enabled throughout. Independent ASan/UBSan strict runs pass in
240.65 seconds and 73.63 seconds respectively, with PNG files byte-identical
to the Dev captures and committed references. New regressions verify lazy media selection,
reuse,
accumulation resets on media toggles, and failure caching without losing the
surface pipeline. Shared C owner code also compiles with Windows clang-cl.

The two legacy Metal PNG references were captured before the corrected sampling,
unlit-emission and receiver-origin transport fixes (`f5f55763`, `cbde1f11`). Their
mean differences were .110726 (Cornell) and .036885 (gallery). Reviewed strict
captures now replace those two PNGs. As a control, the pre-fix `442476c8` PT owner
and native library, rebuilt into a standalone harness with API validation on
and instrumentation off, produce PNG files byte-identical to both strict repaired
captures. The pipeline repair itself therefore preserves these pixels. Scene,
4096 samples, seed0, eight bounces, exposure1, denoise-off settings and the
original .02/.10 image limits are preserved. The separate frozen unit-test
`pt_cornell_4096.f32` oracle is unchanged; DENG-443 remains open.

The Cornell capture contains 4096 geometry pixels, four material identities and
normal span2. The gallery remains the original six white dielectric/metal furnace
spheres in a uniform white environment: its near-white image has 1821 geometry
pixels, six material identities and normal span1.98997. First-hit guide checks prove that the scenes are populated;
independent Lambert/GGX furnace energy oracles are also retained.

Final-denoise follow-up (DENG-454): the preparation pass estimates noise
without an unconditional spatial box blur. Progressive path tracing records
per-pixel Welford M2 independently for raw path radiance R/G/B. The existing
beauty accumulation supplies the previous mean, without changing its samples.
Preparation converts both the mean and variance using the same current albedo:
`variance[c] = M2[c] / (N * (N - 1) * max(albedo[c], .001)^2)` for N > 1.
This avoids assigning one luminance uncertainty to three different color channels
or combining demodulated moments with a differently demodulated signal at edges.
The single-sample case retains a per-channel spatial bootstrap.

Reset discards previous moments alongside beauty. Moments are collected even
with final denoise off, so enabling it later uses all samples. The private moments
texture is now RGBA32F (16 bytes per pixel, replacing the earlier 8-byte RG32F
luminance texture), with RGB M2 and a reserved W channel. The PT variance scratch
textures are also RGBA32F; other signals retain scalar variance textures.

The PT wavelet path propagates each channel's variance with squared weights as
in [SVGF, equation 3](https://research.nvidia.com/sites/default/files/pubs/2017-07_Spatiotemporal-Variance-Guided-Filtering%3A/svgf_preprint.pdf).
Its range distance cancels expected noise energy and normalizes by per-channel
uncertainty using [Rousselle et al. 2012, equation 4](https://www.cs.umd.edu/~zwicker/publications/AdaptiveRenderingNLM-SIGA12.pdf),
with the paper's damping factor .45. This uses the per-pixel distance with the
existing B3 wavelet support, not a new patch-filter pass. Known color edges at
equal luminance remain intact, while noisy chromatic differences can be filtered.
Other signal policies retain their existing range and variance behavior.

Preparation preserves the measured sample mean instead of clipping valid bright
lighting detail while keeping unchanged moments. After the first sample it skips
spatial bootstrap neighbor reads. The denoiser binds each guide with its actual
read/write access rather than requesting an unused storage view first.

GPU oracles compare all three progressive M2 values against independently
reconstructed path samples at every prefix and after reset. A preparation oracle
preserves a bright mean `(10, 5, 2.5)` among darker neighbors with unequal albedos
`(.5, .25, .125)`, checking independent variance values `(1, 4, 9)` and the
`1e-8` floor. Color-edge tests distinguish zero uncertainty (exact edge retention)
from known RGB noise (analytic B3 weights `11/16` and `5/16` across a step).
Constant independent input variance 2 still becomes `2 * 4900 / 65536` after one
B3 filter; disabling propagation preserves the original variance resource.

The fixed final-denoise comparison improves from FLIP 0.141325818 originally,
through 0.0986457165 after the first variance/preparation correction,
0.0761701224 with measured luminance variance, and 0.0744708595 with the sample
mean preserved, to 0.0630165582 with per-channel raw moments and noise-corrected
RGB distance. It still fails the original 0.05 limit. Its 16/4096 sample counts,
scene and numerical oracle are unchanged. Reference energy remains 106.239134.
This unit comparison has Metal API validation on and shader instrumentation
off. The strict full-PT golden verification above is separate.

Emissive-triangle visibility follow-up (DENG-454): the shadow segment now aims
from the offset receiver origin to the original sampled emitter point. Its BSDF
direction and area PDF still use the unoffset shading point. Previously, keeping
the old visibility direction and distance could include the sampled emitter at
grazing angles. The production GPU proposal regression now checks 256 grazing
segments against an independent emitter plane and a halfway blocker: false emitter
hits fall from 256 to zero, with no lost blockers. Its original 256 receiver-normal
checks also pass. Strict Metal validation passes in Dev and ASan/UBSan.

This correction changes the Cornell golden image. The original strict 4096-sample
run finishes in 256.42 seconds but fails the unchanged image comparison (mean
0.027522 against 0.02; p99 0.079175 against 0.10). The gallery passes unchanged in
75.28 seconds. The stored references are preserved pending review; this is an
open image-comparison failure, not a passing golden result. At that checkpoint both numerical quality
issues remained open: frozen-reference FLIP 0.0800533 and final-denoise FLIP
0.0634995892. Those Dev and ASan/UBSan suites each passed 28 of 30 checks, with
only those two numerical failures.

No-media timing capture (DENG-450):
`pathtracer.no_media_gpu_timings_label_completed_samples` provides the fixed
Cornell16x16, eight-bounce, 24-warm/96-capture measurement fixture in the repository.
After each GPU wait it explicitly collects the graph's completed query results,
then checks the published frame ID against the rendered frame. Reading ordinary
renderer stats immediately after rendering instead measures the preceding frame,
including the final warm-up sample and omitting sample120. The regression catches
all96 shifted labels before collection is added. The corrected collector measures
samples25 through120, with96 distinct frame IDs,120 actual accumulated samples
and no extra render frames. Dev and ASan/UBSan each verify these invariants.

For comparable diagnostic runs, launch from `build/dev/tests/unit/render` with
`MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=0 ./test_render_pathtracer --filter=pathtracer.no_media_gpu_timings_label_completed_samples`.
Each `PT_GPU_FRAME` line includes the sample number, rendered frame ID, timed frame
ID and elapsed milliseconds. The unit test checks attribution and sample counts,
not a timing threshold. This corrects the collection method; it does not establish
the original before/after S17 symmetric ±5% cost criterion or resolve the observed
run-to-run variability. DENG-450 remains open.

Emitter-sidedness correction (DENG-443, DENG-454): PT hit emission now respects the
material's double-sided flag and geometric face orientation, including mirrored
instance transforms. The check is local to PT and does not use the shading normal,
so a normal map cannot make an emitting back face visible. Triangle-light normals
use the same transform orientation. One-sided triangle, rectangle and disk
proposals return zero PDF behind their emitting face; reverse triangle PDFs use
the same support. The shared RT hit structure and other consumers are unchanged.

The production-renderer regression checks Standard green emission and Unlit red
base color, one-sided backs, two-sided backs, front faces and mirrored front/back
instances in both BSDF-only and MIS modes. It verifies all 16 progressive prefixes:
back-face magnitudes stay below 1e-6 and front/two-sided controls remain positive. Separate GPU proposal checks retain front support, reject
one-sided backs, preserve two-sided support, and retain the receiver/endpoint
geometry checks.

The original final-denoise 16/4096-spp comparison now passes at FLIP 0.0423503626
against the unchanged 0.05 limit, with reference energy 104.190551. Dev and
ASan/UBSan agree exactly and each pass 31 of 32 affected checks. The only remaining
numerical failure is the frozen-reference comparison (FLIP 0.0869373 against
0.02). No published reference, scene, sample count or quality limit was changed.
Strict Metal API and shader validation also pass the 20 renderer controls and
the proposal/segment regression (the latter under ASan/UBSan).
The original strict 4096-sample Metal golden comparisons were rerun after this
correction, retaining both 300-second limits and existing references. Cornell
completes in 123.44 seconds but still fails (mean 0.027528 against 0.02, p99
0.079175 against 0.10). Gallery passes in 74.85 seconds with zero FLIP error.
The current Cornell capture differs from the earlier proposed replacement in
just one green byte by one level; neither replacement has been applied.

A separate temporary harness records the corrected Cornell 4096-spp seed-0
numeric reference outside the repository, then runs the unchanged 1024-spp seed-1
comparison against it: FLIP 0.00934483 passes the original 0.02 limit. This
qualifies a possible numeric reference replacement, not the existing frozen
oracle: that original comparison still fails at 0.0869373. The stored F32 and
PNG references remain unchanged pending their separate review decisions.
