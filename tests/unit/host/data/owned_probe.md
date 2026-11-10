# Owned-game probe data

`owned_probe.bin` is the existing synthetic lifecycle payload. `owned_probe.dpak`
contains those exact bytes as asset `probe`, type `host_owned_probe`, with no
dependencies. Its platform is ANY; no shader, GPU runtime or game content is included.

Source-mode fixtures keep using `owned_assets.toml`. The Shipping host discovers
the top-level pack before game initialization; its asset system intentionally does
not resolve loose manifest sources. Game callbacks and all original GPU
ownership/teardown assertions remain unchanged.
The two CPU fixture checks validate exact payload bytes and one load/unload without
creating a device, renderer, window or host loop.
They exercise source mode and actual Shipping host pack discovery before registering
the custom probe type, followed by the original manifest/load sequence.

To regenerate in a tools-enabled native build:

```sh
cmake --build build/dev --target test_owned_probe_generator
build/dev/tests/unit/host/fixtures/test_owned_probe_generator "$PWD/tests/unit/host/data"
```

The generator uses the repository's normal pack writer with alignment 8 and names
included. It runs only when invoked explicitly. Keep the pack byte-identical on
regeneration and verify both CPU fixture checks before accepting an update.
