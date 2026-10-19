# Path-tracer references

These scenes run the actual renderer and native RT pipeline at 4096 samples, seed 0, eight bounces, a fixed camera, exposure 1 and final denoise off. `r.pt.golden` enforces fixed sampling strategy/seed and disables optional final denoising. Both scenes assert actual primary geometry, distinct material identities and varying world normals.

`pathtracer_cornell` authors a bounded colored room using the original S14 card mesh by reference. Imported cards face engine -Y; scene transforms include the uniform conversion to the intended +Z authoring basis. `pathtracer_gallery_subset` uses the original S14 `scenes/furnace` assets unchanged (six dielectric/metal spheres with varied roughness in a uniform environment). No original gallery assets or loader are copied.

Capture reviewed local references with the existing harness `build/dev/tests/golden/de-golden --backend metal --scene pathtracer_cornell --update-refs`, and likewise for `pathtracer_gallery_subset`. CI refuses reference updates. Normal CTest registration compares per-backend/tier references; the checked-in captures are actual Metal images. Vulkan/DXR captures require suitable native RT hardware.

For an explicit cross-backend comparison against those Metal images, run `python3 tests/golden/pathtracer/compare.py --exe <de-golden> --backend vulkan --out <directory>` (or d3d12). The script stages clearly attributed Metal baselines temporarily and preserves their hashes, target backend and actual outcomes. It does not create fake Vulkan/DXR references. Exit 77 remains unsupported, never success. The manual `Path tracer hardware references` workflow wires this to installed Linux/Windows self-hosted runners labeled `rt`; no workflow is dispatched by this feature.

Scene FLIP thresholds are fixed in `tests/golden/scenes.toml` (mean .02, p99 .10). Reference files remain unfiltered. The separate final-denoise 16/4096-spp numerical test retains its original .05 mean criterion; a red result is tracked separately and does not justify changing references or thresholds.


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
scene and numerical oracle are unchanged; reference energy remains 106.239134.
The full-PT measurement has Metal API validation on and shader instrumentation
off. DENG-455 remains open; this is not a validation-enabled PT golden pass.
