# Path-tracer references

These scenes run the actual renderer and native RT pipeline at 4096 samples, seed 0, eight bounces, a fixed camera, exposure 1 and final denoise off. `r.pt.golden` enforces fixed sampling strategy/seed and disables optional final denoising. Both scenes assert actual primary geometry, distinct material identities and varying world normals.

`pathtracer_cornell` authors a bounded colored room using the original S14 card mesh by reference. Imported cards face engine -Y; scene transforms include the uniform conversion to the intended +Z authoring basis. `pathtracer_gallery_subset` uses the original S14 `scenes/furnace` assets unchanged (six dielectric/metal spheres with varied roughness in a uniform environment). No original gallery assets or loader are copied.

Capture reviewed local references with the existing harness `build/dev/tests/golden/de-golden --backend metal --scene pathtracer_cornell --update-refs`, and likewise for `pathtracer_gallery_subset`. CI refuses reference updates. Normal CTest registration compares per-backend/tier references; the checked-in captures are actual Metal images. Vulkan/DXR captures require suitable native RT hardware.

For an explicit cross-backend comparison against those Metal images, run `python3 tests/golden/pathtracer/compare.py --exe <de-golden> --backend vulkan --out <directory>` (or d3d12). The script stages clearly attributed Metal baselines temporarily and preserves their hashes, target backend and actual outcomes. It does not create fake Vulkan/DXR references. Exit 77 remains unsupported, never success. The manual `Path tracer hardware references` workflow wires this to installed Linux/Windows self-hosted runners labeled `rt`; no workflow is dispatched by this feature.

Scene FLIP thresholds are fixed in `tests/golden/scenes.toml` (mean .02, p99 .10). Reference files remain unfiltered. The separate final-denoise 16/4096-spp numerical test retains its original .05 mean criterion; a red result is tracked separately and does not justify changing references or thresholds.


Final-denoise follow-up (DENG-454): the preparation pass estimates noise
without an unconditional spatial box blur. Progressive path tracing now records
per-pixel demodulated luminance mean and Welford M2 alongside the unmodified
beauty accumulation. With N > 1, the initial variance of the sample mean is
`M2 / (N * (N - 1))`; the single-sample case retains the spatial bootstrap.
Reset discards the previous moments along with the beauty accumulation. Moments
are collected even with final denoise off, so enabling it later uses all samples.
This adds one RG32F texture (8 bytes per pixel) and a read/write per path sample.

The PT wavelet path propagates variance using squared weights as in
[SVGF, equation 3](https://research.nvidia.com/sites/default/files/pubs/2017-07_Spatiotemporal-Variance-Guided-Filtering%3A/svgf_preprint.pdf).
Its RGB range weights preserve color boundaries even at equal luminance.
Other signal policies retain their existing range and variance behavior.
Preparation preserves the measured sample mean instead of clipping valid bright
lighting detail with a spatial firefly clamp while keeping the original moments.
After the first sample it also avoids the spatial bootstrap neighbor reads.
The denoiser binds each guide with its actual read/write access instead of
requesting an unused storage view before replacing it with a sampled view.

GPU oracles compare progressive mean and M2 against independently reconstructed
path samples at every prefix and after reset, preserve equal-luminance color
edges with a legacy-filter negative control, and check that independent input
variance 2 becomes `2 * 4900 / 65536` after one separable B3 filter while constant
radiance stays unchanged. Disabling propagation preserves the original variance
resource. A separate preparation GPU oracle preserves a bright mean of
`(10, 5, 2.5)` among darker neighbors and independently checks alternating
variance-of-mean values of 1 and the `1e-8` floor. The previous clamp reduced
that mean to approximately `(1, .5, .25)` without changing its variance.

The fixed final-denoise comparison improves from FLIP 0.141325818 originally,
through 0.0986457165 after the first variance/preparation correction, to
0.0761701224 with measured path variance and RGB range weights, then
0.0744708595 with the sample mean preserved. It still fails
the original 0.05 limit. Its 16/4096 sample counts, scene and numerical oracle
are unchanged; the 4096-sample reference energy remains 106.239134.
This measurement has Metal API validation enabled and shader instrumentation
disabled; DENG-455 remains open. It is not a validation-enabled path-tracer
golden pass. Further reconstruction work is needed to meet the quality gate.
