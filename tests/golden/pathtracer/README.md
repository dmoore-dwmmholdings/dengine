# Path-tracer references

These scenes run the actual renderer and native RT pipeline at 4096 samples, seed 0, eight bounces, a fixed camera, exposure 1 and final denoise off. `r.pt.golden` enforces fixed sampling strategy/seed and disables optional final denoising. Both scenes assert actual primary geometry, distinct material identities and varying world normals.

`pathtracer_cornell` authors a bounded colored room using the original S14 card mesh by reference. Imported cards face engine -Y; scene transforms include the uniform conversion to the intended +Z authoring basis. `pathtracer_gallery_subset` uses the original S14 `scenes/furnace` assets unchanged (six dielectric/metal spheres with varied roughness in a uniform environment). No original gallery assets or loader are copied.

Capture reviewed local references with the existing harness `build/dev/tests/golden/de-golden --backend metal --scene pathtracer_cornell --update-refs`, and likewise for `pathtracer_gallery_subset`. CI refuses reference updates. Normal CTest registration compares per-backend/tier references; the checked-in captures are actual Metal images. Vulkan/DXR captures require suitable native RT hardware.

For an explicit cross-backend comparison against those Metal images, run `python3 tests/golden/pathtracer/compare.py --exe <de-golden> --backend vulkan --out <directory>` (or d3d12). The script stages clearly attributed Metal baselines temporarily and preserves their hashes, target backend and actual outcomes. It does not create fake Vulkan/DXR references. Exit 77 remains unsupported, never success. The manual `Path tracer hardware references` workflow wires this to installed Linux/Windows self-hosted runners labeled `rt`; no workflow is dispatched by this feature.

Scene FLIP thresholds are fixed in `tests/golden/scenes.toml` (mean .02, p99 .10). Reference files remain unfiltered. The separate final-denoise 16/4096-spp numerical test retains its original .05 mean criterion; a red result is tracked separately and does not justify changing references or thresholds.


Final-denoise follow-up (DENG-454): the preparation pass now estimates noise
without an unconditional spatial box blur. The PT wavelet path propagates the
variance of its weighted estimate between levels, using squared weights as in
[SVGF, equation 3](https://research.nvidia.com/sites/default/files/pubs/2017-07_Spatiotemporal-Variance-Guided-Filtering%3A/svgf_preprint.pdf).
Other signal policies retain their existing conservative variance behavior.
A GPU oracle checks that a constant independent input variance of 2 becomes
`2 * 4900 / 65536` after one separable B3 filter, while constant radiance stays
unchanged; disabling propagation preserves the original variance resource.

The fixed final-denoise comparison improves from FLIP 0.141325818 to
0.0986457165, but still fails its original 0.05 limit. Its 16/4096 sample counts,
scene and numerical oracle are unchanged. This measurement has Metal API
validation enabled and shader instrumentation disabled; DENG-455 remains open.
It is not a validation-enabled path-tracer golden pass. Further work is needed
to distinguish Monte Carlo noise from real illumination gradients.
