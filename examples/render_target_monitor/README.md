# Render-target monitor

`monitor.c` is a reusable public-API example, compiled by the native text/render
acceptance target. It includes only installed `dengine` headers and its own header.
It creates a camera view targeting an owned GPU texture and binds that texture to
the base-colour slot of a runtime material on a real mesh. No render internals,
custom composite pass or asset-handle wrapping is required by the game code.

After attaching the renderer to the world and loading `mesh/card#card` and
`mat/white` from `tests/unit/text/screen-assets.toml`, pass their handles to
`monitor_example_open` with a zeroed/unopened monitor state. An existing main camera
sees the monitor on layer 2; set its exposure for the intended indoor scene rather
than the default daylight exposure. The
producer camera sees layer 1 and renders at order -10, before the main view at 0.
Populate layer 1 with the scene the camera should show. Move the producer camera
entity to change its viewpoint. The sample rotates the supplied card mesh from its
imported -Y normal to face the +Z main camera.

Call `monitor_example_resize` between frames to create a new texture and producer
view, update the material and camera, and retire the old resources. Invalid
dimensions leave the existing monitor intact. A failed binding does not replace
the old texture. Replacement can block while outstanding render/GPU work drains.
Destroy the monitor with `monitor_example_close` before destroying its world or
renderer; its loaded mesh/material assets still follow the game's asset lifetime.

The binding retains the GPU texture generation even if the owner requests texture
destruction. It does not take ownership: every caller-created texture still needs
one destroy call. Cloning a bound material retains each texture separately; cloning
after the owner has requested texture destruction fails instead of creating new
leases on retired ownership. `de_material_set_params` leaves GPU overrides intact;
binding a zero GPU texture restores that slot's ordinary asset texture.

Do not sample a view's own target or a target produced later in the frame. Layer
masks separate producer geometry from monitor geometry; the renderer rejects
feedback or unordered consumers. A texture without an active producer view must
already be in `DE_ACCESS_SHADER_READ`. These declarations also cover common
material sampling in compute and ray-tracing shaders, but this example's physical
acceptance uses rasterized Unlit and Standard materials.

Build instructions are in `docs/building.md`. Focused acceptance:

```sh
cmake --build build/dev --target test_text_render test_render_material_instances
build/dev/tests/unit/text/test_text_render --filter=text_batch.metal_public_material_monitor_feed_and_resize
build/dev/tests/unit/render/test_render_material_instances '--filter=materials.gpu_texture*'
```
