# Path-tracer references

These scenes run the actual renderer and native RT pipeline at 4096 samples, seed 0, eight bounces, a fixed camera, exposure 1 and final denoise off. `r.pt.golden` enforces fixed sampling strategy/seed and disables optional final denoising. Both scenes assert actual primary geometry, distinct material identities and varying world normals.

`pathtracer_cornell` authors a bounded colored room using the original S14 card mesh by reference. Imported cards face engine -Y; scene transforms include the uniform conversion to the intended +Z authoring basis. `pathtracer_gallery_subset` uses the original S14 `scenes/furnace` assets unchanged (six dielectric/metal spheres with varied roughness in a uniform environment). No original gallery assets or loader are copied.

Capture reviewed local references with the existing harness `build/dev/tests/golden/de-golden --backend metal --scene pathtracer_cornell --update-refs`, and likewise for `pathtracer_gallery_subset`. CI refuses reference updates. Normal CTest registration compares per-backend/tier references; the checked-in captures are actual Metal images. Vulkan/DXR captures require suitable native RT hardware.

For an explicit cross-backend comparison against those Metal images, run `python3 tests/golden/pathtracer/compare.py --exe <de-golden> --backend vulkan --out <directory>` (or d3d12). The script stages clearly attributed Metal baselines temporarily and preserves their hashes, target backend and actual outcomes. It does not create fake Vulkan/DXR references. Exit 77 remains unsupported, never success. The manual `Path tracer hardware references` workflow wires this to installed Linux/Windows self-hosted runners labeled `rt`; no workflow is dispatched by this feature.

Scene FLIP thresholds are fixed in `tests/golden/scenes.toml` (mean .02, p99 .10). Reference files remain unfiltered. The separate final-denoise 16/4096-spp numerical test retains its original .05 mean criterion; a red result is tracked separately and does not justify changing references or thresholds.
