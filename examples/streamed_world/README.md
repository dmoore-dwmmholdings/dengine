# Streamed World

The smallest game with a streamed world (DENG-836). `project/assets.toml` declares `[worlds.arena]`:
6 cells of 256 m in a 3 x 2 grid (x 0..768, y 0..512, Z up), with 19 entities from two source
scenes. `de-cook world` wrote the cells to `project/assets/worlds/arena/`; the test
`example_streamed_world_cook` fails when they are stale. To cook again:

```sh
build/dev/tools/cook/de-cook world examples/streamed_world/project/assets.toml --world arena
```

The game loads the manifest only. In an editor session the `world.streaming` service (DENG-831)
opens `arena` and moves the sources. DIDE-401 runs it on macOS as
`build/dev/examples/streamed_world/streamed_world/streamed_world` (Dev build; the editor session
adds its own arguments). `--dev-dir <dir>` mounts a scratch copy of `project/`.
