# mesh_scene: a 3D mesh for editor render-mode probes (DENG-817)

A small Dev sample with real 3D mesh output behind `--editor-service`. It creates the GPU
renderer, loads `assets.toml` and keeps the cooked model resident. `mesh_scene.scene` (a
perspective camera, a sun, the torus `Body` and the pyramid `Lid` from `mesh.gltf`) is placed by
the editor's `scene.load`. The game spawns nothing itself. `mesh.gltf` is a CC0 copy of
`examples/model_inspection/inspection.gltf`.

Launch for DIDE (a static executable, like flappy):

```sh
build/dev/examples/mesh_scene/mesh_scene/mesh_scene --editor-service <endpoint> --no-splash
```

Then `scene.load` the absolute path of `examples/mesh_scene/mesh_scene.scene`, and create a
Scene view with `fovY` > 0 and no `orthoHeight` (a perspective camera; the protocol camera looks
along +Y, so use position `[1, -7, 2]`, identity rotation).

The check `ctest -R editor_mesh_scene_modes` (macOS, Python with `msgpack`; skips with 77
otherwise) reads back the Scene view and requires wireframe, uv0, uv1, meshletIds and normals to
be non-black and to differ from Lit.
