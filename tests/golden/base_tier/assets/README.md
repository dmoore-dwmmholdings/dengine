# Base-tier production golden assets (DENG-343)

All geometry, materials, textures and environments here are procedural, authored for
DEngine and dedicated to CC0-1.0. No downloaded or third-party artwork is included.
`gen_assets.py` uses the existing procedural gallery geometry/PNG/EXR functions; run
it with Python 3 from any directory. Commit generated assets and reviewed references.

The seven scene families have Medium, High and OFF registrations. They use the
ordinary asset importers, ECS Extract, renderer views and production effects. Cameras
and EV100 are authored in each scene. There is no auto exposure, wall-clock noise or
settle-until-unchanged capture. After asset preparation (at most 48 renderer frames),
render through absolute frame 95 and capture. Optional GPU timing queries are OFF;
rendering work and GPU validation remain enabled. Local-shadow update budget is 192
so coherent point faces warm without introducing a scene-dependent schedule.

- shadows_csm: outdoor columns at 0/8/20/40/75/110 m, shadow distance 160 m.
- shadows_local: spot and rect lights plus a point INSIDE an open slatted cage.
- shadows_pcss_contact: four varying-height plates and thin poles, finite sun radius.
- ao_gtao: room corners, closely spaced ledges and solid boxes.
- reflections_ssr_probes: glossy floor, colored boxes and box-projected DEPR probe.
- gi_probes_ssgi: Cornell colors, emissive ceiling and an eight-probe DEGI volume;
  Medium uses static probes, High adds screen-space GI, OFF disables indirect GI.
- refraction_ss: tinted glass plates in front of a checker background.

OFF disables the family's screen-space/shadow effect, retaining geometry, exposure,
lights and committed reflection/irradiance assets as the documented fallback. Other
unrelated effects are OFF in every family. Refraction is enabled for Medium/High and
its production same-pixel fallback is selected by OFF. References use the existing
harness limits (mean 0.005, p99 0.05), with no threshold adjustment from measurements.

`bakes/reflection.deprobe` is a production DEPR blob, and
`bakes/irradiance.degi` is a distinct production DEGI blob. Default scene assets load
them through the blob asset loader. Capturing never silently substitutes a live bake:
readiness and `baked` status are checked. To deliberately rebuild canonical bakes:

```
DE_GOLDEN_CAPTURE_BAKES=1 build/dev/tests/golden/de-golden --backend metal --scene '*probes*_high' --out /tmp/base-bake
python3 tests/golden/base_tier/assets/gen_assets.py
DE_GOLDEN_CAPTURE_BAKES=1 DE_GOLDEN_VERIFY_BAKES=1 build/dev/tests/golden/de-golden --backend metal --scene '*probes*_high' --out /tmp/base-bake-check
```

The capture variants omit baked assets; the production `r.probes_bake`/`r.gi_bake`
commands export complete versioned blobs. Verification compares every byte, including
headers. Backend goldens load these SAME committed canonical bytes. Their capture
images are backend-specific, not regenerated bake assets.

Use `base_tier/capture.py --exe <de-golden> --backend metal|vulkan|d3d12 --out <dir>`
for all 21 cases twice. It preserves raw logs, rejects skips/renderer errors, and
requires identical PNG bytes between captures. An absent reference is accepted only
by this explicit capture utility, never by ordinary CTest. CI's optional
`base_tier_capture=true` input produces the same Vulkan/lavapipe and WARP artifacts.
Review captured PNGs and copy capture1 images into the matching existing refs tree.
