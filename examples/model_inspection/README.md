# Cooked model inspection and GPU previews

This Dev sample gives DIDE real multi-mesh metadata through editor IPC 1.3. It creates
and attaches the ordinary GPU renderer so model/mesh/material assets can cook and upload,
but places no model into the game world. The inspector triggers the first asynchronous load.
`asset.renderModel` renders the selected cooked mesh, or the whole node hierarchy, in a
separate offscreen world using the actual imported materials. Preview entities never enter
the game world. Requested LOD and yaw affect the GPU image; shorter whole-model mesh LOD
chains clamp to their last level. A selected mesh rejects a nonexistent LOD.

```sh
cmake --build build/dev --target model_inspection test_editor_model_inspection test_editor_preview_image
ctest --test-dir build/dev -R 'editor_model_inspection|editor_preview_image' --output-on-failure
build/dev/examples/model_inspection/model_inspection/model_inspection
```

The CC0 fixture is generated with `python3 examples/model_inspection/generate.py` using
only Python's standard library. `inspection.gltf` embeds its one buffer: no external files,
downloads or textures. Body is a 192-triangle torus with Wood and Metal primitives; Lid is a
six-triangle pyramid with UV0/UV1 and a Metal material. Root/Pivot nodes exercise hierarchy.
The importer generates actual mesh LODs, meshlets and quantized streams.

`asset.modelInspect {id, mesh}` requires `assets.modelInspection`. It returns true node/mesh
asset references and one selected mesh's vertex count, cooked LOD counts and object-space
errors in metres, named stream formats, actual UV channel indices, bounds, position
dequantization offset/scale and configured quantization-error budget, and per-submesh material IDs and
index/meshlet ranges per LOD. These are not inferred camera switch distances. The existing
legacy `asset.modelInfo` contract remains separate. The skeleton summary may be empty even
when source skin data exists; bind-pose inspection remains part of the broader viewer work.

The service uses the existing bounded async-load queue and returns Busy while loading;
it never waits for an asset on the main thread. Complete inspection is limited to 8192 nodes,
1024 mesh IDs, 4096 submeshes and eight LODs. Oversized or inconsistent data fails explicitly
instead of returning a partial snapshot. No asset pointer crosses the protocol. The internal
adapter borrows loaded strings/float spans only until synchronous response serialization.

The DIDE viewer selects a mesh from its dropdown or a linked hierarchy node. It shows LOD
and quantization errors with their actual units, resets incompatible LOD/UV selections, and
refreshes on source/engine asset changes. Older engines retain the labelled first-mesh view.
The preview keeps its last compatible image visible during turntable updates. Diagnostic
overlays currently return explicit Unsupported errors; returning to None resumes actual
rendering. Source-mode rendering is required. Standalone material previews, importer-settings
comparisons, diagnostic overlays and full skeletal bind-pose inspection remain separate work.
Metadata and preview requests each retain one active request and one replaceable pending
request; close/session/source changes invalidate late results. Compatible completed turntable
frames can publish while a newer angle waits, preventing starvation when rendering is slower
than rotation. The viewer has no per-mesh image cache. Its backend uses a fixed retry deadline
and validates PNG integrity, dimensions, decoded allocation limits and non-animated content.

The service owns at most one reusable private asset system, renderer and ECS world, borrowing
only the existing GPU device and task system. Its staging ring is 16 MiB; it adds no filesystem
watchers. Each request owns one readback and one RGBA image buffer, with dimensions 1–1024
square and a bounded PNG below 5 MiB. This is a buffer bound, not a total GPU/RSS budget: the
renderer and requested model/material dependencies also allocate resources. One request may
be deferred, with a 30-second deadline; overlapping requests return Busy. Model/source changes
replace the context, and idle contexts release after five seconds. Disconnect and shutdown
release the context too. Rendering supports at most eight material slots per mesh, matching
the renderer component; larger meshes fail explicitly rather than silently losing slots.

`editor.previewStats` is a read-only engine console command reporting `contexts`, `pending`,
`rgba_bytes` and `readback_bytes`. These gauges describe owned preview buffers, not full
application memory use or a qualification of the reported new-game memory spike.

The opt-in native acceptance copies this fixture to a disposable project and checks real
mesh/LOD/yaw/whole-model GPU pixels, live material edits and restoration, game-world isolation,
idle cleanup, invalid requests and manifest-driven scale reload through DIDE. From the sibling DIDE repo,
with its current native test-harness binary:

```sh
DIDE_E2E_BIN="$PWD/target/debug/dide" \
DIDE_MODEL_INSPECTION_GAME="$PWD/../engine/build/dev/examples/model_inspection/model_inspection/model_inspection" \
DIDE_MODEL_INSPECTION_EXAMPLE="$PWD/../engine/examples/model_inspection" \
cargo test --locked -p dide-e2e --test asset-viewers real_model_mesh_inspection_and_reload -- --nocapture
```

Both model variables are required when either is set. `DIDE_VIEWER_CAPTURE_DIR` optionally
saves native light/dark/high-contrast captures after control transitions settle. This is a
focused GPU feature test, not a whole-engine release, full-workload memory benchmark or
cross-platform runtime qualification.
