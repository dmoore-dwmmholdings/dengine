# DEngine

C11 game engine. The spec is `SPEC.md`. The API sketches are in `include/dengine/*.h`.

The work is planned in Andare project **DENG** (initiative DENG-1; decisions DENG-2). Building it:
- Orchestrator loop: `docs/agents/LOOP.md`. Run it with `/loop Follow docs/agents/LOOP.md`.
- Per-issue worker: `docs/agents/WORKER.md`
- Code, git, test and verification rules: `docs/agents/CONVENTIONS.md`
- Project documentation: Trama space **DENG**. Read it for context, keep it updated: `docs/agents/DOCS.md`

## Build and test quick start

Full guide: `docs/building.md`. Full rules: `docs/agents/CONVENTIONS.md`.

```sh
cmake --preset dev                  # host preset (macOS Dev on the build host)
cmake --build --preset dev
ctest --preset dev                  # everything; narrow with -L unit, -L check, -R <name>
cmake --preset windows-xwin-dev && cmake --build --preset windows-xwin-dev  # needs xwin
```

Where things live:

- `include/dengine/`: public headers (`de_*.h`)
- `src/<mod>/`: one directory per module; `src/host/` is the app host (main loop, game API)
- `cmake/`: build helpers, and `cmake/toolchains/` for cross builds
- `shaders/`: Slang sources
- `tools/`: CLIs and checks (`cook/`, `pack/`, `vendor/`, `check_layers/`, `editor/`)
- `third_party/`: vendored libraries, pinned in `third_party/vendor.toml`
- `tests/`: `unit/`, `fuzz/`, `golden/`, `determinism/`
- `examples/`; `docs/` (build guide, agent rules; module pages live in Trama space DENG)

Adding code:

- Sources: `de_add_module()` in `src/<mod>/CMakeLists.txt`. Tests: `de_add_test()`.
- Never edit the root `CMakeLists.txt`.
- A change to a module updates its Trama page `Module: <mod>` (`docs/agents/DOCS.md`).

Before review: `ctest --preset dev -L check` (`header_check`, `layering_check`) passes, and so
does the full `ctest --preset dev`.
