# Sky Hop

An original, playable native D-Engine arcade example. Flap the gold bird through teal gates.
Built directly on the public D-Engine window, Metal RHI, asset/shader and input APIs; no browser,
SDL renderer, sprite subsystem or external game framework.

From this checkout:

```sh
cmake --preset dev
cmake --build --preset dev --target sky_hop -j 8
examples/flappy_bird/run.sh
```

The launcher finds the executable relative to itself; the asset root is compiled into the target,
so it works from any current directory. On macOS, the normal launcher opens the native
`build/dev/examples/flappy_bird/sky_hop.app` bundle through Launch Services (display name
**Sky Hop**, bundle ID `com.dengine.skyhop`). You can also open that app directly in Finder.
Command-line flags run its executable directly so smoke/check output and exit codes remain
available. Other platforms retain the plain executable. Dev's existing source shader cooker is required. No new
assets or dependencies are downloaded.

- **Space / left click:** start, flap, or restart after a crash.
- **P:** pause / resume. **R:** restart immediately. **Escape:** quit.
- Avoid the top rail, ground and pipes. Each passed pair earns one point.
- Best score lasts for this application session. No audio or save file is included.

The 320×400 logical pixel-art canvas letterboxes in a resizable 640×800 native window. Fixed
120 Hz simulation uses a bounded accumulator; rendering and upload rectangles reuse two retained
buffers with GPU fences. The collision circle is deliberately a little more forgiving than the
bird's beak/wing artwork.

## Bounded verification

```sh
examples/flappy_bird/run.sh --self-check
examples/flappy_bird/run.sh --offscreen --smoke --frames 360 --check --screenshot /tmp/flappy-demo.png
```

`--self-check` exercises actual game functions for flap/gravity, frozen pause, one-time scoring,
pipe/floor collision and restart. `--smoke` runs an automated flap controller through the same
simulation; it is a test mode, not the normal playable controls. `--frames N --screenshot PATH`
captures an actual GPU-rendered final frame and exits. `--offscreen` omits window creation.
`--check` verifies that bird and ink pixels exist in the actual readback. The default run opens
the real window and waits for your input.

Visual direction: crisp, friendly, readable. A limited cream/sky/ink/teal/gold palette, original
geometric bird and gates, and a compact bespoke 5×7 bitmap alphabet keep the action legible.
Start/play/crash/pause are explicit states, with a single primary flap action and always-visible
secondary keys. Keyboard and mouse are equivalent; pause freezes motion. Native bitmap drawing
does not expose screen-reader semantics or operating-system reduced-motion preferences.
