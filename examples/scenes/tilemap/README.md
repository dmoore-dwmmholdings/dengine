# Tilemap sample scene

A 16 x 9 Tiled map (`maps/level.tmj`, external tileset `tiles/terrain.tsj`) with two layers:
`back` (flowers, a stone platform) and `ground` (grass, animated water, slopes, a brick wall; the
layer property `collision = true` makes its tiles solid). `tilemap.scene.toml` puts a side-view
camera, two lights and a `DeTilemap` entity over it. A game shows it with:

```c
de_tilemap_attach(world, assets, NULL);         /* before loading the scene */
de_asset_manifest_load(assets, "tilemap:/assets.toml");
de_load_handle h = de_asset_load_bundle(assets, "tilemap", 0);
/* ... when READY: */
de_prefab_instantiate(assets, world, de_asset_scene(assets, de_asset_id_from("scene/tilemap")),
                      DE_ENTITY_NULL, NULL);
```

The golden image `tilemap_lit_two_layers` (tests/golden/tilemap/) renders this scene.
