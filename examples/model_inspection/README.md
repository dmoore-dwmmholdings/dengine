# Cooked model inspection

This Dev sample gives DIDE real multi-mesh metadata through editor IPC 1.3. It creates
and attaches the ordinary GPU renderer so model/mesh/material assets can cook and upload,
but places no model into the game world. The inspector triggers the first asynchronous load.
It does not implement or simulate the still-separate `asset.renderModel` GPU preview.

```sh
cmake --build build/dev --target model_inspection test_editor_model_inspection
ctest --test-dir build/dev -R editor_model_inspection --output-on-failure
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
The preview area explains when GPU rendering is unsupported while leaving metadata usable.
Metadata and preview requests each retain one active request and one replaceable pending
request; close/session changes invalidate late results. The viewer has no per-mesh image cache.

The opt-in native acceptance copies this fixture to a disposable project and checks real
mesh selection and manifest-driven scale reload through DIDE. From the sibling DIDE repo,
with its current native test-harness binary:

```sh
DIDE_E2E_BIN="$PWD/target/debug/dide" \
DIDE_MODEL_INSPECTION_GAME="$PWD/../engine/build/dev/examples/model_inspection/model_inspection/model_inspection" \
DIDE_MODEL_INSPECTION_EXAMPLE="$PWD/../engine/examples/model_inspection" \
cargo test -p dide-e2e --test asset-viewers real_model_mesh_inspection_and_reload -- --nocapture
```

Both model variables are required when either is set. `DIDE_VIEWER_CAPTURE_DIR` optionally
saves native light/dark/high-contrast captures after control transitions settle. This is a
focused feature test, not a whole-engine release, GPU-preview or platform-qualification claim.
