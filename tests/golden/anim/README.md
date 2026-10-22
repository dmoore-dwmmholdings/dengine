# Animation golden fixture

`anim_cesium_pose_pair` renders the existing S11 CesiumMan asset at 0 and 0.4
seconds through the animation system, extraction, palette upload and GPU skinning.
Each 256×256 pose occupies half of a 512×256 image. Fixed camera, EV100, unlit
white material, zero frame delta and disabled temporal lighting keep the image
independent of wall-clock time. Both Update and PostUpdate run, making the fixture
usable with the legacy S19 player and the staged S27 player.

The scene also requires visible geometry in both halves and at least 500 changed
pixels between poses. `anim_cesium_frozen_selftest` deliberately renders time zero
in both halves and must fail the normal image comparison. It uses the same
thresholds as the existing RHI triangle golden: mean FLIP 0.005, p99 0.05.

The initial Metal reference was captured with the unchanged legacy player at
merged engine commit `118440d516ee36c121ac4b609ba330848bb13d56`, using this new
fixture, on Apple M5 Pro / macOS 26.6.2. The staged player at `df32cb54` was then
compared against that independent reference: mean/p99/max FLIP all zero. The
frozen negative control reports mean 0.024828 and p99 0.979080. Normal visible
pixel counts are 4087 and 3025, with 2164 changed pixels.

Reference SHA-256:
`ae748e02222991a8be7ebfb42c8a7f4d4c2ae9801a0c482fcab23a0a8a29495d`

Source asset: `tests/unit/import/data/anim/CesiumMan.glb`, SHA-256
`b7001eaeea8254bd44773bcd247e78696d94169388fbb2a1800fc69434e777d9`.
CesiumMan © 2017 Cesium, CC BY 4.0; upstream attribution and licence details are in
`tests/unit/import/data/anim/KHRONOS_SAMPLES.txt`. This fixture replaces textured
materials with white and does not render the Cesium logo.

Use the existing `de-golden` harness and `docs/testing/golden.md` workflow. Build
`de_golden`; the registered tests are `golden.metal.anim_cesium_pose_pair` and
`golden.metal.anim_cesium_frozen_selftest`. Reference generation selects only
`anim_cesium_pose_pair`, never the intentional failure scene. No existing image
reference or threshold was changed. The initial capture/comparison used Metal
shader validation off; the final registered CTests passed with their unchanged
`MTL_SHADER_VALIDATION=1` setting.

Only Metal is qualified here. No Vulkan/D3D12 image reference or M1 performance
qualification follows from this fixture. DENG-180's original M1 performance gate
remains separate. Runtime architecture and current issue status live in Trama's
`Module: anim`.
