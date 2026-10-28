# Flappy

A Flappy Bird clone written as a host game (`de_game_get_api`, built with `de_add_game`). It uses
Sky Hop's rules (`../flappy_bird/game.h`) and draws everything with UI nodes and text.

- **Space, Up or left click:** flap (also starts and restarts).
- **P:** pause. **R:** restart. **Esc:** quit.

Build and run from the checkout:

```sh
cmake --build --preset windows-clang-cl-dev --target flappy
build/windows-clang-cl-dev/examples/flappy/flappy/flappy.exe
```

In DIDE, **File → Open Project…** and pick `examples/flappy/Flappy.dproj` (or pick it from the
Welcome page). The project's workspace is the engine checkout. The title bar shows the
configuration (Dev) and the run target (**Flappy (engine session)**); the first open configures
Dev. Press Run (or F5): DIDE builds `flappy` and starts the engine session (the Dev editor
service, DENG-622). Pick the **Flappy** run target to run or debug the game on its own.

The level is a scene, `flappy.scene` (DENG-648): a camera, the sky, rail and ground, the bird, the
pipes and the clouds are entities with `DeTransform` and `DeSprite` (pixel art from
`art/generate.py`), tagged `FlappyBird`, `FlappyPipe` and `FlappyCloud`. The game moves them from
its state each frame; the score and messages are UI text. Run on its own, the game loads the scene
itself. In an engine session the game window stays hidden (DENG-647; `DE_EDITOR_SHOW_WINDOW=1`
keeps it) and DIDE opens the scene: the project starts in the World lead, side-on in 2D, and the
hierarchy, inspector and scene view show the entities. Save writes edits back to `flappy.scene`.

The engine session opens in Edit mode, with the simulation held. Play, Pause, Step and Stop come from the IDE. The game state is the `FlappyState`
component on the "Game state" entity, so the inspector shows and edits it live (bird height,
velocity, score, the pipes), and Stop puts it back. In Edit mode the scene stays as authored (the
system that moves it runs in `Update`, which the editor holds); Play runs the game in it.

## Shipping it

Debug and Dev load the checkout's sources (`assets.toml`, which names only files under this
folder). Release has no cooker, so it loads `flappy.dpak` from beside the executable. The
`flappy_pack` target cooks that pack in a Dev build (`build/<preset>/examples/flappy/pack/`), with
`flappy-cook`, `de-cook cook` linked with the game, since the scene uses Flappy's own components
and a Windows game is a static executable that `--game` can't load.

Publish (the toolbar's upload button) does it all: it builds `flappy_pack` in Dev, builds `flappy`
in Release, and uploads the program folder plus the pack to itch.io (`[publish.itch]` in
`Flappy.dproj`). The upload runs from any folder.

Known engine issues this example works around:

- It renders with Vulkan (`game.toml` asks for a Vulkan surface). On D3D12 the renderer presents
  black frames on a hardware adapter. The UI golden scenes show it too: correct on WARP, blank on
  the GPU.
- It sets `render_scale = 1` and no upscaler, because the UI is drawn in output pixels into the
  internal-resolution target, so a lower render scale magnifies it.
- It converts its palette to linear, because UI colours written to an sRGB swapchain are
  gamma-encoded twice.
