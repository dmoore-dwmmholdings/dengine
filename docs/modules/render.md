# Renderer

## Forward participants (DENG-322 provider)

`de_render_add_forward_participant` registers a bounded renderer-owned provider:
up to 16 registrations and 65,536 total declared items. The main-thread call
waits for CPU rendering before changing the registry. Duplicate callback/user
pairs and invalid capacities fail without allocating resources. Removal drains
both CPU rendering and GPU submissions before the caller frees its storage.

The render-thread gather callback receives a frame-owned opaque context and a
bounded output span. Its items contain a render-origin-relative centre, stable
entity identity, caller packet index, optional authored layer/order and an
always-on-top flag. Returning more than capacity, or a non-finite centre, refuses
the batch. Gather and build callbacks read immutable Extract packets; they must
not read ECS or asset state. The renderer allocates the item/sort scratch from
its fence-owned frame arena.

Ordinary meshes and participants share the existing layer/order, descending view
depth and stable entity ordering. Equal participant keys retain registration and
item order. Always-on-top participants form a final stable group. The renderer
splits ordinary mesh runs only where a participant belongs, invoking its graph
builder at that exact position. The same colour target dependencies preserve
interleaving; this also retains the existing mesh RT refraction capture path.
Sorting remains per object/root, rather than per triangle.

Participants add their own passes using `de_render_forward_color` and optional
`de_render_forward_depth`. Each pass calls `de_render_forward_declare`, which
shares the ordinary forward pass's lighting, shadow, IBL and GI resource
declarations, plus scene tables and colour ordering. Execute callbacks obtain
current bindless indices with `de_render_forward_bindings`. The shader bindings
match `lighting_common.slang`'s `DeLightingView` and `DeClusterLists`; custom
shaders reuse the existing lighting functions. UI consumers need no private
renderer C headers.

A scene containing only participants receives a real cleared reverse-Z depth
target, HDR scene colour and clustered lights. Its shared lighting-input pass
publishes constants without dispatching nonexistent deferred tiles. Opaque
screen-space RT production is skipped when no opaque G-buffer exists; forward
surfaces retain their own direct clustered shading.

## Host window surfaces

The host creates its window before the game's `init` callback. `[window] surface`
in `game.toml` defaults to `"auto"`: Metal on macOS, Vulkan on Linux, and no
API-specific surface for D3D12 on Windows. A game selecting another backend must
set `surface = "vulkan"`, `"metal"`, or `"none"` to match it. Headless hosts do
not create a window. Game initialization can create a device and renderer on
`engine->window` directly. Assign successful creations to `engine->gpu` and
`engine->renderer` to transfer teardown to the host; it unloads assets before
destroying the renderer, device, and window. Release game-owned load handles in
game shutdown rather than destroying an assigned renderer while its assets live.

## Immutable view and pixel context

Pixel sprite snapshots retain each view's last successfully rendered camera/grid/plane.
Current and previous pivots snap independently; grid history uses absolute coordinates
across render-origin rebases. Disabling sprite snapping preserves the previously snapped
pose for that transition frame, while bounds and TLAS continue to follow only the current pose.

`de_renderer_extract_view_info` is valid after `de.render.extract_views` during
Extract. Camera matrices, double-precision render origin, layer mask, effective
settings, viewport dimensions, pixel settings and fitted content rectangle are
captured with that exact Extract token. Submission frame/ring are initially
`UINT64_MAX` / `UINT32_MAX`. Render hooks query the matching metadata with
`de_view_frame_info_get`; participant callbacks use
`de_render_forward_view_info`. Render frame/ring then refer to the actual
submission, whose slot has already passed its reuse fence.

Changing a view's grid/settings after Extract affects the next extracted frame.
The current render uses the frozen configuration, including camera snap and
scroll residual. Metadata's internal dimensions remain internal even when a
later upscale stage changes the current hook target to output resolution.
`viewport_px` locates the view in its destination; `content_px` is local to the
view output and uses the renderer's actual integer/fractional fit function.

`DE_HOOK_PIXEL_UI` runs on tonemapped internal pixels immediately before palette
conversion and the existing upscale. This allows pixel UI to share scene scale,
letterbox and palette. `DE_HOOK_UI` keeps its existing output-resolution stage.
A participant's callback/data must remain valid through the submission fence;
the metadata and graph context are immutable until that frame retires.

## Provider verification

Focused native/CPU checks pass: seven participant tests, ten forward regression
cases, eleven view cases, three pixel cases and layering (32 total). Metal
readbacks verify a real translucent mesh / participant / mesh blend against an
independent blend oracle, participant-only clustered point lighting, opaque wall
occlusion versus no-depth overlay, exact Extract grid/origin metadata at 100 km,
and 480×270 pixels upscaled ×4 with zero output mismatches. The analytic
point-light readback is RGB 6.48438 / 0.64844 / 0.12964 versus unlit 0.83301;
depth-tested output matches the wall-only reference. Header C11/C++17 checks pass.
Vulkan and D3D12 execution remain hardware qualifications. This provider
checkpoint does not complete DENG-322's UI consumer, pointer or world/pixel UI
goldens.

`de_view_layout_info` is an advisory main-thread query for PostUpdate layout;
view zero selects the renderer's main view. It uses the same fitted content
rectangle calculation as rendering. A consumer must reconcile this advisory grid
with its exact Extract metadata before publishing geometry. It must skip a
mismatched packet rather than combine next-frame layout with an older camera.

`de_render_forward_alloc` provides bounded, aligned temporary storage from the
current renderer frame arena. Zero bytes, non-power-of-two alignment, alignment
above 4096, overflow and exhaustion return NULL. The context and allocations
last through that submission's fence; callbacks must not retain them beyond it.
